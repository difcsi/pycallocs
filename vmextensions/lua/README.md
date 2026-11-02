# luaproxy — liballocs-driven C interop for Lua

The Lua counterpart of the pycallocs Python extension (`vmextensions/cpython/*.c`): C
allocations are exposed to Lua as proxy objects driven by liballocs uniqtype
metadata, and foreign C functions are callable from Lua with automatic
marshalling through libffi.

```lua
local allocs = require "luaproxy"
local m = allocs.load("libcomposite.so")

local hw = m.hello_world(4, 8.5)   -- construct a C struct (positional)
print(hw.hello, hw.world)          -- field access through liballocs metadata
m.print_hw(hw)                     -- pass by value to a C function
m.compl_hw(hw)                     -- pass by pointer; callee mutates in place
m.save_hw(hw)                      -- C may keep the pointer: the object stays
                                   -- alive after Lua drops it (GC policy)

local w = allocs.int(5)            -- out-parameter cell: an int* from Lua
local cb = allocs.int.fun(allocs.int)(function(x) return x + 1 end)
m.fold_int(5, cb)                  -- Lua function as a C function pointer
```

## Building

Standalone CMake project (deliberately not wired into the root build):

```sh
cmake -S vmextensions/lua -B vmextensions/lua/build
cmake --build vmextensions/lua/build
```

Products:

- `luaproxy.so` — Lua C module for `require "luaproxy"`. Does **not** link
  liblua (contrib/lua's `liblua.a` is non-PIC); Lua symbols resolve from the
  host interpreter, which contrib/lua links with `-Wl,-E`. liballocs symbols
  resolve from the `LD_PRELOAD`ed `liballocs_preload.so`, exactly like the
  Python `allocs` extension.
- `luahost` — embedded dev/test driver (liblua.a + luaproxy statically), so
  no `package.cpath` setup is needed while developing.

Location contract mirrors `setup.py`: `STACKSCAN`, `LIBALLOCS` (cache vars
with environment fallback), `USE_ALASKA` for the (untested in v1) Alaska
configuration. The main pycallocs build must exist (`build/`): the basetypes
provider meta-DSO and the allocscc-compiled test fixtures come from there.

## Running

Everything runs under the liballocs preload:

```sh
export LD_PRELOAD=$LIBALLOCS/lib/liballocs_preload.so:$BUILD/basetypes/libbasetypes_provider.so
export META_BASE=$BUILD/tests/meta                 # writable metadata dir
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288
./vmextensions/lua/build/luahost script.lua                  # embedded host
# or, with the vendored interpreter:
LUA_CPATH='vmextensions/lua/build/?.so' ./contrib/lua/lua script.lua
```

## Tests

Lua tests live beside the Python ones, `tests/lua/<name>/<name>.lua` +
`<name>.out`, and use the same allocscc-compiled fixtures
(`build/tests/libs`). Runner:

```sh
tests/vmextensions/lua/run_tests.sh            # under luahost
tests/vmextensions/lua/run_tests.sh --stock    # under contrib/lua/lua + require
tests/vmextensions/lua/run_tests.sh basic_ops  # a single test
```

## Semantics (differences from the Python port)

- **1-based indexing**: `p[1] .. p[#p]`; out-of-range reads yield `nil`
  (ipairs-friendly), out-of-range writes raise. Slices are `p:sub(i, j)`
  (inclusive); char data reads back with `p:str()`.
- **Structs**: `T()` zero-init, `T(v1, v2, ...)` positional, `T{f = v}`
  named, `T(other)` copy. Plain tables coerce where C expects a struct.
  The offset-0 first-member chain gives C "struct inheritance" for both
  field lookup and type acceptance.
- **Numeric fidelity**: no bignums — `uint64 > 2^63` degrades to
  `lua_Number`, `long double` through double; `_Complex` marshals as
  `{re=, im=}` tables.
- **Cells**: `allocs.int(5)` is a length-1 array usable wherever C expects
  `int*`; `T.ptr()` likewise for `T**` out-params. Type algebra: `T.ptr`,
  `T.array`, `T.fun(argt, ...)`, `T.name`.
- **Lifetime**: proxies attach a liballocs lifetime policy (chunks live while
  the proxy lives); tracked C pointer stores drive GC-policy callbacks that
  mirror `proxy_pointing_addr_dict` as a strong slot→proxy table, so a C
  global holding the last pointer to an object keeps it alive
  (`struct_global`). Foreign calls run inside a defer-free window
  (`struct_global_swap`). Closures are retained while a C slot stores their
  trampoline pointer (`closure_lifetime`).
- **Cycles leak by design**: Lua has no cyclic-GC traverse protocol over
  foreign memory, so host-C reference cycles (e.g. `br.ptr = br`) are
  uncollectable. Break them manually with `proxy:release()`, which clears
  the tracked pointer slots, detaches the lifetime policy and poisons the
  proxy. See `tests/lua/gc_struct_cycle` (a documented-leak demo).

## Limitations (v1)

- One `lua_State` per process (liballocs GC-policy callbacks carry no
  context); a second state raises on `require`.
- Alaska (`USE_ALASKA`) compiles but is untested; all `ss_*` calls are
  no-op passthroughs without it.
- No materialisation of plain Lua values passed as `T*` arguments (pass a
  proxy or use the table→struct by-value coercion); no keyword arguments to
  foreign functions; bit fields and `SUBRANGE` kinds are unsupported
  (reported per-symbol in `module.__unsupported`).
