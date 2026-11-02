#include "foreign_library.h"
#include <liballocs.h>

// Bridge for foreign callers (e.g. linkpy trampolines passing a C object to
// Python by reference): given a raw C address encoded as a Python int, return
// a pycallocs proxy wrapping the pointee. Raises ValueError when liballocs has
// no proxyable type at that address (untyped memory or a scalar/void pointee).
static PyObject *allocs_proxy_from_address(PyObject *self, PyObject *arg)
{
    void *addr = PyLong_AsVoidPtr(arg);
    if (addr == NULL && PyErr_Occurred()) return NULL;

    PyObject *proxy = (PyObject *) Proxy_GetOrCreateBase(addr); // new ref or NULL
    if (proxy == NULL)
    {
        PyErr_Format(PyExc_ValueError,
                     "proxy_from_address: no liballocs proxy type for address %p", addr);
        return NULL;
    }
    return proxy;
}

// Debug/verification introspection: expose a proxy's underlying pointer (an
// Alaska handle when the pointee is handle-backed) plus, when the Alaska
// refcount runtime is present, whether it is a handle and its current handle
// refcount. Used by the case-study verification drivers to give mechanical
// evidence for lifetime claims; returns (ptr, is_handle, refcount|None).
static PyObject *allocs_debug_handle_info(PyObject *self, PyObject *arg)
{
    if (!PyObject_TypeCheck(arg, &Proxy_Type))
    {
        PyErr_SetString(PyExc_TypeError, "expected a pycallocs proxy");
        return NULL;
    }
    ProxyObject *proxy = (ProxyObject *) arg;
    extern unsigned long alaska_get_refcount(void *) __attribute__((weak));
    int is_handle = ss_is_handle(proxy->p_ptr);
    PyObject *rc = Py_None;
    if (is_handle && &alaska_get_refcount)
        rc = PyLong_FromUnsignedLong(alaska_get_refcount(proxy->p_ptr));
    else
        Py_INCREF(Py_None);
    PyObject *ret = Py_BuildValue("(NiN)",
            PyLong_FromVoidPtr(proxy->p_ptr), is_handle, rc);
    return ret;
}

static PyMethodDef allocs_methods[] = {
    {"proxy_from_address", allocs_proxy_from_address, METH_O,
     "Wrap a raw C address (int) in a pycallocs proxy for the pointee."},
    {"debug_handle_info", allocs_debug_handle_info, METH_O,
     "(ptr, is_handle, refcount|None) for a proxy -- verification aid."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef allocsmodule =
{
    PyModuleDef_HEAD_INIT,
    "allocs",   /* name of module */
    NULL,       /* module documentation, may be NULL */
    -1,         /* size of per-interpreter state of the module,
                   or -1 if the module keeps state in global variables. */
    allocs_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_allocs(void)
{
    if (PyType_Ready(&LibraryLoader_Type) < 0) return NULL;
    if (PyType_Ready(&Proxy_Type) < 0) return NULL;
    if (PyType_Ready(&ForeignType_Type) < 0) return NULL;
    if (PyType_Ready(&FunctionProxy_Metatype) < 0) return NULL;
    if (PyType_Ready(&CompositeProxy_Metatype) < 0) return NULL;
    if (PyType_Ready(&AddressProxy_Metatype) < 0) return NULL;

    Proxy_InitGCPolicy();

    PyObject *m = PyModule_Create(&allocsmodule);
    if (m == NULL) return NULL;

    Py_INCREF(&LibraryLoader_Type);
    PyModule_AddObject(m, "LibraryLoader", (PyObject *) &LibraryLoader_Type);
    Py_INCREF(&Proxy_Type);
    PyModule_AddObject(m, "Proxy", (PyObject *) &Proxy_Type);
    Py_INCREF(&ForeignType_Type);
    PyModule_AddObject(m, "ForeignType", (PyObject *) &ForeignType_Type);
    Py_INCREF(&FunctionProxy_Metatype);
    PyModule_AddObject(m, "FunctionProxyType", (PyObject *) &FunctionProxy_Metatype);
    Py_INCREF(&CompositeProxy_Metatype);
    PyModule_AddObject(m, "CompositeProxyType", (PyObject *) &CompositeProxy_Metatype);
    Py_INCREF(&AddressProxy_Metatype);
    PyModule_AddObject(m, "AddressProxyType", (PyObject *) &AddressProxy_Metatype);

    // Add basic C scalar types
#define ADD_TYPE_WITH_NAME(typname, typ) \
    PyModule_AddObject(m, typname, (PyObject *) ForeignType_GetOrCreate(&__uniqtype__##typ))
#define ADD_TYPE(typ) ADD_TYPE_WITH_NAME(#typ, typ)
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

    // Whether the specialised PyObject_to_T / T_to_PyObject fast path is compiled
    // in. When on, by-value struct returns come back as types.SimpleNamespace
    // (via T_to_PyObject) rather than as proxies, so tests can branch on this.
#ifdef PYCALLOCS_HAVE_SPECIALISE
    PyModule_AddIntConstant(m, "SPECIALISE_CONVERSION", 1);
#else
    PyModule_AddIntConstant(m, "SPECIALISE_CONVERSION", 0);
#endif

    return m;
}
