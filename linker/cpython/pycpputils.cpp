#include <pycpputils.hpp>
#include <stdexcept>
#include <string>

void init_py()
{
    Py_Initialize();
    PyObject *sysPath = PySys_GetObject("path");
    PyList_Append(sysPath, PyUnicode_FromString("src"));
}

PyObject *import_module(const char *name)
{
    PyObject *pName, *pModule;
    pName = PyUnicode_FromString(name);
    pModule = PyImport_Import(pName);
    Py_DECREF(pName);
    return pModule;
}
PyObject *import_function(PyObject *module, const char *name)
{
    PyObject *pFunc;
    pFunc = PyObject_GetAttrString(module, name);
    if (pFunc && PyCallable_Check(pFunc))
    {
        return pFunc;
    }
    else
    {
        PyErr_Print();
        return NULL;
    }
}
PyObject *call_function(PyObject *pFunc, PyObject *args)
{
    PyObject *pValue;
    pValue = PyObject_CallObject(pFunc, args);
    if (pValue != NULL)
    {
        return pValue;
    }
    else
    {
        PyErr_Print();
        return NULL;
    }
}

// Argument marshalling is handled generically by arg_to_PyObject /
// T_to_PyObject (pycpputils.hpp) via the run_function templates — no hardcoded
// per-type argument packing here.

// PyObject_to_T is a template defined in pycpputils.hpp so that every use site
// (including the runtime-generated linkstub.cpp) can instantiate it.

// Bridge for by-reference (pointer) arguments: wrap a raw C address in a
// pycallocs proxy by calling the `allocs` module's proxy_from_address(int).
// Going through the Python C-API (already linked into the stub) avoids the
// RTLD_LOCAL symbol-visibility problem of calling Proxy_GetOrCreateBase directly.
PyObject *proxy_from_c_ptr(const void *ptr)
{
    // Cache allocs.proxy_from_address for the process lifetime. The lazy init
    // runs under the GIL, which every marshalling caller holds.
    static PyObject *bridge = nullptr;
    if (bridge == nullptr)
    {
        PyObject *mod = PyImport_ImportModule("allocs");
        if (mod == nullptr) return nullptr;
        bridge = PyObject_GetAttrString(mod, "proxy_from_address");
        Py_DECREF(mod);
        if (bridge == nullptr) return nullptr;
    }

    PyObject *addr = PyLong_FromVoidPtr(const_cast<void *>(ptr));
    if (addr == nullptr) return nullptr;
    PyObject *proxy = PyObject_CallOneArg(bridge, addr); // new ref or NULL+exc
    Py_DECREF(addr);
    return proxy;
}
