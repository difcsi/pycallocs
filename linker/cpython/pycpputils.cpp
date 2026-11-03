#include <pycpputils.hpp>
#include <stdexcept>
#include <string>

namespace linkpy::py {

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
PyObject *resolve_callable(CallSite &site, PyObject **module_out)
{
    if (site.module_name == nullptr)
    {
        site.module_name = PyUnicode_InternFromString(site.module);
        site.function_name = PyUnicode_InternFromString(site.function);
        if (site.module_name == nullptr || site.function_name == nullptr)
        {
            PyErr_Print();
            throw std::runtime_error("Failed to intern the call site's names");
        }
    }
    // The live sys.modules entry: a dictionary lookup, no import machinery.
    // NULL without an error means "not imported": the first call, or the host
    // dropped it, so import it the way import_module did. (A module that
    // another thread is still initialising would be returned as is; calls
    // come from the thread that loaded the library, under the GIL.)
    PyObject *module = PyImport_GetModule(site.module_name);
    if (module == nullptr)
    {
        if (PyErr_Occurred())
        {
            PyErr_Print();
            throw std::runtime_error("Failed to import module");
        }
        module = PyImport_Import(site.module_name);
        if (module == nullptr)
        {
            PyErr_Print();
            throw std::runtime_error("Failed to import module");
        }
    }
    PyObject *function = PyObject_GetAttr(module, site.function_name);
    if (function == nullptr || !PyCallable_Check(function))
    {
        PyErr_Print();
        Py_XDECREF(function);
        Py_DECREF(module);
        throw std::runtime_error("Failed to import function");
    }
    *module_out = module;
    return function;
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

// Inverse bridge for by-reference arguments arriving FROM a Python host: the
// proxy's underlying address, via allocs.address_of. Older allocs builds
// predate address_of, so fall back to debug_handle_info (whose first tuple
// element is the same pointer) -- that keeps a prebuilt extension usable.
void *ptr_from_proxy(PyObject *obj)
{
    static PyObject *bridge = nullptr;
    static bool bridge_is_debug = false;
    if (bridge == nullptr)
    {
        PyObject *mod = PyImport_ImportModule("allocs");
        if (mod == nullptr)
        {
            PyErr_Print();
            throw std::runtime_error("ptr_from_proxy: cannot import allocs "
                                     "(is the pycallocs extension on PYTHONPATH?)");
        }
        bridge = PyObject_GetAttrString(mod, "address_of");
        if (bridge == nullptr)
        {
            PyErr_Clear();
            bridge = PyObject_GetAttrString(mod, "debug_handle_info");
            bridge_is_debug = true;
        }
        Py_DECREF(mod);
        if (bridge == nullptr)
        {
            PyErr_Print();
            throw std::runtime_error("ptr_from_proxy: allocs has neither "
                                     "address_of nor debug_handle_info");
        }
    }

    PyObject *res = PyObject_CallOneArg(bridge, obj);
    if (res == nullptr)
    {
        PyErr_Print();
        throw std::runtime_error("ptr_from_proxy: not a pycallocs proxy "
                                 "(by-reference arguments must be proxies over "
                                 "liballocs-typed C memory)");
    }
    PyObject *addr_obj = bridge_is_debug ? PyTuple_GetItem(res, 0) /* borrowed */
                                         : res;
    void *addr = addr_obj ? PyLong_AsVoidPtr(addr_obj) : nullptr;
    bool failed = (PyErr_Occurred() != nullptr);
    Py_DECREF(res);
    if (failed)
    {
        PyErr_Print();
        throw std::runtime_error("ptr_from_proxy: cannot read proxy address");
    }
    return addr;
}

} // namespace linkpy::py
