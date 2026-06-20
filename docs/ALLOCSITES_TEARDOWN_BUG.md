# `allocsites` meta-generation crash — root cause, fix, and workaround

While bringing up the pycallocs test fixtures' meta-DSOs, the liballocstool
`allocsites` tool (driven by `tools/Makefile.meta`) crashed on `arrays.so`,
leaving `*-allocsites.c.err` behind and failing the whole meta build. This
note records what it actually was, since the obvious hypotheses were wrong.

All patched files live in the `stackscan` submodule:
`contrib/stackscan/contrib/liballocs/...`.

## TL;DR

There were **two independent heap-corruption bugs** in the meta path, plus a
disproven hypothesis:

1. **NOT a DWARF-5 problem.** `arrays.so` (and `alphanum.so`) are `-gdwarf-4`.
   The version hypothesis is out.
2. **Bug #1 (FIXED) — stale typename in `read_allocs_line`.** A real logic bug
   that caused the same synthetic type to be created ~18× into one CU and
   corrupted the heap mid-run.
3. **Bug #2 (WORKED AROUND) — upstream libdwarf/libdwarfpp DIE-teardown
   overflow.** A pre-existing heap buffer overflow, surfaced once bug #1 was
   fixed and the tool ran to completion. Independent of liballocs, lifetime
   policies, and `LD_AUDIT`.

## Bug #1 — stale `alloc_typename` (fixed)

`liballocstool/src/allocsites-info.cpp`, `read_allocs_line()`:

When a `.allocs` line has a typename it does `alloc_typename = ...`; the
no-typename branch did **nothing**. The caller `read_allocsites()` declares
`alloc_typename`/`might_be_array` *once*, outside its read loop, so a typeless
line silently inherited the previous line's typename.

`arrays.so.allocs` lists the one synthetic-typed site (`make_full`'s
open-struct-plus-array `malloc`) followed by ~18 typeless entries (PLT stubs,
`_init`, `__wrap_*`). All 18 inherited the synthetic typename and were flagged
`is_synthetic`, so `dwarfidl::create_dies()` rebuilt the same composite type
~18× into a single CU → `corrupted size vs. prev_size while consolidating`
*during processing*.

**Fix:** clear the out-params in the no-typename branch (per-line statelessness,
which is the correct contract — only `read_allocsites` calls it). After the fix
the synthetic type is created exactly once and the emitted table is correct
(4 entries for arrays.c's 4 malloc sites).

## Bug #2 — DIE-teardown heap overflow (worked around)

With bug #1 fixed the tool runs to completion and emits the **complete, correct,
byte-for-byte deterministic** allocsites table, then crashes **at exit** during
C++/atexit teardown:

```
~vector<allocsite>  →  ~allocsite  →  ~iterator_df<type_die>
  →  intrusive_ptr_release  →  structure_type_die::~structure_type_die
  →  Die::deleter::operator()  →  dwarf_dealloc(dbg, die, DW_DLA_DIE)
  →  _int_free_merge_chunk  →  "corrupted size vs. prev_size while consolidating"
```

Characterisation:

- **Trigger:** an allocsite whose type resolves to a **struct with member
  children** (`open_struct`, `named`). A base type (`int`, no children) is fine.
  `alphanum.so` only *appeared* to pass because `alphanum.c` has **no `malloc`
  at all** → zero typed allocsites → the struct-DIE path is never exercised.
  This bug therefore blocks any fixture with a struct-typed allocation
  (basic, composite, nested_struct, bintree, inheritance, …).
- **Class:** heap **buffer overflow**, not a double-free — `MALLOC_CHECK_=3`
  still reports the consolidate error rather than "double free detected". The
  bad write happens earlier; it is only detected when the adjacent chunk is
  freed during teardown.
- **Location:** the bundled libdwarf is statically linked into `libdwarfpp.so`
  (no separate `libdwarf.so`), so this is a single coherent build — not an
  ABI/version mismatch. The overflow is in the libdwarf/libdwarfpp DIE
  create/use/free machinery (upstream).

### Relationship to our recent changes and to `LD_AUDIT` — NONE (proven)

`allocsites` is a build-time DWARF tool. For the pycallocs meta build it runs
**standalone**: `tools/Makefile.meta` invokes `$(ALLOCSITES) < ….allocs`
directly. Verified on this machine:

- `ldd tools/.libs/allocsites` — **no liballocs**; `nm -D` shows **no**
  `__notify_free` / lifetime-policy / malloc-hook symbols.
- No `/etc/ld.so.preload`; `LD_PRELOAD`/`LD_AUDIT` unset.
- It crashes with **neither** preload **nor** auditor (exit 139), and also with
  liballocs preload only / preload+auditor (exit 134 — the preload's hooked
  malloc just reports the same corruption differently).

So bug #2 is independent of the lifetime-policies off-by-8 fix
(`generic_malloc_index.h`), the `LD_AUDIT` TLS initial-exec fix
(`terminal-indirect-dlsym.c`), and linkpy's auditor — none of that code is
even mapped into the process.

The "**`allocsites` works if lifetime policies are off** (in liballocs' own
test suite)" observation is a *different execution context*: liballocs' own
tests run the meta tools **under the liballocs preload**. With lifetime policies
**on**, every `free` (including the teardown `dwarf_dealloc` frees) runs
`pre_nonnull_free → __notify_free → get_info`; with them **off**, `__notify_free`
is an empty weak stub. That changes whether/where the latent overflow trips —
but it is not the standalone cause. Skipping teardown fixes the standalone case
regardless, which is consistent with bug #2 being purely an upstream teardown
overflow.

### Workaround

`liballocs/tools/allocsites.cpp`, end of `main()`: the allocsites table is fully
written to stdout *before* any teardown. `allocsites` is a short-lived batch
process, so we flush and `std::_Exit(EXIT_SUCCESS)`, skipping the C++/atexit
teardown (the OS reclaims memory on exit). This side-steps the upstream overflow
without affecting output. Verified: full `arrays.so.allocs` → exit 0, identical
4-entry table; `Makefile.meta` produces `arrays.so-allocsites.c` (not `.err`).

### Proper fix (upstream, not yet done)

Root-cause the overflow in libdwarf/libdwarfpp DIE deallocation. Reproduce
standalone with a one-line input:

```
grep __uniqtype__open_struct /usr/lib/meta/…/arrays.so.allocs \
  | env -u LD_PRELOAD -u LD_AUDIT tools/.libs/allocsites >/dev/null
```

then rebuild `libdwarfpp` (+ bundled `libdwarf`) with `-fsanitize=address` to
pin the offending write. Until then the teardown-skip stands.
