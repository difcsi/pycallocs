# liballocs private-heap corruption under Node/V8 (open bug)

**Status:** diagnosed, not fixed. Deterministic reproduction available.
**Impact:** intermittent SIGSEGV/SIGABRT of processes running under
`liballocs_preload.so`; in practice ~1 test per few `tests/js` (v8proxy)
suite runs, occasionally other small processes. Python and Lua suites are
unaffected at their current allocation traffic.
**Where:** liballocs runtime, private `dlmalloc` heap — *not* the v8proxy
extension (ruled out by core autopsy, below).
**Diagnosed:** 2026-08-13, while bringing up `vmextensions/v8` (Node addon).

---

## 1. Symptoms

- A process under the liballocs preload dies with SIGSEGV (exit 139) or
  SIGABRT (exit 134), at an apparently arbitrary point — sometimes before
  the script produces any output, sometimes mid-run, sometimes at exit.
- Failure moves between tests across runs; any single test passes when
  retried. Under default ASLR the rate is roughly one failure per few
  full-suite runs of `tests/vmextensions/v8/run_tests.sh`.
- Often (not always) preceded on stderr by many
  `Warning: mapping of <file> could not extend preceding bigalloc`
  messages, and by the private heap visibly growing in large steps
  (`liballocs_preload.so\`dlmalloc` mmaps of ~0x23fe000 bytes).

## 2. Core-dump autopsy (the smoking gun)

Captured with `kernel.core_pattern=/var/crash/core.%e.%p.%t`,
`coredump_filter=0x3`, `ulimit -c 4000000` (bounded — do **not** use
unlimited; piped handlers like apport buffer cores in RAM).
Core: `/var/crash/core.MainThread.176844.1786631019` (878 MB, from a
`tests/js/struct_global` run at `MAPPING_SEQUENCE_MAX_LEN=32`).

Backtrace (node v24.2.0, preload built from
`/home/zoltan/Develop/stackscan/contrib/liballocs` @ `e78a96d` + local
fixes):

```
#0 dlmalloc (bytes=<opt>) at contrib/libsystrap/contrib/dlmalloc.c:4639
#1 __wrap_dlmalloc at src/dlmalloc-ext.c:194
#2 __alloc_file_metadata (nsegs=<opt>) at src/allocators/static-file.c:165
#3 __runt_files_notify_load at librunt/src/files.c:328
#4 __static_file_allocator_notify_load at src/allocators/static-file.c:489
#5 dlopen (filename=".../tests/libs/composite/composite.so", flag=258)
   at librunt/src/preload.c:143
#6 LibraryLoader_Load at vmextensions/v8/library_loader.cc:191
```

The faulting site is dlmalloc's **designated-victim split** path
(`if (nb <= gm->dvsize) { size_t rsize = gm->dvsize - nb; ... }`).
Registers at the fault:

```
rax 0x71000000e8b0   (chunk ptr, private heap)
rcx 0x71000000eb90   (p + nb)
rdx 0x620  rsi 0x621 (request ~1568B: file metadata for composite.so)
rdi 0xfffffffffffffd20   <-- rsize = dvsize - nb = -736
```

`rsize` is **negative**: `gm->dvsize` was smaller than `nb` even though the
`nb <= gm->dvsize` branch was taken — i.e. the **dv/dvsize bookkeeping was
already corrupted** before this (entirely routine) allocation. The crash
site is a victim, not the culprit: something earlier scribbles over
dlmalloc's chunk metadata inside the private heap.

## 3. The capacity correlation (what the culprit tracks)

`struct mapping_sequence` has a fixed-capacity `mappings[]` array
(`MAPPING_SEQUENCE_MAX_LEN`, `src/liballocs_private.h`). Node needed it
raised from 8 (node's executable alone maps >8 VMAs; at 8 node cannot even
boot — see §6). Varying it, with a **clean** `src/` rebuild each time:

| MAX_LEN | behaviour under node |
|---|---|
| 8  | node unbootable ("no more room in sequence structure (2)" then SIGSEGV) |
| 16 | corruption crash **4/4 = deterministic** with the pinned repro (§4) |
| 32 | corruption crash intermittent (~1 test per few suite runs) — current setting |

The crash rate tracks how often sequences **hit capacity** — every overflow
emits `could not extend preceding bigalloc` and takes the
sequence-overflow/fresh-bigalloc path in `src/allocators/mmap.c`
(`__augment_mapping_sequence` returning false; see `not_too_many` around
mmap.c:1181 and the reason strings at mmap.c:1328/1392). Prime suspect:
that overflow path (or a caller that ignores its failure) corrupts private
heap memory — plausibly a write into a full `mappings[]` array or into a
freed/stale `mapping_sequence`, since those objects live in exactly the
private dlmalloc heap that ends up corrupted. V8 produces the trigger
traffic: thousands of scattered `OS::Allocate` mmaps/munmaps at randomized
addresses, plus meta-DSO `get_or_map_file_range` mappings that repeatedly
overflow their sequences.

## 4. Deterministic reproduction

The failure is address-layout- and traffic-dependent; pinning both makes it
deterministic at `MAX_LEN=16`:

```sh
# 1. in /home/zoltan/Develop/stackscan/contrib/liballocs:
#    set MAPPING_SEQUENCE_MAX_LEN to 16 in src/liballocs_private.h, then
. contrib/env.sh
LIFETIME_POLICIES=1 make -C src clean && LIFETIME_POLICIES=1 make -C src

# 2. in /home/zoltan/Develop/pycallocs:
LIBALLOCS=$PWD/contrib/liballocs
env LD_PRELOAD="$LIBALLOCS/lib/liballocs_preload.so:$PWD/build/basetypes/libbasetypes_provider.so" \
    LD_LIBRARY_PATH="$LIBALLOCS/lib" \
    GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288 \
    META_BASE=$PWD/build/tests/meta \
    PYCALLOCS_TEST_LIBS=$PWD/build/tests/libs \
    V8PROXY_MODULE=$PWD/vmextensions/v8/build/v8proxy.node \
    setarch -R node --expose-gc --random-seed=42 \
    tests/js/struct_global/struct_global.js
# -> SIGSEGV, 4/4 in testing
```

Notes:
- `setarch -R` (no kernel ASLR) alone does NOT change the rate — V8 places
  its reservations with its own RNG; `--random-seed=42` pins that too.
- Under gdb the crash does not reproduce (layout perturbation masks it);
  use core dumps (§2 settings) or `rr` with the deterministic recipe.
- At `MAX_LEN=32` the same recipe crashes ~2/3 of the time on
  `struct_global`; other tests fail when the whole suite runs.

## 5. What was ruled out

- **v8proxy itself**: the faulting stack is pure liballocs (dlopen →
  file-metadata alloc → dlmalloc); the extension frame is just the caller.
  The Lua port (same proxy architecture, same fixtures) never crashes this
  way at its traffic level.
- **Kernel ASLR**: `setarch -R` alone did not change the failure rate.
- **Stale objects from incremental rebuilds**: earlier in this effort,
  incremental `make -C src` after changing `liballocs_private.h` produced
  object files disagreeing about `sizeof(struct mapping_sequence)` and
  caused an unrelated-looking intermittent crash in arbitrary processes.
  All measurements above use clean rebuilds. (Gotcha worth keeping: several
  `src/` objects appear to lack a header dependency on
  `liballocs_private.h`.)

## 6. Context: the three liballocs changes that made node bootable

Applied to `/home/zoltan/Develop/stackscan/contrib/liballocs` during v8proxy
bring-up (all needed *before* the corruption became observable):

1. **`tools/stubgen.h`** — a comment inserted by commit `e78a96d` ("alaska
   fixes") lacked a line-continuation backslash, terminating
   `ALLOC_EVENT_INDEXING_DEFS4` mid-body. Broke *all* compilation of
   allocation stubs: allocscc fixture builds AND the runtime's own
   `default-lib-malloc.c`. One-character fix (add the `\`).
2. **`src/allocators/static-file.c`** (`plug_hole_or_abort`) — node's
   executable maps into two bigallocs that abut **exactly**; the
   hole-plugging path then called `mmap(len=0)` → EINVAL → abort. Now a
   zero-size hole skips the plug and coalesces directly.
3. **`src/liballocs_private.h`** — `MAPPING_SEQUENCE_MAX_LEN` 8 → 32
   (node's executable maps more than 8 VMAs).

(2) and (3) are workarounds that widen capacity/coverage; the *proper* fix
for both is probably whatever also fixes §3 — sequences that can grow, or
an overflow path that is actually sound.

## 7. Suggested next steps

- Audit the `__augment_mapping_sequence` failure path and every caller that
  can see `not_too_many == false` (mmap.c ~1181, ~1328, ~1392): what
  happens to the bigalloc/sequence invariants and to the *private-heap
  allocations* involved when a sequence is full? The zero-hole coalesce in
  `static-file.c` also augments in a loop with (compiled-out) asserts on
  `did_augment` — a silent drop there leaves `lower_seq->end` inconsistent.
- With the deterministic `MAX_LEN=16` repro, record under `rr`, then
  hardware-watchpoint `gm->dv`/`gm->dvsize` (or the clobbered chunk header)
  and reverse-continue to the scribbling store.
- Alternatively build the private dlmalloc with `DEBUG`/`FOOTERS`
  (chunk-integrity checks) to move the failure to the corrupting free/write
  instead of the later allocation.
- Consider making `mapping_sequence` growable (the FIXME at its definition
  already suggests a flexible array member) — V8-class workloads will keep
  finding whatever ceiling is chosen.

## 8. Artifacts

- Core: `/var/crash/core.MainThread.176844.1786631019` (MAX_LEN=32 build)
- Repro stderr logs: `/tmp/sg_*.err`, `/tmp/sgs_fail.stderr` (volatile)
- Core capture settings used: `kernel.core_pattern=/var/crash/core.%e.%p.%t`,
  `coredump_filter=0x3`, `ulimit -c 4000000`
