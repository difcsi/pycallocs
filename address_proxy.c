#include "foreign_library.h"

typedef struct {
    PyTypeObject tp_base;
    ForeignTypeObject *pointee_type;
} AddressProxyTypeObject;

static PyObject *addrproxytype_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign address proxy types");
    return NULL;
}

PyTypeObject AddressProxy_Metatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.AddressProxyType",
    .tp_doc = "Type for foreign address proxies",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(AddressProxyTypeObject),
    .tp_new = addrproxytype_new,
};

static PyObject *addrproxy_item(ProxyObject *self, Py_ssize_t index)
{
    // TODO: Check array length
    void *item = self->p_ptr + index * Py_TYPE(self)->tp_itemsize;
    ForeignTypeObject *itemtype = ((AddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    return itemtype->ft_getfrom(item, itemtype, self->p_allocator);
}

static int addrproxy_ass_item(ProxyObject *self, Py_ssize_t index, PyObject *value)
{
    // TODO: Check array length
    void *item = self->p_ptr + index * Py_TYPE(self)->tp_itemsize;
    ForeignTypeObject *itemtype = ((AddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    return itemtype->ft_storeinto(value, item, itemtype);
}

PySequenceMethods addrproxy_sequencemethods = {
    .sq_item = (ssizeargfunc) addrproxy_item,
    .sq_ass_item = (ssizeobjargproc) addrproxy_ass_item,
};

PySequenceMethods addrproxy_sequencemethods_readonly = {
    .sq_item = (ssizeargfunc) addrproxy_item,
};

static PyObject *addrproxy_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    // TODO: Find meaningful constructors for pointers
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign addresses");
    return NULL;
}

static PyObject *addrproxy_repr(ProxyObject *self)
{
    // TODO: Show all the elements in case of an array
    PyObject *item_obj = addrproxy_item(self, 0);
    PyObject *item_repr = PyObject_Repr(item_obj);
    Py_DECREF(item_obj);
    PyObject *repr = PyUnicode_FromFormat("<[%U]>", item_repr);
    Py_DECREF(item_repr);
    return repr;
}

static PyObject *addrproxy_getfrom(void *data, ForeignTypeObject *type, PyObject *allocator)
{
    void *ptr = *(void **) data; // Data is an address to a pointer

    // NULL -> None
    if (!ptr) Py_RETURN_NONE;

    ProxyObject *obj = PyObject_New(ProxyObject, type->ft_proxy_type);
    if (obj)
    {
        obj->p_ptr = ptr;
        // TODO: find a safe allocator
        Py_INCREF(Py_None);
        obj->p_allocator = Py_None;
    }
    return (PyObject *) obj;
}

static int addrproxy_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    // None -> NULL
    if (obj == Py_None)
    {
        *(void **) dest = NULL;
    }

    AddressProxyTypeObject *proxy_type = (AddressProxyTypeObject *) type->ft_proxy_type;

    // Check if obj is an address proxy object
    if (PyObject_TypeCheck(obj, (PyTypeObject *) proxy_type))
    {
        *(void **) dest = ((ProxyObject *) obj)->p_ptr;
        return 0;
    }

    // Check if obj is a proxy to the pointee type
    PyTypeObject *pointee_proxy = proxy_type->pointee_type->ft_proxy_type;
    if (pointee_proxy && PyObject_TypeCheck(obj, pointee_proxy))
    {
        *(void **) dest = ((ProxyObject *) obj)->p_ptr;
        return 0;
    }

    // Else type error
    PyErr_Format(PyExc_TypeError, "expected reference to object of type %s, got %s",
        UNIQTYPE_NAME(proxy_type->pointee_type->ft_type), Py_TYPE(obj)->tp_name);
    return -1;
}

ForeignTypeObject *AddressProxy_NewType(const struct uniqtype *type)
{
    const struct uniqtype *pointee_type = type->related[0].un.t.ptr;
    ForeignTypeObject *pointee_ftype = ForeignType_GetOrCreate(pointee_type);

    if (!pointee_ftype) return NULL;

    AddressProxyTypeObject *htype =
        PyObject_GC_NewVar(AddressProxyTypeObject, &AddressProxy_Metatype, 0);
    htype->pointee_type = pointee_ftype;

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base, // <- Keep the object base
        .tp_name = UNIQTYPE_NAME(type),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(pointee_type),
        .tp_base = &Proxy_Type,
        .tp_new = addrproxy_new,
        .tp_repr = (reprfunc) addrproxy_repr,
    };

    if (pointee_type->un.base.kind == COMPOSITE)
    {
        // Direct access to fields without [0] indirection
        htype->tp_base.tp_getset = pointee_ftype->ft_proxy_type->tp_getset;
    }

    if (ForeignType_IsTriviallyCopiable(pointee_ftype))
    {
        htype->tp_base.tp_as_sequence = &addrproxy_sequencemethods;
    }
    else
    {
        // Disable silent copy into the array
        htype->tp_base.tp_as_sequence = &addrproxy_sequencemethods_readonly;
    }

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        Py_DECREF(htype);
        return NULL;
    }

    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_proxy_type = (PyTypeObject *) htype;
    ftype->ft_constructor = NULL;
    ftype->ft_getfrom = addrproxy_getfrom;
    ftype->ft_storeinto = addrproxy_storeinto;
    return ftype;
}
