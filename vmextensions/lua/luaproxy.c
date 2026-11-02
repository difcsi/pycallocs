/* luaproxy module entry point: port of src/allocs_module.c.
 *   local allocs = require "luaproxy"
 *   local lib = allocs.load("libfoo.so")
 * The module table also carries the C scalar base types as callable type
 * objects (allocs.int, allocs.double, ...) and proxy_from_address for foreign
 * callers handing us raw addresses. */

#include "luaproxy.h"

#include <stdio.h>

static int allocs_proxy_from_address(lua_State *L)
{
    lua_Integer addr = luaL_checkinteger(L, 1);
    if (!LuaProxy_GetOrCreateBase(L, (void *)(uintptr_t)addr))
        return luaL_error(L,
                "proxy_from_address: no liballocs proxy type for address %p",
                (void *)(uintptr_t)addr);
    return 1;
}

/* The inverse of proxy_from_address: a proxy's underlying addresslike pointer
 * as an integer. Used by linkpy's cross-language host wrappers to pass a
 * proxied C object onward by reference (the address crosses the C trampoline
 * raw; the callee VM re-proxies it). */
static int allocs_address_of(lua_State *L)
{
    LuaProxy *p = LuaProxy_Untyped(L, 1);
    if (!p)
        return luaL_error(L, "address_of: expected a luaproxy proxy");
    lua_pushinteger(L, (lua_Integer)(uintptr_t)p->p_ptr);
    return 1;
}

static const luaL_Reg luaproxy_funcs[] = {
    {"load", LuaLibraryLoader_Load},
    {"proxy_from_address", allocs_proxy_from_address},
    {"address_of", allocs_address_of},
    {NULL, NULL}
};

#define WEAK_UNIQTYPE(t) extern struct uniqtype __uniqtype__##t __attribute__((weak))
WEAK_UNIQTYPE(void);
WEAK_UNIQTYPE(int);
WEAK_UNIQTYPE(unsigned_int);
WEAK_UNIQTYPE(signed_char);
WEAK_UNIQTYPE(unsigned_char);
WEAK_UNIQTYPE(long_int);
WEAK_UNIQTYPE(unsigned_long_int);
WEAK_UNIQTYPE(short_int);
WEAK_UNIQTYPE(short_unsigned_int);
WEAK_UNIQTYPE(float);
WEAK_UNIQTYPE(double);

static void add_type_field(lua_State *L, const char *name, struct uniqtype *u)
{
    if (!u) return; /* weak symbol unresolved: base-types provider not preloaded */
    LuaForeignType *ft = LuaForeignType_GetOrCreate(L, u);
    if (!ft) { lua_pop(L, 1); return; }
    LuaForeignType_PushWrapper(L, ft);
    lua_setfield(L, -2, name);
}

int luaopen_luaproxy(lua_State *L)
{
    /* liballocs GC-policy callbacks carry no context: luaproxy serves exactly
     * one lua_State per process (its main thread). */
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State *mainL = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (lp_globalL && lp_globalL != mainL)
        return luaL_error(L, "luaproxy already serves another lua_State "
                "(one state per process)");
    lp_globalL = mainL;

    LuaProxy_InitGCPolicy(L);
    LuaForeignType_InitMeta(L);

    luaL_newlib(L, luaproxy_funcs);

#define ADD_TYPE_WITH_NAME(name, t) add_type_field(L, name, &__uniqtype__##t)
#define ADD_TYPE(t) ADD_TYPE_WITH_NAME(#t, t)
    ADD_TYPE(void);
    ADD_TYPE(int);
    ADD_TYPE(unsigned_int);
    ADD_TYPE(signed_char);
    ADD_TYPE(unsigned_char);
    if ((char)-1 < 0) ADD_TYPE_WITH_NAME("char", signed_char);
    else ADD_TYPE_WITH_NAME("char", unsigned_char);
    ADD_TYPE(long_int);
    ADD_TYPE(unsigned_long_int);
    ADD_TYPE(short_int);
    ADD_TYPE_WITH_NAME("unsigned_short_int", short_unsigned_int);
    ADD_TYPE(float);
    ADD_TYPE(double);
#undef ADD_TYPE
#undef ADD_TYPE_WITH_NAME

    lua_pushstring(L, "0.1");
    lua_setfield(L, -2, "_VERSION");
    return 1;
}
