# LinkPy per-call overhead reduction: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make calls through LinkPy's JIT-compiled trampolines cheaper without changing what they mean: resolve the Python callable without a full import per call, build struct objects without re-importing `types`, stop copying by-value struct arguments and zero-filling results twice, and pin every number with a benchmark fixture.

**Architecture:** The code generator (`gen_code` in `linker/common/auditor.cpp`) emits one static per-trampoline `CallSite` record and passes it to `run_function`; the shared marshaller (`common/pycpputils.hpp`) resolves the callable through `sys.modules` and an interned attribute lookup on every call (reload-safe), calls through the vectorcall protocol, caches `types.SimpleNamespace` and interned field names, takes arguments by `const&`, and zeroes only the unused tail of `char[N]` members. Two benchmark fixtures under `tests/linker/cpython/` print µs per call and run as ordinary tests.

**Tech Stack:** C++26 with `-freflection` (g++-16), CPython 3.14 C API, CMake/ctest, the LinkPy LD_AUDIT auditor.

**Spec:** `docs/LINKPY_OVERHEAD_2026-11-02.md` in the main working tree (sections 2 and 3 give the measured candidates and the reasoning; section 0 the target numbers).

## Global Constraints

- By-value semantics, DWARF as the sole source of types, the `.py.h` grammar, every existing fixture's behaviour, quiet output and the GIL parked between calls stay as they are (brief, "Rules").
- `importlib.reload()` and replacement of a `sys.modules` entry must be seen by the next call (spec 2.1); keeping a callable across calls is rejected (spec 2.2).
- The Python → C `char[N]` contract stays: at most N bytes copied in, the remainder zero (README, "Strings and byte buffers inside by-value structs").
- The Lua marshaller keeps compiling against the same generated trampolines (`CALLEE_NS` selects `linkpy::lua`): any signature `gen_code` emits must exist in both namespaces.
- One commit per task on branch `linkpy-overhead` (worktree `.worktrees/linkpy-overhead`); never push; the main working tree's uncommitted changes are not touched.
- After every task: `ctest` in the worktree build must pass every test (33, the liballocs-gated ones are not configured here); Unit's suites (`UNITD=<wrapper> .venv/bin/pytest test/test_python_*.py` → 54 passed / 7 skipped, and `src/python/test_unit_wsgi.py` → all passed) run against the branch through a wrapper that only changes the LinkPy paths.

## Review Focus

- A Python function that raises: `run_function` must still print the traceback and throw `std::runtime_error("Failed to call function")` (Task 3 keeps the existing error path; `python_simple_divide`'s silence check covers the quiet path).
- A module not yet imported at the first call (host set `PYTHONPATH` after load): the `sys.modules` lookup misses and the real import must run (Task 3, `python_env_after_load` already covers it).
- A trampoline called from a second thread: the static `CallSite` is initialised under the GIL, which every call holds, so interning races cannot happen (Task 3 test `python_rebind` runs single-threaded; `python_gil_release` keeps the GIL contract).
- A struct result whose `bytes` field is longer than the array: still rejected with the same message (Task 6 keeps the length check before the copy; `python_bytes_array` covers the happy path).
- An aggregate with a nested aggregate member: interned-name lookup must work per member of each type (Task 5; `python_struct` and `python_generated_header` instantiate nested-free structs only, so Task 5 adds a nested struct to `bench_structs`'s fixture to cover it).

---

### Task 1: Benchmark fixtures

**Files:**
- Create: `tests/linker/cpython/bench_divide/include/mytest.py.h`, `.../bench_divide/src/mytest.py`, `.../bench_divide/src/main.c`
- Create: `tests/linker/cpython/bench_structs/include/shapes.py.h`, `.../bench_structs/src/shapes.py`, `.../bench_structs/src/main.c`
- Modify: `tests/linker/cpython/CMakeLists.txt` (register both with `add_linkpy_test(... LANG python)`)
- Modify: `linker/README.md` (how to read the numbers: `ctest -R python_bench -V`)

**Interfaces:**
- Produces: two ctests, `python_bench_divide` and `python_bench_structs`, exit 0 on success and print one line per operation in the form `bench <op>: <µs> us/call (<N> calls)`; `LINKPY_BENCH_CALLS` overrides N (default 200000 for divide, 20000 for structs).

- [ ] **Step 1: Write `bench_divide`** — `mytest.py.h`: `extern float divide(int a, int b);`; `mytest.py`: `def divide(a, b): return a / b`; `main.c`: warm 1000 calls, then N timed calls of `divide(6, 3)` with `clock_gettime(CLOCK_MONOTONIC)`, print `bench divide(6,3): %.3f us/call (%d calls)`, exit 0 when the sum of results is positive.
- [ ] **Step 2: Write `bench_structs`** — `shapes.py.h` with `#define CHUNK 65536`, `#define HEAD_MAX 32768`, `struct chunk { unsigned char data[CHUNK]; int len; };`, `struct point { int x; int y; };`, `struct answer { int kind; int status; struct point origin; int headers_len; unsigned char headers[HEAD_MAX]; };`, `struct meta { int target; int tls; const char *method; const char *request_uri; const char *path; const char *query; const char *version; const char *remote; const char *local_addr; const char *local_port; const char *server_name; };`, functions `extern int take(int req, struct chunk c);` (returns `c.len`), `extern struct chunk give(int req, int n);` (`data` = n bytes of `'X'`, `len` = n), `extern struct answer ask(int req, struct meta m);` (`kind 1, status 200, origin (3,4), headers = b"Content-Length\0" + ...`). `shapes.py` prebuilds the three `bytes` once. `main.c` times `take` with len 65536 and 100, `give(65536)`, `give(0)`, `ask(meta)`; checks `take` echoes the length, `give(65536).data[65535] == 'X'`, `give(0).len == 0`, `ask().origin.y == 4`; prints one `bench` line each.
- [ ] **Step 3: Register both in `tests/linker/cpython/CMakeLists.txt`** after `str_field`, with a comment that they are benchmarks that pass on exit code.
- [ ] **Step 4: Build and run** — `cmake --build linker/build -j12 && ctest --test-dir linker/build -R python_bench -V`; expected: both pass and print their lines (record them in the commit message as the baseline).
- [ ] **Step 5: Document** the two tests in the README's "Quiet by default, the GIL, and the linkstub cache" neighbourhood (one paragraph) and commit: `git add tests/linker/cpython/bench_divide tests/linker/cpython/bench_structs tests/linker/cpython/CMakeLists.txt linker/README.md docs/superpowers/plans/2026-11-03-linkpy-overhead.md && git commit -m "test(linker): benchmark fixtures for the per-call and by-value struct costs"`.

### Task 2: Reload-semantics fixture

**Files:**
- Create: `tests/linker/cpython/rebind/include/mytest.py.h`, `.../rebind/src/mytest.py`, `.../rebind/src/main.c`
- Modify: `tests/linker/cpython/CMakeLists.txt`

**Interfaces:**
- Produces: ctest `python_rebind`, which Task 3 must keep green.

- [ ] **Step 1: Write the fixture** — header: `extern int value(); extern void rebind(); extern void replace();`. `mytest.py`: `value()` returns 1; `rebind()` does `global value; value = lambda: 2`; `replace()` builds `types.ModuleType(__name__)` with `value = lambda: 3` and the same `rebind`/`replace`, and stores it in `sys.modules[__name__]`. `main.c`: `value() == 1`, `rebind()`, `value() == 2`, `replace()`, `value() == 3`, exit 1 on any mismatch with a message naming the step.
- [ ] **Step 2: Register** `add_linkpy_test(rebind LANG python)` with a comment: each call sees the module's current attribute and the current `sys.modules` entry.
- [ ] **Step 3: Run** `ctest --test-dir linker/build -R python_rebind`; expected: PASS (today's marshaller imports per call).
- [ ] **Step 4: Commit**: `git commit -m "test(linker): pin that a call sees rebinding and sys.modules replacement"`.

### Task 3: Per-trampoline call-site record

**Files:**
- Modify: `common/pycpputils.hpp` (declarations, `run_function`, `run_function_void`)
- Modify: `linker/cpython/pycpputils.cpp` (`resolve_callable`)
- Modify: `linker/lua/luacpputils.hpp` (`CallSite` + overloads)
- Modify: `linker/common/auditor.cpp:143-170` (`gen_code`)
- Modify: `linker/common/linkject.hpp:124-134` (draft injected codegen, same shape)
- Test: `python_bench_divide`, `python_rebind`, full ctest

**Interfaces:**
- Produces, in `linkpy::py`: `struct CallSite { const char *module; const char *function; PyObject *module_name; PyObject *function_name; };` (aggregate; the generated code writes `static linkpy::py::CallSite site{"mod", "fn"};`); `PyObject *resolve_callable(CallSite &site, PyObject **module_out);` (new references; `*module_out` may be NULL only on failure; throws `std::runtime_error` after `PyErr_Print()` as `import_module`/`import_function` did); `template <typename T, typename... Args> T run_function(CallSite &site, int argc, Args... args);` and `run_function_void(CallSite &site, int argc, Args... args)`.
- Produces, in `linkpy::lua`: `struct CallSite { const char *module; const char *function; };` and `run_function`/`run_function_void` overloads taking `const CallSite &` that forward to the existing string overloads.
- `gen_code` emits: `static {NS}::CallSite site{{"{module}", "{name}"}};` as the first statement of the trampoline body and calls `{NS}::run_function<{T}>(site, {argc}, args...)` / `run_function_void(site, ...)`.

- [ ] **Step 1: Record the baseline** — `ctest -R python_bench_divide -V` number into the commit message draft.
- [ ] **Step 2: Implement `resolve_callable`** in `pycpputils.cpp`: intern `site.module_name`/`function_name` with `PyUnicode_InternFromString` on first use; `PyImport_GetModule(site.module_name)`; on NULL without a pending error, `PyImport_Import(site.module_name)`; `PyObject_GetAttr(module, site.function_name)` + `PyCallable_Check`; error path: `PyErr_Print()`, drop references, throw `std::runtime_error("Failed to import module")` / `("Failed to import function")` as today.
- [ ] **Step 3: Change `run_function`/`run_function_void`** to the new signature; remove the two `std::string` parameters and the `import_module`/`import_function` calls (keep those functions: `verify_function` in the auditor uses them).
- [ ] **Step 4: Add the Lua `CallSite` and overloads** in `luacpputils.hpp`.
- [ ] **Step 5: Change `gen_code`** and the `linkject.hpp` draft to emit the static record and pass it.
- [ ] **Step 6: Build, run** `ctest --test-dir linker/build`; expected: all pass (`python_rebind`, `python_env_after_load`, `python_simple_divide` silence, the `lua_*` and `cross_*` tests included).
- [ ] **Step 7: Measure** `python_bench_divide` and `python_bench_structs`; expected: `divide` from about 0.75–0.84 µs to about 0.3 µs; record both in the commit message.
- [ ] **Step 8: Unit suites** through the branch wrapper (both green) and `tools/linkpy-bench.py` empty / 64 KiB p50 recorded.
- [ ] **Step 9: Commit**: `git commit -m "perf(linker): resolve the callable per call through sys.modules, not a full import"`.

### Task 4: Vectorcall

**Files:**
- Modify: `common/pycpputils.hpp` (`pack_args` → `pack_argv`, `run_function`, `run_function_void`)
- Test: full ctest, `python_bench_divide`

- [ ] **Step 1: Replace the tuple** with `PyObject *argv[sizeof...(Args) + 1]`, `argv[0] = nullptr`, `pack_argv(argv + 1, args...)` (fold of `arg_to_PyObject`), `PyObject_Vectorcall(pFunc, argv + 1, sizeof...(Args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr)`, then `Py_DECREF` each argument; keep `call_function` for the void path's error printing or inline the same `PyErr_Print()`.
- [ ] **Step 2: Build, ctest** (all pass), **measure**, **commit**: `git commit -m "perf(linker): call through the vectorcall protocol"`.

### Task 5: Cached namespace class and interned field names

**Files:**
- Modify: `common/pycpputils.hpp` (`T_to_PyObject` aggregate branch, `PyObject_to_T` aggregate branch, new `field_name<M>()`)
- Modify: `tests/linker/cpython/bench_structs/...` (already has the nested `struct point`)
- Test: full ctest, `python_bench_structs` (`ask`)

- [ ] **Step 1: Add** `template <std::meta::info M> PyObject *field_name()` returning a function-local static `PyUnicode_InternFromString(std::define_static_string(std::meta::identifier_of(M)))`.
- [ ] **Step 2: Cache the class**: function-local `static PyObject *ns_cls` in the aggregate branch of `T_to_PyObject`, imported once (`PyImport_ImportModule("types")` + `GetAttrString`), never released.
- [ ] **Step 3: Use interned names**: `PyObject_SetAttr(obj, field_name<member>(), attr)` and `PyObject_GetAttr(pValue, field_name<member>())`; error messages keep the `field` C string.
- [ ] **Step 4: Build, ctest** (all pass; `python_struct`, `python_struct_arg`, `python_bytes_array`, `python_str_field`, `python_generated_header`, `cross_*`), **measure** `ask`, **commit**: `git commit -m "perf(linker): cache types.SimpleNamespace and intern struct field names"`.

### Task 6: Arguments by reference, zero only the tail

**Files:**
- Modify: `common/pycpputils.hpp` (`run_function`, `run_function_void`, `pack_argv` take `const Args &...`; `PyObject_to_T` aggregate branch uses `T result;`; `PyObject_into_array` zeroes `[n, N)`)
- Test: `python_bytes_array` (zero tail), `python_bench_structs` (`take`, `give`)

- [ ] **Step 1: `const Args &... args`** in the three templates (the trampoline's own parameter stays by value: it is the C ABI).
- [ ] **Step 2: Tail zeroing**: `T result;` in the aggregate branch (every member is assigned by the loop or filled by `PyObject_into_array`), and `std::memset(reinterpret_cast<char *>(dest) + n, 0, N - n)` after the `memcpy` in `PyObject_into_array`; update the two comments that describe the zero-fill.
- [ ] **Step 3: Build, ctest** (all pass), **measure** (`take` 64 KiB expected to roughly halve), **commit**: `git commit -m "perf(linker): pass struct arguments by reference and zero only the unused array tail"`.

### Task 7: Build the auditor optimised by default

**Files:**
- Modify: `linker/CMakeLists.txt` (default `CMAKE_BUILD_TYPE` to `RelWithDebInfo` when unset)
- Test: full ctest; cache-hit start time of `python_bench_divide` with `LINKPY_BENCH_CALLS=1` before/after

- [ ] **Step 1: Measure** `time` of a cache-hit run of `bench_divide` with `LINKPY_BENCH_CALLS=1` (three runs).
- [ ] **Step 2: Add** after `project()`: `if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES) set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE) endif()` with a comment naming the SHA-256 cache key as the reason.
- [ ] **Step 3: Reconfigure, build, ctest** (all pass), **measure** again, **commit**: `git commit -m "build(linker): optimise the auditor by default (the cache key hashes megabytes per start)"`.

### Task 8: Final numbers and hand-off

- [ ] **Step 1: Run** the full ctest once more, both Unit suites, `tools/linkpy-bench.py` for upstream and the branch (pinned to the P-cores, six rounds, min and median p50).
- [ ] **Step 2: Append** a "Results on branch linkpy-overhead" table to `docs/LINKPY_OVERHEAD_2026-11-02.md` in the worktree (benchmark fixtures before/after per task, Unit p50, test status) and commit: `git commit -m "docs: results of the overhead work"`.
