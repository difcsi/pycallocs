#!/usr/bin/env python3
"""Cold-start / first-call latency for pycallocs (run under the liballocs env).

Steady-state timing deliberately warms up first, hiding two real costs this
measures instead:
  * library load + DWARF type discovery (the import), and
  * the FIRST call of each kind. On a SPECIALISE_CONVERSION build the first
    by-value struct call pays the one-time g++-16 translator compile; warm calls
    don't, so cold/warm on byval_* is the compile spike laid bare.

Run it with the environment supplied for you:  python run_benchmarks.py --coldstart
(or set LD_PRELOAD/LD_AUDIT yourself and run this directly).

Caveat: all workloads share one process, so a type's translator compiles on
whichever workload touches it first (byval_ret, by dict order). For strict
per-type cold numbers, run one workload at a time with --only via the backend.
"""
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))             # repo root (allocs, elflib)
sys.path.insert(0, _HERE)                              # benchmarks (benchlib, workloads)
sys.path.insert(0, os.path.join(_HERE, "backends"))   # bench_pycallocs


def _ns():
    return time.perf_counter_ns()


def main():
    try:
        import allocs
    except Exception as e:                # noqa: BLE001
        print("cannot import allocs -- run under the liballocs runtime "
              "(use: python run_benchmarks.py --coldstart). reason:", e)
        return 2

    t0 = _ns()
    import bench_pycallocs as B            # triggers lib load + type discovery
    load_ms = (_ns() - t0) / 1e6

    spec = getattr(allocs, "SPECIALISE_CONVERSION", None)
    inject = getattr(allocs, "INJECT_CONVERSION", None)
    print(f"# pycallocs cold start  (SPECIALISE_CONVERSION={spec}, INJECT_CONVERSION={inject})")
    print(f"library load + type discovery: {load_ms:.1f} ms\n")
    print(f"{'workload':<14}{'cold (us)':>12}{'warm (us)':>12}{'cold/warm':>12}")
    print("-" * 50)

    WARM = 2000
    for name, make in B.RUNNERS.items():
        try:
            fn = make()
            t = _ns(); fn(); cold_us = (_ns() - t) / 1e3
            t = _ns()
            for _ in range(WARM):
                fn()
            warm_us = (_ns() - t) / 1e3 / WARM
            ratio = (cold_us / warm_us) if warm_us else float("inf")
            print(f"{name:<14}{cold_us:>12.2f}{warm_us:>12.3f}{ratio:>11.0f}x")
        except Exception as e:            # noqa: BLE001
            print(f"{name:<14}  error: {type(e).__name__}: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
