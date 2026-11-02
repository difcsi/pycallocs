/* Minimal embedded host for luaproxy development and tests: statically links
 * liblua.a + the luaproxy objects, so no package.cpath setup is needed and
 * luaopen_luaproxy runs at a controlled point. Run under the liballocs
 * preload:
 *   LD_PRELOAD=liballocs_preload.so ./luahost script.lua [args...]
 * The stock contrib/lua interpreter + require "luaproxy" is the deliverable
 * configuration; this host exists to take dlopen/search-path variables out of
 * day-1 debugging. */

#include <stdio.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int luaopen_luaproxy(lua_State *L);

/* Lua swallows errors in __gc finalizers and reports them only through the
 * warning system; surface them on stderr so lifetime bugs are not silent. */
static void warnf(void *ud, const char *msg, int tocont)
{
    (void)ud;
    fprintf(stderr, "lua warning: %s%s", msg, tocont ? "" : "\n");
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s script.lua [args...]\n", argv[0]);
        return 2;
    }

    lua_State *L = luaL_newstate();
    if (!L)
    {
        fprintf(stderr, "cannot create Lua state\n");
        return 1;
    }
    luaL_openlibs(L);
    lua_setwarnf(L, warnf, NULL);

    /* Preload luaproxy into package.loaded so scripts require it as usual. */
    luaL_requiref(L, "luaproxy", luaopen_luaproxy, 0);
    lua_pop(L, 1);

    /* Standard `arg` table: arg[0] = script, arg[1..] = script arguments. */
    lua_createtable(L, argc - 2, 1);
    for (int i = 1; i < argc; ++i)
    {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");

    int status = luaL_dofile(L, argv[1]);
    if (status != LUA_OK)
    {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    lua_close(L);
    return 0;
}
