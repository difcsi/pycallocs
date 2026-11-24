#!/usr/bin/env python3
"""cross_bench.py [-B build_dir] [-r rounds] [-n calls] [--routes a,b,...]
               [--out dir] [--timeout seconds] [--allow-missing]

Microseconds per Lua -> Python call, by route: a bridge written by hand, cffi
in embedding mode, lupa, and PolyLink by value, by reference, and by reference
from the contrib/lua fork (a native foreign value). Every route is a ctest of
the LinkPy build that runs the same Lua loop (lua_py/benchloop.lua) against the
same Python callee and prints `bench <workload>: <us> us/call`; this script
runs them pinned to one core (BENCH_CPU, default 2), interleaved within each
round, and reports min and median over the rounds, as wall_bench.sh does. The
workload the report's figure uses is `describe` (a struct Point in, the sum of
its fields out); `divide` (two ints in, a float out) is the fixed cost of a
call, on the routes that have it.

A route that is not in the build is reported as `missing`, one that does not
produce a checked `describe` time as `failed`, each with the reason; the others
are still measured. Exit status 0 when every selected route was measured,
1 otherwise (--allow-missing forgives `missing`, never `failed`), 2 on a usage
error. Writes <out>/figure10.csv and <out>/figure10.json (default
<build>/bench/fig10/) and keeps its own linkstub cache there. Take the numbers
on a quiet machine and compare runs made the same way only (README.md).

  cross_bench.py                       # every route, 7 rounds of 200000 calls
  cross_bench.py -r 2 -n 20000 --allow-missing    # a quick look
  cross_bench.py --routes hand,polylink-value -r 9
"""
import argparse
import csv
import datetime
import json
import os
import platform
import re
import signal
import statistics
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from typing import Callable

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))


@dataclass(frozen=True)
class Route:
    id: str
    label: str        # the bar's label in the figure
    ctest: str        # the test that runs this route
    expect_info: dict = field(default_factory=dict)   # `info k=v` lines the run must print


ROUTES = [
    Route("hand", "hand-written bridge", "bench_lua_py_hand"),
    Route("cffi", "cffi", "bench_lua_py_cffi"),
    Route("lupa", "lupa", "bench_lua_py_lupa"),
    Route("polylink-value", "PolyLink by value", "cross_bench_lua_py"),
    Route("polylink-ref", "PolyLink by reference", "cross_bench_lua_py_ref", {"view": "proxy"}),
    Route("polylua-native", "PolyLua native view", "cross_bench_lua_py_native", {"view": "native"}),
]
WORKLOADS = ("describe", "divide")
BASELINE = "hand"
PRIME_CALLS = 1000
COLUMNS = ["route", "label", "workload", "status", "min_us", "median_us",
           "rel_to_hand", "rounds", "calls", "note"]


@dataclass
class TestSpec:
    """How ctest would run a test."""
    __test__ = False          # not a test class, whatever a collector thinks of the name
    command: list
    workdir: object           # str or None
    env: dict


@dataclass
class RunResult:
    ok: bool
    times: dict               # workload -> us per call
    info: dict
    note: str                 # why not, when not ok


@dataclass
class RouteData:
    status: str               # "ok" | "missing" | "failed"
    note: str
    samples: dict             # workload -> [us per call, one per round]
    info: dict


# --------------------------------------------------------------- reading ctest
def spec_from_ctest_json(doc, name):
    """The TestSpec of test `name` in a `ctest --show-only=json-v1` document,
    or None when the build has no such test (or it has no command)."""
    for test in doc.get("tests", []):
        if test.get("name") != name:
            continue
        if not test.get("command"):
            return None
        workdir, env = None, {}
        for prop in test.get("properties", []):
            if prop["name"] == "WORKING_DIRECTORY":
                workdir = prop["value"]
            elif prop["name"] == "ENVIRONMENT":
                for entry in prop["value"]:
                    key, _, value = entry.partition("=")
                    env[key] = value
        return TestSpec(list(test["command"]), workdir, env)
    return None


# ------------------------------------------------------------ reading a run
_BENCH = re.compile(r"bench (.+?): ([0-9]+(?:\.[0-9]+)?) us/call")
_INFO = re.compile(r"info ([A-Za-z_][A-Za-z0-9_]*)=(.*)")

# What the liballocs runtime and the interpreters print around a result, none
# of which says why a run failed.
_NOISE = re.compile(
    r"MMAP:|lazily mapped|Warning: mapping of|liballocs summary|^queries |"
    r"^[=-]{8,}$|^stack traceback:|^bench |^info ")


def parse_bench_output(text):
    """(times, info) from a run's output: `bench <workload>: <us> us/call ...`
    and `info <key>=<value>` lines; everything else is ignored."""
    times, info = {}, {}
    for line in text.splitlines():
        line = line.strip()
        m = _BENCH.match(line)
        if m:
            times[m.group(1)] = float(m.group(2))
            continue
        m = _INFO.match(line)
        if m:
            info[m.group(1)] = m.group(2).strip()
    return times, info


def failure_note(text):
    """The one line of a failed run's output that says why."""
    last = None
    for raw in text.splitlines():
        line = raw.strip()
        if "Assertion failed" in line:
            return line
        # Indented lines are traceback entries, not the error itself.
        if line and not raw[0].isspace() and not _NOISE.search(line):
            last = line
    return last or "no output"


def _exit_text(code):
    if code < 0:
        try:
            return "killed by " + signal.Signals(-code).name
        except ValueError:
            return f"killed by signal {-code}"
    return f"exit {code}"


def check_run(route, exit_code, text):
    """Judge one run of a route from its exit code and its output."""
    times, info = parse_bench_output(text)
    if exit_code != 0:
        return RunResult(False, times, info, f"{_exit_text(exit_code)}: {failure_note(text)}")
    if "describe" not in times:
        return RunResult(False, times, info, "no describe line in the output")
    for key, want in route.expect_info.items():
        if info.get(key) != want:
            return RunResult(False, times, info, f"info {key}={info.get(key)}, expected {want}")
    return RunResult(True, times, info, "")


# ----------------------------------------------------------------- measuring
def measure(routes, specs, rounds, calls, runner: Callable):
    """Run every route: once to prime it (the linkstub is compiled and cached,
    the route is validated), then `rounds` times with `calls` calls, the routes
    interleaved within each round. `specs` maps a ctest name to its TestSpec or
    None; `runner(route, spec, calls)` returns a RunResult. A route that fails
    once is not run again."""
    data = {}
    for route in routes:
        if specs.get(route.ctest) is None:
            data[route.id] = RouteData("missing", f"no ctest {route.ctest} in the build", {}, {})
        else:
            data[route.id] = RouteData("ok", "", {}, {})

    def run(route, n, keep):
        d = data[route.id]
        if d.status != "ok":
            return
        result = runner(route, specs[route.ctest], n)
        d.info.update(result.info)
        if not result.ok:
            d.status, d.note = "failed", result.note
        elif keep:
            for workload, us in result.times.items():
                d.samples.setdefault(workload, []).append(us)

    for route in routes:
        run(route, PRIME_CALLS, keep=False)
    for _ in range(rounds):
        for route in routes:
            run(route, calls, keep=True)
    return data


def rows(routes, data, calls):
    """The table: for a measured route one row per workload it has samples
    for, for any other route one row that says why not."""
    base = data.get(BASELINE)
    out = []
    for route in routes:
        d = data[route.id]
        row = {"route": route.id, "label": route.label, "workload": "", "status": d.status,
               "min_us": "", "median_us": "", "rel_to_hand": "", "rounds": "",
               "calls": str(calls), "note": d.note}
        if d.status != "ok":
            out.append(row)
            continue
        for workload in WORKLOADS:
            samples = d.samples.get(workload)
            if not samples:
                continue
            rel = ""
            if base is not None and base.status == "ok" and base.samples.get(workload):
                rel = f"{min(samples) / min(base.samples[workload]):.3f}"
            out.append({**row, "workload": workload,
                        "min_us": f"{min(samples):.3f}",
                        "median_us": f"{statistics.median(samples):.3f}",
                        "rel_to_hand": rel, "rounds": str(len(samples))})
    return out


def write_csv(table, path):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=COLUMNS)
        writer.writeheader()
        writer.writerows(table)


def exit_status(data, *, allow_missing):
    statuses = {d.status for d in data.values()}
    if "failed" in statuses or ("missing" in statuses and not allow_missing):
        return 1
    return 0


# ------------------------------------------------------------------ running
def run_subprocess(route, spec, calls, *, cpu, cache_dir, timeout):
    """Run a route's test command the way ctest would, pinned to `cpu`: the
    caller's environment without an auditor or a preload of its own, then the
    test's."""
    env = {k: v for k, v in os.environ.items() if k not in ("LD_AUDIT", "LD_PRELOAD")}
    env.update(spec.env)
    env["LINKPY_BENCH_CALLS"] = str(calls)
    env["LINKPY_CACHE_DIR"] = cache_dir
    command = ["taskset", "-c", str(cpu)] + spec.command
    try:
        p = subprocess.run(command, cwd=spec.workdir, env=env, capture_output=True,
                           text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return RunResult(False, {}, {}, f"no result after {timeout} s")
    except OSError as e:
        return RunResult(False, {}, {}, f"cannot run {command[3]}: {e.strerror}")
    return check_run(route, p.returncode, p.stdout + "\n" + p.stderr)


def _git(directory):
    """{'commit', 'dirty'} of the checkout holding `directory`, or None."""
    def out(*args):
        return subprocess.run(["git", "-C", directory, *args], capture_output=True,
                              text=True, check=True).stdout.strip()
    try:
        return {"commit": out("rev-parse", "HEAD"),
                "dirty": bool(out("status", "--porcelain", "--untracked-files=no"))}
    except (OSError, subprocess.CalledProcessError):
        return None


def _cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _print_table(table):
    heads = ["route", "workload", "status", "min_us", "median_us", "rel_to_hand", "rounds", "note"]
    widths = {h: max(len(h), *(len(r[h]) for r in table)) for h in heads[:-1]}
    print("  ".join(h.ljust(widths[h]) for h in heads[:-1]) + "  note")
    for r in table:
        print("  ".join(r[h].ljust(widths[h]) for h in heads[:-1]) + "  " + r["note"])


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Microseconds per Lua -> Python call, by route (see the header of this file).")
    ap.add_argument("-B", dest="build", default=os.path.join(REPO, "linker", "build"),
                    help="the LinkPy CMake build (default: linker/build of this tree)")
    ap.add_argument("-r", dest="rounds", type=int, default=7, help="timed rounds (default 7)")
    ap.add_argument("-n", dest="calls", type=int, default=200000,
                    help="timed calls per workload and round (default 200000)")
    ap.add_argument("--routes", help="comma-separated route ids (default: all): "
                    + ",".join(r.id for r in ROUTES))
    ap.add_argument("--out", help="output directory (default: <build>/bench/fig10)")
    ap.add_argument("--timeout", type=int, default=300, help="seconds allowed per run (default 300)")
    ap.add_argument("--allow-missing", action="store_true",
                    help="exit 0 although a route is not in the build")
    args = ap.parse_args(argv)

    def usage_error(message):
        print(f"cross_bench: {message}", file=sys.stderr)
        return 2

    if args.rounds < 1 or args.calls < 1:
        return usage_error("-r and -n must be at least 1")
    cpu_text = os.environ.get("BENCH_CPU", "2")
    if not cpu_text.isdigit() or int(cpu_text) >= (os.cpu_count() or 1):
        return usage_error(f"BENCH_CPU={cpu_text} is not a CPU of this machine "
                           f"(0..{(os.cpu_count() or 1) - 1})")
    cpu = int(cpu_text)

    routes = ROUTES
    if args.routes:
        by_id = {r.id: r for r in ROUTES}
        wanted = [x for x in args.routes.split(",") if x]
        unknown = [x for x in wanted if x not in by_id]
        if unknown or not wanted:
            return usage_error(f"unknown route {', '.join(unknown) or '(none given)'}; "
                               f"routes are {', '.join(by_id)}")
        routes = [r for r in ROUTES if r.id in wanted]      # figure order, each once

    build = os.path.abspath(args.build)
    try:
        listing = subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                                 capture_output=True, text=True, check=True).stdout
        doc = json.loads(listing)
    except (OSError, subprocess.CalledProcessError, ValueError):
        return usage_error(f"cannot list the tests of {build} (is it a configured LinkPy build?)")
    specs = {r.ctest: spec_from_ctest_json(doc, r.ctest) for r in routes}

    out = os.path.abspath(args.out) if args.out else os.path.join(build, "bench", "fig10")
    cache_dir = os.path.join(out, "cache")
    os.makedirs(cache_dir, exist_ok=True)

    started = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    load = os.getloadavg()[0]
    print(f"cross_bench: {len(routes)} routes, {args.rounds} rounds of {args.calls} calls, "
          f"cpu {cpu}, load {load:.2f}", file=sys.stderr)

    def runner(route, spec, calls):
        print(f"  {route.id} ({calls} calls)", file=sys.stderr, flush=True)
        return run_subprocess(route, spec, calls, cpu=cpu, cache_dir=cache_dir, timeout=args.timeout)

    data = measure(routes, specs, args.rounds, args.calls, runner)
    table = rows(routes, data, args.calls)

    write_csv(table, os.path.join(out, "figure10.csv"))
    fork = specs.get("cross_bench_lua_py_native")
    meta = {
        "date": started,
        "repo": _git(REPO),
        "lua_fork": _git(os.path.dirname(fork.command[0])) if fork else None,
        "cpu_model": _cpu_model(),
        "pinned_cpu": cpu,
        "load_1min_at_start": load,
        "kernel": platform.release(),
        "build": build,
        "rounds": args.rounds,
        "calls": args.calls,
        "routes": {r.id: {"label": r.label, "ctest": r.ctest,
                          "command": specs[r.ctest].command if specs[r.ctest] else None,
                          **asdict(data[r.id])} for r in routes},
    }
    with open(os.path.join(out, "figure10.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    _print_table(table)
    print(f"written: {os.path.join(out, 'figure10.csv')} and figure10.json", file=sys.stderr)
    return exit_status(data, allow_missing=args.allow_missing)


if __name__ == "__main__":
    sys.exit(main())
