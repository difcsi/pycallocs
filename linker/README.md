# LinkPy

LinkPy is a smarter linker that can link Python code!

At the moment, LinkPy is a linker auditor, but in an ideal world, it should be within the linker.

For pass-by-value functions, LinkPy is a stand-alone solution that can be used to link C code from and to Python code: It reflects on user-defined C types, and generates the necessary packing and unpacking shims during load.

LinkPy recovers function types from the DWARF embedded in the stub init library.
DWARF is the sole source of types; the old `signatures.lmap` text file has been
removed. (`FunLinkEntry::to_string()` and the matching parse still exist, so an
entry can be round-tripped as a string in memory — that is how the type-checker
runner below is fed — but nothing reads or writes an `.lmap` file on disk.)

The DWARF also carries the *struct layouts* of any by-value aggregate a stub
function passes or returns, so the auditor emits those struct definitions into
the generated `linkstub.cpp` ahead of the trampolines (`load_dwarf_structs` in
`dwarfmap.cpp`). No struct types are hard-coded.

## Loading at runtime (`dlopen`)

The link-stub redirect happens in `la_objsearch`, which the dynamic linker calls
both for load-time `DT_NEEDED` dependencies *and* for `dlopen`. So a program need
not link `libinit.so` at load time: it can resolve the foreign functions at
runtime with `dlopen("libinit.so")` + `dlsym`, and the auditor swaps in the
JIT-compiled `liblinkstub.so` just the same. The match is by *basename*, so the
init library is recognised however the caller spells it — a bare soname
(`"libinit.so"`), a relative path (`"./libinit.so"`), or an absolute one.

The auditor generates and compiles the stub in a private temporary directory
(`mkdtemp` under `$TMPDIR`/`/tmp`), not in `./gen` and `./dist` under the caller's
CWD, and hands the loader the stub's absolute path. So the redirect resolves
regardless of the working directory the `dlopen` was issued from, and nothing is
written into — or required to pre-exist in — the caller's tree; the temp dir is
removed at process exit (the `.so` stays mapped until then). (`LD_AUDIT` must still
be set on the process; without it nothing intercepts the load and the stub aborts
with *"forgot to load auditor"*.) See the `*_dlopen_divide` tests
(`add_linkpy_test(dlopen_divide ... DLOPEN)`) for an end-to-end example in both
languages.

## Generating headers from Python type hints

`cpython/pyhgen.py` derives a C `*.py.h` stub header from the type hints in a
Python module, so headers need not be hand-written when the signatures are fully
annotated:

```text
python3 cpython/pyhgen.py --src <src_dir> --out <out_dir> [--hand <handwritten_dir>]
```

For each `<mod>.py` it emits `<mod>.py.h`: an `extern` declaration per top-level
function and a `struct` per local class used by one. The supported mapping is
`int → int`, `float → double`, `bool → bool`, `str → const char *`,
`None → void` (return), and an annotated local `class X` → `struct X`. A function
is only emitted when its return and every parameter are annotated; an
unannotated/unsupported signature fails with a precise message rather than
producing a wrong header. With `--hand`, an existing hand-written header wins and
is copied verbatim.

The generated header then feeds the normal pipeline (pystubgen → stub library →
DWARF → auditor), so nothing downstream changes. See the `python_generated_header`
test (`tests/linker/cpython/`, `add_linkpy_test(generated_header LANG python
GENERATE_HEADERS)`) for an end-to-end example, including a struct return type.

## Type-checking Python against C types

`cpython/pytypecheck.py` is the dual of `pyhgen.py`: instead of *generating* C
headers from Python hints, it *checks* a Python module against the C types
LinkPy will bind it to, and reports whether the module is a good candidate for
linking. It is the static form of the question the auditor asks at marshal time
— *does the loaded Python actually match the C signature?*

```text
python3 cpython/pytypecheck.py --src <src_dir> (--include <hdr_dir> | --header <f> | --lmap <f|->)
                             [--strict] [--json] [-q]
```

It reads the C signatures from one of two sources:

* `--include`/`--header` — the `*.py.h` stub headers (same grammar as
  `pystubgen`); this mode also recovers `struct` layouts so class fields are
  checked.
* `--lmap -` — colon-delimited `FunLinkEntry::to_string()` lines on stdin. This
  is an in-memory interchange, **not** a file: nobody writes a `signatures.lmap`.

For the linker/dwarfmap side there is a C++ front end, **`linkpychk`**, so the
type rules live in one place (the Python tool) rather than being re-implemented
in C++:

```text
linkpychk <stub_init.so> <src_dir> [--strict] [--json] [-q]
```

It recovers the C function signatures straight from the stub library's DWARF
with `load_dwarf_map()` (the very reader the auditor uses), serialises them with
`to_string()`, and pipes them into `pytypecheck.py --lmap -`. So the linker
extension can answer *“is the loaded Python a good candidate to bind?”* directly
from the same DWARF it already has, with no intermediate file. Extra flags are
forwarded, so `--json` yields a machine-readable verdict. (Only function
signatures travel this path; struct-field checks require header mode.)

The C→Python mapping is the inverse of pyhgen's (`int → int`, `double/float →
float`, `bool → bool`, `const char * → str`, `void` return `→ None`,
`struct X` / `struct X *` → `class X`). Findings are graded by linkability:
an **error** is a real incompatibility (a declared C function with no matching
Python def, an arity mismatch, or a present-but-contradictory hint), while a
**warning** is a non-fatal gap (a missing hint where C has a concrete type —
gradual typing still binds at runtime). A module is a *good candidate* iff it
has zero errors; the exit code is `0` for a candidate, `1` for errors, `2` on a
usage/parse failure. `--strict` promotes warnings to errors (every linked
signature must be fully and correctly annotated). See the `python_typecheck_*`
tests under `tests/linker/cpython/`.

## Linking Lua

LinkPy links Lua the same way it links Python. The core — DWARF type recovery
(`dwarfmap.cpp`), the `FunLinkEntry` signature model, the stub generator
(`pystubgen`), and heap-hints — is language-neutral, so only the front-end
tooling, the marshalling, and the VM template are Lua-specific. Lua has no native
type syntax, so the Lua tools read **LuaLS / EmmyLua `---@` annotations**:

```lua
local M = {}

---@class Point
---@field x integer
---@field y integer

---@param a integer
---@param b integer
---@return integer
function M.add(a, b) return a + b end

return M
```

* `lua/luahgen.py` — the Lua dual of `pyhgen.py`: derives a C `<mod>.lua.h`
  from the `---@param`/`---@return`/`---@class`/`---@field` hints. The mapping is
  `integer → int`, `number → double`, `boolean → bool`, `string → const char *`,
  no/`nil` return `→ void`, and an annotated `---@class X` `→ struct X`.
* `lua/luatypecheck.py` — the Lua dual of `pytypecheck.py`: checks a `<mod>.lua`
  against the recovered C types (`--include` headers or `--lmap -`). The C++ front
  end is **`lualinkchk`** — the same runner as `linkpychk`, pointed at the Lua
  checker.
* `lua/luacpputils.hpp` + `lua/template_lua.cpp` — the Lua marshalling (the
  dual of `pycpputils.hpp` / `template.cpp`), keeping the same `run_function<T>`
  interface so the auditor's codegen is unchanged. Scalars and by-value structs
  cross C↔Lua; structs are marshalled field-by-field with the same C++26 static
  reflection, through Lua tables instead of attributes.
* **`audit_lua.so`** — the Lua LD_AUDIT library: the same `common/auditor.cpp` built
  with `-DLINKPY_LANG_LUA` so it embeds Lua instead of CPython.

The Python and Lua tools share their C-side logic — header/lmap parsing, type
classification, the check grading, and `.h` rendering — in `common/linktypes.py`,
so the two stay in lockstep. The Lua runtime path needs liblua (PUC-Lua 5.3+, for
the integer subtype); CMake discovers it via pkg-config and builds `audit_lua.so`
and the `lua_*` tests when present, skipping them otherwise. The Python and Lua
suites are mirrored under `tests/linker/cpython/` and `tests/linker/lua/` and registered through
the same language-parameterized helpers; see the `lua_*` and `lua_typecheck_*`
tests for end-to-end examples, including a struct return and a header derived from
`---@` hints.

By-reference (pointer) arguments work on the Lua path too: a non-`char*`
pointer argument is wrapped in a **luaproxy** proxy (`vmextensions/lua`) via
`require("luaproxy").proxy_from_address` — the exact mirror of the Python
path's pycallocs proxy — so writes through the proxy propagate back into the
live C object. Like the Python path it needs the liballocs runtime: the caller
compiled with `allocscc` (typed heap), the liballocs preload + base-types
provider, and `luaproxy.so` reachable via `LUA_CPATH`. Both `*_ptr_arg` tests
are gated behind `-DLINKPY_BYREF_TESTS=ON`. Note that luaproxy serves one
`lua_State` per process; under linkpy that state is the linkstub's embedded VM.

## Cross-language calls (Python ↔ Lua)

LinkPy can also bridge two VMs: a **Python program calling Lua functions**, or a
**Lua program calling Python functions**, with the user's C header as the ABI
contract in the middle. The callee side is exactly the existing pipeline (the
`.py.h`/`.lua.h` header → `pystubgen` → `libinit.so` → the callee auditor JITs
`liblinkstub.so` with the `extern "C"` trampolines). What's new is the **host
side**: when `LINKPY_HOST_LANG` names a VM, the auditor appends host-language
binding code to the same linkstub — for a Python host a real CPython module per
foreign scripting module (inserted into `sys.modules`), for a Lua host an
exported `luaopen_linkstub(L)` that fills `package.loaded`. Each generated
wrapper converts host-VM values to C values with the host marshaller
(`linkpy::py` / `linkpy::lua`), calls the trampoline, and converts the result
back; the callee marshaller inside the trampoline does the other half. Host
objects and callee objects never meet — **the user-defined C struct is the
by-value pivot**: `PyObject ↔ struct Point ↔ Lua table`.

The host program loads the bridge through a tiny helper:

```python
# python app.py — Lua callee (audit_lua.so)
import linkpy                      # linker/cpython/linkpy.py (on PYTHONPATH)
shapes = linkpy.load("shapes")     # dlopen ./libinit.so -> auditor redirect
p = shapes.make_point(3, 4)        # crosses Python -> struct Point -> Lua table
shapes.describe(p)
```

```lua
-- lua app.lua — Python callee (audit.so)
local linkpy = require("linkpy")   -- linker/lua/linkpy.lua (on LUA_PATH)
local shapes = linkpy.load("shapes")
local p = shapes.make_point(3, 4)  -- crosses Lua -> struct Point -> Python
shapes.describe(p)
```

Environment, on top of the usual four variables (`LD_AUDIT` names the **callee**
auditor; the host language never changes the auditor binary):

| variable | value |
| --- | --- |
| `LINKPY_HOST_LANG` | `c` (default, today's behavior) \| `python` \| `lua` |
| `HOST_TEMPLATE_PATH` | `cpython/template_host_py.cpp` or `lua/template_host_lua.cpp` (required when host ≠ c) |

and `LINKSTUB_BUILD_CMD` must compile both marshalling `.cpp` files with both
VMs' include paths but **link only the callee VM's library**: the host VM
belongs to the host process, so host `Py*`/`lua_*` references stay undefined
and resolve against the host interpreter binary (CPython links
`-export-dynamic`; `lua` links `-Wl,-E` — a host binary that exports neither
cannot serve as a cross-language host). Linking the host VM's library instead
would alias two interpreters through ELF interposition, which is also why a
same-language host+callee combination is rejected outright.

Semantics and limits:

* **By value by default.** Scalars (`int`/`double`/`float`/`bool`), `char*`/
  `const char*` strings, and user-defined structs cross as copies through the
  C layout. Without the opt-in below, a pointer parameter or pointer return in
  a cross-language signature is rejected by the auditor before the JIT compile
  with *"by-reference does not cross language boundaries"*.
* **By reference with `LINKPY_CROSS_BYREF=1`.** A pointer parameter then
  crosses as a **proxy handoff**: the generated host wrapper extracts the raw
  address from the host VM's proxy (`allocs.address_of` / luaproxy's
  `address_of`), it crosses the C trampoline as a plain pointer, and the
  callee marshaller re-proxies it (`proxy_from_address` on either side) — one
  live C object, two VMs, writes visible in both. This requires the liballocs
  runtime (preload + base-types provider), the object to be liballocs-typed
  (e.g. minted from an allocscc-compiled library through the vmextensions
  loaders), and both proxy extensions reachable (`allocs` on `PYTHONPATH`,
  `luaproxy.so` on `LUA_CPATH`). The linkpy helpers load the init library with
  `RTLD_GLOBAL` so the callee VM's library is visible to the proxy extension
  its VM dlopens. Pointer *returns* stay rejected (no ownership story yet).
  See the `cross_*_bump` fixtures, gated behind `-DLINKPY_BYREF_TESTS=ON`.
* **Single callee language per init library** — every stub must carry the same
  `.py.`/`.lua.` marker (now tracked as `FunLinkEntry::lang`).
* **Module names must be free** in the host VM: registration refuses to
  overwrite an existing `sys.modules` / `package.loaded` entry.
* One `libinit.so` per process (the auditor JITs once); calls are expected on
  the thread that loaded the library (the embedded-callee GIL/state pattern is
  the same single-thread one the C-host path uses).

The `cross_*` tests under `tests/linker/cross/` are end-to-end examples of all
of the above, registered via `add_linkpy_cross_test(<fixture> HOST <python|lua>
CALLEE <python|lua>)` in `tests/linker/tests.cmake` — including the negative
pointer-rejection fixture. Unresolved design questions (pointer-return
ownership, typed allocation from the header, lifetime coordination, threading,
symbol hygiene, CI) are collected in
`docs/CROSS_LANGUAGE_OPEN_QUESTIONS.md`.

## Strings and byte buffers inside by-value structs

Two more member kinds cross inside a by-value struct, on top of scalars and
nested structs:

* **`char *` / `const char *` fields** arrive in Python as `str` (`NULL` →
  `None`), the mapping a top-level `char *` argument already had. This is the
  C → Python direction only; a `char *` coming *back* from Python would need an
  ownership story and is still rejected.
* **Fixed-size `char[N]` / `unsigned char[N]` fields** cross as `bytes` in both
  directions. C → Python the whole array arrives (a `bytes` of length N; a
  length field of the struct's own says how much of it means anything), so
  embedded NULs survive, which a `char *` could never carry. Python → C a
  `bytes`/`bytearray` of at most N bytes is copied in and the remainder stays
  zero, so a `char[N]` comes back NUL-terminated for C. The DWARF reader renders
  the member as `elem name[N]` in the emitted struct definition.

This is what lets variable-length data (an HTTP body, a block of headers) cross
a by-value boundary: a struct with an array of a fixed capacity plus a length.
See the `python_bytes_array` and `python_str_field` tests.

## Hosting in a long-running program

Three behaviours exist for hosts such as servers that load an init library
and then configure things before calling into it:

* **The interpreter starts at the first call**, not when the linkstub is
  loaded (`ensure_vm` in `template.cpp`), so a host may set `PYTHONPATH` or
  other interpreter settings between loading the library and the first call
  (`python_env_after_load`).
* **The init library's directory is on the module search path**, like `src`:
  a language module can keep its Python half next to its `libinit.so`, with
  no `PYTHONPATH` to set and none to lose when the host replaces the
  environment (`python_init_lib_dir`). The auditor emits the directory into the
  linkstub as `LINKPY_INIT_LIB_DIR`.
* **The auditor reads its settings from a snapshot taken at process start.**
  The auditor lives in its own link-map namespace with its own libc, whose
  `environ` keeps pointing at the original environment block; a host that
  reuses that block for its process title would leave `getenv` there reading
  garbage. `la_version` copies the block into static storage (no heap: the
  host's runtime is not up yet) and `linkpy_getenv` serves it
  (`python_environ_moved`). The JIT compiler gets that environment explicitly,
  minus `LD_AUDIT`/`LD_DEBUG`/`LD_PRELOAD`.

## Quiet by default, the GIL, and the linkstub cache

* **Quiet.** Neither the auditor nor the linkstub prints anything unless asked:
  `-DLINKPY_DEBUG=ON` at configure time for the tools, `-DDEBUG` in
  `LINKSTUB_BUILD_CMD` for the runtime linkstub. The JIT compiler's diagnostics
  stay visible either way (`python_simple_divide` asserts on the silence).
* **Signals.** The interpreter is started with `Py_InitializeEx(0)`: Python's
  signal handlers are not installed in the host (`python_no_signal_handlers`).
* **GIL.** Once the embedded interpreter is up, the linkstub parks the GIL
  (`PyEval_SaveThread`); every trampoline takes it for the duration of its call
  (`GilGuard` in `pycpputils.hpp`). A long-running C host therefore lets Python
  threads an application started keep running between calls
  (`python_gil_release`).
* **Cache.** The compiled `liblinkstub.so` is cached under
  `$LINKPY_CACHE_DIR` (empty disables), else `$XDG_CACHE_HOME/linkpy`, else
  `~/.cache/linkpy`, else (no or unwritable `HOME`, as for a service)
  `$TMPDIR/linkpy-cache-<uid>`, keyed by a SHA-256 of the auditor binary, the
  template(s), the stub library's DWARF, the build command and the init
  library's directory. An entry is only loaded when it, its directory and the
  cache directory are owned by the current user (or root) and not writable by
  group or others (`python_linkstub_cache_untrusted`); a failed store prints
  one line and the process compiles its own. The key also covers every source
  file and `-I` header directory named in the build command and the compiler
  binary; entries untouched for 30 days are evicted when a new one is stored;
  processes starting together on a cold cache share one compile through a
  lock on the key (`python_linkstub_cache_key`, `python_linkstub_cache_concurrent`).
  Build the runtime linkstub with `-O2`: it is on the host's request path.
* **`src`.** The `src` directory next to the working directory is searched for
  modules only when it exists, as an absolute path (`python_no_src_dir`).
* **First call from several threads.** The interpreter starts once
  (`std::call_once`); the thread that started it is the one that finalises it
  at exit (`python_concurrent_first_call`). An oversized environment entry is
  skipped with a note rather than cutting the snapshot short (`python_environ_huge`). A second process with the
  same inputs (a forked app server, a restarted test) loads the cached file and
  never runs the compiler (`python_linkstub_cache`). On a hit the auditor also
  skips its load-time check that the Python module defines every stub function;
  a missing one surfaces at call time instead.

## TODOS

* `pyhgen.py` covers scalars + by-value local-class structs; container/typing
  generics (`list`, `dict`, `Optional`, ...) are not yet mapped. `pytypecheck.py`
  treats those (and unknown C scalars / opaque pointers) as unconstrained rather
  than checking them — the two should grow matching coverage.
