#include "foreign_library.h"
#include <libcrunch.h>

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

typedef struct {
    ProxyObject p_base;
    Py_ssize_t ap_length;
} AddressProxyObject;

static Py_ssize_t addrproxy_length(AddressProxyObject *self)
{
    return self->ap_length;
}

static PyObject *addrproxy_item(AddressProxyObject *self, Py_ssize_t index)
{
    if (index >= self->ap_length)
    {
        PyErr_SetString(PyExc_IndexError, "address proxy index out of range");
        return NULL;
    }
    void *item = self->p_base.p_ptr + index * Py_TYPE(self)->tp_itemsize;
    ForeignTypeObject *itemtype = ((AddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    return itemtype->ft_getfrom(item, itemtype);
}

static int addrproxy_ass_item(AddressProxyObject *self, Py_ssize_t index, PyObject *value)
{
    if (index >= self->ap_length)
    {
        PyErr_SetString(PyExc_IndexError, "address proxy index out of range");
        return -1;
    }
    void *item = self->p_base.p_ptr + index * Py_TYPE(self)->tp_itemsize;
    ForeignTypeObject *itemtype = ((AddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    return itemtype->ft_storeinto(value, item, itemtype);
}

PySequenceMethods addrproxy_sequencemethods = {
    .sq_length = (lenfunc) addrproxy_length,
    .sq_item = (ssizeargfunc) addrproxy_item,
    .sq_ass_item = (ssizeobjargproc) addrproxy_ass_item,
};

PySequenceMethods addrproxy_sequencemethods_readonly = {
    .sq_length = (lenfunc) addrproxy_length,
    .sq_item = (ssizeargfunc) addrproxy_item,
};

static PyObject *addrproxy_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    // This constructor is not used for pointers but is provided for array types
    // There must be exactly one sequence argument used as the array initializer
    // TODO: Add possibility to create an array specifying size only
    // TODO: What about explicitly sized array types ?
    //       It's currently impossible to name them in Python

    unsigned nargs = PyTuple_GET_SIZE(args);
    if (nargs != 1 || (kwargs && PyDict_Size(kwargs)))
    {
        PyErr_SetString(PyExc_TypeError,
            "AddressProxyObject.__new__ takes exactly one positional argument");
        return NULL;
    }

    PyObject *arg = PyTuple_GET_ITEM(args, 0);
    Py_ssize_t len = PySequence_Size(arg);
    if (len == -1)
    {
        PyErr_SetString(PyExc_TypeError,
            "AddressProxyObject.__new__ argument must be a sized sequence");
        return NULL;
    }

    AddressProxyObject *obj = (AddressProxyObject *) type->tp_alloc(type, 0);
    if (obj)
    {
        obj->p_base.p_ptr = malloc(len * type->tp_itemsize);
        obj->ap_length = len;
        Proxy_Register((ProxyObject *) obj);
        free(obj->p_base.p_ptr);
    }
    return (PyObject *) obj;
}

static int addrproxy_init(AddressProxyObject *self, PyObject *args, PyObject *kwargs)
{
    unsigned nargs = PyTuple_GET_SIZE(args);
    if (nargs > 1 || (kwargs && PyDict_Size(kwargs)))
    {
        PyErr_SetString(PyExc_TypeError,
            "AddressProxyObject.__init__ takes zero or one positional argument");
        return -1;
    }

    // Default initialization => zero initialize the underlying array
    if (nargs == 0)
    {
        memset(self->p_base.p_ptr, 0, self->ap_length * Py_TYPE(self)->tp_itemsize);
        return 0;
    }

    PyObject *arg = PyTuple_GET_ITEM(args, 0);
    PyObject *seqarg = PySequence_Fast(arg, "AddressProxyObject.__init__ argument must be a sequence");
    if (!seqarg) return -1;

    unsigned arglen = PySequence_Fast_GET_SIZE(seqarg);
    if (arglen > self->ap_length)
    {
        PyErr_Format(PyExc_ValueError, "Sequence given is too long "
            "to be stored at this address (only %d items)", self->ap_length);
        Py_DECREF(seqarg);
        return -1;
    }

    ForeignTypeObject *itemtype = ((AddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    unsigned itemsize = Py_TYPE(self)->tp_itemsize;
    for (unsigned i = 0 ; i < arglen ; ++i)
    {
        PyObject *value = PySequence_Fast_GET_ITEM(seqarg, i);
        void *item = self->p_base.p_ptr + i * itemsize;
        if(itemtype->ft_storeinto(value, item, itemtype) < 0)
        {
            Py_DECREF(seqarg);
            return -1;
        }
    }

    // Zero initialize the rest of the array
    if (self->ap_length > arglen)
    {
        void *first_uninit_item = self->p_base.p_ptr + arglen * itemsize;
        memset(first_uninit_item, 0, (self->ap_length - arglen) * itemsize);
    }

    Py_DECREF(seqarg);
    return 0;
}

static PyObject *addrproxy_repr(AddressProxyObject *self)
{
    static _Bool nested_call = 0;
    if (nested_call)
    {
        // Do not print below more than one level of indirection
        // This breaks repr infinite recursion for cyclic structs
        return PyUnicode_FromString("<[...]>");
    }
    nested_call = 1;

    PyObject *repr_acc = PyUnicode_New(0, 0);
    for (int i = 0; i < self->ap_length; ++i)
    {
        PyObject *item_obj = addrproxy_item(self, i);
        PyObject *item_repr = PyObject_Repr(item_obj);
        Py_DECREF(item_obj);
        const char* fmt = i == 0 ? "%U%U" : "%U, %U";
        PyObject *next_repr_acc = PyUnicode_FromFormat(fmt, repr_acc, item_repr);
        Py_DECREF(repr_acc);
        Py_DECREF(item_repr);
        repr_acc = next_repr_acc;
    }

    PyObject *repr = PyUnicode_FromFormat("<[%U]>", repr_acc);
    Py_DECREF(repr_acc);
    nested_call = 0;
    return repr;
}

// Compute the length of the pointed array (= 1 for pointer to single cell)
static void addressproxy_initlength(AddressProxyObject *self, ForeignTypeObject *type)
{
    void *ptr = self->p_base.p_ptr;
    AddressProxyTypeObject *proxy_type = (AddressProxyTypeObject *) type->ft_proxy_type;
    const struct uniqtype *ptyp = proxy_type->pointee_type->ft_type;

    if (UNIQTYPE_SIZE_IN_BYTES(ptyp) == 0 || !UNIQTYPE_HAS_KNOWN_LENGTH(ptyp))
    {
        self->ap_length = 0;
        return;
    }

    __libcrunch_bounds_t bounds = __fetch_bounds_internal(ptr, ptr, ptyp);
    // If ptr is not the base, it is a single element inside a larger array
    if (__libcrunch_get_base(bounds, ptr) == ptr)
    {
        unsigned long byte_size = __libcrunch_get_size(bounds, ptr);
        self->ap_length = byte_size / UNIQTYPE_SIZE_IN_BYTES(ptyp);
    }
    else self->ap_length = 1;
}

static PyObject *addrproxy_getfrom(void *data, ForeignTypeObject *type)
{
    void *ptr = *(void **) data; // Data is an address to a pointer

    // NULL -> None
    if (!ptr) Py_RETURN_NONE;

    AddressProxyObject *obj = PyObject_New(AddressProxyObject, type->ft_proxy_type);
    if (obj)
    {
        // Extend lifetime of the pointed object
        ProxyObject *base_proxy = Proxy_GetOrCreateBase(ptr);
        if (base_proxy)
        {
            Proxy_AddRefTo(base_proxy, (const void **) &obj->p_base.p_ptr);
            Py_DECREF(base_proxy);
        }

        obj->p_base.p_ptr = ptr;
        addressproxy_initlength(obj, type);
    }
    return (PyObject *) obj;
}

// Function prototype usually found in liballocs_private.h, or added by the
// trapptrwrites CIL pass. Here we have none of these available.
void __notify_ptr_write(const void **dest, const void *val);

static int addrproxy_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    // None -> NULL
    if (obj == Py_None)
    {
        __notify_ptr_write((const void **) dest, NULL);
        *(void **) dest = NULL;
        return 0;
    }

    AddressProxyTypeObject *proxy_type = (AddressProxyTypeObject *) type->ft_proxy_type;

    // Check if obj is an address proxy object
    if (PyObject_TypeCheck(obj, (PyTypeObject *) proxy_type))
    {
        __notify_ptr_write((const void **) dest, ((ProxyObject *) obj)->p_ptr);
        *(void **) dest = ((ProxyObject *) obj)->p_ptr;
        return 0;
    }

    // Check if obj is a proxy to the pointee type
    PyTypeObject *pointee_proxy = proxy_type->pointee_type->ft_proxy_type;
    if (pointee_proxy && PyObject_TypeCheck(obj, pointee_proxy))
    {
        __notify_ptr_write((const void **) dest, ((ProxyObject *) obj)->p_ptr);
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
        .tp_basicsize = sizeof(AddressProxyObject),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(pointee_type),
        .tp_base = &Proxy_Type,
        .tp_new = addrproxy_new,
        .tp_init = (initproc) addrproxy_init,
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
        Py_DECREF(pointee_ftype);
        PyObject_GC_Del(htype);
        return NULL;
    }

    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_proxy_type = (PyTypeObject *) htype;
    ftype->ft_constructor = NULL;
    ftype->ft_getfrom = addrproxy_getfrom;
    ftype->ft_copyfrom = addrproxy_getfrom;
    ftype->ft_storeinto = addrproxy_storeinto;
    return ftype;
}

static PyObject *arrayproxy_getfrom(void *data, ForeignTypeObject *type)
{
    AddressProxyObject *obj = (AddressProxyObject *) Proxy_GetFrom(data, type);

    if(obj)
    {
        // If possible take static length information from type
        if (UNIQTYPE_HAS_KNOWN_LENGTH(type->ft_type))
        {
            obj->ap_length = UNIQTYPE_ARRAY_LENGTH(type->ft_type);
        }
        else addressproxy_initlength(obj, type);
    }

    return (PyObject *) obj;
}

static int arrayproxy_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    // TODO: Meaningful array copy
    PyErr_SetString(PyExc_TypeError, "Cannot copy full arrays (for now)");
    return -1;
}

ForeignTypeObject *ArrayProxy_NewType(const struct uniqtype *type)
{
    const struct uniqtype *elem_type = type->related[0].un.t.ptr;
    const struct uniqtype *addr_type = __liballocs_get_or_create_address_type(elem_type);
    ForeignTypeObject *addr_ftype = ForeignType_GetOrCreate(addr_type);
    if(!addr_ftype) return NULL;

    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    Py_INCREF(addr_ftype->ft_proxy_type);
    ftype->ft_proxy_type = addr_ftype->ft_proxy_type;
    Py_INCREF(addr_ftype->ft_proxy_type);
    ftype->ft_constructor = (PyObject *) addr_ftype->ft_proxy_type;
    Py_DECREF(addr_ftype);
    ftype->ft_getfrom = arrayproxy_getfrom;
    ftype->ft_copyfrom = NULL;
    ftype->ft_storeinto = arrayproxy_storeinto;
    return ftype;
}
