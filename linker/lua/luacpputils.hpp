#pragma once

// The Lua sibling of pycpputils.hpp: it marshals C values to/from a Lua VM so a
// generated trampoline can call a Lua function by value. It deliberately keeps
// the SAME run_function<T> / run_function_void interface pycpputils.hpp exposes,
// so the auditor's codegen (gen_code) is identical for both languages -- only
// the header the linkstub #includes and the library it links against differ.
//
// Lua's C API is stack-based rather than handle-based (CPython's PyObject*), so
// where pycpputils builds an argument tuple and reads attributes, this pushes
// arguments onto the Lua stack and reads table fields. Aggregates/structs are
// still marshalled field-by-field with C++26 static reflection (P2996); only the
// per-field accessor changes (lua_getfield/lua_setfield instead of
// PyObject_GetAttrString/PyObject_SetAttrString).

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <meta>
#include <type_traits>
#include <string>
#include <stdexcept>

// Everything lives in linkpy::lua so a cross-language linkstub TU can include
// this header together with pycpputils.hpp (linkpy::py) without the
// run_function templates colliding.
namespace linkpy::lua {

// VM lifecycle + module resolution (defined in src/luacpputils.cpp), mirroring
// pycpputils.cpp's init_py/import_module. One lua_State is created lazily and
// shared across calls; template_lua.cpp creates it eagerly in a constructor and
// closes it on unload.
lua_State *lua_state();
void init_lua();
void close_lua();

// Push module.function onto the stack (resolving the module via `require`), or
// throw std::runtime_error. Used by the run_function templates below.
void push_module_function(lua_State *L, const std::string &module,
                          const std::string &function);

// Non-throwing existence check used by the auditor's verify step: true iff
// `require(module)[function]` (or a global of that name) is a function.
bool function_exists(lua_State *L, const std::string &module,
                     const std::string &function);

// Bridge for by-reference (pointer) arguments: push a luaproxy proxy wrapping
// the raw C address, via require("luaproxy").proxy_from_address(addr) -- the
// Lua dual of pycpputils' proxy_from_c_ptr. Throws std::runtime_error when
// luaproxy is not loadable or liballocs has no proxyable type at the address
// (mirroring the Python path's "cannot proxy pointer argument"). Defined in
// luacpputils.cpp.
void push_proxy_from_c_ptr(lua_State *L, const void *ptr);

// The inverse, for by-reference arguments ARRIVING from a Lua host: extract
// the raw C address from the luaproxy proxy at stack index idx, via
// luaproxy's address_of. Throws std::runtime_error when the value is not a
// proxy or luaproxy is unavailable. Defined in luacpputils.cpp.
void *ptr_from_proxy(lua_State *L, int idx);

// Throw a std::runtime_error built from the Lua error object on the stack top
// (which it pops), prefixed with `what`. Mirrors pycpputils' PyErr_Print + throw.
[[noreturn]] inline void lua_raise(lua_State *L, const std::string &what)
{
    const char *msg = lua_tostring(L, -1);
    std::string m = msg ? msg : "(no error message)";
    lua_pop(L, 1);
    throw std::runtime_error(what + ": " + m);
}

// Read the Lua value at stack index `idx` into a C/C++ value of type T. Defined
// here (like pycpputils' PyObject_to_T) so every translation unit that
// instantiates it -- in particular the runtime-generated linkstub.cpp -- can emit
// the needed code. Aggregates are marshalled field-by-field via C++26 static
// reflection: each non-static data member is read from the table's same-named
// key, recursing through lua_to.
template <typename T>
T lua_to(lua_State *L, int idx)
{
    if constexpr (std::is_same<T, int>::value)
    {
        if (lua_isinteger(L, idx))
            return static_cast<int>(lua_tointeger(L, idx));
        if (lua_isnumber(L, idx))
            return static_cast<int>(lua_tonumber(L, idx));
        throw std::runtime_error("Invalid return type (expected integer)");
    }
    else if constexpr (std::is_same<T, float>::value)
    {
        if (lua_isnumber(L, idx))
            return static_cast<float>(lua_tonumber(L, idx));
        throw std::runtime_error("Invalid return type (expected number)");
    }
    else if constexpr (std::is_same<T, double>::value)
    {
        if (lua_isnumber(L, idx))
            return static_cast<double>(lua_tonumber(L, idx));
        throw std::runtime_error("Invalid return type (expected number)");
    }
    else if constexpr (std::is_same<T, bool>::value)
    {
        return lua_toboolean(L, idx) != 0;
    }
    else if constexpr (std::is_same<T, std::string>::value)
    {
        const char *s = lua_tostring(L, idx);
        if (s == nullptr)
            throw std::runtime_error("Invalid return type (expected string)");
        return std::string(s);
    }
    else if constexpr (std::is_aggregate_v<T>)
    {
        if (!lua_istable(L, idx))
            throw std::runtime_error("Invalid return type (expected table for struct)");
        // Absolute index: we push/pop while reading fields, which would shift a
        // relative (negative) index.
        const int table = lua_absindex(L, idx);
        T result{};
        constexpr auto ctx = std::meta::access_context::current();
        template for (constexpr auto member : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, ctx)))
        {
            constexpr const char *field = 
                std::define_static_string(std::meta::identifier_of(member));
            lua_getfield(L, table, field);   // pushes table[field]
            using FieldT = [:std::meta::type_of(member):]; 
            result.[:member:] = lua_to<FieldT>(L, -1);
            lua_pop(L, 1);                    // pop table[field]
        }
        return result;
    }
    else
    {
        throw std::runtime_error("Unsupported return type for lua_to");
    }
}

// Inverse of lua_to: push a C/C++ value onto the Lua stack. Aggregates become a
// table whose keys mirror the struct's fields, so a value round-trips:
// T -> { field = ... } -> lua_to<T>.
template <typename T>
void push_value(lua_State *L, const T &value)
{
    if constexpr (std::is_same<T, int>::value)
        lua_pushinteger(L, static_cast<lua_Integer>(value));
    else if constexpr (std::is_same<T, float>::value)
        lua_pushnumber(L, static_cast<lua_Number>(value));
    else if constexpr (std::is_same<T, double>::value)
        lua_pushnumber(L, static_cast<lua_Number>(value));
    else if constexpr (std::is_same<T, bool>::value)
        lua_pushboolean(L, value ? 1 : 0);
    else if constexpr (std::is_same<T, std::string>::value)
        lua_pushstring(L, value.c_str());
    else if constexpr (std::is_aggregate_v<T>)
    {
        lua_newtable(L);
        constexpr auto ctx = std::meta::access_context::current();
        template for (constexpr auto member : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, ctx)))
        {
            constexpr const char *field =
                std::define_static_string(std::meta::identifier_of(member));
            push_value(L, value.[:member:]);  // pushes the field value
            lua_setfield(L, -2, field);        // table[field] = value; pops it
        }
    }
    else
    {
        throw std::runtime_error("Unsupported type for push_value");
    }
}

// Decide, per top-level argument, how a C value becomes a Lua value. This
// mirrors arg_to_PyObject in pycpputils.hpp:
//  - char* / const char*  -> Lua string (by value)
//  - any other pointer T*  -> a luaproxy proxy wrapping the pointee, so the
//    object is passed BY REFERENCE and Lua writes propagate back into C.
//    Needs the liballocs runtime (typed allocation + luaproxy on LUA_CPATH);
//    without it the call throws, exactly like the Python path.
//  - everything else       -> push_value (by value)
template <typename T>
void push_arg(lua_State *L, const T &value)
{
    if constexpr (std::is_same_v<T, const char *> || std::is_same_v<T, char *>)
    {
        if (value == nullptr) lua_pushnil(L);
        else lua_pushstring(L, value);
    }
    else if constexpr (std::is_pointer_v<T>)
    {
        if (value == nullptr) lua_pushnil(L);
        else push_proxy_from_c_ptr(L, value);   // T* converts to const void*
    }
    else
    {
        push_value(L, value);
    }
}

template <typename... Args>
void run_function_void(const std::string &module, const std::string &function,
                       int argc, Args... args)
{
    (void)argc;
    lua_State *L = lua_state();
    push_module_function(L, module, function);   // function now on the stack
    (push_arg(L, args), ...);                     // then each argument, in order
    if (lua_pcall(L, static_cast<int>(sizeof...(Args)), 0, 0) != LUA_OK)
        lua_raise(L, "Failed to call function '" + function + "'");
}

template <typename T, typename... Args>
T run_function(const std::string &module, const std::string &function,
               int argc, Args... args)
{
    (void)argc;
    lua_State *L = lua_state();
    push_module_function(L, module, function);
    (push_arg(L, args), ...);
    if (lua_pcall(L, static_cast<int>(sizeof...(Args)), 1, 0) != LUA_OK)
        lua_raise(L, "Failed to call function '" + function + "'");
    T result = lua_to<T>(L, -1);
    lua_pop(L, 1);                                // keep the stack balanced
    return result;
}

// The per-trampoline record gen_code emits for both languages (the shape of
// linkpy::py::CallSite's leading members); here it only carries the names.
struct CallSite
{
    const char *module;
    const char *function;
};

template <typename... Args>
void run_function_void(const CallSite &site, int argc, Args... args)
{
    run_function_void(std::string(site.module), std::string(site.function), argc, args...);
}

template <typename T, typename... Args>
T run_function(const CallSite &site, int argc, Args... args)
{
    return run_function<T>(std::string(site.module), std::string(site.function), argc, args...);
}

} // namespace linkpy::lua
