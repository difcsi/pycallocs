#include "foreign_library.h"

static PyObject *foreigntype_call(ForeignTypeObject *self, PyObject *args, PyObject *kwargs)
{
    if (!self->ft_constructor)
    {
        PyErr_SetString(PyExc_TypeError, "Cannot build data of this type in Python");
        return NULL;
    }

    return PyObject_Call(self->ft_constructor, args, kwargs);
}

static PyObject *foreigntype_repr(ForeignTypeObject *self)
{
    return PyUnicode_FromFormat("<foreign type '%s'>", UNIQTYPE_NAME(self->ft_type));
}

static void foreigntype_dealloc(ForeignTypeObject *self)
{
    Py_XDECREF(self->ft_constructor);
}

static PyObject *foreigntype_ptr(ForeignTypeObject *self)
{
    Py_RETURN_NOTIMPLEMENTED;
}

static PyObject *foreigntype_array(ForeignTypeObject *self, PyObject *arg)
{
    Py_RETURN_NOTIMPLEMENTED;
}

static PyMethodDef foreigntype_methods[] = {
    {"ptr", (PyCFunction) foreigntype_ptr, METH_NOARGS,
        "Get the type of pointers to the current type."},
    {"array", (PyCFunction) foreigntype_array, METH_O,
        "Get the type of an array of n elements of the current type, "
        "where n is the single parameter."},
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
    .tp_methods = foreigntype_methods,
};

PyObject *void_getfrom(void *data, ForeignTypeObject *type, PyObject *allocator)
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
    switch (type->un.info.kind)
    {
        case VOID:
        {
            ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
            ftype->ft_type = type;
            ftype->ft_constructor = NULL;
            ftype->ft_getfrom = void_getfrom;
            ftype->ft_storeinto = void_storeinto;
            return ftype;
        }
        case BASE:
            return ForeignBaseType_New(type);
        case COMPOSITE:
            return ForeignComposite_NewType(type);
        case SUBPROGRAM:
            return ForeignFunction_NewType(type);
        case ADDRESS:
        case ARRAY:
        case ENUMERATION:
        case SUBRANGE:
        default:
            PyErr_Format(PyExc_ImportError,
                    "Cannot create foreign type for '%s'", UNIQTYPE_NAME(type));
            return NULL; // Not handled
    }
}

// Returns a new reference
ForeignTypeObject *ForeignType_GetOrCreate(const struct uniqtype *type)
{
    // This function makes the assumption that uniqtype's have infinite lifetime
    // This may not be the case in real world...

    static PyObject *typdict = NULL;
    if (!typdict) typdict = PyDict_New();

    PyObject *typkey = PyLong_FromVoidPtr((void *) type);
    PyObject *ptype = PyDict_GetItem(typdict, typkey);
    if (ptype) Py_INCREF(ptype);
    else
    {
        ptype = (PyObject *) ForeignType_New(type);
        if (ptype) PyDict_SetItem(typdict, typkey, ptype);
    }

    Py_DECREF(typkey);
    return (ForeignTypeObject *) ptype;
}

