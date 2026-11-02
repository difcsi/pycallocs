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

By-reference (pointer) arguments are not yet supported on the Lua path: there is
no pycallocs-style proxy for Lua, so a pointer argument is passed as opaque light
userdata and writes do not propagate back (the Python by-reference path remains
the reference implementation for that).

## TODOS

* `pyhgen.py` covers scalars + by-value local-class structs; container/typing
  generics (`list`, `dict`, `Optional`, ...) are not yet mapped. `pytypecheck.py`
  treats those (and unknown C scalars / opaque pointers) as unconstrained rather
  than checking them — the two should grow matching coverage.
