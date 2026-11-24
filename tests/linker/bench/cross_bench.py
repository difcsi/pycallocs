#!/usr/bin/env python3
"""cross_bench.py [-B build_dir] [-r rounds] [-n calls] [--routes a,b,...]
               [--precision percent] [--max-rounds n] [--out dir]
               [--timeout seconds] [--allow-missing]

Microseconds per Lua -> Python call, by route, and the lines of code each
route takes: a bridge written by hand, cffi in embedding mode, lupa, and
PolyLink by value, by reference, and by reference from the contrib/lua fork
(a native foreign value). Every route is a ctest of the LinkPy build that runs
the same Lua loop (lua_py/benchloop.lua) against the same Python callee and
prints `bench <workload>: <us> us/call`. The workload the report's figure uses
is `describe` (a struct Point in, the sum of its fields out); `divide` (two
ints in, a float out) is the fixed cost of a call, on the routes that have it.

Method: each route is run once with 1000 calls (the linkstub is compiled and
cached, the route is validated), then every route once at full length with the
result thrown away (the core leaves whatever state the priming left it in),
then the rounds, each round starting one route further down the list. Every
run is pinned to one CPU, chosen on the machine at hand: one of its fastest
cores, and not CPU 0's when there is another (BENCH_CPU overrides). When the
`describe` times of a route still vary between rounds by more than
--precision (interquartile range over median, default 5 percent), rounds are
added, up to --max-rounds (default three times -r), and what is still above
it then is named in a warning. Reported per route and workload: min, median
and that spread over the rounds, and the median over the rounds of the ratio
to the hand-written bridge in the same round. Lines of code are counted with cloc
over the files that exist for one route only (Route.sources).

A route that is not in the build is reported as `missing`, one that does not
produce a checked `describe` time as `failed`, each with the reason; the others
are still measured. Exit status 0 when every selected route was measured,
1 otherwise (--allow-missing forgives `missing`, never `failed`), 2 on a usage
error. Writes, under <out> (default <build>/bench/fig10/), figure10.csv,
figure10.json (every sample, commits, machine, build settings) and
figure10.tex (the figure, as a tikzpicture), and keeps its own linkstub cache
there. Take the numbers on a quiet machine and compare runs made the same way
only (README.md).

  cross_bench.py                       # every route, 7 rounds of 200000 calls
  cross_bench.py -r 2 -n 20000 --allow-missing    # a quick look
  cross_bench.py --routes hand,polylink-value -r 9
"""
import argparse
import csv
import datetime
import json
import math
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
    sources: tuple = ()   # the files that exist for this route only, relative to the repository


_BASE = "tests/linker/bench/lua_py/"
_CROSS = "tests/linker/cross/"
_BYREF = (_CROSS + "bench_lua_py_ref/include/shapes.py.h", _CROSS + "bench_lua_py_ref/src/app.lua",
          _CROSS + "bench_lua_py_ref/clib/points.c")

# `sources` is what a route needs besides the callee (shapes.py) and the timing
# loop (benchloop.lua), which all share: its glue and its caller. Build-system
# lines are not counted for any route.
ROUTES = [
    Route("hand", "hand-written bridge", "bench_lua_py_hand", {},
          (_BASE + "handbridge.c", _BASE + "app_hand.lua")),
    Route("cffi", "cffi", "bench_lua_py_cffi", {},
          (_BASE + "cffi_build.py", _BASE + "cffishim.c", _BASE + "app_cffi.lua")),
    # lupa's launcher must run the shared libpython every other route embeds.
    Route("lupa", "lupa", "bench_lua_py_lupa", {"python": "shared"}, (_BASE + "app_lupa.py",)),
    Route("polylink-value", "PolyLink by value", "cross_bench_lua_py", {},
          (_CROSS + "bench_lua_py/include/shapes.py.h", _CROSS + "bench_lua_py/src/app.lua")),
    Route("polylink-ref", "PolyLink by reference", "cross_bench_lua_py_ref", {"view": "proxy"}, _BYREF),
    Route("polylua-native", "PolyLua native view", "cross_bench_lua_py_native", {"view": "native"}, _BYREF),
]
WORKLOADS = ("describe", "divide")
BASELINE = "hand"
PRIME_CALLS = 1000
COLUMNS = ["route", "label", "workload", "status", "min_us", "median_us", "spread_pct",
           "rel_to_hand", "rounds", "calls", "route_loc", "note"]


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
def spread(samples):
    """How much a route's rounds disagree: the interquartile range over the
    median. One slow round among many does not move it."""
    if len(samples) < 2:
        return 0.0
    q1, _, q3 = statistics.quantiles(samples, n=4, method="inclusive")
    return (q3 - q1) / statistics.median(samples)


def measure(routes, specs, rounds, calls, runner: Callable, *, precision=None, max_rounds=None):
    """Run every route: once to prime it (the linkstub is compiled and cached,
    the route is validated), once at full length with the result thrown away
    (so that no route is timed in the state the priming left the core in),
    then `rounds` times with `calls` calls. Within a round the routes run one
    after the other, and each round starts one route further down the list, so
    that none is always the one that follows another. `specs` maps a ctest name
    to its TestSpec or None; `runner(route, spec, calls)` returns a RunResult.
    A route that fails once is not run again. With a `precision`, rounds are
    added after the requested ones, up to `max_rounds` in all, for as long as
    the `describe` samples of some route have a spread() above it."""
    data = {}
    for route in routes:
        if specs.get(route.ctest) is None:
            data[route.id] = RouteData("missing", f"no ctest {route.ctest} in the build", {}, {})
        else:
            data[route.id] = RouteData("ok", "", {}, {})

    def healthy():
        return [r for r in routes if data[r.id].status == "ok"]

    def run(route, n, keep):
        d = data[route.id]
        result = runner(route, specs[route.ctest], n)
        d.info.update(result.info)
        if not result.ok:
            d.status, d.note = "failed", result.note
        elif keep:
            for workload, us in result.times.items():
                d.samples.setdefault(workload, []).append(us)

    for route in healthy():
        run(route, PRIME_CALLS, keep=False)
    for route in healthy():
        run(route, calls, keep=False)
    def settled():
        return all(spread(data[r.id].samples.get("describe", [])) <= precision for r in healthy())

    r = 0
    while r < rounds or (precision is not None and r < (max_rounds or rounds) and not settled()):
        order = healthy()
        if not order:
            break
        start = r % len(order)
        for route in order[start:] + order[:start]:
            if data[route.id].status == "ok":
                run(route, calls, keep=True)
        r += 1
    return data


def _rel(samples, base):
    """Median over the rounds of this route's time over the baseline's in the
    same round: the two were taken moments apart, so drift of the machine
    during the run cancels. Empty when there is no usable baseline."""
    if not base or len(base) != len(samples) or min(base) <= 0:
        return ""
    return f"{statistics.median(s / b for s, b in zip(samples, base)):.3f}"


def rows(routes, data, calls, loc=None):
    """The table: for a measured route one row per workload it has samples
    for, for any other route one row that says why not. `loc` maps a route id
    to its lines of code (route_loc), when they were counted."""
    base = data.get(BASELINE)
    out = []
    for route in routes:
        d = data[route.id]
        lines = (loc or {}).get(route.id)
        row = {"route": route.id, "label": route.label, "workload": "", "status": d.status,
               "min_us": "", "median_us": "", "spread_pct": "", "rel_to_hand": "", "rounds": "",
               "calls": str(calls), "route_loc": "" if lines is None else str(lines),
               "note": d.note}
        if d.status != "ok":
            out.append(row)
            continue
        for workload in WORKLOADS:
            samples = d.samples.get(workload)
            if not samples:
                continue
            base_samples = base.samples.get(workload) if base is not None and base.status == "ok" else None
            out.append({**row, "workload": workload,
                        "min_us": f"{min(samples):.3f}",
                        "median_us": f"{statistics.median(samples):.3f}",
                        "spread_pct": f"{100 * spread(samples):.1f}",
                        "rel_to_hand": _rel(samples, base_samples), "rounds": str(len(samples))})
    return out


def noisy_rows(table, precision):
    """(route, workload, spread in percent) of the rows whose rounds disagree
    by more than `precision`."""
    return [(row["route"], row["workload"], float(row["spread_pct"])) for row in table
            if row["spread_pct"] and float(row["spread_pct"]) > 100 * precision]


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


# ------------------------------------------------------------ lines of code
def parse_cloc_json(text, root):
    """({path relative to root: lines of code}, cloc version) from the output
    of `cloc --json --by-file`."""
    doc = json.loads(text)
    version = doc.get("header", {}).get("cloc_version")
    counts = {}
    for path, entry in doc.items():
        if path in ("header", "SUM") or "code" not in entry:
            continue
        counts[os.path.relpath(path, root) if os.path.isabs(path) else path] = entry["code"]
    return counts, version


def count_sources(routes, repo=REPO):
    """Lines of code of every route's sources, by cloc: ({path: lines}, cloc
    version), or ({}, None) when cloc cannot be run. A string that holds code
    (the interface cffi is given, the Lua chunk lupa runs) is counted as code,
    which is why the two Python files keep their prose in comments."""
    paths = sorted({os.path.join(repo, p) for r in routes for p in r.sources})
    try:
        out = subprocess.run(["cloc", "--json", "--by-file", "--quiet", "--docstring-as-code", *paths],
                             capture_output=True, text=True, check=True).stdout
        return parse_cloc_json(out, repo)
    except (OSError, subprocess.CalledProcessError, ValueError):
        return {}, None


def route_loc(routes, counts):
    """{route id: lines of code of its sources}; None for a route one of whose
    files was not counted."""
    return {r.id: sum(counts[p] for p in r.sources) if all(p in counts for p in r.sources) else None
            for r in routes}


# ------------------------------------------------------------------- figure
@dataclass
class Bar:
    id: str
    label: str
    cost: object      # median us per call of `describe`, or None when not measured
    loc: object       # lines of code of the route, or None when not counted


PLOT_HEIGHT = 3.6     # cm, the height of the tallest bar of each kind
_PITCH = 1.8          # cm from one route to the next
_BAR = 0.68           # cm, the width of a bar
_GAP = 0.04           # cm from the centre of a route to each of its two bars


def figure_bars(routes, data, loc):
    bars = []
    for route in routes:
        d = data[route.id]
        samples = d.samples.get("describe") if d.status == "ok" else None
        bars.append(Bar(route.id, route.label,
                        statistics.median(samples) if samples else None,
                        (loc or {}).get(route.id)))
    return bars


def bar_height(value, top):
    """Height in cm of a bar of `value` in a plot whose tallest is `top`."""
    return PLOT_HEIGHT * value / top if top > 0 else 0


def axis_ticks(top):
    """Tick values for an axis from 0 to `top`: the coarsest step of the form
    1, 2 or 5 times a power of ten that gives at least two ticks."""
    if top <= 0:
        return []
    exponent = math.floor(math.log10(top))
    for e in (exponent, exponent - 1):
        for m in (5, 2, 1):
            step = m * 10 ** e
            n = int(top / step + 1e-9)
            if n >= 2:
                return [round(step * k, 10) for k in range(1, n + 1)]
    return []


def _cm(x):
    return f"{x:.3f}".rstrip("0").rstrip(".")


def render_tikz(bars, header):
    """The figure as a tikzpicture: per route a bar for the per-call cost
    (left axis) and one for the lines of code (right axis), each kind scaled
    to its own tallest; a route without a measurement gets a dashed outline
    and a question mark. `header` lines become comments at the top."""
    top_cost = max((b.cost for b in bars if b.cost is not None), default=0)
    top_loc = max((b.loc for b in bars if b.loc is not None), default=0)
    base = next((b.cost for b in bars if b.id == BASELINE and b.cost is not None), None)
    base_h = bar_height(base, top_cost) if base is not None else None
    unknown_h = base_h if base_h else PLOT_HEIGHT / 4
    width, top = _PITCH * len(bars), PLOT_HEIGHT + 0.6

    def swatch(colour):
        return r"\fcolorbox{black}{%s}{\rule{0pt}{0.7ex}\rule{0.7ex}{0pt}}" % colour

    out = ["% " + line for line in header]
    out.append(r"\begin{tikzpicture}[x=1cm, y=1cm, >=Latex, font=\footnotesize]")
    out.append(r"  \draw (0,0) -- (%s,0);" % _cm(width))
    out.append(r"  \draw[->] (0,0) -- (0,%s) node[above, anchor=south west, xshift=-6mm, font=\scriptsize] "
               r"{%s\ per-call cost ($\mu$s)};" % (_cm(top), swatch("blue!15")))
    for t in axis_ticks(top_cost):
        y = _cm(bar_height(t, top_cost))
        out.append(r"  \draw (0,%s) -- (-0.08,%s) node[left, font=\scriptsize] {%g};" % (y, y, t))
    if top_loc > 0:
        out.append(r"  \draw[->] (%s,0) -- (%s,%s) node[above, anchor=south east, xshift=6mm, font=\scriptsize] "
                   r"{%s\ lines of code};" % (_cm(width), _cm(width), _cm(top), swatch("orange!25")))
        for t in axis_ticks(top_loc):
            y = _cm(bar_height(t, top_loc))
            out.append(r"  \draw (%s,%s) -- (%s,%s) node[right, font=\scriptsize] {%g};"
                       % (_cm(width), y, _cm(width + 0.08), y, t))
    if base_h:
        out.append(r"  \draw[gray!60, densely dashed] (0,%s) -- (%s,%s);" % (_cm(base_h), _cm(width), _cm(base_h)))
        out.append(r"  \node[right, font=\scriptsize, text=gray!70] at (%s,%s) {baseline};"
                   % (_cm(width + 0.7), _cm(base_h)))
    for i, b in enumerate(bars):
        centre = _PITCH * (i + 0.5)
        x0, x1 = centre - _GAP - _BAR, centre - _GAP
        if b.cost is not None:
            h, style, text = bar_height(b.cost, top_cost), "fill=blue!15", f"{b.cost:.2f}"
        else:
            h, style, text = unknown_h, "gray, densely dashed", "?"
        out.append(r"  \draw[%s] (%s,0) rectangle (%s,%s);" % (style, _cm(x0), _cm(x1), _cm(h)))
        out.append(r"  \node[above, font=\scriptsize] at (%s,%s) {%s};" % (_cm((x0 + x1) / 2), _cm(h), text))
        if b.loc is not None:
            x0, x1, h = centre + _GAP, centre + _GAP + _BAR, bar_height(b.loc, top_loc)
            out.append(r"  \draw[fill=orange!25] (%s,0) rectangle (%s,%s);" % (_cm(x0), _cm(x1), _cm(h)))
            out.append(r"  \node[above, font=\scriptsize] at (%s,%s) {%d};" % (_cm((x0 + x1) / 2), _cm(h), b.loc))
        out.append(r"  \node[below, align=center, font=\scriptsize] at (%s,0) {%s};"
                   % (_cm(centre), b.label.replace(" ", r"\\", 1)))
    out.append(r"\end{tikzpicture}")
    return "\n".join(out) + "\n"


# --------------------------------------------------------------- provenance
def cmake_cache_values(text, names):
    """{name: value or None} for the named entries of a CMakeCache.txt."""
    found = {}
    for line in text.splitlines():
        m = re.match(r"([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if m and m.group(1) in names:
            found[m.group(1)] = m.group(2)
    return {name: found.get(name) for name in names}


# ------------------------------------------------------------------ running
def pick_cpu(allowed, max_freq, siblings):
    """The CPU to pin the runs to: among the `allowed` ones, one of those with
    the highest maximum frequency (the performance cores of a hybrid machine;
    all of them where the frequencies are equal or unknown), and not CPU 0 or
    a hardware thread of its core, which takes most of the interrupts, unless
    that leaves none. `max_freq` maps a CPU to its maximum frequency,
    `siblings` to the set of hardware threads of its core."""
    top = max((max_freq.get(c, 0) for c in allowed), default=0)
    fastest = {c for c in allowed if max_freq.get(c, 0) == top}
    return min(fastest - (siblings.get(0) or {0}) or fastest)


def _read_topology(cpus):
    """(max_freq, siblings) of pick_cpu, from sysfs; empty where it has none."""
    max_freq, siblings = {}, {}
    for c in cpus:
        base = f"/sys/devices/system/cpu/cpu{c}/"
        try:
            with open(base + "cpufreq/cpuinfo_max_freq") as f:
                max_freq[c] = int(f.read())
        except (OSError, ValueError):
            pass
        try:
            with open(base + "topology/thread_siblings_list") as f:
                threads = set()
                for part in f.read().strip().split(","):
                    lo, _, hi = part.partition("-")
                    threads.update(range(int(lo), int(hi or lo) + 1))
                siblings[c] = threads
        except (OSError, ValueError):
            pass
    return max_freq, siblings


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
    heads = ["route", "workload", "status", "min_us", "median_us", "spread_pct", "rel_to_hand", "rounds",
             "route_loc", "note"]
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
    ap.add_argument("--precision", type=float, default=5.0,
                    help="spread between rounds, in percent of the median, above which rounds "
                         "are added and a warning is printed (default 5)")
    ap.add_argument("--max-rounds", type=int, help="most rounds to run in all (default 3 times -r)")
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
    if args.precision <= 0:
        return usage_error("--precision must be positive")
    max_rounds = args.max_rounds if args.max_rounds is not None else 3 * args.rounds
    if max_rounds < args.rounds:
        return usage_error("--max-rounds must be at least -r")
    allowed = os.sched_getaffinity(0)
    cpu_text = os.environ.get("BENCH_CPU")
    if cpu_text is None:
        cpu, cpu_choice = pick_cpu(allowed, *_read_topology(allowed)), "chosen automatically"
    elif cpu_text.isdigit() and int(cpu_text) in allowed:
        cpu, cpu_choice = int(cpu_text), "BENCH_CPU"
    else:
        return usage_error(f"BENCH_CPU={cpu_text} is not a CPU this process may run on "
                           f"({','.join(map(str, sorted(allowed)))})")

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
    print(f"cross_bench: {len(routes)} routes, {args.rounds} to {max_rounds} rounds of {args.calls} calls, "
          f"cpu {cpu} ({cpu_choice}), load {load:.2f}", file=sys.stderr)
    if load > 1.5:
        print(f"cross_bench: warning: the 1-minute load is {load:.2f}; numbers taken on a busy "
              "machine are not comparable", file=sys.stderr)

    def runner(route, spec, calls):
        print(f"  {route.id} ({calls} calls)", file=sys.stderr, flush=True)
        return run_subprocess(route, spec, calls, cpu=cpu, cache_dir=cache_dir, timeout=args.timeout)

    data = measure(routes, specs, args.rounds, args.calls, runner,
                   precision=args.precision / 100, max_rounds=max_rounds)
    counts, cloc_version = count_sources(routes)
    if cloc_version is None:
        print("cross_bench: cloc could not be run; route_loc is left empty", file=sys.stderr)
    loc = route_loc(routes, counts)
    table = rows(routes, data, args.calls, loc)
    write_csv(table, os.path.join(out, "figure10.csv"))
    noisy = noisy_rows(table, args.precision / 100)
    for route_id, workload, percent in noisy:
        print(f"cross_bench: warning: {route_id} {workload} varies by {percent:.1f}% between rounds "
              f"(wanted {args.precision:g}%); the machine was not quiet enough", file=sys.stderr)

    repo = _git(REPO)
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as f:
            build_config = cmake_cache_values(f.read(), [
                "CMAKE_BUILD_TYPE", "PYTHON_LIB", "LINKPY_LUA_INTERP", "LINKPY_POLYLUA_INTERP",
                "LINKPY_LIBALLOCS_PRELOAD", "LINKPY_BENCH_LUPA_PYTHON"])
    except OSError:
        build_config = {}
    fork = build_config.get("LINKPY_POLYLUA_INTERP")
    meta = {
        "date": started,
        "repo": repo,
        "lua_fork": _git(os.path.dirname(fork)) if fork else None,
        "cpu_model": _cpu_model(),
        "pinned_cpu": cpu,
        "pinned_cpu_choice": cpu_choice,
        "precision_percent": args.precision,
        "max_rounds": max_rounds,
        "rows_above_precision": [list(n) for n in noisy],
        "load_1min_at_start": load,
        "kernel": platform.release(),
        "build": build,
        "build_config": build_config,
        "cloc_version": cloc_version,
        "rounds": args.rounds,
        "calls": args.calls,
        "routes": {r.id: {"label": r.label, "ctest": r.ctest,
                          "command": specs[r.ctest].command if specs[r.ctest] else None,
                          "environment": {k: v for k, v in (specs[r.ctest].env if specs[r.ctest] else {}).items()
                                          if k in ("LD_AUDIT", "LD_PRELOAD", "LINKPY_CROSS_BYREF", "PYTHONPATH")},
                          "route_loc": loc[r.id],
                          "sources": {p: counts.get(p) for p in r.sources},
                          **asdict(data[r.id])} for r in routes},
    }
    with open(os.path.join(out, "figure10.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")

    commit = (repo["commit"][:7] + (", modified tree" if repo["dirty"] else "")) if repo else "unknown commit"
    header = [
        f"Generated by tests/linker/bench/cross_bench.py (pycallocs {commit}) on {started}; do not edit.",
        f"{_cpu_model()}, cpu {cpu}, {args.calls} calls per round, load {load:.2f} at start; "
        + ("no route varied by more than" if not noisy else "SOME ROUTES VARIED BY MORE THAN")
        + f" {args.precision:g}% between rounds.",
        "Blue: median microseconds per call of describe(Point). Orange: lines of code that exist",
        f"for the route only, by cloc {cloc_version}. Each kind of bar is scaled to its own tallest.",
        "To regenerate, run the script and copy <build>/bench/fig10/figure10.tex over this file.",
    ]
    with open(os.path.join(out, "figure10.tex"), "w") as f:
        f.write(render_tikz(figure_bars(routes, data, loc), header))

    _print_table(table)
    print(f"written: figure10.csv, figure10.json and figure10.tex in {out}", file=sys.stderr)
    return exit_status(data, allow_missing=args.allow_missing)


if __name__ == "__main__":
    sys.exit(main())
