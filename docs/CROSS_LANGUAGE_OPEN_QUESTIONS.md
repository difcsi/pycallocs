# Cross-language calls: open questions

Status as of 2026-08-12. The linker (LinkPy) supports Python↔Lua calls through
JIT host bindings, with the user's C header as the by-value ABI pivot, and —
behind `LINKPY_CROSS_BYREF=1` — by-reference pointer parameters that cross as
proxy handoffs (one live C object proxied by both VMs). All 29 linker tests
pass, including both `cross_*_bump` by-ref fixtures. What follows are the
questions deliberately left open, grouped by how much design they need.

## Semantics

### 1. Pointer returns — who owns the object?

A cross-language signature returning a pointer (`char *` included) is rejected
today. Admitting it needs an ownership policy: if the callee VM allocates the
object, its lifetime is tied to that VM's GC, and the host receives... what?
A proxy that pins the callee-side object (attach a second lifetime policy on
handoff)? A copy? Options to weigh:

- **Proxy handoff, symmetric with parameters**: the returned address is
  re-proxied by the host; both VMs' lifetime policies attach. Requires the
  callee to return liballocs-typed memory (a callee-side `malloc` inside VM
  code is typed only if it went through a typed allocator — a Lua table or
  Python object is NOT addressable C memory, so this only works for objects
  the callee itself received or minted from C).
- **Reject forever, provide out-params instead**: the caller allocates
  (`points.new_point`-style), the callee mutates by reference. This is what
  works today and may simply be the right shape.

### 2. Struct fields that are pointers

By-value structs cross field-by-field via reflection; a pointer *field*
currently hits the marshallers' "unsupported type" branch at runtime, not the
auditor guard. Should the guard (`cross_reject_reason`) also walk the
DWARF-recovered struct definitions and reject pointer members up front? And
under `LINKPY_CROSS_BYREF`, should a pointer field become a nested proxy
handoff (consistent but subtle) or stay rejected?

### 3. Type identity across the boundary is not validated

The host wrapper extracts a raw address from *any* proxy; the callee re-proxies
whatever liballocs says lives there. Passing a proxy over a `struct Dog` to a
`struct Point *` parameter is not caught — the callee just sees a Dog. Is
liballocs-as-single-source-of-truth acceptable (current stance), or should the
host wrapper compare the proxy's uniqtype name against the header's parameter
type and raise on mismatch? (Needs a `type_name_of(proxy)` API on both
extensions.)

### 4. Same-language host+callee stays rejected

`python`-host + `python`-callee is rejected because the linkstub would alias
two interpreters through ELF interposition. If "call an isolated second
interpreter of the same language" ever becomes a real want (sandboxing,
version bridging), it needs a namespace/`dlmopen`-style design of its own.

## Allocation & lifetime

### 5. Can a VM host mint typed structs straight from the cross-language header?

Today a by-ref object must originate from an allocscc-compiled C library
(`clib/points.c` → `points.so` → `elflib`/`allocs.load`). The header's
`struct Point` itself has no uniqtype on the host side. The "full story"
option: generate a **liballocs meta-DSO for the JIT-compiled linkstub at audit
time** (its DWARF already carries every struct), set `META_BASE`, and extend
the loaders/helpers so `linkpy.type("Point")(3, 4)` mints a typed instance
with no helper library. Open sub-questions: is liballocs' meta toolchain
(Makefile.meta) robust when run from inside the auditor against a DSO in a
mkdtemp dir? Does the runtime compile budget tolerate it? (This is the
historically fragile meta-DSO/allocsite layer — spike before committing.)

### 6. Lifetime coordination between two GC-attached proxies

Both VMs attach liballocs lifetime policies to the same chunk (policy ids 1
and 2; the bitmask supports it and the bump fixtures prove coexistence for
reads/writes). Not yet exercised or designed:

- **Free/unreachability races**: host drops its proxy and its GC detaches
  while the callee still holds one — the policy bitmask should keep the chunk
  alive, but no test proves the interleavings (esp. Lua's leak-by-design
  cycles vs CPython's cyclic GC).
- **Defer-free windows don't span the cross call**: pycallocs'
  `proxy_defer_depth` and luaproxy's `lp_defer_depth` are independent
  counters. A host→C→callee call should probably bracket BOTH (or the
  trampoline should open the callee-side window) so a chunk returned/dropped
  mid-call can't be freed under the other VM's feet.
- **`__liballocs_register_gc_policy` has no context cookie** — the root cause
  of luaproxy's one-`lua_State`-per-process limit. Changing the callback
  signature (or registering per-instance trampolines) unlocks multi-state Lua
  and cleaner per-VM dispatch. Coordinate with liballocs upstream.

## Concurrency

### 7. Threading and the GIL

Everything is proven single-threaded-on-the-loading-thread. Open: calls into a
Python callee from a second host thread (needs `PyGILState_Ensure/Release`
inside `linkpy::py::run_function` — localized change, deliberately deferred);
Lua coroutines calling wrappers (same `lua_State` family, probably fine, but
unverified); two host threads racing the first `linkpy.load` (the auditor's
`INIT` flag and workdir are unsynchronized).

## Mechanics & hygiene

### 8. Flat global symbol namespace

Trampolines are unprefixed `extern "C"` symbols (`bump`, `divide`) and the
helpers now load the linkstub `RTLD_GLOBAL` (required so the callee VM's
library is visible to the proxy extension its VM dlopens). Two foreign modules
exporting the same function name, or a collision with any other global symbol,
is undiagnosed. Options: `MOD_PREFIX`-style name mangling for the C symbols
(the host bindings already namespace by module, so only the trampoline layer
needs it), or at least a duplicate-name check in the auditor.

### 9. Mixed callee languages in one init library

`FunLinkEntry::lang` exists and the auditor rejects mixed maps, but one binary
calling both Python AND Lua modules (via one libinit or several) is
ungeneralized: one auditor artifact per callee language, single `TEMPLATE_PATH`
/ `LINKSTUB_BUILD_CMD` env vars, and `LD_AUDIT` chaining does not compose
(the first auditor's rewrite hides `libinit.so` from the second). A
multi-VM-callee linkstub is architecturally closer than auditor chaining.

### 10. The prebuilt `allocs.so` predates `address_of`

`allocs.address_of` exists in source (`vmextensions/cpython/allocs_module.c`)
but the built extension is the older one; `linkpy::py::ptr_from_proxy` falls
back to `debug_handle_info[0]`. Rebuilding `allocs` must target the SAME
Python as the host/linkstub (system 3.14 here, not contrib 3.15) — the build
recipe with the alaska clang is finicky, which is why the rebuild was skipped.
Decide when to rebuild and whether the version-alignment story (3.14 vs 3.15)
should be made explicit in the build system.

### 11. Error propagation fidelity

A callee-side error crosses up to three runtimes (callee VM → C++ exception →
host VM error). Today: `PyErr_Print`/`lua_raise` produce a message, but
callee tracebacks are printed to stderr rather than carried into the host
exception, and the generic "Failed to call function" wrappers lose detail.
Worth a pass once the semantics settle.

### 12. `LINKPY_INJECT_STUBS` (P3294) is incompatible with host bindings

The draft token-injection back end aborts when `LINKPY_HOST_LANG != c`. If the
injection path ever becomes real (a P3294 compiler exists), the host-binding
codegen needs an injection twin, and the callee-namespace qualifier must be
parameterized there (`linkject.hpp` is hardwired to `linkpy::py`).

## Infrastructure

### 13. CI and environment reproducibility

The cross tests need liblua 5.5 + a lua interpreter; the by-ref tests
additionally need the full liballocs stack, `allocscc` with
`LIBALLOCS_OCAML_PREFIX` pointing at `contrib/cil` (no `lib/ocaml` install),
the basetypes provider from the main build, and `luaproxy.so` built. None of
this runs in CI today. Also: the top-level build skips rebuilding the linker
when a stale `linker/dist/audit.so` exists, and the linker must be configured
against SYSTEM python3 (an embedded contrib/cpython cannot find its stdlib
from a foreign binary — decide whether to fix contrib embedding via
PYTHONHOME/installed prefix or codify system-python as the linker's contract).

### 14. Per-call overhead

`run_function` re-resolves module + function on every call (Python:
`import_module`/`import_function` each time; Lua: `require` each time —
amortized by `package.loaded` but still a table walk), and the by-ref path
re-`require`s the proxy extension per pointer argument. Fine for correctness
testing; profile and cache (registry refs / static PyObject caches) before
calling the bridge fast. The JIT compile itself (~1-2 s at first load) is the
price of the auditor design; the P3294 "inject at stub-library build time"
idea (docs/P3294_TOKEN_INJECTION_CODEGEN.md) is the known escape hatch.
