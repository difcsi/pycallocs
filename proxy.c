#include "foreign_library.h"

static PyObject *proxy_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    ProxyObject *obj = (ProxyObject *) type->tp_alloc(type, 0);
    if (obj)
    {
        obj->p_ptr = PyMem_Malloc(type->tp_itemsize);
        obj->p_allocator = (PyObject *) obj;
    }
    return (PyObject *) obj;
}

static void proxy_dealloc(ProxyObject *self)
{
    if (self->p_allocator == (PyObject *) self) PyMem_Free(self->p_ptr);
    else Py_DECREF(self->p_allocator);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyTypeObject Proxy_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.Proxy",
    .tp_doc = "Base type for foreign proxy objects",
    .tp_basicsize = sizeof(ProxyObject),
    .tp_itemsize = 0, // Size of the underlying object
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = proxy_new,
    .tp_dealloc = (destructor) proxy_dealloc,
};

PyObject *Proxy_GetFrom(void *data, ForeignTypeObject *type, PyObject *allocator)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    ProxyObject *obj = PyObject_New(ProxyObject, proxy_type);
    if (obj)
    {
        if (allocator)
        {
            obj->p_ptr = data;
            Py_INCREF(allocator);
            obj->p_allocator = allocator;
        }
        else
        {
            obj->p_ptr = PyMem_Malloc(UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
            memcpy(obj->p_ptr, data, UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
            obj->p_allocator = (PyObject *) obj;
        }
    }
    return (PyObject *) obj;
}

int Proxy_StoreInto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    if (!PyObject_TypeCheck(obj, proxy_type))
    {
        PyErr_Format(PyExc_TypeError, "expected value of type %s, got %s",
            proxy_type->tp_name, Py_TYPE(obj)->tp_name);
        return -1;
    }
    ProxyObject *proxy = (ProxyObject *) obj;
    memcpy(dest, proxy->p_ptr, UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
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
    ftype->ft_storeinto = Proxy_StoreInto;
    return ftype;
}
