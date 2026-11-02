#include "hostgen.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Group entries by module, preserving first-seen order (a std::map would
// alphabetize, and stable output makes the generated linkstub diffable).
std::vector<std::pair<std::string, std::vector<const FunLinkEntry *>>>
group_by_module(const std::vector<FunLinkEntry> &linkmap)
{
    std::vector<std::pair<std::string, std::vector<const FunLinkEntry *>>> mods;
    for (const auto &e : linkmap)
    {
        auto it = std::find_if(mods.begin(), mods.end(),
                               [&](const auto &m) { return m.first == e.module_name; });
        if (it == mods.end())
            mods.push_back({e.module_name, {&e}});
        else
            it->second.push_back(&e);
    }
    return mods;
}

// A pointer parameter that is NOT an escaping one (see param_escapes) is a
// single-level char pointer: a C string, converted through std::string on the
// host side and passed to the trampoline as .c_str(). Escaping pointers only
// reach us under LINKPY_CROSS_BYREF and become proxy handoffs instead.
bool is_string_param(const std::pair<std::string, std::string> &p)
{
    return p.first.find('*') != std::string::npos ||
           p.second.find('*') != std::string::npos;
}

// The full pointer type spelling for a by-ref param. The declarator's '*' may
// ride in the name field ("struct Point" / "*p"), so reattach any such stars.
std::string pointer_type_of(const std::pair<std::string, std::string> &p)
{
    std::string t = p.first;
    const auto stars_in_name = std::count(p.second.begin(), p.second.end(), '*');
    for (long i = 0; i < stars_in_name; ++i)
        t += " *";
    return t;
}

// Parameter name usable as a local identifier (the '*' of a declarator may
// ride in the name field depending on how it was spelled).
std::string local_name(const std::pair<std::string, std::string> &p, std::size_t i)
{
    std::string n = p.second;
    n.erase(std::remove(n.begin(), n.end(), '*'), n.end());
    if (n.empty())
        n = "arg" + std::to_string(i);
    return n;
}

} // namespace

void gen_host_python(std::ostream &out, const std::vector<FunLinkEntry> &linkmap)
{
    const auto mods = group_by_module(linkmap);

    out << "// ---- linkpy host bindings (Python host) ----\n\n";

    for (const auto &[mod, funs] : mods)
        for (const FunLinkEntry *f : funs)
        {
            // Internals are linkpy_-prefixed so a parameter named e.g. `args`
            // cannot collide with the wrapper's own locals.
            out << std::format(
                "static PyObject *linkpy_wrap_{0}_{1}(PyObject *, PyObject *linkpy_args)\n"
                "{{\n"
                "    try\n"
                "    {{\n"
                "        if (PyTuple_Size(linkpy_args) != {2})\n"
                "        {{\n"
                "            PyErr_SetString(PyExc_TypeError, \"{0}.{1} expects {2} argument(s)\");\n"
                "            return NULL;\n"
                "        }}\n",
                mod, f->name, f->params.size());

            std::string call_args;
            for (std::size_t i = 0; i < f->params.size(); ++i)
            {
                const auto &p = f->params[i];
                const std::string n = local_name(p, i);
                const char *sep = i ? ", " : "";
                if (param_escapes(p))
                {
                    // By-reference (LINKPY_CROSS_BYREF): the host's pycallocs
                    // proxy yields its raw address; the callee marshaller
                    // re-proxies it on the other side of the trampoline.
                    out << std::format(
                        "        {0} {1} = reinterpret_cast<{0}>("
                        "linkpy::py::ptr_from_proxy(PyTuple_GetItem(linkpy_args, {2})));\n",
                        pointer_type_of(p), n, i);
                    call_args += sep + n;
                }
                else if (is_string_param(p))
                {
                    out << std::format(
                        "        std::string {0}_str = linkpy::py::PyObject_to_T<std::string>("
                        "PyTuple_GetItem(linkpy_args, {1}));\n",
                        n, i);
                    call_args += sep + n + "_str.c_str()";
                }
                else
                {
                    out << std::format(
                        "        {0} {1} = linkpy::py::PyObject_to_T<{0}>("
                        "PyTuple_GetItem(linkpy_args, {2}));\n",
                        p.first, n, i);
                    call_args += sep + n;
                }
            }

            // Call the extern "C" trampoline defined above in this TU (the
            // C-struct ABI pivot; the callee marshaller takes over inside it).
            // A by-ref path would attach here: a pointer param would become a
            // proxy handoff in this prologue, trampoline unchanged.
            if (f->return_type == "void")
                out << std::format(
                    "        {0}({1});\n"
                    "        Py_RETURN_NONE;\n",
                    f->name, call_args);
            else
                out << std::format(
                    "        {0} linkpy_ret = {1}({2});\n"
                    "        return linkpy::py::T_to_PyObject<{0}>(linkpy_ret);\n",
                    f->return_type, f->name, call_args);

            out <<
                "    }\n"
                "    catch (const std::exception &e)\n"
                "    {\n"
                "        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, e.what());\n"
                "        return NULL;\n"
                "    }\n"
                "}\n\n";
        }

    for (const auto &[mod, funs] : mods)
    {
        out << std::format("static PyMethodDef linkpy_methods_{}[] = {{\n", mod);
        for (const FunLinkEntry *f : funs)
            out << std::format(
                "    {{\"{0}\", linkpy_wrap_{1}_{0}, METH_VARARGS, "
                "\"linkpy cross-language: {1}.{0}\"}},\n",
                f->name, mod);
        out << "    {NULL, NULL, 0, NULL}};\n";
        out << std::format(
            "static struct PyModuleDef linkpy_moddef_{0} = {{\n"
            "    PyModuleDef_HEAD_INIT, \"{0}\", \"linkpy cross-language module\", -1,\n"
            "    linkpy_methods_{0}, NULL, NULL, NULL, NULL}};\n\n",
            mod);
    }

    out <<
        "extern \"C\" void linkpy_register_python_modules(void)\n"
        "{\n"
        "    PyGILState_STATE linkpy_gil = PyGILState_Ensure();\n"
        "    PyObject *linkpy_modules = PyImport_GetModuleDict(); // borrowed\n";
    for (const auto &[mod, funs] : mods)
        out << std::format(
            "    if (PyDict_GetItemString(linkpy_modules, \"{0}\") != NULL)\n"
            "        fprintf(stderr, \"linkpy: ERROR: sys.modules already has '{0}'; "
            "refusing to overwrite\\n\");\n"
            "    else\n"
            "    {{\n"
            "        PyObject *linkpy_mod = PyModule_Create(&linkpy_moddef_{0});\n"
            "        if (linkpy_mod == NULL ||\n"
            "            PyDict_SetItemString(linkpy_modules, \"{0}\", linkpy_mod) < 0)\n"
            "            PyErr_Print();\n"
            "        Py_XDECREF(linkpy_mod);\n"
            "    }}\n",
            mod);
    out <<
        "    PyGILState_Release(linkpy_gil);\n"
        "}\n"
        "\n"
        "// Runs at dlopen. With a Python host the interpreter is already up (the\n"
        "// dlopen typically comes from ctypes, under the GIL -- PyGILState_Ensure\n"
        "// is correct either way), and the callee VM constructor from the callee\n"
        "// template above has already run (constructors execute in TU order). The\n"
        "// registration entry point stays exported for hosts that dlopen before\n"
        "// interpreter init.\n"
        "__attribute__((constructor)) static void linkpy_host_register(void)\n"
        "{\n"
        "    if (Py_IsInitialized())\n"
        "        linkpy_register_python_modules();\n"
        "}\n";
}

void gen_host_lua(std::ostream &out, const std::vector<FunLinkEntry> &linkmap)
{
    const auto mods = group_by_module(linkmap);

    out << "// ---- linkpy host bindings (Lua host) ----\n\n";

    for (const auto &[mod, funs] : mods)
        for (const FunLinkEntry *f : funs)
        {
            // Error handling: luaL_error longjmps across C frames, so every C++
            // local must be destroyed first -- the inner scope guarantees that;
            // only the POD buffer outlives it.
            out << std::format(
                "static int linkpy_wrap_{0}_{1}(lua_State *linkpy_L)\n"
                "{{\n"
                "    char linkpy_err[512] = \"unknown error\";\n"
                "    {{   // scope: C++ locals destroyed before luaL_error's longjmp\n"
                "        try\n"
                "        {{\n"
                "            if (lua_gettop(linkpy_L) != {2})\n"
                "                snprintf(linkpy_err, sizeof linkpy_err,\n"
                "                         \"{0}.{1} expects {2} argument(s)\");\n"
                "            else\n"
                "            {{\n",
                mod, f->name, f->params.size());

            std::string call_args;
            for (std::size_t i = 0; i < f->params.size(); ++i)
            {
                const auto &p = f->params[i];
                const std::string n = local_name(p, i);
                const char *sep = i ? ", " : "";
                if (param_escapes(p))
                {
                    // By-reference (LINKPY_CROSS_BYREF): the host's luaproxy
                    // proxy yields its raw address; the callee marshaller
                    // re-proxies it on the other side of the trampoline.
                    out << std::format(
                        "                {0} {1} = reinterpret_cast<{0}>("
                        "linkpy::lua::ptr_from_proxy(linkpy_L, {2}));\n",
                        pointer_type_of(p), n, i + 1);
                    call_args += sep + n;
                }
                else if (is_string_param(p))
                {
                    out << std::format(
                        "                std::string {0}_str = "
                        "linkpy::lua::lua_to<std::string>(linkpy_L, {1});\n",
                        n, i + 1);
                    call_args += sep + n + "_str.c_str()";
                }
                else
                {
                    out << std::format(
                        "                {0} {1} = linkpy::lua::lua_to<{0}>(linkpy_L, {2});\n",
                        p.first, n, i + 1);
                    call_args += sep + n;
                }
            }

            if (f->return_type == "void")
                out << std::format(
                    "                {0}({1});\n"
                    "                return 0;\n",
                    f->name, call_args);
            else
                out << std::format(
                    "                {0} linkpy_ret = {1}({2});\n"
                    "                linkpy::lua::push_value<{0}>(linkpy_L, linkpy_ret);\n"
                    "                return 1;\n",
                    f->return_type, f->name, call_args);

            out <<
                "            }\n"
                "        }\n"
                "        catch (const std::exception &e)\n"
                "        {\n"
                "            snprintf(linkpy_err, sizeof linkpy_err, \"%s\", e.what());\n"
                "        }\n"
                "    }\n"
                "    return luaL_error(linkpy_L, \"linkpy: %s\", linkpy_err);\n"
                "}\n\n";
        }

    for (const auto &[mod, funs] : mods)
    {
        out << std::format("static const luaL_Reg linkpy_funcs_{}[] = {{\n", mod);
        for (const FunLinkEntry *f : funs)
            out << std::format("    {{\"{0}\", linkpy_wrap_{1}_{0}}},\n", f->name, mod);
        out << "    {NULL, NULL}};\n\n";
    }

    out <<
        "// Called by the linkpy.lua helper (package.loadlib hands it over) with\n"
        "// the HOST's lua_State: Lua has no process-global state, so registration\n"
        "// cannot happen in a constructor. Registers every foreign module in\n"
        "// package.loaded (a plain require() then finds them too) and returns\n"
        "// { modname = table, ... }. No C++ locals here, so luaL_error is safe.\n"
        "extern \"C\" int luaopen_linkstub(lua_State *linkpy_L)\n"
        "{\n"
        "    lua_newtable(linkpy_L);                                      // result\n"
        "    luaL_getsubtable(linkpy_L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);\n";
    for (const auto &[mod, funs] : mods)
        out << std::format(
            "    lua_getfield(linkpy_L, -1, \"{0}\");\n"
            "    if (!lua_isnil(linkpy_L, -1))\n"
            "        return luaL_error(linkpy_L, \"linkpy: package.loaded['{0}'] "
            "already exists; refusing to overwrite\");\n"
            "    lua_pop(linkpy_L, 1);\n"
            "    luaL_newlib(linkpy_L, linkpy_funcs_{0});\n"
            "    lua_pushvalue(linkpy_L, -1);\n"
            "    lua_setfield(linkpy_L, -3, \"{0}\");   // package.loaded['{0}'] = tbl\n"
            "    lua_setfield(linkpy_L, -3, \"{0}\");   // result['{0}'] = tbl\n",
            mod);
    out <<
        "    lua_pop(linkpy_L, 1);   // drop package.loaded; result stays on top\n"
        "    return 1;\n"
        "}\n";
}
