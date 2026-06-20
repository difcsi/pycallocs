#!/usr/bin/env python3
"""Orchestrate the pycallocs benchmark backends and aggregate a report.

Each backend is timed in its OWN subprocess with its OWN environment:

  * pycallocs runs under the liballocs/linkpy runtime it requires
    (LD_PRELOAD=liballocs_preload.so:libbasetypes_provider.so, LD_AUDIT=audit.so,
    the static-TLS GLIBC_TUNABLES).
  * native / ctypes / cffi / pure run in a CLEAN environment -- forcing them
    under the liballocs preload would tax them with malloc interception they
    never pay in real use, which would be a dishonest comparison.

That asymmetry is deliberate and correct: it measures each tool as it is actually
used. (For attribution rather than comparison, --ctypes-under-liballocs adds a
second ctypes column inside the liballocs env to isolate the preload tax.)

Examples:
  python run_benchmarks.py --check                 # validate identical results
  python run_benchmarks.py --quick                 # fast smoke run
  python run_benchmarks.py --out report.md         # full run -> report.md + .csv
  python run_benchmarks.py --backends pycallocs \\
      --pycallocs-build spec=build-spec/lib... \\
      --pycallocs-build generic=build-generic/lib...   # internal config matrix
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))     # benchmarks/
ROOT = os.path.dirname(HERE)                           # repo root
BACKENDS_DIR = os.path.join(HERE, "backends")

sys.path.insert(0, HERE)
import workloads   # noqa: E402

DEFAULT_BUILD_DIR = os.path.join(ROOT, "build")

BACKEND_SCRIPTS = {
    "native":    os.path.join(BACKENDS_DIR, "bench_native.py"),
    "ctypes":    os.path.join(BACKENDS_DIR, "bench_ctypes.py"),
    "cffi":      os.path.join(BACKENDS_DIR, "bench_cffi.py"),
    "pure":      os.path.join(BACKENDS_DIR, "bench_pure.py"),
    "pycallocs": os.path.join(BACKENDS_DIR, "bench_pycallocs.py"),
}
COLUMN_ORDER = ["native", "ctypes", "cffi", "pycallocs", "pure"]


# --------------------------------------------------------------------------- env
def discover_paths(build_dir):
    la = os.path.join(ROOT, "contrib", "stackscan", "contrib", "liballocs")
    return {
        "liballocs_lib": os.path.join(la, "lib"),
        "preload": os.path.join(la, "lib", "liballocs_preload.so"),
        "basetypes": os.path.join(build_dir, "basetypes", "libbasetypes_provider.so"),
        "audit": os.path.join(ROOT, "contrib", "linkpy", "dist", "audit.so"),
    }


def _join_pp(parts, env):
    if env.get("PYTHONPATH"):
        parts = parts + [env["PYTHONPATH"]]
    return os.pathsep.join(p for p in parts if p)


def pycallocs_env(paths, extra_pythonpath=None):
    e = dict(os.environ)
    e["LD_PRELOAD"] = paths["preload"] + ":" + paths["basetypes"]
    e["LD_AUDIT"] = paths["audit"]
    ldlp = [paths["liballocs_lib"]]
    if e.get("LD_LIBRARY_PATH"):
        ldlp.append(e["LD_LIBRARY_PATH"])
    e["LD_LIBRARY_PATH"] = ":".join(ldlp)
    e["GLIBC_TUNABLES"] = "glibc.rtld.optional_static_tls=524288"
    e["PYTHONPATH"] = _join_pp(([extra_pythonpath] if extra_pythonpath else []) + [ROOT, HERE], e)
    return e


def clean_env():
    e = dict(os.environ)
    for k in ("LD_PRELOAD", "LD_AUDIT"):
        e.pop(k, None)
    e["PYTHONPATH"] = _join_pp([HERE], e)
    return e


def missing_pycallocs_artifacts(paths):
    return [p for p in (paths["preload"], paths["basetypes"], paths["audit"])
            if not os.path.exists(p)]


# ----------------------------------------------------------------------- running
def run_one(label, script, env, args, json_dir):
    out_json = os.path.join(json_dir, label.replace("/", "_") + ".json")
    cmd = [sys.executable, script, "--json", out_json]
    if args.quick:
        cmd.append("--quick")
    if args.check:
        cmd.append("--check")
    if args.only:
        cmd += ["--only", args.only]
    try:
        proc = subprocess.run(cmd, env=env, cwd=HERE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=args.timeout, text=True)
    except subprocess.TimeoutExpired:
        return {"backend": label, "error": f"timeout after {args.timeout}s", "workloads": {}}
    if proc.returncode != 0 or not os.path.exists(out_json):
        tail = " / ".join((proc.stderr or "").strip().splitlines()[-3:]) or "no output"
        return {"backend": label, "error": f"exit {proc.returncode}: {tail}", "workloads": {}}
    with open(out_json) as f:
        return json.load(f)


def collect(args):
    paths = discover_paths(args.build_dir)
    selected = args.backends.split(",") if args.backends else list(BACKEND_SCRIPTS)
    json_dir = args.json_dir or tempfile.mkdtemp(prefix="pycallocs-bench-")
    os.makedirs(json_dir, exist_ok=True)
    results = {}

    for name in selected:
        if name == "pycallocs":
            missing = missing_pycallocs_artifacts(paths)
            if missing:
                results["pycallocs"] = {"backend": "pycallocs", "workloads": {},
                                        "error": "missing runtime artifacts: " + ", ".join(missing)}
                continue
            builds = args.pycallocs_build or []
            if builds:
                for spec in builds:
                    lab, _, path = spec.partition("=")
                    label = f"pycallocs[{lab}]"
                    env = pycallocs_env(paths, os.path.abspath(path) if path else None)
                    results[label] = run_one(label, BACKEND_SCRIPTS["pycallocs"], env, args, json_dir)
            else:
                results["pycallocs"] = run_one("pycallocs", BACKEND_SCRIPTS["pycallocs"],
                                              pycallocs_env(paths), args, json_dir)
        else:
            results[name] = run_one(name, BACKEND_SCRIPTS[name], clean_env(), args, json_dir)

    if args.ctypes_under_liballocs and "ctypes" in selected:
        results["ctypes@liballocs"] = run_one(
            "ctypes@liballocs", BACKEND_SCRIPTS["ctypes"], pycallocs_env(paths), args, json_dir)

    return results, json_dir


# --------------------------------------------------------------------- reporting
def order_columns(results):
    cols = []
    for base in COLUMN_ORDER:
        for label in results:
            if (label == base or label.startswith(base + "[") or label.startswith(base + "@")) \
                    and label not in cols:
                cols.append(label)
    for label in results:           # stragglers
        if label not in cols:
            cols.append(label)
    return cols


def _rec(results, col, wl):
    return results.get(col, {}).get("workloads", {}).get(wl)


def _row_names(args):
    names = workloads.ORDER
    if args.only:
        keep = set(args.only.split(","))
        names = [n for n in names if n in keep]
    return names


def report_check(results, args):
    cols = order_columns(results)
    lines = ["# pycallocs benchmark -- result validation", "",
             "Every backend must compute the workload's expected value.", ""]
    header = ["workload", "expected"] + cols
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "|".join(["---"] * len(header)) + "|")
    ok = True
    for wl in _row_names(args):
        exp = workloads.EXPECTED[wl]
        cells = [wl, repr(exp)]
        for col in cols:
            rec = _rec(results, col, wl)
            if rec is None:
                cells.append("—")
            elif "error" in rec:
                cells.append("ERR")
            elif workloads.matches(wl, rec.get("value")):
                cells.append(f"✓ {rec.get('value')}")
            else:
                ok = False
                cells.append(f"✗ {rec.get('value')}")
        lines.append("| " + " | ".join(str(c) for c in cells) + " |")
    lines += ["", _availability(results)]
    return "\n".join(lines), ok


def _availability(results):
    out = ["## backends", ""]
    for label, data in results.items():
        if data.get("error"):
            out.append(f"- **{label}**: unavailable — {data['error']}")
        else:
            n = sum(1 for r in data.get("workloads", {}).values() if "error" not in r)
            out.append(f"- **{label}**: ok ({n} workloads, python {data.get('python', '?')})")
    return "\n".join(out)


def _fmt(v):
    if v >= 1000:
        return f"{v:,.0f}"
    if v >= 100:
        return f"{v:.0f}"
    return f"{v:.1f}"


def report_timings(results, args):
    cols = order_columns(results)
    rows = _row_names(args)
    base = "native" if "native" in cols else None

    def table(title, cellfn):
        out = [f"## {title}", "",
               "| workload | " + " | ".join(cols) + " |",
               "|" + "|".join(["---"] * (len(cols) + 1)) + "|"]
        for wl in rows:
            cells = [wl]
            for col in cols:
                cells.append(cellfn(wl, col))
            out.append("| " + " | ".join(cells) + " |")
        return "\n".join(out)

    def ns_cell(wl, col):
        rec = _rec(results, col, wl)
        if rec is None:
            return "—"
        if "error" in rec:
            return "n/a"
        return _fmt(rec["min_ns"])

    def ratio_cell(wl, col):
        rec = _rec(results, col, wl)
        brec = _rec(results, base, wl) if base else None
        if rec is None:
            return "—"
        if "error" in rec or not brec or "error" in brec or brec["min_ns"] <= 0:
            return "n/a"
        return f"{rec['min_ns'] / brec['min_ns']:.1f}×"

    parts = ["# pycallocs benchmark", "",
             f"Per-call wall time, **minimum over rounds**, in nanoseconds "
             f"({'quick' if args.quick else 'full'} run). "
             f"Each backend timed in its own process; pycallocs under the "
             f"liballocs/linkpy runtime, the rest clean. Lower is better.", "",
             "Workloads:", ""]
    for wl in rows:
        parts.append(f"- `{wl}` — {workloads.DESCRIPTIONS[wl]}")
    parts += ["", table("per-call time (ns)", ns_cell)]
    if base:
        parts += ["", table("slowdown vs native (×)", ratio_cell)]
    parts += ["", _availability(results)]
    return "\n".join(parts)


def write_csv(results, path, args):
    cols = order_columns(results)
    lines = ["workload,metric," + ",".join(cols)]
    for wl in _row_names(args):
        for metric in ("min_ns", "median_ns", "stdev_ns"):
            row = [wl, metric]
            for col in cols:
                rec = _rec(results, col, wl)
                row.append("" if not rec or "error" in rec else f"{rec.get(metric, ''):.3f}"
                           if isinstance(rec.get(metric), float) else "")
            lines.append(",".join(row))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


# -------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backends", help="comma list (default: all): " + ",".join(BACKEND_SCRIPTS))
    ap.add_argument("--only", help="comma list of workloads to run")
    ap.add_argument("--quick", action="store_true", help="fast smoke run")
    ap.add_argument("--check", action="store_true", help="validate identical results, no timing")
    ap.add_argument("--out", default=os.path.join(HERE, "report.md"),
                    help="markdown report path (CSV written alongside)")
    ap.add_argument("--build-dir", default=DEFAULT_BUILD_DIR,
                    help="CMake build dir holding basetypes/libbasetypes_provider.so")
    ap.add_argument("--pycallocs-build", action="append", metavar="LABEL=PATH",
                    help="run pycallocs against a build whose allocs*.so is at PATH (repeatable)")
    ap.add_argument("--ctypes-under-liballocs", action="store_true",
                    help="extra ctypes column inside the liballocs env (preload-tax attribution)")
    ap.add_argument("--coldstart", action="store_true",
                    help="run coldstart.py under the liballocs env instead of the matrix")
    ap.add_argument("--json-dir", help="keep per-backend JSON here (default: a temp dir)")
    ap.add_argument("--timeout", type=int, default=900, help="per-backend timeout (s)")
    args = ap.parse_args()

    if args.coldstart:
        paths = discover_paths(args.build_dir)
        missing = missing_pycallocs_artifacts(paths)
        if missing:
            print("cannot run coldstart -- missing runtime artifacts: " + ", ".join(missing))
            sys.exit(2)
        sys.exit(subprocess.call([sys.executable, os.path.join(HERE, "coldstart.py")],
                                env=pycallocs_env(paths), cwd=HERE))

    results, json_dir = collect(args)

    if args.check:
        text, ok = report_check(results, args)
        print(text)
        print(f"\n(JSON in {json_dir})")
        sys.exit(0 if ok else 1)

    text = report_timings(results, args)
    print(text)
    with open(args.out, "w") as f:
        f.write(text + "\n")
    csv_path = os.path.splitext(args.out)[0] + ".csv"
    write_csv(results, csv_path, args)
    print(f"\nWrote {args.out} and {csv_path}  (JSON in {json_dir})")


if __name__ == "__main__":
    main()
