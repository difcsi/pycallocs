#include <luacpputils.hpp>
#include <stdexcept>
#include <string>

// The Lua sibling of pycpputils.cpp: VM lifecycle + module resolution. The
// marshalling templates (lua_to / push_value / run_function) live in the header
// so the runtime-generated linkstub can instantiate them; this file holds the
// non-template glue. One lua_State is created lazily and shared per shared
// object (the auditor's verify pass and the linkstub each get their own).

namespace {
lua_State *g_L = nullptr;
}

void init_lua()
{
    if (g_L != nullptr)
        return;
    g_L = luaL_newstate();
    if (g_L == nullptr)
        throw std::runtime_error("init_lua: luaL_newstate failed (out of memory)");
    luaL_openlibs(g_L);

    // Make the linked Lua modules importable by a bare `require("mod")`. They
    // live in ./src (the test harness symlinks the module directory there),
    // mirroring how pycpputils.cpp adds "src" to sys.path. Prepend our search
    // templates so require finds ./src/mod.lua.
    static const char *kPathSetup =
        "package.path = 'src/?.lua;src/?/init.lua;./?.lua;' .. package.path";
    if (luaL_dostring(g_L, kPathSetup) != LUA_OK)
    {
        std::string msg = lua_tostring(g_L, -1) ? lua_tostring(g_L, -1) : "?";
        lua_close(g_L);
        g_L = nullptr;
        throw std::runtime_error("init_lua: failed to set package.path: " + msg);
    }
}

lua_State *lua_state()
{
    if (g_L == nullptr)
        init_lua();
    return g_L;
}

void close_lua()
{
    if (g_L != nullptr)
    {
        lua_close(g_L);
        g_L = nullptr;
    }
}

void push_module_function(lua_State *L, const std::string &module,
                          const std::string &function)
{
    const int base = lua_gettop(L);

    lua_getglobal(L, "require");              // [require]
    lua_pushstring(L, module.c_str());        // [require, "module"]
    if (lua_pcall(L, 1, 1, 0) != LUA_OK)      // -> [errobj] on failure
    {
        std::string msg = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_settop(L, base);
        throw std::runtime_error(
            "Failed to require module '" + module + "': " + msg);
    }

    // require returned the module value. Idiomatically a table of functions;
    // index it. Otherwise fall back to a global of that name (modules that just
    // define globals).
    if (lua_istable(L, -1))
    {
        lua_getfield(L, -1, function.c_str()); // [modval, fn]
        lua_remove(L, -2);                     // [fn]
    }
    else
    {
        lua_pop(L, 1);                         // []
        lua_getglobal(L, function.c_str());    // [fn?]
    }

    if (!lua_isfunction(L, -1))
    {
        lua_settop(L, base);
        throw std::runtime_error(
            "Function '" + function + "' not found in module '" + module + "'");
    }
}

bool function_exists(lua_State *L, const std::string &module,
                     const std::string &function)
{
    const int base = lua_gettop(L);
    bool ok = false;
    try
    {
        push_module_function(L, module, function);
        ok = true;
    }
    catch (const std::exception &)
    {
        ok = false;
    }
    lua_settop(L, base);   // drop the resolved function (or partial state)
    return ok;
}
