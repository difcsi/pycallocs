í# pycallocs: CMake migration + liballocs/alaska relocation — handoff notes

_Last updated: 2026-06-08_

This document records the state of the autotools→CMake migration, the submodule
relocation, and the one **open runtime blocker** so the work can be resumed
cleanly.

## What was requested

1. liballocs and alaska should be submodules of **stackscan**, not of pycallocs;
   pycallocs build references repointed accordingly.
2. Move pycallocs from autotools to **CMake** (treat liballocs/alaska as
   pre-built deps).
3. Port the tests (`tests/Makefile`, `tests/libs/Makefile`) to **CTest**.
4. Build the bundled CPython, then stackscan, then pycallocs.
5. Use `~/Develop/stackscan` as the stackscan folder (symlinked in).
6. Branch constraints: **alaska = `main-rc`**, **liballocs = `lp2`**.
7. Port `linkpy` to CMake.
8. The extension itself is built with **plain gcc** — only the test library code
   it loads is built with `allocscc`→alaska.
9. Make the base-types provider a permanent build artifact.
10. Update `minicrunch` to the current (lp2) liballocs API.
11. Include `lifetime_policies.c` in the liballocs build — **`lifetime_policies.o`
    must stay** (pycallocs needs the symbols).

## Status: DONE

### Build system
- `CMakeLists.txt` (top-level) replaces autotools. Locates liballocs/alaska under
  `STACKSCAN_DIR` (default `contrib/stackscan/contrib/...`). Options:
  `USE_ALLOCSCC` (OFF), `USE_ALASKA` (ON), `DEBUG`. `PYTHON` is a STRING cache var
  (not FILEPATH — so `"python3"` stays resolvable). Builds the extension via
  `pip install -e . --no-build-isolation`. Targets: `build-python`,
  `basetypes-provider`, `check`.
- `setup.py` is env-driven (`LIBALLOCS`, `USE_ALLOCSCC`, `DEBUG`). CIL float
  defines are conditional on `use_allocscc` (alaska's wrapper re-splits args on
  spaces, which breaks `-D_Float128=long double`). Sources include `minicrunch.c`.
- `contrib/Makefile` exports `LIBALLOCS`/`ALASKA` from `$(STACKSCAN)/contrib/...`;
  cpython configured with `CFLAGS=-fPIC` (so `libpython3.15.a` embeds into
  linkpy's `audit.so`); `build-linkpy` uses CMake.
- `contrib/linkpy/CMakeLists.txt` — C++23, queries PYTHON via sysconfig, builds
  `pystubgen` + `audit` (`.so`) into `dist/`.
- `.github/workflows/buildantest.yaml`, `.gitignore` updated for CMake.
- `contrib/stackscan/.gitmodules`: liballocs branch → `lp2`.

### Base-types provider (permanent artifact)
- `basetypes/` (`basetypes.c`, `aliases.inc.c`, `gen_provider.sh`) → built into
  `build/basetypes/libbasetypes_provider.so` (target `basetypes-provider`).
  Preloaded after `liballocs_preload.so` in the test harness.
- **Why it exists:** the extension imports spelled-out base-type uniqtype symbols
  (`__uniqtype__short_int$16`, `float32`, `double64`) while dwarftypes emits
  canonicalized names (`int$16`, `float$32`, `float$64`). `aliases.inc.c` is a
  `.set` alias block bridging the two. The aliases **must be appended to the
  generated `dwarftypes.c` TU** — GAS `.set alias,target` only works when target
  is defined in the same object. Resolves all 11 imported base-type uniqtypes.
- `basetypes.c` is compiled `-gdwarf-4` because dwarftypes aborts on the DWARF-5
  system libc.

### liballocs (in `~/Develop/stackscan/contrib/liballocs`, branch `lp2`)
- `src/Makefile`: `LIFETIME_EXT_OBJ` was computed but never linked. Added
  `lifetime_policies.o` to `CORE_OBJS` (before the object lists expand), guarded
  by `ifneq ($(LIFETIME_POLICIES),)`. Top Makefile.am does
  `export LIFETIME_POLICIES = 1`.
- `src/lifetime_policies.c`: added `#include <stdio.h>` (latent bug; it had never
  been compiled before so its `FILE`/`fprintf` use via `liballocs_private.h` never
  surfaced).
- Result: `liballocs_preload.so` now exports `__liballocs_attach_lifetime_policy`,
  `__liballocs_detach_lifetime_policy`, `__liballocs_register_gc_policy`.

### minicrunch (lp2 API update) — `minicrunch.c`, `include/minicrunch.h`
- `__liballocs_search_subobjects_spanning` → `__liballocs_walk_subobjects_spanning`
  with the new callback signature: explicit `containing` /
  `containing_span_start_offset` params instead of a `uniqtype_containment_ctxt`
  list.
- Removed `cache_containment_facts`, `cache_bounds`, `cache_fake_bounds` (gone in
  lp2).
- `pointer_to___uniqtype__*` → `&__uniqtype__*`.
- Dropped `#include "liballocs_private.h"` — its `debug_printf` macro relies on
  hidden-visibility helpers (`get_exe_command_basename`, only defined under
  `IN_LIBRUNT_DSO`) that aren't usable from a separate DSO. Uses
  `fprintf(stderr, ...)` for the rare diagnostic paths.

### Link state: FULLY GREEN
The extension compiles and **all symbols resolve**:
- base-type uniqtypes ← basetypes provider
- `__liballocs_{attach,detach}_lifetime_policy`, `__liballocs_register_gc_policy`
  ← rebuilt liballocs preload
- `aborted_unknown_storage`, `err_object_of_unknown_storage` ← dummyweaks

## OPEN BLOCKER: runtime heap corruption (EXIT 139)

`import allocs` now reaches **runtime** and segfaults. So does plain
`python -c "print()"` under the preload — i.e. this is **not** specific to the
extension.

### Symptom / backtrace
```
SIGSEGV / heap corruption in glibc:
  _int_free_merge_chunk / _int_free_create_chunk
    (corrupted chunk size ~131088, nextsize ~65528)
  <- hook_free                (contrib/libmallochooks/src/hook2event.c:96)
  <- _PyObject_Free           (CPython obmalloc)
  <- _io__Buffered_close_impl (during Python startup/shutdown)
```

### Key facts
- **Regression correlates exactly with linking `lifetime_policies.o`.** Before it
  was linked, plain `python -c "print()"` ran fine under the preload ("PLAIN OK").
- `PYTHONMALLOC=malloc` does **not** avoid it.
- The crash is in the **free path** of liballocs's malloc hooks, on
  *uninstrumented* CPython allocations.
- User constraint: **`lifetime_policies.o` must stay.** Fix must be in the
  lifetime-policy / free-path interaction, not removal.

### Working hypothesis
Activating `lifetime_policies.o` changes behaviour of the generic-malloc free
path. The most suspicious mechanism is the **per-chunk lifetime insert**: the
lifetime machinery reads/writes a `INSERT_TYPE` trailer
(`lifetime_insert_for_chunk(allocstart, a->get_size)` in
`get_lifetime_insert_info`, `lifetime_policies.c:50-69`) and `__notify_free`
(`lifetime_policies.c:257`) runs in the free path. For CPython's obmalloc arenas
(which are *not* individual malloc chunks the way liballocs expects), computing an
insert location from `a->get_size` likely lands **outside / mid-chunk**, so the
write corrupts an adjacent glibc chunk header → the `_int_free` abort.

In short: the lifetime free-path assumes every freed pointer is a liballocs-known
chunk with room for an `INSERT_TYPE` trailer; obmalloc pool/arena frees violate
that assumption.

### Where to look next (resume here)
1. `contrib/stackscan/contrib/liballocs/contrib/libmallochooks/src/hook2event.c:96`
   — what `hook_free` does before calling the real free; whether the
   lifetime/`__notify_free` path runs unconditionally.
2. `src/lifetime_policies.c`:
   - `get_lifetime_insert_info` (`:50`) — does `ALLOCATOR_HANDLE_LIFETIME_INSERT(a)`
     correctly *exclude* obmalloc/generic allocations that have no insert trailer?
   - `__notify_free` (`:257`) / `notify_copy_for_type` — guard against types/sizes
     that don't correspond to a real instrumented chunk.
3. `INSERT_TYPE` / `lifetime_insert_for_chunk` definition in
   `src/liballocs_private.h` — confirm trailer layout & where it's placed relative
   to the chunk, and whether `a->get_size` returns a usable size for obmalloc.
4. Reproduce minimally: `LD_PRELOAD=<preload>:<provider> python -c "print()"`
   under gdb; set a hardware watchpoint on the corrupted chunk header to catch the
   **first** bad write (confirm it originates in the lifetime insert path).

This is a liballocs runtime bug exposed by the (requested) linking of
`lifetime_policies.o`; the fix belongs in liballocs's free/lifetime path.
