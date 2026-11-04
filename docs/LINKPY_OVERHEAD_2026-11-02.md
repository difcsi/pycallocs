<!-- Copied from the measurement session's scratchpad on 2026-11-02. The $S paths below
     name that session's scratchpad directory, which is ephemeral; the harnesses and variant
     headers it refers to are described in the text well enough to be recreated. -->

# LinkPy per-call and per-byte overhead: measurements and recommendations

Measured 2026-11-02 on this machine (i7-1365U, 2 P-cores + 8 E-cores, laptop governor; shared
with other work during the session, see "Noise"). No file in `~/Develop/pycallocs` or
`~/Develop/unit` was modified; every experiment lives under the scratchpad directory
`/tmp/claude-1002/-home-zoltan-Develop-pycallocs/4c7c3692-2b9f-463f-b177-d33a258ccc21/scratchpad`
(referred to as `$S` below), and every candidate change was tried as a *shadow copy* of
`common/pycpputils.hpp` placed first on the JIT compiler's include path, so the tree's own
header never changed.

The tree moved during the session: at 20:39 a parallel edit landed in `auditor.cpp`,
`linkstub_cache.cpp`, `template.cpp`, `pycpputils.cpp` and the README (`Py_InitializeEx(0)`,
`std::call_once` in `ensure_vm`, cache hardening, more tests). Everything in section 2 onwards
was measured on that later state; the 13:xx baselines in section 1 are from the earlier one.

## 0. Summary

| change | `divide(6,3)` µs/call | eight-call `empty` request µs | 64 KiB body request µs | 64 KiB upload request µs | fixtures |
|---|---|---|---|---|---|
| tree as is (linkstub at `-O0`) | 0.84 | 19.0 | 53.5 | 81.6 | 33/36 (3 expected failures) |
| + call-site record: `sys.modules` + interned getattr per call | 0.29 | 14.5 | 46.9 | 74.9 | — |
| + vectorcall instead of a tuple | 0.25 | 14.6 | 45.3 | 72.7 | — |
| + cached `SimpleNamespace` class, interned field names | 0.24 | 12.5 | 42.8 | 71.1 | 11/11 |
| + `const Args&...` (no by-value copies of struct args) | 0.24 | 12.1 | 42.9 | 64.7 | — |
| + no `T result{}`, zero only the array tail | 0.24 | 11.8 | 42.0 | 65.0 | 11/11 |
| tree header, linkstub at `-O2` (no code change) | 0.75 | 17.4 | 49.3 | 79.9 | 11/11 |

(µs per request = time inside the `py_*` trampolines of one Unit request, driven by a C
harness against the real `unit_wsgi.py`; min over 5 interleaved rounds on a P-core. Full
tables in sections 2 and 3.)

Headline numbers:

* The per-call fixed cost is dominated by `PyImport_Import` + `PyObject_GetAttrString` on
  every call: 6,300 of the 13,340 instructions of a `divide` call. Resolving the callable
  through `sys.modules` and an interned attribute lookup on each call keeps reload
  semantics exactly and takes `divide` from 0.84 to 0.29 µs.
* For Unit's `empty` request the trampolines cost 19.0 µs; the measured candidates bring
  that to 11.8 µs (−38%). Of what remains, 4.0 µs is the adapter's own Python (measured by
  calling it from Python, section 4), ~0.9 µs the mandatory zero-fill of the terminating
  `py_next_chunk` result and of `py_call`'s header array, and the rest is CPython API work
  per field and the GIL.
* The 64 KiB data paths are copy-bound. The marshaller holds three avoidable 64 KiB copies
  on the way in (two by-value parameter copies: −4.9 µs per `py_body`; the full-array
  `bytes`: −1.2 µs for a small body) and one avoidable 64 KiB `memset` on the way out
  (−0.4 µs). What is left is inherent to by-value crossing (one copy into the C struct,
  the caller's copy out of the return slot) plus the adapter's own copying
  (`bytes(buffer[:CHUNK])`, `buffer += data`), which is the larger part: of the 53.5 µs a
  64 KiB response costs at the boundary, 24 µs are spent inside the adapter's Python, and
  keeping the pieces in a list instead of one `bytearray` cuts that to 6 µs with the same
  48 adapter tests passing (section 4).
* Two things outside the marshaller cost more than any of the above:
  the linkstub is compiled with no `-O` flag at all, and on the tree as it is now a
  cache-hit process start spends 2.5 billion instructions (97% of the process) computing
  the cache key with an unoptimised SHA-256 over ~11 MB of inputs (section 5).
* libgccjit (now installed, 16.0.1) compiles the nine trampolines to a `.so` in 60–170 ms
  against 3–5 s for the `g++` shell-out under the same load (1.8 s on the quiet
  afternoon machine), but it has no C++ front end: the current reflection-based
  marshaller cannot go through it (section 6).

## 1. Baselines (quiet afternoon machine, tree before the 20:39 change)

* `divide(6, 3)`: 0.79 µs/call (median of 3×5×200,000 calls, pinned to a P-core); the
  brief's 0.75. callgrind: 9,900 instructions per `run_function<float,int,int>` call,
  6,290 of them in `import_module` (`PyUnicode_FromString` + `PyImport_Import`).
* Unit-shaped harness (`$S/unitshape/main.c`: the real `unit_wsgi.py` and `unit_wsgi.py.h`,
  the C side's call sequence, no router), µs inside the trampolines per request:

  | app | per request | `py_begin` | `py_header` (each) | `py_body` | `py_call` | `py_next_chunk` (each) | `py_close` |
  |---|---|---|---|---|---|---|---|
  | `empty` | 16.5 | 1.13 | 1.32 | — | 7.9 | 3.6 (the len=0 terminator) | 1.2 |
  | `body_generate` 64 KiB | 44.2 | 1.19 | 1.26 | — | 10.5 | 13.7 avg (≈24 + 3.6) | 1.4 |
  | `mirror`, 64 KiB upload | 70.0 | 1.37 | 1.33 | 8.6 | 11.9 | 18.6 avg | 6.9 |

* Unit end to end (`tools/linkpy-bench.py`, 10,000 keep-alive GETs): the brief's numbers
  (p50 50 vs 83 µs empty, 72 vs 167 µs 64 KiB) could not be reproduced within noise; the
  same binary swung between 84 and 181 µs p50 across back-to-back runs on this machine
  today. See section 4 for the controlled runs.

## 2. Per-call fixed costs

Primitive costs on this machine (CPython 3.14.4 C API, 200,000 iterations, quiet machine,
P-core; `$S/copycost/copycost.c`):

| operation | µs |
|---|---|
| `PyImport_Import(name)` of an already-imported module | 0.37 |
| `PyImport_GetModule(interned)` (`sys.modules` lookup) | 0.054 |
| `PyObject_GetAttrString` + `PyCallable_Check` | 0.075 |
| `PyObject_GetAttr(interned)` + `PyCallable_Check` | 0.021 |
| `PyGILState_Ensure` + `Release` (GIL parked, uncontended) | 0.042 |
| `PyTuple_New(2)` + two `PyLong_FromLong` + `SetItem` + dealloc | 0.030 |
| `PyImport_ImportModule("types")` | 0.38 |
| `SimpleNamespace()` | 0.065 |
| `PyObject_SetAttrString` / `PyObject_SetAttr(interned)` | 0.071 / 0.026 |
| `PyObject_GetAttrString` / `PyObject_GetAttr(interned)` | 0.074 / 0.020 |
| `PyUnicode_FromString` of a short C string | 0.027 |

Candidates, each measured as a shadow header (`$S/variants/<name>/pycpputils.hpp`, flags
in `$S/variants/master/pycpputils_opt.hpp`), cumulative, quiet-machine run, min of 5 rounds:

| variant | `divide` µs | instr./call | `empty` µs/req | `py_begin` µs | `py_call` µs |
|---|---|---|---|---|---|
| tree (`-`) | 0.84 | 13,340 | 18.97 | 1.32 | 9.26 |
| `v1_callsite`: per-call-site record, `PyImport_GetModule` + `PyObject_GetAttr` (interned) each call | 0.29 | 6,117 | 14.53 | 0.61 | 8.29 |
| `v1b`: same but the callable kept forever | 0.19 | 4,688 | 13.52 | 0.50 | 8.04 |
| `v2`: v1 + `PyObject_Vectorcall` on a stack array | 0.25 | 5,785 | 14.55 | 0.61 | 8.27 |
| `v3`: v2 + cached `SimpleNamespace` class + interned field names | 0.24 | 5,828 | 12.52 | 0.62 | 6.42 |
| `O2`: tree header, linkstub compiled `-O2` | 0.75 | 11,925 | 17.38 | 1.09 | 8.80 |

Recommendations and the reasoning:

1. **Call-site record (do this).** `gen_code` should emit one `static linkpy::py::CallSite
   site{"module", "function"};` per trampoline and pass it to `run_function`, instead of
   two string literals that become two `std::string` temporaries per call. The record
   holds the two names as interned `PyObject*`. Each call resolves
   `PyImport_GetModule(site.module)` (new ref, NULL without error when not imported, in
   which case fall back to `PyImport_Import` as today) and `PyObject_GetAttr(mod,
   site.function)`. This is semantically identical to today: `importlib.reload()` re-executes
   into the same module object, so the fresh `getattr` sees the new function; replacing
   `sys.modules[name]` is seen by the next call; a missing function fails at call time as
   today. Cost 0.14 + 0.045 µs instead of 0.64 + 0.19. The experiment keyed the record by
   the literals' addresses (header-only stand-in); the codegen form needs no lookup.
2. **Keeping the callable (`v1b`) is rejected** for 0.05–0.10 µs: it silently ignores
   `importlib.reload` and `sys.modules` replacement. If that last 0.1 µs is ever wanted, the
   justifiable form is a `PyDict_Watch` (3.12+) on `module.__dict__` and on `sys.modules`
   that drops the cached pointer on any write; not worth it now.
3. **Vectorcall**: 0.04 µs per call on `divide`, nothing measurable on the Unit shapes (the
   tuple is a rounding error next to a 64 KiB struct). Cheap and harmless; take it with 1,
   mainly because the argument array on the stack also removes the `PyTuple_SetItem`
   reference-stealing subtlety. Python functions already take the vectorcall path
   internally, so `PyObject_CallObject` only costs the tuple.
4. **Names as `const char*`**: subsumed by 1.
5. **`GilGuard`**: 0.08 µs, inherent to the parked-GIL contract; unchanged. `ensure_vm` is
   now a `std::call_once` per call (the 20:39 change); its fast path is a few instructions.
6. **`-O2` for the linkstub** (`LINKSTUB_BUILD_CMD` in `tests/linker/tests.cmake` and Unit's
   `auto/modules/python`; the README on disk already says to): 0.84 → 0.75 µs and ~8% on the
   request shapes, no code change. It roughly doubles the cold JIT compile (section 5), so
   pair it with the prebuilt marshaller object.

## 3. By-value structs carrying arrays

Per-copy costs on this machine (quiet, P-core): `memset` 64 KiB 0.80 µs, 32 KiB 0.10 µs
(64 KiB no longer fits the P-core's 48 KiB L1), `memcpy` 64 KiB 1.0 µs,
`PyBytes_FromStringAndSize` of 64 KiB 1.05 µs, of 100 bytes 0.017 µs.

Where the copies are today, for `py_body(int, struct unit_wsgi_bytes)` (64 KiB in):
caller's by-value argument (ABI, inherent) → `run_function(Args... args)` copy →
`pack_args(Args... args)` copy → `PyBytes_FromStringAndSize(data, 65536)` regardless of
`len` → Python `chunk.data[:chunk.len]` slice. For `py_next_chunk` (64 KiB out):
`T result{}` zero-fill → `memcpy` from the `bytes` → NRVO through `PyObject_to_T`,
`run_function` and the trampoline into the caller's return slot (verified in the `-O0` and
`-O2` disassembly: no further copy) → Unit's `*chunk = py_next_chunk(id)` copies once more
(Unit's code; GCC cannot prove `chunk` unaliased).

Cumulative variants (quiet run, min of 5 rounds), µs per call:

| variant | `py_body` 64 KiB | `py_body` 100 B | `py_next_chunk` 64 KiB | `py_next_chunk` len 0 | `body64k` µs/req | `mirror64k` µs/req | `mirror100` µs/req |
|---|---|---|---|---|---|---|---|
| tree | 9.97 | 7.06 | 16.45 avg | 3.99 | 53.5 | 81.6 | 31.7 |
| v3 (section 2) | 8.10 | 5.74 | 14.72 avg | 3.20 | 42.8 | 71.1 | 22.3 |
| `v4`: v3 + `const Args&...` in `run_function`/`pack_args` | 5.05 | 3.18 | 14.71 avg | 2.97 | 42.9 | 64.7 | 20.0 |
| `v5`: v4 + no `T result{}`, `PyObject_into_array` zeroes only the tail | 4.95 | 3.19 | 14.29 avg | 3.10 | 42.0 | 65.0 | 20.1 |
| `v6`: v5 + any buffer-protocol object accepted into `char[N]` | 4.93 | 3.27 | 13.67 avg | 3.08 | 40.4 | 63.5 | 20.4 |
| `v7`: v6 + C→Python `bytes` of the sibling `len` only | 5.03 | 2.02 | 14.25 avg | 3.21 | 42.2 | 65.4 | 19.5 |

(callgrind, instructions per `py_body` with a 100-byte upload: 221k tree → 205k v3 → 74k v4
→ 7.8k v7.)

Recommendations:

1. **`const Args&...` (do this).** Two 64 KiB copies per struct argument for free: `py_body`
   9.97 → 5.05 µs. `arg_to_PyObject` already takes `const T&`; only `run_function`,
   `run_function_void` and `pack_args` change. The trampoline's own by-value parameter stays
   (it is the C ABI).
2. **Zero only the tail (do this, small).** `T result;` plus `memset(dest + n, 0, N - n)` in
   `PyObject_into_array` keeps the documented "remainder stays zero" contract and saves one
   64 KiB `memset` per full chunk (~0.4 µs measured). It cannot help the terminating
   `len = 0` chunk, whose tail is the whole array: that 64 KiB zero-fill plus the 32 KiB
   headers array of `py_call` are inherent to by-value crossing of fixed-capacity structs
   (~1.5 µs per request on `empty`). Scalars are all assigned by the reflection loop, so
   nothing stays indeterminate except padding.
3. **NRVO**: nothing to do; the return chain is already copy-free at `-O0` and `-O2`. The one
   remaining copy out is Unit's `*chunk = py_next_chunk(id)` (1.5 µs per 64 KiB); Unit could
   return into a local or write into the shared memory directly, but that is Unit's code.
4. **Buffer protocol on the way back (`v6`)**: no measurable gain by itself (within noise of
   v5), and the adapter cannot exploit a `memoryview` of its `bytearray` because a resize
   (`del buffer[:n]`) is refused while a view is exported. Harmless and additive, but it
   does not pay for a new convention; not recommended now.
5. **Used-length convention (`v7`)**: the only win is C→Python `bytes` of the used length
   (a small POST body: `py_body` 3.2 → 2.0 µs; nothing for full chunks, nothing on the way
   out). The rule tested (an integral sibling named `len` or `<array>_len` bounds the
   array) is precise and DWARF-driven, but it changes the contract the `python_bytes_array`
   fixture pins (`len(b.data) == 16` → 5: the fixture check fails as expected), and Unit's
   `headers`/`headers_len` pair would match it only on the way out, where it does nothing.
   Rejected as a default; if wanted, make it opt-in per field (e.g. only `<array>_len`) and
   document it, 1.2 µs per small upload is the whole prize.

## 4. Python-side costs in Unit's adapter

`$S/adapter_py_cost.py` drives the same request sequence by calling `unit_wsgi.py`'s
functions from Python (the struct arguments as `SimpleNamespace`s, the 64 KiB `bytes` the
marshaller would build), so the numbers are the adapter's own cost with no marshalling;
the harness number minus this is the boundary's share. Session-start adapter, µs per
request (two reps, the smaller):

| app | inside the adapter | of which `py_call` | `py_next_chunk` (each) | `py_close` | harness total (section 3) |
|---|---|---|---|---|---|
| `empty` | 4.0 | 2.5 | 0.26 | 0.25 | 19.0 |
| `body_generate` 10 B | 5.0 | 2.8 | 0.33 | 0.27 | 24.9 |
| `body_generate` 64 KiB | 23.9 | 5.2 | 8.5 | 0.33 | 53.5 |
| `mirror` 100 B up | 5.9 | 3.2 | 0.37 | 0.28 | 31.7 |
| `mirror` 64 KiB up | 55.2 | 19.2 | 13.8 | 5.4 | 81.6 |

So on the small shapes the adapter is a fifth of the boundary cost and the marshaller the
rest; on the 64 KiB shapes it is the other way round: the adapter's `bytearray` handling
(`r.buffer += data` in `_accept`, `bytes(r.buffer[:WSGI_CHUNK])` and `del` in
`py_next_chunk`, `io.BytesIO(bytes(r.body))`, and freeing it all in `py_close`) is 24 of
53 µs and 55 of 82 µs.

A scratchpad copy keeps the response pieces in a `collections.deque` of the `bytes` the
application produced (`$S/unitcopy/src/python/unit_wsgi.py`, 26 changed lines):
`_accept` appends, `py_next_chunk` pops one piece and hands it over unchanged when it is at
most `WSGI_CHUNK` long, slices only an oversized one, and joins consecutive small pieces up
to `WSGI_CHUNK` so an application writing many tiny strings still crosses in few calls.
Semantics (order, Content-Length accounting, error ordering) are unchanged and the 48
adapter unit tests pass on it, on top of the adapter as it is on disk now. Adapter-side cost
per request: `body_generate` 64 KiB 23.9 → 6.2 µs, `mirror` 64 KiB 55.2 → 8.3 µs (the
`_accept` copy, the slice-and-`bytes` double copy and the `py_close` freeing all go away);
`empty` and the small shapes unchanged. Through the harness (same core, min of four rounds,
a quieter moment than section 3's run, hence the lower baseline), µs per request inside
the trampolines:

| adapter | marshaller | `empty` | `body` 10 B | `body` 64 KiB | `mirror` 100 B | `mirror` 64 KiB |
|---|---|---|---|---|---|---|
| as at session start | tree | 16.2 | 23.1 | 46.0 | 30.1 | 80.9 |
| as at session start | v5 | 11.3 | 15.4 | 38.5 | 18.6 | 60.1 |
| chunk list | tree | 17.2 | 23.3 | 25.0 | 29.4 | 34.6 |
| chunk list | v5 | 11.2 | 16.0 | 17.8 | 19.3 | 22.5 |

Marshaller and adapter changes are additive: the 64 KiB response goes from 46 to 18 µs at
the boundary and the 64 KiB upload from 81 to 22 µs.

This needs nothing from the marshaller (a `bytes` already crosses as is), so under the
brief's rule it is a finding rather than a change to make; it is the single largest item
on the 64 KiB path. Note that the request-body side of the adapter was already changed in
the same spirit in a parallel edit at 20:42 today (`r.body` is now a list of chunks joined
once for `wsgi.input`), which is why the copy was re-based on the current file.

## 4b. Unit end to end

`tools/linkpy-bench.py` (10,000 keep-alive GETs on one connection), six rounds per
configuration interleaved, the whole thing (client, unitd, router, app) pinned to the two
P-cores; p50 in µs. Each LinkPy configuration is a scratchpad `linkpy-unitd` wrapper
(`$S/unitwrap/<cfg>/`) that differs from Unit's only in the build command (variant include
path, scratchpad cache); Unit itself was rebuilt by nobody.

| configuration | `empty` p50 min / median | `body_generate` 64 KiB p50 min / median | best req/s (empty / 64 KiB) | config apply, min ms |
|---|---|---|---|---|
| upstream (`unit-baseline`) | 46 / 50 | 61 / 70 | 21,600 / 16,100 | 12 |
| LinkPy, tree as is | 61 / 78 | 119 / 149 | 15,100 / 7,900 | 45 |
| tree header, linkstub `-O2` | 71 / 88 | 113 / 166 | 14,100 / 8,400 | 46 |
| v3 (call-site + vectorcall + ns cache) | 65 / 68 | 105 / 143 | 15,100 / 8,800 | 45 |
| v5 (v3 + `const&` + tail-only zeroing) | 55 / 80 | 108 / 150 | 16,800 / 8,500 | 46 |
| v7 (v5 + used-length `bytes`) | 66 / 86 | 118 / 138 | 15,500 / 8,200 | 45 |

The upstream numbers match the brief's (50 / 72). Round-to-round spread is 20–30 µs even
pinned, so this table ranks only coarsely; it agrees with the harness in direction and
size (harness: −7 µs on `empty`, −11.5 µs on the 64 KiB body for v5; end to end, min p50:
−6 and −11). The LinkPy build's gap to upstream on `empty` goes from +15 to +9 µs with the
marshaller changes alone; on the 64 KiB body from +58 to about +45, with the adapter's own
copying (section 4) the bulk of what remains. Config apply (app start, warm cache) is
45 ms against 12 ms upstream: the cache-key hashing of section 5.

## 5. Start-up and compile costs (outside the per-call path, but larger)

* **The linkstub is compiled at `-O0`.** `LINKSTUB_BUILD_CMD` in `tests/linker/tests.cmake`
  and Unit's `auto/modules/python` carry no `-O` flag (`DW_AT_producer` of the cached
  `liblinkstub.so` confirms). The auditor itself is built with an empty `CMAKE_BUILD_TYPE`,
  i.e. `-O0` too.
* **Cold JIT**: with the tree's command, `g++-16 -std=c++26 -freflection -g` compiles
  `linkstub.cpp` *and* `pycpputils.cpp` from source each time. Breakdown (two reps, machine
  not quiet, so read the ratios): `linkstub.cpp` 2.0 s, `pycpputils.cpp` 1.5–2.4 s, link
  0.06 s at `-O0`; 2.5–4.6 s, 2.9–3.1 s, 0.1 s at `-O2`. Through the auditor with a fresh
  cache: 3.1–4.1 s (`-O0`) vs 4.0–4.8 s (`-O2`) for `simple_divide`, 4.7–5.6 s vs
  5.5–7.0 s for the Unit-shaped stub (1.8 s on the quiet afternoon machine for the former).
  Compiling the marshaller's `.cpp` once at LinkPy build time and linking the object (or a
  static library) removes ~45% of every cold start; a precompiled header of
  `pycpputils.hpp` + `Python.h` + `<meta>` helped only at `-O2` here (3.7 vs 4.6 s).
* **Warm start (cache hit) costs 0.21–0.35 s per process** on the tree as it is now
  (`python3 -c pass` is 0.03 s). callgrind: 2.48 of 2.56 billion instructions are
  `linkstub_cache_key` → `Sha256::update`. The 20:39 change made the key cover every
  `.h`/`.hpp` directly under each `-I` directory (1.9 MB of Python headers), every regular
  file named in the command (`libpython3.14.so`, 8.2 MB, in the tests' command) and the
  auditor binary (0.9 MB), hashed by a straightforward SHA-256 compiled at `-O0`
  (≈ 230 instructions per byte). Before that change the key cost 224 M instructions. Unit's
  command links `-lpython3.14` so it hashes ~3 MB, but every Unit app process and the
  discovery process pay it (config apply 66–132 ms vs upstream 25–36 ms in today's runs,
  53 vs 34 ms in the notes). Fixes, in order of cheapness: build the auditor with `-O2`
  (≈10× on the hash); key the big inputs by `(path, size, mtime)` like the compiler binary
  already is, or by the ELF build-id; hash the small inputs only.
* **libgccjit**: see section 6.

## 6. Compiling in-process instead of shelling out to `g++` (libgccjit)

What exists: `libgccjit-16` is now installed (`/usr/lib/gcc/x86_64-linux-gnu/16/libgccjit.so`,
header under `.../16/include`). It is GCC's middle and back end behind a C API that builds
GIMPLE-level IR (types, functions, blocks, statements) and compiles it in-process; it has
no C, C++ or any source-language front end. The alternatives on this machine do not fit
either: `libcc1.so` (present) is GDB's compile plug-in and still spawns `cc1plus`; the
installed clangs (16–21) do not accept `-freflection`/`^^`, so an in-process clang could not
compile the reflection marshaller.

Proof of concept (`$S/jitpoc/poc.c`): the auditor-side information LinkPy already has
(function signatures and struct layouts from DWARF) is enough to build Unit's eight
trampolines plus `divide` through the API: struct types by value, `char[N]` fields, a
per-field loop that calls a small C support runtime (`linkpy_rt_enter` = GIL + callable
resolution, `box_long/str/bytes`, `ns_new/ns_set`, `call`, `getattr`, `unbox_*`, `leave`).
`gcc_jit_context_compile_to_file(GCC_JIT_OUTPUT_KIND_DYNAMIC_LIBRARY)` produces a 23 KB
`liblinkstub_jit.so` with the nine exported symbols:

| | cold compile, this machine (not quiet) |
|---|---|
| libgccjit `-O0`, with debuginfo | 59–169 ms (median ≈ 95 ms) |
| libgccjit `-O2` | 209–473 ms |
| `g++` shell-out, tree command (`-O0`) | 3.1–5.6 s (1.8 s quiet) |

Caveats found while doing it: libgccjit still `execve`s `as`, `collect2` and `ld` for the
assemble and link steps (its "embedded driver" only avoids the `gcc` driver process), so it
is not subprocess-free, just free of the 2–4 s C++ front end; its in-memory mode
(`gcc_jit_context_compile` + `dlopen`) needs the support runtime's symbols resolvable at
that moment (the PoC's undefined helpers made it fail), while LinkPy needs a file anyway
because `la_objsearch` hands the loader a path. Memory: 35 MB RSS for the compile.

What adopting it would mean: the trampolines and the per-struct marshalling would be
generated by the auditor as IR from the DWARF `StructDef`s, i.e. a second code generator
that duplicates, outside C++, what `T_to_PyObject`/`PyObject_to_T` express with `template
for` today; the language-neutral support runtime (GIL, callable resolution, boxing,
namespace objects, error reporting) becomes a prebuilt C library linked by the generated
`.so`; `g++`, `Python.h` and `<meta>` are no longer needed on the deployment machine; the
cache remains useful but matters ~30× less. The reflection marshaller would stay for the
Lua and host-side paths unless those move too. A cheaper middle ground that keeps the
design as is: prebuild `pycpputils.o` and a PCH (above), which halves the shell-out without
replacing it. A further step that needs no compiler at all is noted in section 7.

## 7. Other findings

* **Instruction attribution inside libpython** was not available under `LD_AUDIT`
  (callgrind shows those frames as unnamed), so per-API attribution came from the primitive
  micro-benchmark instead; attribution per trampoline and per marshaller function is exact.
* **No audit trampoline overhead**: `la_objopen`/`la_symbind64` are compiled only under
  `DEBUG`, so PLT calls from the linkstub are not routed through the auditor.
* **A compiler-free design exists**: the C ABI work the JIT does could be done by libffi
  closures (`ffi_prep_closure_loc` supports struct-by-value arguments and returns) bound at
  `la_symbind64` time, with marshalling driven by a descriptor table the auditor emits from
  DWARF. It would remove the compile entirely (cold start in milliseconds) at ~20–50 ns pe
  call for the closure and an interpretive field loop. It replaces the reflection
  marshaller, which is the project's stated design, so it is a direction to weigh, not a
  recommendation.
* **Noise**: the machine ran `ffmpeg` (6 cores) and an IDE indexer for part of the session;
  all comparisons were re-run on a quieter machine (load ≤ 2.5 with the callgrind batch on
  the E-cores) with interleaved rounds and min-of-N; absolute numbers are 10–20% above the
  quiet-afternoon baselines. Unit end-to-end p50 needed pinning and six rounds to rank even
  coarsely (section 4b).

## 8. Test status

* Baseline `ctest` (tree before 20:39): 33/36, the three expected failures
  (`python_ptr_arg`, `cross_py_calls_lua_bump`, `cross_lua_calls_py_bump`).
* Shadow-header fixture checks (all `python_*` fixtures run with the variant header
  shadowing the tree's, `$S/check_fixtures.sh`): v3 11/11, v5 11/11, v6 11/11, v7 fails
  `python_bytes_array` only (contract change, see 3.5), tree header at `-O2` 11/11.
* Final `ctest` of the tree as it is now (after the parallel 20:39 edit, which added eight
  tests): 41/44, the same three expected failures.
* Unit's own suite was not re-run (nothing in Unit or LinkPy was changed); the 48 adapter
  unit tests pass on the current adapter and on the chunk-list copy.

## 9. Results on branch `linkpy-overhead` (2026-11-03)

Branch base: `fd77ce7` (the committed tree; the uncommitted main-tree work of 2026-11-02 is
not on it). Seven commits: two benchmark fixtures (`python_bench_divide`,
`python_bench_structs`), a fixture pinning reload semantics (`python_rebind`), and the
changes of sections 2 and 3 as recommended there, plus the start-up fix of section 5.
Everything was measured against the base built as its own worktree from the same
sources; "tip" is the branch head.

### Instructions per call

callgrind, inclusive, with the one-off interpreter start subtracted; each state's
header JIT-compiled into its own linkstub (the base's cache key does not see a
header-only change, see findings); linkstubs built with the tests' command (no `-O`).
`take` and `give` are the average of their two shapes (64 KiB and 100 B in; 64 KiB and
empty out).

| state | `divide(6,3)` | `take` | `give` | `ask(meta)` |
|---|---|---|---|---|
| base | 10,962 | 219,746 | 114,996 | 88,594 |
| + call-site record (`sys.modules` + interned `getattr` per call) | 3,617 | 212,580 | 107,813 | 81,304 |
| + vectorcall | 3,361 | 212,250 | 107,374 | 81,033 |
| + cached `SimpleNamespace` class, interned field names | 3,332 | 203,113 | 105,373 | 55,911 |
| + `const Args&...`, zero only the array tail | 3,346 | 72,021 | 72,604 | 55,803 |

### Microseconds per call

Quiet machine (load ≈ 1), one P-core, six interleaved rounds, min / median:

| | `divide(6,3)` | `take(64 KiB)` | `take(100 B)` | `give(64 KiB)` | `give(0)` | `ask(meta)` |
|---|---|---|---|---|---|---|
| base | 0.816 / 0.853 | 6.06 / 6.21 | 6.11 / 6.22 | 4.33 / 4.44 | 3.08 / 3.17 | 4.85 / 5.00 |
| tip | 0.211 / 0.220 | 2.65 / 2.79 | 2.69 / 2.80 | 2.73 / 2.80 | 2.41 / 2.47 | 2.47 / 2.52 |

Per task (five rounds, min, taken while the base was being built on the other cores, so
about 10% above the table above): call-site record 0.252 / 6.10 / 6.56 / 4.53 / 3.11 /
4.85; vectorcall 0.244 / 6.60 / 6.59 / 4.54 / 3.16 / 4.83; namespace cache 0.240 / 5.67 /
5.55 / 4.16 / 2.75 / 2.72; by-reference and tail zeroing 0.235 / 2.95 / 2.86 / 2.97 / 2.73 /
2.69.

### Process start

Cache-hit start of a one-call `bench_divide` process: base 293 M instructions (224 M of
them the SHA-256 of the auditor's own 0.85 MB binary, compiled without optimisation),
0.02 s; tip 70 M instructions (47 M the interpreter start, 1.4 M the key), 0.01 s. The
auditor is now built RelWithDebInfo by default and keyed by its GNU build-id; with the
build type alone the key got slower (365 M instructions: the binary grew to 5.6 MB of
debug info and was still hashed by content).

### Unit end to end

`tools/linkpy-bench.py`, 10,000 keep-alive GETs, everything pinned to the P-cores, six
interleaved rounds; base and tip run through wrappers that differ from Unit's
`linkpy-unitd` only in the LinkPy paths (and a `PYTHONPATH` standing in for the
module-beside-libinit feature the base lacks).

| configuration | `empty` p50 min / median (µs) | 64 KiB body p50 min / median (µs) | best req/s | app start (ms) |
|---|---|---|---|---|
| upstream | 46 / 48 | 65 / 68 | 21,300 / 15,300 | 12 |
| LinkPy, base | 60 / 71 | 117 / 142 | 15,500 / 7,900 | 38 |
| LinkPy, tip | 56 / 67 | 114 / 134 | 16,500 / 8,100 | 28 |

Round-to-round spread is ±15 µs, so the end-to-end gain (4 µs on `empty`, 3–8 µs on the
64 KiB body, 10 ms per app start) is at the edge of what this benchmark resolves; it is
consistent with the harness of section 3 (the marshaller's share of the `empty`
request 19 → 12 µs) once the adapter's own Python, untouched here, is counted: on the
64 KiB body it is 24 of the remaining 53 µs (section 4), and the by-value design's own
copies another part.

### Tests

`ctest` 34/34 after every task, on a wiped linkstub cache (see findings). Unit's suite
through the branch wrapper: 54 passed, 13 skipped, and the same two failures as the
base through the same wrapper — `test_python_isolation_rootfs` (fails on the main tree
too, out of scope per `LINKPY_NOTES.md`) and `test_python_application_path` (its
`environment` override drops the wrapper's `PYTHONPATH`, and the base lacks the
module-beside-libinit feature that makes it unnecessary). Adapter tests 48/48.

### Review fix pass

The whole-branch review found no critical issue and three to fix before merging, each
now with a fixture that failed first: the suite could not see a marshaller edit on a
warm cache (the tests' build command now carries a hash of the marshaller sources,
`-DLINKPY_MARSHALLER=<sha>`, so the key changes with them); the rewritten templates
leaked the module and function references on every throw path (`python_marshaller_refs`,
a C++ host catching the exception: reference counts 10 → 110 over 50 raising calls before,
unchanged after); and padded by-value results carried stack garbage in their padding
where the toolchain's zero-initialisation used to clear it (`python_padded`;
`has_padding<T>()` clears such results first, padding-free ones are still written
once). The suite is 36 tests.

### Rejected, unchanged from sections 2 and 3

Keeping the callable across calls (0.05–0.1 µs, breaks `importlib.reload`; pinned by
`python_rebind`); buffer-protocol acceptance (no gain by itself); the used-length
convention (changes the `python_bytes_array` contract for 1.2 µs on small uploads).

### Findings for the hand-off

- On this base the linkstub cache key covers the auditor (now by build-id), templates,
  DWARF and build command, but not the marshaller header, so a header-only change is
  served a stale linkstub; the tests on the branch wipe the cache. The uncommitted
  main-tree change already hashes the `-I` directories and sources named in the build
  command; merging it (same function, `linkstub_cache_material`) resolves this.
- `heaphints` finds no escaping pointer parameters in DWARF compiled at `-O2`; the
  fixtures are pinned to `-O0`, but a user building the stub library optimised would
  get an empty hint list.
- The Lua marshaller still takes its arguments by value (the same extra copies the
  Python one had); out of scope here.
- The largest remaining item on Unit's 64 KiB paths is the adapter's own copying
  (section 4), which needs nothing from the marshaller.
