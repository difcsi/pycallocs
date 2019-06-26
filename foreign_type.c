#include "foreign_library.h"

static PyObject *foreigntype_call(ForeignTypeObject *self, PyObject *args, PyObject *kwargs)
{
    if (!self->ft_constructor)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot build data of this type in Python");
        return NULL;
    }

    return self->ft_constructor(args, kwargs, self);
}

static PyObject *foreigntype_repr(ForeignTypeObject *self)
{
    return PyUnicode_FromFormat("<foreign type '%s'>", UNIQTYPE_NAME(self->ft_type));
}

static void foreigntype_dealloc(ForeignTypeObject *self)
{
    Py_XDECREF(self->ft_proxy_type);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *foreigntype_ptr(ForeignTypeObject *self)
{
    const struct uniqtype *ptrtype = __liballocs_get_or_create_address_type(self->ft_type);
    return (PyObject *) ForeignType_GetOrCreate(ptrtype);
}

static PyObject *foreigntype_array(ForeignTypeObject *self)
{
    const struct uniqtype *arrtype =
        __liballocs_get_or_create_array_type((struct uniqtype*) self->ft_type, 0);
    if (!arrtype)
    {
        PyErr_Format(PyExc_ValueError, "Cannot create array of type '%s'",
                UNIQTYPE_NAME(self->ft_type));
        return NULL;
    }
    return (PyObject *) ForeignType_GetOrCreate(arrtype);
}

static PyGetSetDef foreigntype_getters[] = {
    {"ptr", (getter) foreigntype_ptr, NULL,
        "Get the type of pointers to the current type.", NULL},
    {"array", (getter) foreigntype_array, NULL,
        "Get the type of arrays to the current type, "
        "size is fixed at object construction.", NULL},
    {NULL}
};

PyTypeObject ForeignType_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignType",
    .tp_doc = "Foreign datatype",
    .tp_basicsize = sizeof(ForeignTypeObject),
    .tp_call = (ternaryfunc) foreigntype_call,
    .tp_repr = (reprfunc) foreigntype_repr,
    .tp_dealloc = (destructor) foreigntype_dealloc,
    .tp_getset = foreigntype_getters,
};

PyObject *void_getfrom(void *data, ForeignTypeObject *type)
{
    Py_RETURN_NONE;
}
int void_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    if (obj != Py_None)
    {
        PyErr_SetString(PyExc_TypeError, "The only accepted value for void foreign type is None");
        return -1;
    }
    return 0;
}

// Call the good specialization of ForeignType
static ForeignTypeObject *ForeignType_New(const struct uniqtype *type)
{
    if (!type)
    {
        PyErr_Format(PyExc_SystemError, "Called ForeignType_New with NULL parameter");
        return NULL;
    }
    switch (UNIQTYPE_KIND(type))
    {
        case VOID:
        {
            ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
            ftype->ft_type = type;
            ftype->ft_proxy_type = NULL;
            ftype->ft_constructor = NULL;
            ftype->ft_getfrom = void_getfrom;
            ftype->ft_copyfrom = void_getfrom;
            ftype->ft_storeinto = void_storeinto;
            return ftype;
        }
        case BASE:
            return ForeignBaseType_New(type);
        case COMPOSITE:
            return CompositeProxy_NewType(type);
        case SUBPROGRAM:
            return FunctionProxy_NewType(type);
        case ADDRESS:
            return AddressProxy_NewType(type);
        case ARRAY:
            return ArrayProxy_NewType(type);
        case ENUMERATION:
        case SUBRANGE:
        default:
            PyErr_Format(PyExc_ImportError,
                    "Cannot create foreign type for '%s'", UNIQTYPE_NAME(type));
            return NULL; // Not handled
    }
}

static void ForeignType_Init(ForeignTypeObject *self, const struct uniqtype *type)
{
    switch (UNIQTYPE_KIND(type))
    {
        case COMPOSITE:
            CompositeProxy_InitType(self, type);
            break;
        case ADDRESS:
            AddressProxy_InitType(self, type);
            break;
        case ARRAY:
            ArrayProxy_InitType(self, type);
            break;
        default:
            break;
    }
}

// Returns a new reference
ForeignTypeObject *ForeignType_GetOrCreate(const struct uniqtype *type)
{
    // This function makes the assumption that uniqtype's have infinite lifetime
    // Our ForeignTypeObject's have too (no GC and storage in a static table)

    static PyObject *typdict = NULL;
    if (!typdict) typdict = PyDict_New();

    PyObject *typkey = PyLong_FromVoidPtr((void *) type);
    PyObject *ptype = PyDict_GetItem(typdict, typkey);
    if (ptype) Py_INCREF(ptype);
    else
    {
        ptype = (PyObject *) ForeignType_New(type);
        if (ptype)
        {
            PyDict_SetItem(typdict, typkey, ptype);
            ForeignType_Init((ForeignTypeObject *) ptype, type);
        }
    }
    Py_DECREF(typkey);

    return (ForeignTypeObject *) ptype;
}

bool ForeignType_IsTriviallyCopiable(const ForeignTypeObject *type)
{
    switch (UNIQTYPE_KIND(type->ft_type))
    {
        case COMPOSITE:
        case ARRAY:
        case SUBPROGRAM:
            return false;
        default:
            return true;
    }
}
