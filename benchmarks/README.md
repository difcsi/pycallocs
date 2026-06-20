# pycallocs benchmarks

Measures the cost of pycallocs's Python↔C crossing three ways:

1. **Head-to-head** against ctypes, cffi, a hand-written C extension (the
   ceiling), and pure Python (the floor) on identical work.
2. **Internal config matrix** — pycallocs against its own build variants
   (`SPECIALISE_CONVERSION` on/off, by-value vs by-pointer) for regression
   tracking.
3. **Hotspot profiling** — where per-call time goes inside the extension.

There is no manual struct/signature declaration in the pycallocs column: the
types come from the DWARF in `bench.so`. That absence is the project's value
proposition; these numbers are the price.

## Layout

```
benchmarks/
  libs/bench/bench.c        one fixture source, built two ways (allocscc + gcc)
  workloads.py              canonical workload names + expected results
  benchlib.py               self-contained timing core (warmup, rounds, min/median)
  backends/
    bench_pycallocs.py      system under test  (loads bench.so via elflib)
    bench_ctypes.py         ctypes.Structure + restype/argtypes
    bench_cffi.py           ffi.cdef + ffi.dlopen (ABI mode)
    bench_pure.py           pure-Python floor
    bench_native.py + native/benchnative.c   hand-written extension (ceiling)
  run_benchmarks.py         orchestrator: per-backend subprocess + report
  coldstart.py              first-call latency (captures SPECIALISE compile)
  profile_hotspots.sh       perf record/stat wrapper  (+ _profile_driver.py)
```

## Prerequisites

- pycallocs built and importable (`build/` from the top-level CMake) and the
  liballocs runtime present — the same prerequisites the test suite has.
- `cffi` for that one column: `pip install cffi` (absent → it's skipped, not fatal).
- `perf` for `profile_hotspots.sh`; `g++-16` is already needed by SPECIALISE builds.
- No pyperf needed — the timing core is self-contained (see `benchlib.py` for why).

## Build the fixtures

```bash
benchmarks/build.sh                 # standalone, or:
cmake -S . -B build -DBUILD_BENCHMARKS=ON && cmake --build build -t bench-libs
```

## Run

```bash
# 1. validate every backend computes the same answers (do this first)
python benchmarks/run_benchmarks.py --check

# 2. fast smoke run / full run  (writes report.md + report.csv)
python benchmarks/run_benchmarks.py --quick
python benchmarks/run_benchmarks.py --out benchmarks/report.md

# subset of backends / workloads
python benchmarks/run_benchmarks.py --backends pycallocs,ctypes,native --only byval_ret,byptr_arg

# cold start (library load + first-call, incl. the SPECIALISE g++-16 compile)
python benchmarks/run_benchmarks.py --coldstart

# C-level hotspots of one workload
benchmarks/profile_hotspots.sh byval_ret
```

Via CMake: `cmake --build build -t bench-check | bench-quick | bench | bench-coldstart`.

### Internal config matrix

`SPECIALISE_CONVERSION` is a build-time option, so comparing it means two builds.
Build each so its `allocs*.so` lives somewhere distinct, then point one run at both:

```bash
cmake -S . -B build-spec    -DSPECIALISE_CONVERSION=ON  && cmake --build build-spec
cmake -S . -B build-generic -DSPECIALISE_CONVERSION=OFF && cmake --build build-generic
python benchmarks/run_benchmarks.py --backends pycallocs \
    --pycallocs-build spec=build-spec/lib.linux-x86_64-cpython-314 \
    --pycallocs-build generic=build-generic/lib.linux-x86_64-cpython-314
```

This yields `pycallocs[spec]` and `pycallocs[generic]` columns; expect them to
differ most on the `byval_*` rows. (`build.sh`/`bench-libs` fixtures are shared.)

## Methodology — why the numbers are honest

- **Each backend is timed in its own subprocess with its own environment.**
  pycallocs *requires* the liballocs/linkpy runtime (`LD_PRELOAD` of
  `liballocs_preload.so` + the base-types provider, `LD_AUDIT=audit.so`, and the
  static-TLS `GLIBC_TUNABLES`). The baselines run **clean** — putting them under
  the liballocs preload would charge them for malloc interception they never pay
  in real use. Measuring each tool as it is actually used is the fair comparison,
  not a bug. `--ctypes-under-liballocs` adds an attribution-only column that *does*
  run ctypes under the preload, to separate the preload tax from the FFI cost.
- **Warm steady-state and cold start are separate.** The matrix warms up first;
  `coldstart.py` reports library load and first-call latency (where the SPECIALISE
  translator compile lands). Conflating them would flatter or maul pycallocs
  depending on iteration count.
- **Identical work, validated.** `--check` asserts every backend returns the
  workload's expected value before any timing is trusted.
- **Each tool uses its natural fixture build.** `bench.so` (allocscc, with DWARF +
  uniqtypes) for pycallocs; `libbench_plain.so` (`gcc -O2`) for the rest. The
  fixture functions are leaf and side-effect-free, so the binary/instrumentation
  delta is negligible against the boundary cost under study.
- **Min over rounds.** Reported time is the per-call minimum across rounds, which
  rejects scheduler/GC noise (the statistic pyperf also leans on).

## Reading the report

`per-call time (ns)` is the headline; `slowdown vs native (×)` puts each backend
against the hand-written ceiling. `—` = the backend has no idiom for that workload
(e.g. `native` exposes functions, not Python structs, so it has no
`field_*`/`array_index` cell); `n/a` = it errored; a missing column = that backend
was unavailable (reason in the **backends** section).

Workloads are defined in `workloads.py`. The revealing ones:
`byval_ret`/`byval_arg` exercise the SPECIALISE path, `byptr_arg` the lazy proxy,
`bigstruct_ret` how conversion scales with field count, `field_*` the proxy
getset descriptors.
