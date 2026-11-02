# v8proxy — liballocs-driven C interop for JavaScript (Node.js addon)

The V8 counterpart of the pycallocs CPython extension and luaproxy: C
allocations are exposed to JavaScript as proxy objects driven by liballocs
uniqtype metadata, and foreign C functions are callable with automatic
marshalling through libffi.

```js
const allocs = require('.../v8proxy.node');
const m = allocs.load('libcomposite.so');

const hw = m.hello_world(4, 8.5);   // construct a C struct (positional)
console.log(hw.hello, hw.world);    // field access via named interceptors
m.print_hw(hw);                     // pass by value to a C function
m.compl_hw(hw);                     // pass by pointer; callee mutates in place
m.save_hw(hw);                      // C may keep the pointer: object stays
                                    // alive after JS drops it (GC policy)

const w = allocs.int(5);            // out-parameter cell: an int* from JS
const cb = allocs.int.fun(allocs.int)((x) => x + 1);
m.fold_int(5, cb);                  // JS function as a C function pointer
```

## Building

Standalone CMake project (not wired into the root build):

```sh
cmake -S vmextensions/v8 -B vmextensions/v8/build
cmake --build vmextensions/v8/build
```

Product: `v8proxy.node`, a Node.js native addon using the **raw V8 API**
(property interceptors cannot be expressed in N-API). It links no V8/node
library — those symbols resolve from the `node` executable at `require()`
time — plus `liballocs.so`, the basetypes provider (its `__uniqtype__*` data
symbols bind eagerly at dlopen), libffi.

Node headers come from the node-gyp cache (`~/.cache/node-gyp/<version>`),
overridable via the `NODE_HEADERS` cache var. A configure-time guard parses
node's `config.gypi` and fails if the build needs V8 layout defines (pointer
compression / sandbox / 31-bit smis / external code space) — this node
build (24.x) has all four off, so the addon compiles define-free. A node
built otherwise would need matching defines or object layouts silently break.

## Running

Node is the host; everything runs under the liballocs preload:

```sh
export LD_PRELOAD=$LIBALLOCS/lib/liballocs_preload.so:$BUILD/basetypes/libbasetypes_provider.so
export META_BASE=$BUILD/tests/meta
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288
node --expose-gc script.js
```

Running node under liballocs needed three liballocs fixes (zero-size-hole
coalescing and a larger `MAPPING_SEQUENCE_MAX_LEN` in the mmap allocator for
node's giant executable, plus a stubgen.h line-continuation fix); expect
noisy-but-harmless stderr (xed decode warnings over node's text, untyped
node-internal allocations, and a failed `pthread_create` from node's worker
pool). Known low-rate flake: node under the preload occasionally dies at
startup (empty stdout, before the script runs) -- observed once across ~50
launches, not yet reproduced in isolation; re-run the affected test.

## Tests

`tests/js/<name>/<name>.js` + `<name>.out`, sibling of `tests/py` and
`tests/lua`, using the same allocscc-compiled fixtures:

```sh
tests/vmextensions/v8/run_tests.sh            # all (11 tests)
tests/vmextensions/v8/run_tests.sh basic_ops  # one
```

## Semantics (vs the Lua port)

- **0-based indexing**, `p.length`, out-of-range reads → `undefined`
  (`Array.from` works via array-likeness), out-of-range writes throw
  RangeError. Slices are `p.slice(i, j)` (end-exclusive); char data via
  `p.str()`. No `Symbol.iterator` yet (spread doesn't work; `Array.from`
  does).
- **Structs**: `T()` zero-init, `T(v1, v2, ...)` positional, `T({f: v})`
  object literal, `T(other)` copy. Plain object literals coerce where C
  expects a struct. `Object.keys` sees the C fields (enumerator callback).
  The offset-0 first-member chain gives C "struct inheritance".
- **Numerics**: 64-bit integers outside Number's exact range travel as
  **BigInt** (better fidelity than Lua); stores accept Number and BigInt.
  `_Complex` marshals as `{re, im}` objects; chars as 1-character strings.
- **Cells**: `allocs.int(5)` is a length-1 array usable wherever C expects
  `int*`; `T.ptr()` likewise for `T**`. Type algebra: `T.ptr`, `T.array`,
  `T.fun(argT, ...)`, `T.name`.
- **Lifetime**: registered proxies attach a liballocs lifetime policy;
  tracked C pointer stores drive GC-policy callbacks maintaining a strong
  slot→proxy map (a C global holding the last pointer keeps the object
  alive); foreign calls run in a defer-free window; closures are retained
  while a C slot stores their trampoline. Finalization is V8-weak two-pass:
  first pass (no V8 API) unhooks the cache, second pass detaches the
  lifetime policy (may free the chunk). Tests observe frees with
  `gc(); await new Promise(setImmediate)` under `--expose-gc`.
- **Cycles leak by design** (no traverse protocol over foreign memory);
  break them manually with `proxy.release()`. See `tests/js/gc_struct_cycle`.

## Limitations (v1)

- One Isolate per process (liballocs GC-policy callbacks carry no context);
  Workers get an error on `require`.
- Closure `ffi_closure`/context records are reclaimed only with the proxy;
  a closure freed by GC while C still holds its pointer is protected only
  via the slot-retention path (same policy as the other ports).
- Targets Node 24 / V8 13.6 (untagged External/internal-field API). The
  vendored `contrib/v8` (15.3) is a future migration target: it needs the
  tagged pointer APIs and a sandbox story, plus a depot_tools/gclient
  monolith build that is not scripted yet.
- No materialisation of plain JS values passed as `T*` arguments (pass a
  proxy or use by-value object-literal coercion); bit fields and SUBRANGE
  unsupported (reported per-symbol in `module.__unsupported`).
