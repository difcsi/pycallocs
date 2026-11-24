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
| `cross_bench.py [-B build] [-r N] [-n calls] [--routes ...]` | microseconds per Lua → Python call by route (hand-written bridge, cffi, lupa, PolyLink by value / by reference / from the Lua fork); see below |

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

## Lua → Python by route (`cross_bench.py`)

What one call from Lua into Python costs, depending on what carries it. Every
route is a ctest that runs the same Lua loop (`lua_py/benchloop.lua`: 1000
checked warm-up calls, then `LINKPY_BENCH_CALLS` timed ones) against the same
Python file (`tests/linker/cross/bench_lua_py/src/shapes.py`) and prints the
`bench <workload>: <µs> us/call` line of the other fixtures. The script reads
each test's command and environment from `ctest --show-only=json-v1`, runs
every route once to prime it, then the rounds, routes interleaved, pinned to
`BENCH_CPU`.

| route | ctest | what carries the call | how the struct crosses |
|---|---|---|---|
| `hand` | `bench_lua_py_hand` | `lua_py/handbridge.c`, a Lua C module on the CPython API | copied, field by field, into a `SimpleNamespace` |
| `cffi` | `bench_lua_py_cffi` | a library cffi generates in embedding mode (`lua_py/cffi_build.py`), called from the Lua C module `lua_py/cffishim.c` | copied into a C `struct Point`, which Python sees as a cffi object |
| `lupa` | `bench_lua_py_lupa` | lupa, with Python as the host process and Lua as the guest | not copied: Python gets lupa's wrapper of the Lua table |
| `polylink-value` | `cross_bench_lua_py` | the linkstub, with the generated Lua host wrapper | copied through `struct Point` into a `SimpleNamespace` |
| `polylink-ref` | `cross_bench_lua_py_ref` | the same, with `LINKPY_CROSS_BYREF=1` | not copied: a luaproxy userdata gives its address, Python gets a pycallocs proxy of the same object |
| `polylua-native` | `cross_bench_lua_py_native` | the same, run by the `contrib/lua` fork | not copied: the argument is the fork's native foreign value |

Two workloads. `describe(p)` returns `p.x + p.y` of a `struct Point` holding
(3, 4) and is the one a by-route comparison is about, since the routes differ
in how the aggregate crosses. `divide(6, 3)`, two ints in and a float out, is
the fixed cost of a call; the two by-reference routes do not run it.

What to keep in mind when reading the numbers:

* The baselines run as they would be used, without the auditor and without
  the liballocs preload. The by-reference routes need both.
* `hand` is the best case of a hand-written bridge: it looks the two
  callables up once and holds the GIL from load to exit. PolyLink resolves the
  callable on every call, so that a reload is seen, and parks the GIL between
  calls (`docs/LINKPY_OVERHEAD_2026-11-02.md`, section 2, prices both).
* `polylua-native` still goes through the generated host wrapper; only the
  argument is native. Calling the linkstub's symbol straight from
  `allocs.load`, with no wrapper, needs liballocs metadata for the
  JIT-compiled linkstub and is not implemented
  (`docs/CROSS_LANGUAGE_OPEN_QUESTIONS.md`, question 5).
* lupa brings its own Lua; `figure10.json` records which (`info lua=`), and
  the lupa version.
* The by-reference app first has Python write to the object and reads the
  write back in Lua, and both by-reference routes print which kind of value
  they handed over (`info view=proxy` / `view=native`); the script fails a
  route whose view does not match its name.

A route the build does not have is reported as `missing`, one that does not
produce a checked `describe` time as `failed`, each with the reason, and the
others are still measured. Routes go missing when their prerequisite is absent
at configure time: `-DLINKPY_BYREF_TESTS=ON` for the two by-reference routes,
a `contrib/lua` built with `LIBALLOCS_CONFIG=1` (`-DLINKPY_POLYLUA_INTERP`)
for `polylua-native`, cffi for `cffi`, and for `lupa` an interpreter that can
import it (`-DLINKPY_BENCH_LUPA_PYTHON`, default the venv below). Re-run
`cmake` after installing one.

```sh
python3 -m venv linker/build/bench/venv && linker/build/bench/venv/bin/pip install lupa
cmake linker/build && cmake --build linker/build -j
tests/linker/bench/cross_bench.py                 # 7 rounds of 200000 calls
tests/linker/bench/cross_bench.py -r 2 -n 20000 --allow-missing   # a quick look
python3 tests/linker/bench/test_cross_bench.py    # the script's own tests
```

Output, under `<build>/bench/fig10/`: `figure10.csv` with the columns
`route,label,workload,status,min_us,median_us,rel_to_hand,rounds,calls,note`
(`label` is the bar's name in the report; `rel_to_hand` is `min_us` over the
`hand` route's for the same workload), and `figure10.json` with every sample,
the commits, the machine and its load. The report's per-route figure is the
`min_us` of the `describe` rows, its baseline the `hand` row. Where `min_us`
and `median_us` differ by more than a few percent, look at the samples: the
first route of the first round runs on a core that has just been idle, and can
come out faster there than in any later round.
