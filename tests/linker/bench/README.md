# LinkPy benchmark drivers

The fixtures `python_bench_divide` and `python_bench_structs` (under
`tests/linker/cpython/`) print microseconds per call and run as ordinary
tests; the scripts here drive them, and NGINX Unit, the way the numbers in
`docs/LINKPY_OVERHEAD_2026-11-02.md` were taken. All default to the main
build (`linker/build`) and write their files under `<build>/bench/`.

| script | what it measures |
|---|---|
| `cg_bench.sh [-B build] [-H hdr] [label]` | instructions per call (callgrind), immune to machine load; `-H` measures a header variant |
| `wall_bench.sh [-B build] [-r N] [label[=hdr]]...` | microseconds per call, variants interleaved per round, min / median |
| `start_time.sh [-B build] [-c] [label]` | a cache-hit process start: wall time, with `-c` a callgrind profile |
| `unit_wrapper.sh <linkpy tree> <out dir>` | a `linkpy-unitd` for another LinkPy tree (a worktree of an older commit), for A/B |
| `unit_bench.sh [-r N] [name=unitd]...` | Unit end to end, p50 per request, upstream vs any wrappers |

Recipe that gave stable numbers on a shared laptop: pin the timed process to
one performance core (`BENCH_CPU`, default 2; `BENCH_CPUS` 0-3 for Unit),
interleave the variants inside each round, take the min and the median of at
least five rounds, and rank changes by instruction counts when the machine is
busy. To compare against an older commit, build it in a worktree
(`git worktree add ../old <commit>`, configure and build its `linker/build`,
copy the two bench fixtures in if the commit predates them) and point the
scripts at it with `-B` or `unit_wrapper.sh`.

```sh
tests/linker/bench/cg_bench.sh                      # Ir per call, this build
tests/linker/bench/wall_bench.sh -r 6 current       # us per call, six rounds
tests/linker/bench/start_time.sh -c                 # a cache-hit start, profiled
tests/linker/bench/unit_wrapper.sh ../old /tmp/old  # then:
tests/linker/bench/unit_bench.sh upstream=$HOME/Develop/unit-baseline/build/sbin/unitd \
    old=/tmp/old/linkpy-unitd current=$HOME/Develop/unit/build/linkpy-unitd
```
