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
    return repr;
}

// Compute the length of the pointed array (= 1 for pointer to single cell)
static void addrproxy_initlength(AddressProxyObject *self, ForeignTypeObject *type)
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

    // Try to return directly the base proxy when not breaking the semantics
    // This means we can return something that is not an address proxy if it
    // would be meaningless to do indexing anyway (e.g. because we get
    // something that does not have the exact expected type)
    ProxyObject *base_proxy = Proxy_GetOrCreateBase(ptr);
    if (base_proxy)
    {
        AddressProxyTypeObject *ptype = (AddressProxyTypeObject *) type->ft_proxy_type;
        if (ptype->pointee_type->ft_type == &__uniqtype__void)
        {
            // We are decoding a pointer to void, always return the base proxy
            // when there is one.
            return (PyObject *) base_proxy;
        }

        // Check if the type of the base proxy is compatible
        PyTypeObject *pointee_ptype = ptype->pointee_type->ft_proxy_type;
        if (pointee_ptype && PyObject_TypeCheck(base_proxy, pointee_ptype))
        {
            // We trust liballocs for returning array base types everywhere it
            // is meaningful.
            // In practice, we observe that it returns arrays including in
            // places where there is always a single value.
            return (PyObject *) base_proxy;
        }
    }

    AddressProxyObject *obj = PyObject_GC_New(AddressProxyObject, type->ft_proxy_type);
    if (obj)
    {
        // Extend lifetime of the pointed object
        if (base_proxy)
        {
            Proxy_AddRefTo(base_proxy, (const void **) &obj->p_base.p_ptr);
            Py_DECREF(base_proxy);
        }

        obj->p_base.p_ptr = ptr;
        addrproxy_initlength(obj, type);
    }
    return (PyObject *) obj;
}

// Function prototype usually found in liballocs_private.h, or added by the
// trapptrwrites CIL pass. Here we have none of these available.
void __notify_ptr_write(const void **dest, const void *val);

static _Bool addrproxy_typecheck(PyObject *obj, ForeignTypeObject *type)
{
    AddressProxyTypeObject *proxy_type = (AddressProxyTypeObject *) type->ft_proxy_type;

    // Check if obj is an address proxy object with the exact same pointee type
    if (PyObject_TypeCheck(obj, (PyTypeObject *) proxy_type)) return 1;

    // Check if obj is a proxy to the pointee type (or a "parent" object of it)
    PyTypeObject *pointee_proxy = proxy_type->pointee_type->ft_proxy_type;
    if (pointee_proxy && PyObject_TypeCheck(obj, pointee_proxy))
    {
        // Accept everything except arrays (arrays with exact same type are
        // accepted by the first check).
        // FIXME: What we actually want to check is if the data will be used
        // as a pointer to array, not if the argument itself is an array.
        // But we do not have any clue about this (yet).
        PyTypeObject *objtyp = Py_TYPE(obj);
        if (Py_TYPE(objtyp) != &AddressProxy_Metatype) return 1;
        if (((AddressProxyObject *) obj)->ap_length == 1) return 1;
    }

    if (proxy_type->pointee_type->ft_type == &__uniqtype__void)
    {
        // We are a pointer to void: accept anything under a proxy
        if (PyObject_TypeCheck(obj, &Proxy_Type)) return 1;
    }

    // Else typecheck has failed
    return 0;
}

static int addrproxy_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    // None -> NULL
    if (obj == Py_None)
    {
        __notify_ptr_write((const void **) dest, NULL);
        *(void **) dest = NULL;
        return 0;
    }

    if (!addrproxy_typecheck(obj, type))
    {
        // TODO: We might want to try to convert the data here but we need to
        // handle stack GC first or things will go horribly wrong.
        PyErr_Format(PyExc_TypeError, "expected reference to object of type %s, got %s",
            UNIQTYPE_NAME(UNIQTYPE_POINTEE_TYPE(type->ft_type)), Py_TYPE(obj)->tp_name);
        return -1;
    }

    __notify_ptr_write((const void **) dest, ((ProxyObject *) obj)->p_ptr);
    *(void **) dest = ((ProxyObject *) obj)->p_ptr;
    return 0;
}

static void *addrproxy_getdataptr(PyObject *obj, ForeignTypeObject *type)
{
    // None -> NULL
    if (obj == Py_None)
    {
        static void *nullptr = 0;
        return &nullptr;
    }

    if (addrproxy_typecheck(obj, type)) return &((ProxyObject *) obj)->p_ptr;
    else return NULL;
}

static int addrproxy_traverse(AddressProxyObject *self, visitproc visit, void *arg)
{
    ForeignTypeObject *elem_type = ((AddressProxyTypeObject *) Py_TYPE(self))->pointee_type;
    // Check if the elem type must be traversed
    if (!elem_type->ft_traverse) return 0;

    for (int i = 0 ; i < self->ap_length ; ++i)
    {
        void *item = self->p_base.p_ptr + i * Py_TYPE(self)->tp_itemsize;
        int vret = elem_type->ft_traverse(item, visit, arg, elem_type);
        if (vret) return vret;
    }
    return 0;
}

static int addrproxy_clear(AddressProxyObject *self)
{
    return addrproxy_traverse(self, Proxy_ClearRef, NULL);
}

ForeignTypeObject *AddressProxy_NewType(const struct uniqtype *type)
{
    const struct uniqtype *pointee_type = type->related[0].un.t.ptr;
    if (!pointee_type) return NULL;

    AddressProxyTypeObject *htype =
        PyObject_GC_NewVar(AddressProxyTypeObject, &AddressProxy_Metatype, 0);
    if (!htype) return NULL;
    htype->pointee_type = NULL; // Will be filled in InitType phase

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base, // <- Keep the object base
        .tp_name = UNIQTYPE_NAME(type),
        .tp_basicsize = sizeof(AddressProxyObject),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(pointee_type),
        .tp_base = &Proxy_Type,
        .tp_init = (initproc) addrproxy_init,
        .tp_repr = (reprfunc) addrproxy_repr,
        .tp_traverse = (traverseproc) addrproxy_traverse,
        .tp_clear = (inquiry) addrproxy_clear,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    };

    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    if (ftype)
    {
        ftype->ft_type = type;
        ftype->ft_proxy_type = (PyTypeObject *) htype;
        ftype->ft_constructor = NULL;
        ftype->ft_getfrom = addrproxy_getfrom;
        ftype->ft_copyfrom = addrproxy_getfrom;
        ftype->ft_storeinto = addrproxy_storeinto;
        ftype->ft_getdataptr = addrproxy_getdataptr;
        ftype->ft_traverse = Proxy_TraverseRef;
    }
    else PyObject_GC_Del(htype);
    return ftype;
}

void AddressProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type)
{
    AddressProxyTypeObject *htype = (AddressProxyTypeObject *) self->ft_proxy_type;

    const struct uniqtype *pointee_type = type->related[0].un.t.ptr;
    ForeignTypeObject *pointee_ftype = ForeignType_GetOrCreate(pointee_type);
    htype->pointee_type = pointee_ftype;

    if (pointee_ftype)
    {
        if (UNIQTYPE_IS_COMPOSITE_TYPE(pointee_type))
        {
            // Direct access to fields without [0] indirection
            // Say we are a subclass of the pointee type
            htype->tp_base.tp_base = pointee_ftype->ft_proxy_type;
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
    }

    int typready __attribute__((unused)) = PyType_Ready((PyTypeObject *) htype);
    assert(typready == 0);
}

static PyObject *arrayproxy_ctor(PyObject *args, PyObject *kwargs, ForeignTypeObject *type)
{
    // There must be exactly one sequence argument used as the array initializer
    // TODO: Add possibility to create an array specifying size only
    // TODO: What about explicitly sized array types ?
    //       It's currently impossible to name them in Python

    unsigned nargs = PyTuple_GET_SIZE(args);
    if (nargs != 1 || (kwargs && PyDict_Size(kwargs)))
    {
        PyErr_SetString(PyExc_TypeError,
            "Array constructor takes exactly one positional argument");
        return NULL;
    }

    PyObject *arg = PyTuple_GET_ITEM(args, 0);
    Py_ssize_t len = PySequence_Size(arg);
    if (len == -1)
    {
        PyErr_SetString(PyExc_TypeError,
            "Array constructor argument must be a sized sequence");
        return NULL;
    }

    AddressProxyObject *obj = PyObject_GC_New(AddressProxyObject, type->ft_proxy_type);
    if (obj)
    {
        obj->p_base.p_ptr = malloc(len * type->ft_proxy_type->tp_itemsize);
        obj->ap_length = len;

        __liballocs_set_alloc_type(obj->p_base.p_ptr,
            __liballocs_get_or_create_array_type(
                UNIQTYPE_ARRAY_ELEMENT_TYPE(type->ft_type), len));

        Proxy_Register((ProxyObject *) obj);
        free(obj->p_base.p_ptr);

        if (addrproxy_init(obj, args, kwargs) < 0)
        {
            Py_DECREF(obj);
            return NULL;
        }
    }
    return (PyObject *) obj;
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
        else addrproxy_initlength(obj, type);
    }

    return (PyObject *) obj;
}

static int arrayproxy_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    // TODO: Meaningful array copy
    PyErr_SetString(PyExc_TypeError, "Cannot copy full arrays (for now)");
    return -1;
}

static int arrayproxy_traverse(void *data, visitproc visit, void *arg, ForeignTypeObject *type)
{
    // Create a "fake" proxy object on the stack and call addrproxy_traverse
    AddressProxyObject fake_proxy = { {
        PyObject_HEAD_INIT(type->ft_proxy_type)
        .p_ptr = data
    } };

    if (UNIQTYPE_HAS_KNOWN_LENGTH(type->ft_type))
    {
        fake_proxy.ap_length = UNIQTYPE_ARRAY_LENGTH(type->ft_type);
    }
    else addrproxy_initlength(&fake_proxy, type);

    return addrproxy_traverse(&fake_proxy, visit, arg);
}

ForeignTypeObject *ArrayProxy_NewType(const struct uniqtype *type)
{
    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    if (ftype)
    {
        ftype->ft_type = type;
        ftype->ft_proxy_type = NULL; // Will be filled in InitType phase
        ftype->ft_constructor = arrayproxy_ctor;
        ftype->ft_getfrom = arrayproxy_getfrom;
        ftype->ft_copyfrom = NULL;
        ftype->ft_storeinto = arrayproxy_storeinto;
        ftype->ft_getdataptr = NULL;
        ftype->ft_traverse = arrayproxy_traverse;
    }
    return ftype;
}

void ArrayProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type)
{
    const struct uniqtype *elem_type = type->related[0].un.t.ptr;
    const struct uniqtype *addr_type = __liballocs_get_or_create_address_type(elem_type);
    ForeignTypeObject *addr_ftype = ForeignType_GetOrCreate(addr_type);
    if(!addr_ftype) return;

    Py_INCREF(addr_ftype->ft_proxy_type);
    self->ft_proxy_type = addr_ftype->ft_proxy_type;
    Py_DECREF(addr_ftype);
}
