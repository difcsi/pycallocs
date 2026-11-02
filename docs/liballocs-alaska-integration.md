# liballocs ↔ Alaska integration: translate-then-index

Goal: make liballocs answer metadata queries (`alloc_get_base/size/type`) for
objects allocated by Alaska's `halloc`. The query-side glue is the header-only
`vmextensions/common/handle_query.h`. The allocator-side support lives in
liballocs and Alaska. (This note was written for the retired stackscan repo,
where the glue originated; the CTests it cites, e.g. `tests/halloc_type`, live
there.)

## The obstacle

Alaska's `halloc()` returns a **handle**, not a backing address: a non-canonical
pointer with **bit 63 set** (`(long)p < 0`). The real bytes live elsewhere; the
compiler inserts a translation at every handle dereference. liballocs, in
contrast, indexes every allocation by its **real address** in a pageindex /
bigalloc tree that only spans canonical lower-half addresses. Worse, liballocs
**deliberately leaves Alaska's heap mmap un-indexed** (its mmap allocator skips
`libalaska`-made mappings to avoid an init-time re-entrancy loop), so even the
backing address is unknown to it.

## The solution: translate-then-index + an Alaska allocator in liballocs

Two cooperating halves:

### 1. Translate (query side) — `src/handle_query.c`

Before any address-indexed liballocs query, translate a handle to its backing
pointer with `alaska_translate()` (a no-op on non-handles, so the wrappers are
safe on arbitrary pointers). This removes the top-bit pageindex SIGSEGV and
points the query at the right memory. See `ss_alloc_get_base/size/type` /
`ss_translate`. liballocs only ever sees raw backing pointers — it never learns
about handles.

### 2. Index (allocator side) — a handle-agnostic Alaska allocator in liballocs

`contrib/liballocs/src/allocators/alaska.c` adds `__alaska_heap_allocator`, which
owns Alaska's backing heap and answers `get_info` for backing pointers:

- **Lazy region claim.** Since Alaska's heap mmap is un-indexed, the first query
  for a backing address misses and reaches
  `__alaska_allocator_notify_unindexed_address` (wired into
  `__liballocs_notify_unindexed_address`). It claims the containing **2 MiB
  Alaska page** as a bigalloc owned by `__alaska_heap_allocator`, and the query
  retries and routes to us. Claiming from liballocs' own query path — not
  Alaska's runtime — avoids the re-entrancy the mmap allocator guards against; a
  thread-local guard short-circuits nested entries while we talk to Alaska.
- **base** comes from Alaska's page table via raw-pointer helpers exported by the
  runtime (`contrib/alaska/runtime/rt/liballocs_export.cpp`): `alaska_object_base`,
  `alaska_heap_page_extent` (plus `SizedPage::object_base`).
- **size** is the caller's **exact requested size**, so heap-array lengths are
  exact (mirroring the generic-malloc padding mechanism rather than reporting the
  rounded-up size class). Alaska's `halloc` passes the requested size to the weak
  hook; `get_info` returns it, falling back to `alaska_object_size` (the size
  class) only for un-recorded objects.
- **type** comes from the **recorded allocsite**. allocscc already stashes each
  halloc call site in liballocs' `__current_allocsite` TLS; Alaska's `halloc`/
  `hfree` call weak hooks (`__liballocs_notify_alaska_alloc/free`) that bind/
  unbind the site *and the requested size* to the object's backing base in a side
  table. `get_info` then resolves the site to a `uniqtype` through the ordinary
  `__liballocs_find_allocsite_entry_at` machinery and the per-binary meta-DSO.

The allocator is **not cacheable**: Alaska's GC compacts/relocates objects, so a
cached base/size could go stale.

## Current status (`tests/halloc_type`)

```text
halloc=0x… is_handle=1 backing=0x…
liballocs: allocator=alaska base=0x… size=24 type=0x… name=__ARR_point
RESULT: liballocs answered base/size/type for the halloc'd object
```

The query is handled by the **`alaska`** allocator (`__alaska_heap_allocator` in
`src/allocators/alaska.c`), not the generic-malloc one -- both tests assert this
via `ss_alloc_get_allocator(h)->name == "alaska"`.

i.e. liballocs reports the base, the exact requested size (24 for a `struct
point`, not Alaska's 32-byte size class) and the type. The outermost type of a
heap allocation is liballocs' array-of-element type (`__ARR_point`), exactly as
for a plain `malloc(sizeof(struct point))`. `halloc_type` drills into the
returned uniqtype and asserts the full `struct point` layout (3 members x/y/z at
offsets 0/8/16, each a `long`). The test passes; it used to be registered
`WILL_FAIL` while typing was unimplemented.

`tests/malloc_type` is the same check for the **transparent path**: the program
calls plain `malloc`/`free` and never names Alaska; the Alaska compiler pass
rewrites malloc→halloc, and liballocs still recovers the exact type. The catch is
allocsite metadata: allocscc types the *source* `malloc` site, but the binary
calls `halloc`, so the allocsite matcher must also recognise the rewritten call.
`LIBALLOCS_ALLOC_FNS` is *additive* to allocscc's defaults (malloc/calloc/realloc/
memalign), so registering just the **halloc** family (as `halloc_type` does)
covers both: defaults type the source malloc, and the added halloc matches and
wraps the rewritten call (sets `__current_allocsite`). Listing malloc again would
duplicate its `__wrap_malloc` stub and fail to compile.

## Files

| Where | What |
| --- | --- |
| `vmextensions/common/handle_query.h` | translate-then-index query glue (`ss_alloc_get_base/size/type/allocator`) |
| `contrib/liballocs/src/allocators/alaska.c` | `__alaska_heap_allocator`: lazy claim, `get_info`, allocsite side table |
| `contrib/liballocs/src/pageindex.c` | dispatches `notify_unindexed_address` to the Alaska allocator |
| `contrib/alaska/runtime/rt/liballocs_export.cpp` | raw-pointer helpers (`alaska_object_base/size`, `alaska_heap_page_extent`) |
| `contrib/alaska/runtime/.../SizedPage,HeapPage` | `object_base(ptr)` for interior backing pointers |
| `contrib/alaska/runtime/rt/halloc.cpp` | weak `__liballocs_notify_alaska_alloc/free` calls to bind the allocsite |

## Toolchain fix made here

`allocscc` → Alaska compilation was failing for everyone with
`clang-16: error: cannot specify -o when generating multiple output files`.
Root cause: `contrib/alaska/compiler/bin/alaska` stripped `alaska_translate.bc`
from the forwarded `alaska-config` flags with an **exact path** match, but the
prefix it computes via `realpath` differs from the symlinked path
`alaska-config` emits (`pycallocs/contrib/stackscan/…` vs the resolved
`/home/.../stackscan/…`). The `.bc` then leaked into the `clang -c` step as a
second input. Fixed by matching on basename (`${flag##*/}`), which is robust to
path aliasing.

## Known limitations / follow-ups

- **Huge objects** (Alaska's separate-mmap allocations) are not covered: only the
  sized heap is page-claimed. `get_info` returns "unindexed" for them.
- **Relocation:** the per-object side table is keyed by backing base; GC
  compaction that moves an object leaves a stale entry. base stays correct
  (page-table-derived, uncached) and size falls back to the (still-correct) size
  class, but the exact size and type degrade for moved objects. Re-binding on move
  is a possible follow-up.
- The side table is a fixed-size open-addressing map; on overflow it drops new
  records (type degrades to NULL and size to the rounded-up size class), it never
  faults.
