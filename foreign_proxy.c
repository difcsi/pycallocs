#include "foreign_library.h"

static PyObject *foreignproxy_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    ForeignProxyObject *obj = (ForeignProxyObject *) type->tp_alloc(type, 0);
    if (obj)
    {
        obj->fpo_ptr = PyMem_Malloc(type->tp_itemsize);
        obj->fpo_allocator = (PyObject *) obj;
    }
    return (PyObject *) obj;
}

static void foreignproxy_dealloc(ForeignProxyObject *self)
{
    if (self->fpo_allocator == (PyObject *) self) PyMem_Free(self->fpo_ptr);
    else Py_DECREF(self->fpo_allocator);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyTypeObject ForeignProxy_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignProxy",
    .tp_doc = "Base type for foreign proxy objects",
    .tp_basicsize = sizeof(ForeignProxyObject),
    .tp_itemsize = 0, // Size of the underlying object
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = foreignproxy_new,
    .tp_dealloc = (destructor) foreignproxy_dealloc,
};

static PyObject *foreignproxy_getfrom(void *data, ForeignTypeObject *type, PyObject *allocator)
{
    PyTypeObject *handler_type = (PyTypeObject *) type->ft_constructor;
    ForeignProxyObject *obj = PyObject_New(ForeignProxyObject, handler_type);
    if (obj)
    {
        if (allocator)
        {
            obj->fpo_ptr = data;
            Py_INCREF(allocator);
            obj->fpo_allocator = allocator;
        }
        else
        {
            obj->fpo_ptr = PyMem_Malloc(handler_type->tp_itemsize);
            memcpy(obj->fpo_ptr, data, handler_type->tp_itemsize);
            obj->fpo_allocator = (PyObject *) obj;
        }
    }
    return (PyObject *) obj;
}

static int foreignproxy_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    PyTypeObject *handler_type = (PyTypeObject *) type->ft_constructor;
    if (!PyObject_TypeCheck(obj, handler_type))
    {
        PyErr_Format(PyExc_TypeError, "expected value of type %s, got %s",
        handler_type->tp_name, Py_TYPE(obj)->tp_name);
        return -1;
    }
    ForeignProxyObject *proxy = (ForeignProxyObject *) obj;
    memcpy(dest, proxy->fpo_ptr, handler_type->tp_itemsize);
    return 0;
}

// Steals reference to proxytype
ForeignTypeObject *ForeignProxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype)
{
    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_constructor = (PyObject *) proxytype;
    ftype->ft_getfrom = foreignproxy_getfrom;
    ftype->ft_storeinto = foreignproxy_storeinto;
    return ftype;
}
