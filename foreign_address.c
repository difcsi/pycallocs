#include "foreign_library.h"

typedef struct {
    PyTypeObject tp_base;
    ForeignTypeObject *pointee_type;
} ForeignAddressProxyTypeObject;

static PyObject *foreignaddrtype_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign address proxy types");
    return NULL;
}

PyTypeObject ForeignAddress_ProxyMetatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignAddressProxyType",
    .tp_doc = "Metatype for foreign address proxies",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(ForeignAddressProxyTypeObject),
    .tp_new = foreignaddrtype_new,
};

static PyObject *foreignaddr_item(ForeignProxyObject *self, Py_ssize_t index)
{
    // TODO: Check array length
    void *item = self->fpo_ptr + index * Py_TYPE(self)->tp_itemsize;
    ForeignTypeObject *itemtype = ((ForeignAddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    return itemtype->ft_getfrom(item, itemtype, self->fpo_allocator);
}

static int foreignaddr_ass_item(ForeignProxyObject *self, Py_ssize_t index, PyObject *value)
{
    // TODO: Check array length
    void *item = self->fpo_ptr + index * Py_TYPE(self)->tp_itemsize;
    ForeignTypeObject *itemtype = ((ForeignAddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    return itemtype->ft_storeinto(value, item, itemtype);
}

PySequenceMethods foreignaddr_sequencemethods = {
    .sq_item = (ssizeargfunc) foreignaddr_item,
    .sq_ass_item = (ssizeobjargproc) foreignaddr_ass_item,
};

PySequenceMethods foreignaddr_sequencemethods_readonly = {
    .sq_item = (ssizeargfunc) foreignaddr_item,
};

static PyObject *foreignaddr_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    // TODO: Find meaningful constructors for pointers
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign addresses");
    return NULL;
}

static PyObject *foreignaddr_repr(ForeignProxyObject *self)
{
    // TODO: Show all the elements in case of an array
    PyObject *item_obj = foreignaddr_item(self, 0);
    PyObject *item_repr = PyObject_Repr(item_obj);
    Py_DECREF(item_obj);
    PyObject *repr = PyUnicode_FromFormat("<[%U]>", item_repr);
    Py_DECREF(item_repr);
    return repr;
}

static PyObject *foreignaddr_getfrom(void *data, ForeignTypeObject *type, PyObject *allocator)
{
    void *ptr = *(void **) data; // Data is an address to a pointer

    // NULL -> None
    if (!ptr) Py_RETURN_NONE;

    PyTypeObject *handler_type = (PyTypeObject *) type->ft_constructor;
    ForeignProxyObject *obj = PyObject_New(ForeignProxyObject, handler_type);
    if (obj)
    {
        obj->fpo_ptr = ptr;
        // TODO: find a safe allocator
        Py_INCREF(Py_None);
        obj->fpo_allocator = Py_None;
    }
    return (PyObject *) obj;
}

static int foreignaddr_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    // None -> NULL
    if (obj == Py_None)
    {
        *(void **) dest = NULL;
    }

    ForeignAddressProxyTypeObject *handler_type = (ForeignAddressProxyTypeObject *) type->ft_constructor;

    // Check if obj is an address proxy object
    if (PyObject_TypeCheck(obj, (PyTypeObject *) handler_type))
    {
        *(void **) dest = ((ForeignProxyObject *) obj)->fpo_ptr;
        return 0;
    }

    // Check if obj is a proxy to the pointee type
    PyObject *pointee_ctor = handler_type->pointee_type->ft_constructor;
    if (PyType_Check(pointee_ctor) && PyType_IsSubtype((PyTypeObject *) pointee_ctor, &ForeignProxy_Type))
    {
        if (PyObject_TypeCheck(obj, (PyTypeObject *) pointee_ctor))
        {
            *(void **) dest = ((ForeignProxyObject *) obj)->fpo_ptr;
            return 0;
        }
    }

    // Else type error
    PyErr_Format(PyExc_TypeError, "expected reference to object of type %s, got %s",
        UNIQTYPE_NAME(handler_type->pointee_type->ft_type), Py_TYPE(obj)->tp_name);
    return -1;
}

ForeignTypeObject *ForeignAddress_NewType(const struct uniqtype *type)
{
    const struct uniqtype *pointee_type = type->related[0].un.t.ptr;
    ForeignTypeObject *pointee_ftype = ForeignType_GetOrCreate(pointee_type);

    ForeignAddressProxyTypeObject *htype =
        PyObject_GC_NewVar(ForeignAddressProxyTypeObject, &ForeignAddress_ProxyMetatype, 0);
    htype->pointee_type = pointee_ftype;

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base, // <- Keep the object base
        .tp_name = UNIQTYPE_NAME(type),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(pointee_type),
        .tp_base = &ForeignProxy_Type,
        .tp_new = foreignaddr_new,
        .tp_repr = (reprfunc) foreignaddr_repr,
    };

    if (pointee_type->un.base.kind == COMPOSITE)
    {
        // Direct access to fields without [0] indirection
        htype->tp_base.tp_getset = ((PyTypeObject *)pointee_ftype->ft_constructor)->tp_getset;
        // Disable silent copy into the array
        htype->tp_base.tp_as_sequence = &foreignaddr_sequencemethods_readonly;
    }
    else
    {
        htype->tp_base.tp_as_sequence = &foreignaddr_sequencemethods;
    }

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        Py_DECREF(htype);
        return NULL;
    }

    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_constructor = (PyObject *) htype;
    ftype->ft_getfrom = foreignaddr_getfrom;
    ftype->ft_storeinto = foreignaddr_storeinto;
    return ftype;
}
