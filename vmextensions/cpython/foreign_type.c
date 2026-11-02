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
        __liballocs_get_or_create_flexible_array_type((struct uniqtype*) self->ft_type);
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

static PyObject *foreigntype_fun(ForeignTypeObject *self, PyObject *args)
{
    int nargs = PySequence_Fast_GET_SIZE(args);
    const struct uniqtype *argtypes[nargs];
    for (unsigned i = 0; i < nargs ; ++i)
    {
        PyObject *argtypobj = PySequence_Fast_GET_ITEM(args, i);
        if (!PyObject_TypeCheck(argtypobj, &ForeignType_Type))
        {
            PyErr_SetString(PyExc_TypeError,
                "ForeignType.fun takes only ForeignType arguments");
            return NULL;
        }
        argtypes[i] = ((ForeignTypeObject *)argtypobj)->ft_type;
    }

    const struct uniqtype *funtype =
        __liballocs_get_or_create_subprogram_type((struct uniqtype *) self->ft_type,
                nargs, (struct uniqtype **) argtypes);
    if (!funtype)
    {
        PyErr_Format(PyExc_ValueError, "Failed to create requested function type");
        return NULL;
    }
    return (PyObject *) ForeignType_GetOrCreate(funtype);
}

static PyMethodDef foreigntype_methods[] = {
    {"fun", (PyCFunction) foreigntype_fun, METH_VARARGS,
        "Get a function type with the current type as return type. "
        "All the arguments should be types, and correspond to argument types. "
        "`ret.fun(arg)` is the types of functions taking an argument of type "
        "arg and returning a value of type ret."},
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
    .tp_methods = foreigntype_methods,
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
            return ForeignEnumType_New(type);
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

    // Structural kinds (base, pointer, array) can exist as several identical
    // uniqtype instances in one process: each meta-DSO may carry its own copy,
    // __liballocs_get_or_create_*_type synthesizes more, and symbol uniquing
    // does not always collapse them (observed for __PTR_signed_char$$8: the
    // elflib-side synthesized instance vs. the instance a fixture's meta-DSO
    // references). Their name encodes their structure, so intern them by name;
    // nominal kinds (composites, functions) keep pointer identity.
    PyObject *typkey = NULL;
    switch (UNIQTYPE_KIND(type))
    {
        case BASE: case ADDRESS: case ARRAY:
        {
            const char *name = UNIQTYPE_NAME(type);
            if (name) typkey = PyUnicode_FromString(name);
            break;
        }
        default: break;
    }
    if (!typkey) typkey = PyLong_FromVoidPtr((void *) type);
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

// Construct a one-element "cell" of elem_ftype: a length-1 array proxy backed
// by a fresh allocation of the element type. Array proxies share their proxy
// class with pointers to the same element type, so the cell is accepted
// anywhere C expects a pointer to the element -- the C out-parameter idiom:
//   w = int(); f(w)                      # C: void f(int *)
//   repo = git_repository.ptr(); g(repo) # C: int g(git_repository **)
// After the call, read the result back with w[0] / repo[0].
PyObject *ForeignType_NewCell(ForeignTypeObject *elem_ftype, PyObject *init)
{
    const struct uniqtype *arrtype = __liballocs_get_or_create_flexible_array_type(
            (struct uniqtype *) elem_ftype->ft_type);
    if (!arrtype)
    {
        PyErr_Format(PyExc_TypeError, "cannot create a cell of type '%s'",
                UNIQTYPE_NAME(elem_ftype->ft_type));
        return NULL;
    }
    ForeignTypeObject *arrftype = ForeignType_GetOrCreate(arrtype);
    if (!arrftype) return NULL;
    if (!arrftype->ft_constructor)
    {
        Py_DECREF(arrftype);
        PyErr_Format(PyExc_TypeError, "cell type '%s' is not constructible",
                UNIQTYPE_NAME(arrtype));
        return NULL;
    }
    PyObject *lst = PyList_New(1);
    if (!lst) { Py_DECREF(arrftype); return NULL; }
    Py_INCREF(init);
    PyList_SET_ITEM(lst, 0, init);
    PyObject *ctor_args = PyTuple_Pack(1, lst);
    Py_DECREF(lst);
    PyObject *cell = ctor_args ? arrftype->ft_constructor(ctor_args, NULL, arrftype) : NULL;
    Py_XDECREF(ctor_args);
    Py_DECREF(arrftype);
    return cell;
}

// ft_constructor implementations for scalar and pointer cells. Both accept
// zero args (default-initialise) or one positional arg (initial value), plus
// the "convert mode" calling convention where `args` is a single bare object.
static PyObject *cell_ctor(PyObject *args, PyObject *kwargs,
        ForeignTypeObject *type, PyObject *dflt)
{
    if (kwargs && PyDict_Size(kwargs) > 0)
    {
        PyErr_SetString(PyExc_TypeError, "cell constructor takes no keyword arguments");
        return NULL;
    }
    PyObject *init = NULL;
    if (args && PyTuple_Check(args))
    {
        Py_ssize_t n = PyTuple_GET_SIZE(args);
        if (n > 1)
        {
            PyErr_SetString(PyExc_TypeError, "cell constructor takes at most one argument");
            return NULL;
        }
        if (n == 1) init = PyTuple_GET_ITEM(args, 0);
    }
    else if (args) init = args; // convert mode: a single bare object
    return ForeignType_NewCell(type, init ? init : dflt);
}

PyObject *ForeignType_CellCtorZero(PyObject *args, PyObject *kwargs, ForeignTypeObject *type)
{
    PyObject *zero = PyLong_FromLong(0);
    if (!zero) return NULL;
    PyObject *cell = cell_ctor(args, kwargs, type, zero);
    Py_DECREF(zero);
    return cell;
}

PyObject *ForeignType_CellCtorNull(PyObject *args, PyObject *kwargs, ForeignTypeObject *type)
{
    return cell_ctor(args, kwargs, type, Py_None);
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
