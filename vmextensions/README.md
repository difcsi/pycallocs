# vmextensions — per-VM liballocs interop extensions

Each subfolder is one virtual-machine extension exposing C allocations as
native objects of that VM, driven by liballocs uniqtype metadata, plus a
shared folder for VM-agnostic code.

- `common/` — host-language-agnostic pieces compiled into every extension:
  - `minicrunch.{c,h}` — bounds inference (`__fetch_bounds_internal`) for
    pointer length discovery
  - `dlsym_iter.{c,h}` — `.dynsym` iteration for the library loaders
  - `ffitypes.{c,h}` — uniqtype → `ffi_type` mapping (AMD64 ABI struct
    classification, union patching) for the function proxies
- `cpython/` — the Python extension (`allocs` module; built by the top-level
  `setup.py`, driven by the root CMake `build-python` target). Loaded through
  the `elflib` import hook.
- `lua/` — the Lua extension (`luaproxy.so` + `luahost` dev driver; standalone
  CMake: `cmake -S vmextensions/lua -B vmextensions/lua/build`). See its
  README for semantics and limitations.
- `v8/` — the V8/JavaScript extension (`v8proxy.node`, a Node.js native
  addon using the raw V8 API; standalone CMake:
  `cmake -S vmextensions/v8 -B vmextensions/v8/build`). Node is the host;
  see its README for semantics and the node-under-liballocs notes.

The per-VM folders deliberately mirror each other file-for-file
(foreign_type, foreign_basetype, composite_proxy, address_proxy,
function_proxy, library_loader, proxy): the marshalling protocol is a
per-uniqtype vtable in each VM's idiom (`ForeignTypeObject` over `PyObject*`,
`LuaForeignType` over the Lua stack), while everything liballocs-facing that
does not touch VM values lives in `common/`. Tests live under the top-level
`tests/` (`tests/py/`, `tests/lua/`) and share the same allocscc-compiled
fixtures in `tests/libs/`.
