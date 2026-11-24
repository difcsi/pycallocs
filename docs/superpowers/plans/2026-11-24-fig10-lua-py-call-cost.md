# Figure 10 (Lua → Python per-call cost, by route): implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One command, `tests/linker/bench/cross_bench.py`, that measures what one Lua → Python call costs through each route of the report's Figure 10 and writes the numbers as a CSV the figure can be drawn from.

**Architecture:** Every route is an ordinary ctest that runs the same Lua timing loop against the same Python callee and prints `bench <workload>: <µs> us/call (<N> calls)`. The three PolyLink routes are cross fixtures registered through `add_linkpy_cross_test`; the three baselines (hand-written bridge, cffi embedding, lupa) are small programs under `tests/linker/bench/lua_py/`. The driver reads each test's command and environment from `ctest --show-only=json-v1`, runs the routes interleaved and pinned to one core, and aggregates min / median per route.

**Tech Stack:** Lua 5.5 (system `lua5.5` and the `contrib/lua` fork), CPython 3.14 C API, cffi 2.0 embedding mode, lupa, CMake/ctest, Python 3 standard library only for the driver (`unittest` for its tests).

**Spec:** `~/Desktop/Research/Papers/link/report.tex`, §"The Cost of a Call" (`sec:overhead`, lines 1105-1134), figure `fig:call_overhead` (numbered 10 in `link/report.aux`). The figure has five bars, each with a `?` where a number goes: hand-written bridge (the dashed baseline), cffi / lupa, PolyLink by value, PolyLink by reference, PolyLua native view. Its caption names the function as the placeholder `fn_name_here`. Timing recipe: `tests/linker/bench/README.md`.

## Decisions this plan makes

- **The function is `describe`**: `describe(p)` returns `p.x + p.y` for a `struct Point { int x; int y; }` holding (3, 4), expected result 7. Bars 3 and 4 differ only in how an aggregate crosses, so the function must take one. `divide(6, 3)` (two ints in, a float out, expected 2.0) is measured as a second workload on the routes where it applies: it is the fixed cost of a call, and directly comparable with the existing `python_bench_divide` (C → Python).
- **"cffi / lupa" is measured as two routes**, `cffi` and `lupa`; the figure can show either or both.
- **"PolyLua native view" means**: the `contrib/lua` interpreter is the host, the argument is a native `LUA_TFOREIGN` value instead of a luaproxy userdata, and the call goes through the same generated host wrapper as the by-reference route. Calling the linkstub's symbol directly through `allocs.load` (no generated wrapper) is not implemented: it needs a liballocs meta-DSO for the JIT-compiled linkstub (`docs/CROSS_LANGUAGE_OPEN_QUESTIONS.md`, question 5) and is out of scope.
- **The hand-written bridge is the best case a practitioner would write**: it keeps the two callables and holds the GIL for the life of the process. PolyLink resolves the callable on every call and parks the GIL between calls; the README says so next to the numbers.

## Known state of the tree (observed 2026-11-24, branch `dev`, `linker/build`)

- `cross_lua_calls_py_divide` and `cross_lua_calls_py_point` pass: the by-value route works.
- `cross_lua_calls_py_bump`, `cross_py_calls_lua_bump` and `python_ptr_arg` abort with `Assertion failed at .../generic_malloc_index.h:937: INSERT_DESCRIBES_OBJECT(p_ins)`; `lua_ptr_arg` passes. Every route that runs CPython under the liballocs preload is therefore blocked today, which covers `polylink-ref` and `polylua-native`. The cause was not investigated. **Fixing liballocs is not part of this plan**; the driver must report those two routes as `failed` with that assertion line and still deliver the other four.
- `lupa` is not installed; `cffi` 2.0.0 is. `LINKPY_BYREF_TESTS=ON`, `LINKPY_LUA_INTERP=/usr/bin/lua5.5`, `CMAKE_BUILD_TYPE=RelWithDebInfo`.
- `contrib/lua/lua` is the fork built with `LIBALLOCS_CONFIG=1` (5.5.1).

## Global Constraints

- Route ids, figure labels and ctest names are exactly:

  | route id | figure bar | ctest |
  |---|---|---|
  | `hand` | hand-written bridge | `bench_lua_py_hand` |
  | `cffi` | cffi | `bench_lua_py_cffi` |
  | `lupa` | lupa | `bench_lua_py_lupa` |
  | `polylink-value` | PolyLink by value | `cross_bench_lua_py` |
  | `polylink-ref` | PolyLink by reference | `cross_bench_lua_py_ref` |
  | `polylua-native` | PolyLua native view | `cross_bench_lua_py_native` |

- Output line format, shared with the existing fixtures: `bench <workload>: <µs with 3 decimals> us/call (<N> calls)`. `LINKPY_BENCH_CALLS` sets N (default 200000); 1000 untimed warm-up calls come first.
- Every route calls the one file `tests/linker/cross/bench_lua_py/src/shapes.py`, runs the one loop `tests/linker/bench/lua_py/benchloop.lua`, and embeds the CPython that `linker/build` is configured with (`PYTHON_LIB`). Hand-written C is compiled `-O2 -g`, as `LINKSTUB_BUILD_CMD` is.
- Baselines run without `LD_AUDIT` and without `LD_PRELOAD` (each tool as it is actually used, the rule `benchmarks/run_benchmarks.py` states).
- Timing recipe from `tests/linker/bench/README.md`: pin to `BENCH_CPU` (default 2), interleave the routes inside each round, report min and median over the rounds (default 7, at least 5 for numbers that go in the report).
- A route that cannot run is reported, never dropped: status `missing` (no such ctest in the build) or `failed` (ran and did not produce a checked `describe` line), with the reason in the row.
- Existing tests keep their names, commands and results; the new arguments to `add_linkpy_cross_test` are optional and change nothing when absent.
- The driver writes only under `<build>/bench/fig10/` and uses its own linkstub cache there.
- One commit per task on a branch `fig10-call-cost` taken from `dev`; never push; leave the working tree's other uncommitted changes alone.

## Review Focus

- **A route that is not in the build** (lupa not installed, `LINKPY_BYREF_TESTS=OFF`, the fork not built): a `missing` row naming the ctest, the other routes measured, exit status 1 unless `--allow-missing`. Pinned by `test_missing_route_is_reported` (Task 5).
- **A route that aborts** (today: the liballocs assertion on both by-reference routes): a `failed` row whose note is the `Assertion failed` line, no further runs of that route, CSV still written. Pinned by `test_failed_route_is_not_rerun` and `test_note_prefers_assertion_line` (Task 5).
- **A route that is fast because it does less**: the loop checks every warm-up result against the expected value, and the by-reference app proves the object is shared (a write made in Python is read back in Lua) before timing. Pinned by the `bump` check in Task 2 and `expect` in `benchloop.lua` (Task 1).
- **The wrong interpreter under a label** (stock Lua run as `polylua-native`, or the fork as `polylink-ref`): each by-reference run prints `info view=proxy|native` and the driver fails the route when it does not match. Pinned by `test_info_mismatch_fails_route` (Task 5).
- **liballocs noise around the result** (`MMAP:` lines, the summary block, on stdout or stderr): the parser reads only `bench` and `info` lines. Pinned by `test_parse_ignores_noise` (Task 5).

---

### Task 1: Shared loop, shared callee, and the by-value route

**Files:**
- Create: `tests/linker/bench/lua_py/benchloop.lua`
- Create: `tests/linker/cross/bench_lua_py/include/shapes.py.h`, `.../bench_lua_py/src/shapes.py`, `.../bench_lua_py/src/app.lua`
- Modify: `tests/linker/cross/CMakeLists.txt` (register after `lua_calls_py_point`)

**Interfaces:**
- Produces `benchloop.lua`, a module returning a table with
  - `run1(name, expect, f, a)` and `run2(name, expect, f, a, b)`: 1000 warm-up calls of `f(a)` / `f(a, b)`, each result checked against `expect` (`math.abs(r - expect) < 1e-6` when `expect` is a float, `==` otherwise, `error()` naming the workload on a mismatch); then N timed calls in a plain `for` loop between two `os.clock()` readings; prints the `bench` line.
  - `info(key, value)`: prints `info <key>=<value>`.
- Produces `shapes.py` (used by every later task) and the ctest `cross_bench_lua_py`, which prints `bench divide: …` and `bench describe: …`.
- App files find the loop with `package.path = arg[0]:match("^(.*/)") .. "<relative dir>/?.lua;" .. package.path`; from `tests/linker/cross/<fixture>/src/` the relative dir is `../../../bench/lua_py`.

- [ ] **Step 1: Write the callee and the header**

```python
# tests/linker/cross/bench_lua_py/src/shapes.py
def divide(a: int, b: int) -> float:
    return a / b

def describe(p) -> int:
    return p.x + p.y

def bump(p) -> None:      # by-reference routes only: proves the object is shared
    p.x = p.x + 100
```

```c
/* tests/linker/cross/bench_lua_py/include/shapes.py.h */
struct Point { int x; int y; };
extern float divide(int a, int b);
extern int describe(struct Point p);
```

- [ ] **Step 2: Write `benchloop.lua`** to the interface above.
- [ ] **Step 3: Write `app.lua`**: `linkpy.load("shapes")`, then `run2("divide", 2.0, m.divide, 6, 3)` and `run1("describe", 7, m.describe, {x = 3, y = 4})`, `os.exit(0)`.
- [ ] **Step 4: Register** `add_linkpy_cross_test(bench_lua_py HOST lua CALLEE python)` with a comment that it is a benchmark that passes on exit code.
- [ ] **Step 5: Build and run**

Run: `cmake linker/build && cmake --build linker/build -j12 && LINKPY_BENCH_CALLS=20000 ctest --test-dir linker/build -R '^cross_bench_lua_py$' -V`
Expected: `1/1 Test … Passed`, with one line `bench divide: <x> us/call (20000 calls)` and one `bench describe: <y> us/call (20000 calls)`; `<x>` within an order of magnitude of `python_bench_divide`'s number.

- [ ] **Step 6: Check the result check bites.** Temporarily change `7` to `8` in `app.lua`, rerun: the test must fail with the error naming `describe`. Restore.
- [ ] **Step 7: Commit**: `git add tests/linker/bench/lua_py/benchloop.lua tests/linker/cross/bench_lua_py tests/linker/cross/CMakeLists.txt && git commit -m "test(linker): Lua-host benchmark fixture for the by-value cross route"`

### Task 2: The by-reference route and its PolyLua variant

**Files:**
- Create: `tests/linker/cross/bench_lua_py_ref/include/shapes.py.h`, `.../src/app.lua`, `.../clib/points.c` (same content as `lua_calls_py_bump/clib/points.c`), `.../src/shapes.py` (a symlink to `../../bench_lua_py/src/shapes.py`)
- Modify: `tests/linker/tests.cmake` (`add_linkpy_cross_test`)
- Modify: `tests/linker/cross/CMakeLists.txt`

**Interfaces:**
- Consumes: `benchloop.lua`, `shapes.py` (Task 1).
- Produces, on `add_linkpy_cross_test`, three optional one-value arguments: `FIXTURE <dir>` (source directory under `CMAKE_CURRENT_SOURCE_DIR`, default `<name>`; the work directory and target names still derive from `<name>`), `LUA_INTERP <path>` (default `LINKPY_LUA_INTERP`), `LUA_HOST_CFLAGS <flags>` (default `LUA_CFLAGS_STR`; used for `host_flags` when `HOST lua`).
- Produces a cache variable `LINKPY_POLYLUA_INTERP` (FILEPATH, default `${PYCALLOCS_ROOT}/contrib/lua/lua`).
- Produces ctests `cross_bench_lua_py_ref` and `cross_bench_lua_py_native`; each prints `info view=proxy` or `info view=native`, then `bench describe: …`.

- [ ] **Step 1: Write the header**

```c
/* tests/linker/cross/bench_lua_py_ref/include/shapes.py.h */
struct Point { int x; int y; };
extern int describe(struct Point *p);
extern void bump(struct Point *p);
```

- [ ] **Step 2: Write `app.lua`.** In this order: `local ok, native = pcall(require, "allocs")`; when `ok`, set `package.loaded.luaproxy = native` (the fork's own foreign values then serve where the generated wrapper asks luaproxy for an address); `local allocs = require("luaproxy")`; `info("view", ok and "native" or "proxy")`; `p = allocs.load("./points.so").new_point(3, 4)`; load `shapes` through `linkpy`; call `bump(p)` and assert `p.x == 103`, then set `p.x = 3`; `run1("describe", 7, shapes.describe, p)`.
- [ ] **Step 3: Extend `add_linkpy_cross_test`** with the three arguments. Before editing, save `ctest --test-dir linker/build --show-only=json-v1` to a scratch file for Step 5. Nothing else in the function changes; with none of them given the generated test is identical to today's.
- [ ] **Step 4: Register**, inside the existing `if(LINKPY_BYREF_TESTS)` block:

```cmake
add_linkpy_cross_test(bench_lua_py_ref HOST lua CALLEE python BYREF)
if(EXISTS "${LINKPY_POLYLUA_INTERP}")
    add_linkpy_cross_test(bench_lua_py_native HOST lua CALLEE python BYREF
        FIXTURE bench_lua_py_ref
        LUA_INTERP "${LINKPY_POLYLUA_INTERP}"
        LUA_HOST_CFLAGS "-I${PYCALLOCS_ROOT}/contrib/lua")
else()
    message(STATUS "linkpy: cross_bench_lua_py_native skipped (no ${LINKPY_POLYLUA_INTERP})")
endif()
```

- [ ] **Step 5: Verify nothing else moved.** Run: `cmake linker/build && cmake --build linker/build -j12 && ctest --test-dir linker/build -R '^cross_' -N`. Expected: the seven existing `cross_*` tests plus `cross_bench_lua_py`, `cross_bench_lua_py_ref`, `cross_bench_lua_py_native`. Then the `cross_lua_calls_py_bump` entry of `ctest --test-dir linker/build --show-only=json-v1` must have the same command and `ENVIRONMENT` as in the file saved in Step 3.
- [ ] **Step 6: Run both.** Run: `LINKPY_BENCH_CALLS=20000 ctest --test-dir linker/build -R '^cross_bench_lua_py_(ref|native)$' -V`.
Expected, when the liballocs runtime works: both pass, `info view=proxy` and `info view=native` respectively, one `bench describe` line each.
Expected today: both abort with `generic_malloc_index.h:937: INSERT_DESCRIBES_OBJECT`. In that case run `ctest --test-dir linker/build -R '^cross_lua_calls_py_bump$' --output-on-failure` and confirm it dies on the same line; if it does, the fixture is as verified as the tree allows: record that in the commit message and go on. If `cross_lua_calls_py_bump` passes while the new tests fail, the fixture is wrong: fix it before committing.
- [ ] **Step 7: Commit**: `git add tests/linker/cross/bench_lua_py_ref tests/linker/cross/CMakeLists.txt tests/linker/tests.cmake && git commit -m "test(linker): by-reference and PolyLua-host benchmark fixtures; FIXTURE/LUA_INTERP/LUA_HOST_CFLAGS on add_linkpy_cross_test"`

### Task 3: Hand-written bridge (the baseline)

**Files:**
- Create: `tests/linker/bench/lua_py/handbridge.c`, `tests/linker/bench/lua_py/app_hand.lua`, `tests/linker/bench/lua_py/CMakeLists.txt`
- Modify: `linker/CMakeLists.txt` (inside the `if(LUA_FOUND)` block that adds `tests/cross`: `add_subdirectory("${LINKPY_TESTS_DIR}/bench/lua_py" tests/bench_lua_py)`)

**Interfaces:**
- Consumes: `benchloop.lua`, `shapes.py`.
- Produces `handbridge.so` (CMake target `bench_handbridge`, `MODULE`, no `lib` prefix, compiled `-O2 -g`, linked against `${PYTHON_LIB} ${PY_SYSLIBS}` and not against liblua), exporting `int luaopen_handbridge(lua_State *L)`, which returns a table with `divide(a, b)` and `describe(t)`.
- Produces ctest `bench_lua_py_hand`: command `${LINKPY_LUA_INTERP} <src>/app_hand.lua`, `ENVIRONMENT "PYTHONPATH=${LINKPY_TESTS_DIR}/cross/bench_lua_py/src;BENCH_MODULE_DIR=$<TARGET_FILE_DIR:bench_handbridge>"`. Task 4 reuses `BENCH_MODULE_DIR` and `PYTHONPATH` the same way.

- [ ] **Step 1: Write `handbridge.c`.** `luaopen_handbridge`: `Py_InitializeEx(0)`, import `shapes`, keep new references to `shapes.divide`, `shapes.describe`, `types.SimpleNamespace`, and the interned names `x` and `y`; the GIL stays held. `divide`: two `luaL_checkinteger` → `PyLong` → `PyObject_Vectorcall` → `PyFloat_AsDouble` → `lua_pushnumber`. `describe`: read fields `x` and `y` of the table at index 1, build a `SimpleNamespace` and set both attributes with the interned names, vectorcall, `PyLong_AsLong` → `lua_pushinteger`. A Python exception becomes `luaL_error` with the exception's text; every reference taken in a call is released before it returns, on the error path too.
- [ ] **Step 2: Write `app_hand.lua`.** First `assert(package.loadlib(os.getenv("BENCH_MODULE_DIR") .. "/handbridge.so", "*"))` (RTLD_GLOBAL, so that extension modules the embedded Python imports resolve their `Py*` symbols; `linkpy.lua` does the same), then `package.cpath` gets that directory, `require("handbridge")`, the same two `run` calls as Task 1 Step 3.
- [ ] **Step 3: Write the CMake** and add the subdirectory.
- [ ] **Step 4: Build and run.** Run: `cmake linker/build && cmake --build linker/build -j12 && LINKPY_BENCH_CALLS=20000 ctest --test-dir linker/build -R '^bench_lua_py_hand$' -V`. Expected: passes, both `bench` lines.
- [ ] **Step 5: Check the error path.** Run the test's command by hand with `PYTHONPATH` pointing at an empty directory: it must exit non-zero with a Lua error that contains `No module named 'shapes'`, not crash.
- [ ] **Step 6: Commit**: `git add tests/linker/bench/lua_py linker/CMakeLists.txt && git commit -m "test(linker): hand-written Lua-to-Python bridge as the benchmark baseline"`

### Task 4: cffi embedding and lupa

**Files:**
- Create: `tests/linker/bench/lua_py/cffi_build.py`, `tests/linker/bench/lua_py/cffishim.c`, `tests/linker/bench/lua_py/app_cffi.lua`, `tests/linker/bench/lua_py/app_lupa.py`
- Modify: `tests/linker/bench/lua_py/CMakeLists.txt`

**Interfaces:**
- Consumes: `benchloop.lua`, `shapes.py`, the `BENCH_MODULE_DIR` / `PYTHONPATH` convention (Task 3).
- Produces `cffi_build.py --out <dir>`: writes `<dir>/libshapes_cffi.so`, a cffi embedding library whose `embedding_api` is `struct Point { int x; int y; }; float divide(int, int); int describe(struct Point);` and whose init code imports `shapes` and forwards both `def_extern` functions to it unchanged (the `struct Point` cdata has `.x` and `.y`, so `shapes.describe` takes it as it is).
- Produces `cffishim.so` (target `bench_cffishim`, built like `bench_handbridge`, linked against `libshapes_cffi.so` with an rpath to its directory): `luaopen_cffishim` returns a table with `divide(a, b)` and `describe(t)`; `describe` fills a `struct Point` from the table's `x` and `y` and calls the C function. This file contains no Python API call.
- Produces ctests `bench_lua_py_cffi` (registered only when `${PYTHON} -c "import cffi"` succeeds at configure time) and `bench_lua_py_lupa` (registered only when `${LINKPY_BENCH_LUPA_PYTHON} -c "import lupa"` succeeds; cache variable `LINKPY_BENCH_LUPA_PYTHON`, FILEPATH, default `${CMAKE_BINARY_DIR}/bench/venv/bin/python`). Each skipped registration prints a `message(STATUS …)` that says what to install.
- `app_lupa.py`: creates a `lupa.LuaRuntime`, puts `benchloop.lua`'s directory on the Lua `package.path`, prints `info lupa=<lupa.__version__>` and `info lua=<the runtime's Lua version>`, and runs a Lua chunk that receives `shapes.divide` and `shapes.describe` and makes the same two `run` calls, so the timed loop is Lua calling Python. `describe` receives lupa's wrapper of the Lua table `{x = 3, y = 4}`.

- [ ] **Step 1: cffi.** Write `cffi_build.py`, `cffishim.c`, `app_cffi.lua` (as `app_hand.lua`, loading `cffishim.so` with `"*"`); add the custom command that runs `cffi_build.py`, the module target and the test.
- [ ] **Step 2: Run cffi.** Run: `cmake linker/build && cmake --build linker/build -j12 && LINKPY_BENCH_CALLS=20000 ctest --test-dir linker/build -R '^bench_lua_py_cffi$' -V`. Expected: passes, both `bench` lines.
- [ ] **Step 3: lupa.** Run: `python3 -m venv linker/build/bench/venv && linker/build/bench/venv/bin/pip install lupa`. Write `app_lupa.py` and register the test with `PYTHONPATH` set as in Task 3.
- [ ] **Step 4: Run lupa.** Run: `cmake linker/build && LINKPY_BENCH_CALLS=20000 ctest --test-dir linker/build -R '^bench_lua_py_lupa$' -V`. Expected: passes, two `info` lines, both `bench` lines. If the `info lua=` line does not say 5.5, leave it: the driver records it and the README states that lupa brings its own Lua.
- [ ] **Step 5: Commit**: `git add tests/linker/bench/lua_py && git commit -m "test(linker): cffi-embedding and lupa routes for the Lua-to-Python benchmark"`

### Task 5: The driver

**Files:**
- Create: `tests/linker/bench/cross_bench.py`
- Test: `tests/linker/bench/test_cross_bench.py`

**Interfaces:**
- Consumes: the ctest names of the Global Constraints table; the `bench` and `info` line formats.
- Produces, in `cross_bench.py`:

```python
@dataclass(frozen=True)
class Route:
    id: str; label: str; ctest: str; expect_info: dict[str, str]   # e.g. {"view": "native"}

ROUTES: list[Route]            # the six rows of the table, in figure order
WORKLOADS = ("describe", "divide")

@dataclass
class TestSpec:  command: list[str]; workdir: str | None; env: dict[str, str]
@dataclass
class RunResult: ok: bool; times: dict[str, float]; info: dict[str, str]; note: str
@dataclass
class RouteData: status: str; note: str; samples: dict[str, list[float]]; info: dict[str, str]
                 # status: "ok" | "missing" | "failed"

def spec_from_ctest_json(doc: dict, name: str) -> TestSpec | None
def parse_bench_output(text: str) -> tuple[dict[str, float], dict[str, str]]
def failure_note(text: str) -> str
def check_run(route: Route, exit_code: int, text: str) -> RunResult
def measure(routes, specs: dict[str, TestSpec | None], rounds: int, calls: int,
            runner: Callable[[Route, TestSpec, int], RunResult]) -> dict[str, RouteData]
def rows(routes, data: dict[str, RouteData], calls: int) -> list[dict[str, str]]
def write_csv(rows, path) -> None
def run_subprocess(route, spec, calls, *, cpu: int, cache_dir: str, timeout: int) -> RunResult
def main(argv=None) -> int
```

- Behaviour the signatures do not fix:
  - `spec_from_ctest_json` reads the `ctest --show-only=json-v1` document: `command`, the `WORKING_DIRECTORY` property, and the `ENVIRONMENT` property split at the first `=` of each entry. A test with no `command` key is `None`.
  - `check_run`: `ok` only when the exit code is 0, a `describe` time was parsed, and every `expect_info` item matches; otherwise `note` is, in that order of precedence, `failure_note(text)` for a non-zero exit, `no describe line in the output`, or `info view=<got>, expected <want>`.
  - `failure_note`: the first line containing `Assertion failed`, else the last line that is not liballocs noise (`MMAP:`, `lazily mapped`, `Warning: mapping of`, `liballocs summary`, the summary block's `=`/`-` rules and `queries ` lines), else `no output`.
  - `measure`: one priming call per route with `calls=1000` (compiles and caches the linkstub, validates the route), then `rounds` rounds, each calling every still-healthy route once in `routes` order with the full `calls`. A route whose spec is `None` is `missing`, with the note `no ctest <name> in the build`, and never run; a route that fails once is `failed`, keeps its note, and is not run again.
  - `rows`: for an `ok` route one row per workload it has samples for, in `WORKLOADS` order; for a `missing`/`failed` route one row with an empty workload and empty numbers; columns `route,label,workload,status,min_us,median_us,rel_to_hand,rounds,calls,note`, numbers with 3 decimals; `rel_to_hand` is this row's `min_us` over the `hand` route's `min_us` for the same workload, empty when `hand` has none.
  - `run_subprocess`: environment is the caller's with `LD_AUDIT` and `LD_PRELOAD` removed, then the spec's `env`, then `LINKPY_BENCH_CALLS` and `LINKPY_CACHE_DIR=<cache_dir>`; command prefixed with `taskset -c <cpu>`; cwd is the spec's `workdir`; stdout and stderr both captured and both parsed.
  - `main`: options `-B <build>` (default `../../../linker/build` relative to the script), `-r <rounds>` (default 7), `-n <calls>` (default 200000), `--routes a,b,…`, `--out <dir>` (default `<build>/bench/fig10`), `--timeout <s>` (default 300), `--allow-missing`. `BENCH_CPU` (default 2) must be below `os.cpu_count()`, else exit 2 before running anything. Writes `<out>/figure10.csv` and `<out>/figure10.json` (date, `git rev-parse HEAD` and dirty flag of the repo and of `contrib/lua`, CPU model, pinned CPU, 1-minute load average at start, rounds, calls, and per route: status, note, info, raw samples), prints the table. Exit status 0 when every selected route is `ok`; 1 otherwise, unless `--allow-missing`, which forgives `missing` routes only.

- [ ] **Step 1: Write the failing tests**

```python
# tests/linker/bench/test_cross_bench.py
import io, os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_bench as cb

NOISY = """lua: MMAP: 0x76c90b179000, (nil), 0x1000, 3, 40, (null), -1, 0x0, mmap
lazily mapped a piece of pageindex at 0x0000410ed9200000
info view=proxy
bench divide: 0.310 us/call (200000 calls)
bench describe: 0.412 us/call (200000 calls)
====================================================
liballocs summary:
queries handled by heap case:                    302
"""
ABORT = """lua5.5: Warning: mapping of /x/points.so could not extend preceding bigalloc
Assertion failed at file /x/generic_malloc_index.h:937: INSERT_DESCRIBES_OBJECT(p_ins)
Aborting program lua5.5, pid 1, from address liballocs_preload.so`__assert_fail+0xc9, in 10 seconds
"""
DOC = {"tests": [
    {"name": "cross_bench_lua_py", "command": ["/usr/bin/lua5.5", "/t/app.lua"],
     "properties": [
        {"name": "ENVIRONMENT", "value": ["LINKPY_HOST_LANG=lua", "LINKSTUB_BUILD_CMD=g++ -O2 -DX=1 a.cpp"]},
        {"name": "WORKING_DIRECTORY", "value": "/b/bench_lua_py"}]},
    {"name": "not_built", "properties": []}]}

A = cb.Route("hand", "hand-written bridge", "bench_lua_py_hand", {})
B = cb.Route("polylink-ref", "PolyLink by reference", "cross_bench_lua_py_ref", {"view": "proxy"})
SPEC = cb.TestSpec(["x"], None, {})

class Parse(unittest.TestCase):
    def test_parse_ignores_noise(self):
        times, info = cb.parse_bench_output(NOISY)
        self.assertEqual(times, {"divide": 0.310, "describe": 0.412})
        self.assertEqual(info, {"view": "proxy"})
    def test_spec_from_ctest_json(self):
        s = cb.spec_from_ctest_json(DOC, "cross_bench_lua_py")
        self.assertEqual(s.command, ["/usr/bin/lua5.5", "/t/app.lua"])
        self.assertEqual(s.workdir, "/b/bench_lua_py")
        self.assertEqual(s.env["LINKSTUB_BUILD_CMD"], "g++ -O2 -DX=1 a.cpp")
    def test_spec_absent_or_not_built_is_none(self):
        self.assertIsNone(cb.spec_from_ctest_json(DOC, "nope"))
        self.assertIsNone(cb.spec_from_ctest_json(DOC, "not_built"))
    def test_note_prefers_assertion_line(self):
        self.assertIn("INSERT_DESCRIBES_OBJECT", cb.failure_note(ABORT))
    def test_note_skips_noise(self):
        self.assertEqual(cb.failure_note(NOISY + "lua: boom\n" + "lazily mapped x\n"), "lua: boom")
        self.assertEqual(cb.failure_note(""), "no output")

class Check(unittest.TestCase):
    def test_ok(self):
        r = cb.check_run(B, 0, NOISY)
        self.assertTrue(r.ok); self.assertEqual(r.times["describe"], 0.412)
    def test_nonzero_exit_fails(self):
        r = cb.check_run(B, 134, ABORT)
        self.assertFalse(r.ok); self.assertIn("Assertion failed", r.note)
    def test_missing_describe_fails(self):
        r = cb.check_run(A, 0, "bench divide: 0.3 us/call (10 calls)\n")
        self.assertFalse(r.ok); self.assertIn("describe", r.note)
    def test_info_mismatch_fails_route(self):
        r = cb.check_run(B, 0, NOISY.replace("view=proxy", "view=native"))
        self.assertFalse(r.ok); self.assertIn("view", r.note)

class Measure(unittest.TestCase):
    def run_with(self, script):          # script: {route id: [RunResult, ...]} consumed in order
        calls = []
        def runner(route, spec, n):
            calls.append((route.id, n)); return script[route.id].pop(0)
        return calls, runner
    def ok(self, d): return cb.RunResult(True, {"describe": d}, {}, "")
    def test_prime_then_interleaved_rounds(self):
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(0.2), self.ok(0.3)],
                                       "polylink-ref": [self.ok(9), self.ok(0.5), self.ok(0.4)]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: SPEC}, 2, 5000, runner)
        self.assertEqual(calls, [("hand", 1000), ("polylink-ref", 1000),
                                 ("hand", 5000), ("polylink-ref", 5000),
                                 ("hand", 5000), ("polylink-ref", 5000)])
        self.assertEqual(data["hand"].samples["describe"], [0.2, 0.3])   # the priming run is not a sample
    def test_missing_route_is_reported(self):
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(0.2)]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: None}, 1, 5000, runner)
        self.assertEqual(data["polylink-ref"].status, "missing")
        self.assertIn("cross_bench_lua_py_ref", data["polylink-ref"].note)
        self.assertEqual([c[0] for c in calls], ["hand", "hand"])
    def test_failed_route_is_not_rerun(self):
        bad = cb.RunResult(False, {}, {}, "Assertion failed at x")
        calls, runner = self.run_with({"hand": [self.ok(9), self.ok(0.2), self.ok(0.2)],
                                       "polylink-ref": [bad]})
        data = cb.measure([A, B], {A.ctest: SPEC, B.ctest: SPEC}, 2, 5000, runner)
        self.assertEqual(data["polylink-ref"].status, "failed")
        self.assertEqual(data["polylink-ref"].note, "Assertion failed at x")
        self.assertEqual([c[0] for c in calls].count("polylink-ref"), 1)
        self.assertEqual(data["hand"].status, "ok")

class Rows(unittest.TestCase):
    DATA = {"hand": cb.RouteData("ok", "", {"describe": [0.25, 0.2, 0.3]}, {}),
            "polylink-ref": cb.RouteData("failed", "Assertion failed at x", {}, {})}
    def test_rows(self):
        rs = cb.rows([A, B], self.DATA, 5000)
        self.assertEqual(rs[0], {"route": "hand", "label": "hand-written bridge", "workload": "describe",
            "status": "ok", "min_us": "0.200", "median_us": "0.250", "rel_to_hand": "1.000",
            "rounds": "3", "calls": "5000", "note": ""})
        self.assertEqual(rs[1]["status"], "failed"); self.assertEqual(rs[1]["min_us"], "")
        self.assertEqual(rs[1]["note"], "Assertion failed at x")
    def test_rel_empty_without_baseline(self):
        d = {"hand": cb.RouteData("missing", "no ctest bench_lua_py_hand", {}, {}),
             "polylink-ref": cb.RouteData("ok", "", {"describe": [0.4]}, {"view": "proxy"})}
        self.assertEqual(cb.rows([A, B], d, 10)[1]["rel_to_hand"], "")
    def test_csv_header(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "f.csv"); cb.write_csv(cb.rows([A, B], self.DATA, 5000), p)
            self.assertEqual(open(p).readline().strip(),
                "route,label,workload,status,min_us,median_us,rel_to_hand,rounds,calls,note")

class Routes(unittest.TestCase):
    def test_table(self):
        self.assertEqual([(r.id, r.ctest) for r in cb.ROUTES], [
            ("hand", "bench_lua_py_hand"), ("cffi", "bench_lua_py_cffi"), ("lupa", "bench_lua_py_lupa"),
            ("polylink-value", "cross_bench_lua_py"), ("polylink-ref", "cross_bench_lua_py_ref"),
            ("polylua-native", "cross_bench_lua_py_native")])
        self.assertEqual(cb.ROUTES[4].expect_info, {"view": "proxy"})
        self.assertEqual(cb.ROUTES[5].expect_info, {"view": "native"})

if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run them and see them fail.** Run: `python3 tests/linker/bench/test_cross_bench.py`. Expected: `ModuleNotFoundError: No module named 'cross_bench'`.
- [ ] **Step 3: Implement `cross_bench.py`** to the Interfaces block. Standard library only; executable, `#!/usr/bin/env python3`, with a header comment in the style of `wall_bench.sh` (usage line, what it measures, two examples).
- [ ] **Step 4: Run the tests.** Run: `python3 tests/linker/bench/test_cross_bench.py -v`. Expected: `OK`, 16 tests.
- [ ] **Step 5: Smoke-run against the build.** Run: `tests/linker/bench/cross_bench.py -r 2 -n 20000 --allow-missing; echo "exit $?"`. Expected: a six-row table; `hand`, `cffi`, `lupa`, `polylink-value` are `ok` with `describe` and `divide` times; the two by-reference routes are `ok`, or `failed` with the `INSERT_DESCRIBES_OBJECT` line (then `exit 1`); `linker/build/bench/fig10/figure10.csv` and `figure10.json` exist; `git status` shows no new file outside the two this task creates.
- [ ] **Step 6: Check the CPU guard.** Run: `BENCH_CPU=999 tests/linker/bench/cross_bench.py; echo "exit $?"`. Expected: one line naming `BENCH_CPU`, `exit 2`, nothing run.
- [ ] **Step 7: Commit**: `git add tests/linker/bench/cross_bench.py tests/linker/bench/test_cross_bench.py && git commit -m "test(linker): cross_bench.py, the Lua-to-Python per-call cost by route"`

### Task 6: Document, and take the numbers

**Files:**
- Modify: `tests/linker/bench/README.md` (a row in the script table and a section "Lua → Python by route")
- Modify: `linker/README.md` (two sentences in "Benchmarks" pointing at it)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: README.** The section states: the route table (id, figure bar, what crosses and how: copy through `struct Point`, luaproxy userdata handed over by address, native foreign value handed over by address, `SimpleNamespace` built by hand, cffi `struct Point` cdata, lupa table wrapper); the two workloads and their expected results; that the baselines run without the auditor and the preload; that the hand-written bridge keeps the callables and the GIL while PolyLink resolves per call and parks the GIL; that `polylua-native` goes through the generated host wrapper, not a direct `allocs.load` of the linkstub; that lupa brings its own Lua, whose version is in `figure10.json`; how to install lupa into `linker/build/bench/venv`; the CSV columns; and which column the figure uses (`min_us` of the `describe` rows, `rel_to_hand` for the baseline line).
- [ ] **Step 2: The measurement run.** On a quiet machine (1-minute load about 1): `tests/linker/bench/cross_bench.py -r 7`. Expected: exit 0 with six `ok` routes, or exit 1 with exactly the routes that the "Known state" section says are blocked marked `failed`. For each `ok` route the median is within 15% of the min; if not, the machine was busy: rerun. Do not commit the CSV; paste its table into the commit message.
- [ ] **Step 3: Full suite.** Run: `ctest --test-dir linker/build 2>&1 | tail -15`. Expected: no test fails that did not fail before this branch (compare with the same command on `dev`).
- [ ] **Step 4: Commit**: `git add tests/linker/bench/README.md linker/README.md && git commit -m "docs(linker): how the Figure 10 numbers are taken"`
