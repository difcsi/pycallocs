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

What one call from Lua into Python costs, depending on what carries it, and
how much code each way of carrying it takes. Every route is a ctest that runs
the same Lua loop (`lua_py/benchloop.lua`: 1000 checked warm-up calls, then
`LINKPY_BENCH_CALLS` timed ones, in CPU time, `os.clock`) against the same
Python file (`tests/linker/cross/bench_lua_py/src/shapes.py`) on the same
CPython (the build's `PYTHON_LIB`), and prints the `bench <workload>: <µs>
us/call` line of the other fixtures.

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

### How it measures

The script reads each test's command and environment from
`ctest --show-only=json-v1` and runs it itself, pinned to `BENCH_CPU`:

1. every route once with 1000 calls: the linkstub is compiled and cached, and
   a route that cannot run is found before anything is timed;
2. every route once at full length, result thrown away, so that no route is
   timed in the state the priming left the core in (a route that aborts under
   liballocs leaves it idle for ten seconds, and the next one then runs
   faster than it ever does again);
3. the rounds, each starting one route further down the list.

Per route and workload it reports the min and the median over the rounds, and
`rel_to_hand`: the median over the rounds of the route's time divided by the
`hand` route's time *in the same round*. The two were taken moments apart, so
drift of the machine during the run cancels in the ratio; dividing one
route's best round by another's does not have that property.

Lines of code (`route_loc`) are counted with
`cloc --by-file --docstring-as-code` over the files that exist for one route
only, `Route.sources` in the script: its glue and its caller, that is
everything it needs besides the callee and the timing loop, which all routes
share. Build-system lines are counted for no route (each needs some: a C
module to compile, a generator to run, stubs to generate), nor is `pyrun.c`,
which exists only to make the comparison fair. Strings that hold code, the
interface text cffi is given and the Lua chunk lupa runs, count as code. The
two by-reference routes have the same files, hence the same count; theirs
includes `clib/points.c`, the helper a host needs today to obtain a typed
object. `figure10.json` has the count of every file, for a different rule.

### What to keep in mind when reading the numbers

* The baselines run as they would be used, without the auditor and without
  the liballocs preload. The by-reference routes need both.
* `hand` is the best case of a hand-written bridge: it looks the two
  callables up once and holds the GIL from load to exit. PolyLink resolves the
  callable on every call, so that a reload is seen, and parks the GIL between
  calls (`docs/LINKPY_OVERHEAD_2026-11-02.md`, section 2, prices both).
* By value against by reference is not "a copy against no copy" alone. On
  every call the by-reference host wrapper does `require("luaproxy")` and
  `address_of` through two `lua_pcall`s (`linker/lua/luacpputils.cpp`), the
  Python side calls `allocs.proxy_from_address`
  (`linker/cpython/pycpputils.cpp`), and the whole process, CPython's
  allocator included, runs under the liballocs preload, which the by-value
  route does not. A control for the last point (the by-value route and `hand`
  under the same preload) is not part of the script.
* `polylua-native` still goes through the generated host wrapper; only the
  argument is native. Calling the linkstub's symbol straight from
  `allocs.load`, with no wrapper, needs liballocs metadata for the
  JIT-compiled linkstub and is not implemented
  (`docs/CROSS_LANGUAGE_OPEN_QUESTIONS.md`, question 5).
* lupa brings its own Lua; `figure10.json` records which (`info lua=`), and
  the lupa version. Its test is run by `lua_py/pyrun.c`, a launcher linked
  against `PYTHON_LIB`, not by the `python3` binary lupa is installed for: a
  distribution's `python3` may be linked statically, and then runs this
  callee about 1.6 times faster than the shared library the other routes
  embed. The launcher prints `info python=shared` and the script requires it.
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

### Reproducing the report's figure

Needs, besides the LinkPy build: `cloc`, `taskset`, cffi for the build's
`python3`, and lupa (2.8 was used) in a venv of the same `python3`.

```sh
python3 -m venv linker/build/bench/venv && linker/build/bench/venv/bin/pip install lupa
cmake linker/build && cmake --build linker/build -j
python3 tests/linker/bench/test_cross_bench.py    # the script's own tests
tests/linker/bench/cross_bench.py                 # 7 rounds of 200000 calls
cp linker/build/bench/fig10/figure10.tex <report>/link/fig_call_overhead.tex
```

`tests/linker/bench/cross_bench.py -r 2 -n 20000 --allow-missing` is a quick
look. Output, under `<build>/bench/fig10/`:

* `figure10.csv`, columns
  `route,label,workload,status,min_us,median_us,rel_to_hand,rounds,calls,route_loc,note`
  (`label` is the bar's name in the report);
* `figure10.json`: every sample, the `info` lines, each route's command and
  environment, the per-file line counts, the commits of this repository and
  of the Lua fork, the build's settings, the machine and its load;
* `figure10.tex`: the figure itself, a `tikzpicture` the report `\input`s.
  Per route it draws the median of the `describe` samples (left axis) and
  `route_loc` (right axis), each scaled to its own tallest bar, a dashed line
  at the `hand` route's cost, and a dashed outline with a question mark for a
  route that was not measured. Nothing in it is typed by hand.
