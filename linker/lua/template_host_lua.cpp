// Host-side prologue for a Lua HOST (LINKPY_HOST_LANG=lua). The auditor
// appends this right after the CALLEE template, bringing the Lua marshaller
// into scope for the generated host wrappers and luaopen_linkstub.
//
// Deliberately NO constructor and NO lua_State: Lua has no process-global
// state, so registration happens in the exported luaopen_linkstub(L), called
// by the linkpy.lua helper with the HOST's state (package.loadlib hands both
// the dlopen and the symbol over). The linkstub build must NOT link liblua --
// every lua_* reference stays undefined and resolves against the host `lua`
// binary (linked -Wl,-E / against a shared liblua) -- mirroring vmextensions'
// luaproxy discipline.
#include <luacpputils.hpp>
#include <stdexcept>
#include <cstdio>
