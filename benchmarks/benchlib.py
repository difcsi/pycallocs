"""Self-contained micro-benchmark core shared by every backend adapter.

Why not pyperf? pyperf spawns worker processes by re-exec'ing the interpreter
and tunes CPU affinity. Under the pycallocs runtime (LD_AUDIT=linkpy +
liballocs preload + the static-TLS GLIBC_TUNABLES) every extra exec re-runs the
auditor and is a fresh chance to trip the TLS/reentrancy machinery, so a hard
pyperf dependency is both fragile here and an extra install. Instead each
backend is timed once inside its own subprocess (the orchestrator gives it the
right environment), and within that process this module does the timing:
warm up, calibrate an inner repeat count, then take the MIN over several rounds
(min rejects scheduler/GC noise the way pyperf's min does).

A backend adapter defines RUNNERS = {name: make} where make() does any one-time
setup and returns a zero-argument callable -- the unit to time -- then calls
run_backend(). The callable's return value is recorded so --check can validate
every backend computed the same thing.
"""
import argparse
import json
import statistics
import sys
import timeit


def _jsonable(v):
    if isinstance(v, bool) or isinstance(v, int) or isinstance(v, float) or v is None:
        return v
    if isinstance(v, (list, tuple)):
        return [_jsonable(x) for x in v]
    return repr(v)


def time_callable(fn, *, quick):
    """Time zero-arg `fn`. Returns per-call timing stats in nanoseconds."""
    warmup       = 3   if quick else 25
    target_batch = 0.002 if quick else 0.02   # seconds per inner batch
    rounds       = 5   if quick else 11

    # Warm up: triggers the SPECIALISE g++-16 compile + populates every cache,
    # so we measure steady state, not first-call cost (coldstart.py owns that).
    for _ in range(warmup):
        fn()

    timer = timeit.Timer(fn)

    # Calibrate the inner repeat count so one batch ~ target_batch seconds; this
    # amortises Python's per-call dispatch into a number large enough to measure
    # but keeps fast (scalar) and slow (bigstruct) workloads on a similar clock.
    number = 1
    while True:
        dt = timer.timeit(number)
        if dt >= target_batch or number >= 2_000_000:
            break
        number *= 2

    samples = timer.repeat(repeat=rounds, number=number)
    per_call = [s / number for s in samples]
    return {
        "min_ns":    min(per_call) * 1e9,
        "median_ns": statistics.median(per_call) * 1e9,
        "mean_ns":   statistics.fmean(per_call) * 1e9,
        "stdev_ns":  (statistics.pstdev(per_call) * 1e9) if len(per_call) > 1 else 0.0,
        "inner":     number,
        "rounds":    rounds,
    }


def run_backend(backend_name, runners, expected=None):
    """Entry point for a backend adapter's __main__.

    runners: dict {workload_name: make}, make() -> zero-arg callable.
    Emits a JSON blob {backend, python, workloads:{name:{value,...timing}}}.
    """
    ap = argparse.ArgumentParser(description=f"pycallocs benchmark backend: {backend_name}")
    ap.add_argument("--json", help="write the JSON result here (also printed to stdout)")
    ap.add_argument("--quick", action="store_true", help="fewer rounds / shorter batches")
    ap.add_argument("--check", action="store_true", help="only produce values, skip timing")
    ap.add_argument("--only", help="comma-separated subset of workload names")
    args = ap.parse_args()

    only = set(args.only.split(",")) if args.only else None
    out = {
        "backend": backend_name,
        "python": sys.version.split()[0],
        "workloads": {},
    }

    for name, make in runners.items():
        if only and name not in only:
            continue
        rec = {}
        try:
            fn = make()                # one-time setup
            value = fn()               # produced value for --check / sanity
            rec["value"] = _jsonable(value)
            if expected is not None and name in expected and value != expected[name]:
                # Keep going (float tolerance lives in workloads.matches), just flag it.
                rec["expected"] = _jsonable(expected[name])
            if not args.check:
                rec.update(time_callable(fn, quick=args.quick))
        except Exception as e:           # noqa: BLE001 - a missing idiom is data, not a crash
            rec = {"error": f"{type(e).__name__}: {e}"}
        out["workloads"][name] = rec

    text = json.dumps(out, indent=2)
    if args.json:
        with open(args.json, "w") as f:
            f.write(text)
    print(text)
    return out
