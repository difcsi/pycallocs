#include "foreign_library.h"

// FIXME: Use more efficient representation than PyDict
static PyObject *proxy_dict; // Weak reference over values (using PyLong to store refs)
static PyObject *proxy_pointing_addr_dict; // Strong refs over values

// Returns a borrowed reference
static ProxyObject *proxy_for_addr(const void *addr)
{
    PyObject *key = PyLong_FromVoidPtr((void *) addr);
    PyObject *proxy_ptr = PyDict_GetItem(proxy_dict, key);
    Py_DECREF(key);
    if (!proxy_ptr) return NULL;
    return PyLong_AsVoidPtr(proxy_ptr);
}

void Proxy_AddRefTo(ProxyObject *target_proxy, const void **from)
{
    PyObject *from_key = PyLong_FromVoidPtr(from);
    assert(!PyDict_Contains(proxy_pointing_addr_dict, from_key));
    // This effectively incref target_proxy which is the desired behaviour
    if(PyDict_SetItem(proxy_pointing_addr_dict, from_key, (PyObject *) target_proxy) < 0)
        abort();
    Py_DECREF(from_key);
}

static void proxy_addref(const void *target, const void **from)
{
    ProxyObject *target_proxy = proxy_for_addr(target);
    // We should only be called if a proxy is registered for target
    assert(target_proxy);
    assert(target_proxy->p_ptr == target);

    Proxy_AddRefTo(target_proxy, from);
}

static void proxy_delref(const void *target, const void **from)
{
    /* We can ignore the target and just delete the entry in
     * proxy_pointing_addr_dict. */

    PyObject *from_key = PyLong_FromVoidPtr(from);

#ifndef NDEBUG
    // Sanity check to be sure that we are effectively deleting what we think
    if (target && PyDict_Contains(proxy_pointing_addr_dict, from_key) == 1)
    {
        ProxyObject *proxy =
            (ProxyObject *) PyDict_GetItem(proxy_pointing_addr_dict, from_key);
        assert(proxy->p_ptr == target);
    }
#endif

    /* We can observe spurious calls from inexistant location.
     * Just ignore them => No check for existing key
     * Deleting the item from dict decref the proxy object and can trigger
     * deletion. */
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback); // Save the current exception
    if (PyDict_DelItem(proxy_pointing_addr_dict, from_key) < 0)
    {
        PyErr_Clear();
    }
    PyErr_Restore(type, value, traceback);
    Py_DECREF(from_key);
}

static int proxy_gc_policy_id = -1;

void Proxy_InitGCPolicy()
{
    proxy_dict = PyDict_New();
    proxy_pointing_addr_dict = PyDict_New();
    proxy_gc_policy_id = __liballocs_register_gc_policy(proxy_addref, proxy_delref);
}

void Proxy_Register(ProxyObject *proxy)
{
    PyObject *proxy_key = PyLong_FromVoidPtr(proxy->p_ptr);
    assert(!PyDict_Contains(proxy_dict, proxy_key));

    // proxy_dict must only have a 'weak' reference
    // So use a PyLong for storing it instead of storing the object
    PyObject *proxy_ptr = PyLong_FromVoidPtr(proxy);
    PyDict_SetItem(proxy_dict, proxy_key, proxy_ptr);
    Py_DECREF(proxy_ptr);
    Py_DECREF(proxy_key);

    // Attach lifetime policy to the object to extend its lifetime
    __liballocs_attach_lifetime_policy(proxy_gc_policy_id, proxy->p_ptr);

    // Start tracking the object with the cycle GC
    if (PyType_IS_GC(Py_TYPE(proxy))) PyObject_GC_Track(proxy);
}

// Does nothing if obj has not been registered before
// Call free on the underlying foreign object if we are the last lifetime policy
void Proxy_Unregister(ProxyObject *proxy)
{
    PyObject *proxy_key = PyLong_FromVoidPtr(proxy->p_ptr);
    PyObject *proxy_ptr = PyDict_GetItem(proxy_dict, proxy_key);
    if (proxy_ptr && PyLong_AsVoidPtr(proxy_ptr) == proxy)
    {
        // Stop tracking the object with the cycle GC
        if (PyType_IS_GC(Py_TYPE(proxy))) PyObject_GC_UnTrack(proxy);

        // This calls free on the foreign object if necessary
        __liballocs_detach_lifetime_policy(proxy_gc_policy_id, proxy->p_ptr);
        PyDict_DelItem(proxy_dict, proxy_key);
    }
    Py_DECREF(proxy_key);
}

// Return NULL or a new reference
ProxyObject *Proxy_GetOrCreateBase(void *addr)
{
    // Prevent recursion inside ourself
    static bool creating_base = false;
    if (creating_base) return NULL;

    struct allocator *allocator;
    const void *alloc_start;
    struct uniqtype *alloc_type;
    struct liballocs_err* err;
    err = __liballocs_get_alloc_info(addr, &allocator, &alloc_start, NULL,
            &alloc_type, NULL);
    if (err || !ALLOCATOR_HANDLE_LIFETIME_INSERT(allocator) || !alloc_type)
    {
        return NULL;
    }

    // Check if already in proxy_dict
    ProxyObject *proxy = proxy_for_addr(alloc_start);
    if (proxy)
    {
        Py_INCREF(proxy);
        return proxy;
    }

    ForeignTypeObject *ftyp = ForeignType_GetOrCreate(alloc_type);
    assert(ftyp);
    if (!ftyp->ft_proxy_type)
    {
        Py_DECREF(ftyp);
        return NULL;
    }
    creating_base = true;
    proxy = (ProxyObject *) ftyp->ft_getfrom((void*) alloc_start, ftyp);
    creating_base = false;
    Py_DECREF(ftyp);
    assert(proxy);

    // Register the base proxy
    Proxy_Register(proxy);

    return proxy;
}

static void proxy_dealloc(ProxyObject *self)
{
    Proxy_Unregister(self);

    // Notify deletion of the reference
    proxy_delref(NULL, (const void **) &self->p_ptr);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static int proxy_is_gc(ProxyObject *self)
{
    // Should only be called for GC'd subtypes
    assert(PyType_IS_GC(Py_TYPE(self)));

    // We are GC iff we are a registered base proxy.
    PyObject *proxy_key = PyLong_FromVoidPtr(self->p_ptr);
    PyObject *proxy_ptr = PyDict_GetItem(proxy_dict, proxy_key);
    int res = proxy_ptr && PyLong_AsVoidPtr(proxy_ptr) == self;
    Py_DECREF(proxy_key);
    return res;
}

// TODO: Add Python GC management
PyTypeObject Proxy_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.Proxy",
    .tp_doc = "Base type for foreign proxy objects",
    .tp_basicsize = sizeof(ProxyObject),
    .tp_itemsize = 0, // Size of the underlying object
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_dealloc = (destructor) proxy_dealloc,
    .tp_is_gc = (inquiry) proxy_is_gc,
};

#define PyObject_MaybeGC_New(TYPE, typobj) \
    (PyType_IS_GC(typobj) ? PyObject_GC_New(TYPE, typobj) : PyObject_New(TYPE, typobj))

PyObject *Proxy_GetFrom(void *data, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;

    ProxyObject *base_proxy = Proxy_GetOrCreateBase(data);
    if (base_proxy && Py_TYPE(base_proxy) == type->ft_proxy_type &&
            base_proxy->p_ptr == data)
    {
        // We can reuse the base_proxy as the result
        return (PyObject *) base_proxy;
    }

    ProxyObject *obj = PyObject_MaybeGC_New(ProxyObject, proxy_type);
    if (obj)
    {
        if (base_proxy) Proxy_AddRefTo(base_proxy, (const void **) &obj->p_ptr);
        obj->p_ptr = data;
    }
    Py_XDECREF(base_proxy);
    return (PyObject *) obj;
}

PyObject *Proxy_CopyFrom(void *data, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    ProxyObject *obj = PyObject_MaybeGC_New(ProxyObject, proxy_type);
    if (obj)
    {
        obj->p_ptr = malloc(UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        __liballocs_set_alloc_type(obj->p_ptr, type->ft_type);
        // Should memcpy copy type information ?
        memcpy(obj->p_ptr, data, UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        Proxy_Register(obj);
        free(obj->p_ptr); // Release the manual allocation
    }
    return (PyObject *) obj;
}

int Proxy_StoreInto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    if (PyObject_TypeCheck(obj, proxy_type))
    {
        ProxyObject *proxy = (ProxyObject *) obj;
        memcpy(dest, proxy->p_ptr, UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        return 0;
    }

    // We must try to convert the given object to the requested foreign
    // representation whenever possible
    PyObject *tmpobj = NULL;
    if (type->ft_constructor) tmpobj = type->ft_constructor(obj, NULL, type);
    if (tmpobj)
    {
        ProxyObject *proxy = (ProxyObject *) tmpobj;
        memcpy(dest, proxy->p_ptr, UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        Py_DECREF(tmpobj);
        return 0;
    }
    else
    {
        if (!PyErr_Occurred())
        {
            PyErr_Format(PyExc_TypeError, "expected value of type %s, got %s",
                proxy_type->tp_name, Py_TYPE(obj)->tp_name);
        }
        return -1;
    }
}

void *Proxy_GetDataPtr(PyObject *obj, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    if (!PyObject_TypeCheck(obj, proxy_type)) return NULL;
    return ((ProxyObject *) obj)->p_ptr;
}

int Proxy_ClearRef(PyObject *object, void *arg)
{
    // This function should never be called, instrad Proxy_TraverseRef
    // should detect its address and remove the reference.
    abort();
}

int Proxy_TraverseRef(void *data, visitproc visit, void *arg, ForeignTypeObject* type)
{
    // Find the base proxy referenced by the data pointer, if any
    PyObject *key = PyLong_FromVoidPtr(data);
    if (visit == Proxy_ClearRef)
    {
        proxy_delref(NULL, (const void **) data);
        return 0;
    }
    PyObject *target_base_proxy = PyDict_GetItem(proxy_pointing_addr_dict, key);
    Py_DECREF(key);
    Py_VISIT(target_base_proxy);
    return 0;
}

// Steals reference to proxytype
ForeignTypeObject *Proxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype)
{
    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_proxy_type = proxytype;
    ftype->ft_constructor = NULL;
    ftype->ft_getfrom = Proxy_GetFrom;
    ftype->ft_copyfrom = Proxy_CopyFrom;
    ftype->ft_storeinto = Proxy_StoreInto;
    ftype->ft_getdataptr = Proxy_GetDataPtr;
    ftype->ft_traverse = NULL;
    return ftype;
}
