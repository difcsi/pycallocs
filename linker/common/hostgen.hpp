#pragma once
#include <iosfwd>
#include <vector>
#include "linkmap.hpp"

// HOST-language binding codegen for cross-language calls (LINKPY_HOST_LANG):
// appended to the linkstub after gen_code's extern "C" trampolines, which the
// emitted wrappers call. Each wrapper converts host-VM values to C values with
// the HOST marshaller, calls the trampoline (whose C ABI -- the user's header
// types -- is the by-value pivot), and converts the C result back; the callee
// marshaller inside the trampoline does the other half. Registration makes the
// wrappers reachable idiomatically: one Python module per foreign module
// (inserted into sys.modules), or package.loaded entries via an exported
// luaopen_linkstub(L) for a Lua host (Lua has no process-global state, so the
// host's lua_State must be handed in).
//
// Pure text emission -- the auditor process never touches the host VM's API.
// process_lmap's cross guard runs first, so every signature seen here is
// admissible: params are scalars, single-level char* strings, by-value
// structs, or (only under LINKPY_CROSS_BYREF) escaping pointers that cross as
// proxy handoffs; returns are scalars, structs, or void -- never pointers.
void gen_host_python(std::ostream &out, const std::vector<FunLinkEntry> &linkmap);
void gen_host_lua(std::ostream &out, const std::vector<FunLinkEntry> &linkmap);
