# Token-sequence injection codegen (P3294) — draft back ends

Both pycallocs and linkpy have a phase that **generates C++ source as text and
shells out to a compiler** at run time. These draft alternatives keep that exact
pipeline but replace the *string templating* with **P3294 code injection via
token sequences**: the generated translation unit shrinks to a compile-time
*descriptor* plus one `consteval` block, and the structs/wrappers/trampolines are
synthesised hygienically at the generated TU's own compile time.

The originals remain the default. Each draft is behind its own build flag and is
mutually exclusive with / parallel to the original.

> ⚠️ There is no P3294 compiler yet. None of this has been compiled. The exact
> token-sequence and interpolation spellings (`^^{…}`, `\tokens`, `\id`, `\(…)`,
> `consteval {…}`, `std::meta::queue_injection`) follow P3294R2 / P3289 as
> written and are expected to shift before the proposals land.

## Why injection instead of string codegen

The type layouts (pycallocs) and function signatures (linkpy) are discovered at
**run time** (from a `uniqtype` / from DWARF), so a run-time compile is
unavoidable in both. What *is* avoidable is hand-formatting C++ in `fprintf` /
`std::format`:

- **Hygiene.** Identifiers and types are spliced as reflections/tokens, not
  pasted into a format string — no quoting/escaping hazards, no accidental
  capture.
- **Locality.** The *shape* of the generated code lives in one fixed header of
  token sequences, next to the templates it instantiates, instead of being
  smeared across the C/C++ driver as format strings.
- **The driver collapses** to "serialise the runtime type info into a small
  descriptor"; all C++ structure moves into the injector.

## pycallocs

| | default | draft |
|---|---|---|
| flag | `SPECIALISE_CONVERSION` | `INJECT_CONVERSION` |
| driver | [src/specialise.c](../src/specialise.c) | [src/specialise_inject.c](../src/specialise_inject.c) |
| codegen | `emit_type()` prints `struct`s + `extern "C"` wrappers | `emit_descriptor()` prints `C/M/T` records |
| injector | — | [include/pyc_inject.hpp](../include/pyc_inject.hpp) → `pyc::inject_translators()` |

The generated `.cpp` embeds `pycpputils.hpp` + `pyc_inject.hpp`, then:

```cpp
constexpr char __pyc_desc[] = R"PYCDESC(
C __pyc_T0 2
M int x
M double y
T __pyc_T0
)PYCDESC";
consteval { pyc::inject_translators(__pyc_desc); }
```

`inject_translators` injects one `struct __pyc_TN { … };` per composite (deps
first) and the two `extern "C"` `__pyc_translate` / `__pyc_fromvalue` entry
points `dlsym` looks up — the same symbols `specialise.c` prints. The entry
points (`Specialise_Convert` / `Specialise_FromValue`) and the
build+cache+`dlopen` driver are byte-for-byte the original's; only the body of
the generated TU differs. Call sites now guard on the derived
`PYCALLOCS_HAVE_SPECIALISE` macro so either back end engages them.

## linkpy

| | default | draft |
|---|---|---|
| flag | (always on) | `LINKPY_INJECT_STUBS` |
| codegen | `gen_code()` + struct loop in [auditor.cpp](../contrib/linkpy/src/auditor.cpp) | [src/codegen_inject.cpp](../contrib/linkpy/src/codegen_inject.cpp) → `write_linkstub_injected()` |
| injector | — | [include/linkject.hpp](../contrib/linkpy/include/linkject.hpp) → `linkpy::inject_stubs()` |

`process_lmap()` branches on `LINKPY_INJECT_STUBS`. The injected `linkstub.cpp`
is the usual template + `#include "linkject.hpp"` + a descriptor + one
`consteval` block:

```cpp
constexpr char __linkstub_desc[] = R"DESC(
S|Point|int:x;int:y;
F|mymod|add|int|int:a;int:b;
F|mymod|scale|struct Point|struct Point:p;
)DESC";
consteval { linkpy::inject_stubs(__linkstub_desc); }
```

`inject_stubs` injects each `struct` (dependency order) and one `extern "C"`
trampoline per function — identical to what `gen_code` prints, including the
`run_function<RET>(module, name, argc, args…)` / `run_function_void(…)` forward.
Multi-token C type spellings (`const char *`, `struct Point`) are turned into
token sequences by `type_tokens()`, which emits keywords/`*` as literal tokens
and tags/typedefs via `\id`. `compile_stubs()` is unchanged.

### A more radical variant (not implemented)

linkpy already builds the stub *init* library ahead of audit time, and its DWARF
carries every signature. In principle the trampolines could be injected when the
stub library is compiled, eliminating the run-time compile entirely. The auditor
keeps a separate redirected DSO (`liblinkstub.so`) precisely because the link map
is resolved at audit time, so this draft keeps the run-time compile; the note is
here as the obvious next step.
