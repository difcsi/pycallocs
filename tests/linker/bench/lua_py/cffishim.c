/* The Lua half of the "cffi" route of the Lua -> Python benchmark. The
 * Python half is generated: libshapes_cffi.so (cffi_build.py) exports the
 * callee's functions as C. Stock Lua has no way to call a C function it was
 * not written for, so this module is the glue a user of cffi still writes by
 * hand, once per function; it makes no Python API call. */

#include <lua.h>
#include <lauxlib.h>

/* The interface of libshapes_cffi.so, as given to cffi in cffi_build.py. */
struct Point { int x; int y; };
extern float divide(int a, int b);
extern int describe(struct Point p);

static int shim_divide(lua_State *L)
{
    int a = (int)luaL_checkinteger(L, 1);
    int b = (int)luaL_checkinteger(L, 2);
    lua_pushnumber(L, divide(a, b));
    return 1;
}

static int integer_field(lua_State *L, int table, const char *name)
{
    int is_integer;
    lua_getfield(L, table, name);
    lua_Integer value = lua_tointegerx(L, -1, &is_integer);
    lua_pop(L, 1);
    if (!is_integer)
        luaL_error(L, "cffishim: field '%s' is not an integer", name);
    return (int)value;
}

static int shim_describe(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    struct Point p = { integer_field(L, 1, "x"), integer_field(L, 1, "y") };
    lua_pushinteger(L, describe(p));
    return 1;
}

static const luaL_Reg functions[] = {
    { "divide", shim_divide },
    { "describe", shim_describe },
    { NULL, NULL },
};

int luaopen_cffishim(lua_State *L)
{
    luaL_newlib(L, functions);
    return 1;
}
