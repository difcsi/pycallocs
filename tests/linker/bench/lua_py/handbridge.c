/* The baseline of the Lua -> Python per-call benchmark: the bridge one would
 * write by hand. A Lua C module that embeds CPython and exposes the two
 * functions of the callee (tests/linker/cross/bench_lua_py/src/shapes.py),
 * converting between Lua values and Python objects with the two C APIs.
 *
 * It is the best case such a bridge reaches, on purpose: the callables are
 * looked up once and kept, and the GIL is held from load to exit. (PolyLink
 * resolves the callable on every call, so that a reload is seen, and parks
 * the GIL between calls.) The struct argument crosses as a copy, a
 * types.SimpleNamespace built field by field, which is also what PolyLink's
 * by-value route hands to Python. */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <lua.h>
#include <lauxlib.h>

static PyObject *py_divide;     /* shapes.divide */
static PyObject *py_describe;   /* shapes.describe */
static PyObject *py_namespace;  /* types.SimpleNamespace */
static PyObject *name_x, *name_y;

/* Turn the pending Python exception into a Lua error. Does not return. */
static int raise_python_error(lua_State *L)
{
    PyObject *exc = PyErr_GetRaisedException();
    PyObject *text = exc ? PyObject_Str(exc) : NULL;
    const char *msg = text ? PyUnicode_AsUTF8(text) : NULL;
    lua_pushfstring(L, "handbridge: %s", msg ? msg : "unknown Python error");
    Py_XDECREF(text);
    Py_XDECREF(exc);
    PyErr_Clear();
    return lua_error(L);
}

/* divide(a, b): two Lua integers in, a Lua float out. */
static int bridge_divide(lua_State *L)
{
    lua_Integer a = luaL_checkinteger(L, 1);
    lua_Integer b = luaL_checkinteger(L, 2);

    PyObject *args[2] = { PyLong_FromLongLong(a), PyLong_FromLongLong(b) };
    PyObject *result = NULL;
    if (args[0] && args[1])
        result = PyObject_Vectorcall(py_divide, args, 2, NULL);
    Py_XDECREF(args[0]);
    Py_XDECREF(args[1]);
    if (!result)
        return raise_python_error(L);

    double value = PyFloat_AsDouble(result);
    Py_DECREF(result);
    if (value == -1.0 && PyErr_Occurred())
        return raise_python_error(L);
    lua_pushnumber(L, value);
    return 1;
}

static lua_Integer integer_field(lua_State *L, int table, const char *name)
{
    int is_integer;
    lua_getfield(L, table, name);
    lua_Integer value = lua_tointegerx(L, -1, &is_integer);
    lua_pop(L, 1);
    if (!is_integer)
        luaL_error(L, "handbridge: field '%s' is not an integer", name);
    return value;
}

/* describe(t): a Lua table {x=, y=} in, copied into a SimpleNamespace; a Lua
 * integer out. */
static int bridge_describe(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer x = integer_field(L, 1, "x");
    lua_Integer y = integer_field(L, 1, "y");

    PyObject *point = PyObject_CallNoArgs(py_namespace);
    PyObject *px = PyLong_FromLongLong(x);
    PyObject *py = PyLong_FromLongLong(y);
    PyObject *result = NULL;
    if (point && px && py
        && PyObject_SetAttr(point, name_x, px) == 0
        && PyObject_SetAttr(point, name_y, py) == 0)
        result = PyObject_CallOneArg(py_describe, point);
    Py_XDECREF(px);
    Py_XDECREF(py);
    Py_XDECREF(point);
    if (!result)
        return raise_python_error(L);

    long long value = PyLong_AsLongLong(result);
    Py_DECREF(result);
    if (value == -1 && PyErr_Occurred())
        return raise_python_error(L);
    lua_pushinteger(L, (lua_Integer)value);
    return 1;
}

static const luaL_Reg functions[] = {
    { "divide", bridge_divide },
    { "describe", bridge_describe },
    { NULL, NULL },
};

int luaopen_handbridge(lua_State *L)
{
    /* No signal handlers: the host is Lua. The GIL stays with this thread. */
    if (!Py_IsInitialized())
        Py_InitializeEx(0);

    PyObject *shapes = PyImport_ImportModule("shapes");
    PyObject *types = shapes ? PyImport_ImportModule("types") : NULL;
    if (shapes && types)
    {
        py_divide = PyObject_GetAttrString(shapes, "divide");
        if (py_divide)
            py_describe = PyObject_GetAttrString(shapes, "describe");
        if (py_describe)
            py_namespace = PyObject_GetAttrString(types, "SimpleNamespace");
        if (py_namespace)
            name_x = PyUnicode_InternFromString("x");
        if (name_x)
            name_y = PyUnicode_InternFromString("y");
    }
    Py_XDECREF(shapes);
    Py_XDECREF(types);
    if (!name_y)
        return raise_python_error(L);

    luaL_newlib(L, functions);
    return 1;
}
