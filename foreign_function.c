#include "foreign_library.h"
#include <liballocs.h>
#include <ffi.h>
#include <dwarf.h>

typedef struct {
    PyObject_HEAD
    const char *ff_symname;
    void *ff_funptr;
    const struct uniqtype *ff_type;
    ffi_cif *ff_cif;
    // Ensure that the library is never unloaded while the function is callable
    PyObject *ff_dlloader;
} ForeignFunctionObject;

static void free_ffi_type_arr(ffi_type **arr);
static void free_ffi_type(ffi_type *typ)
{
    if (typ->type == FFI_TYPE_STRUCT)
    {
        free_ffi_type_arr(typ->elements);
        PyMem_Free(typ);
    }
}
static void free_ffi_type_arr(ffi_type **arr)
{
    if (!arr) return;
    for (int i = 0; arr[i]; ++i) free_ffi_type(arr[i]);
    PyMem_Free(arr);
}

// The caller must call free_ffi_type on the result to free the returned type
static ffi_type *ffi_type_for_uniqtype(struct uniqtype *type)
{
    switch (type->un.info.kind)
    {
        case VOID:
            return &ffi_type_void;
        case ARRAY:
        case ADDRESS:
        case SUBPROGRAM:
            return &ffi_type_pointer;
        case ENUMERATION:
            // get the base type for the enumeration
            type = type->related[0].un.t.ptr;
            // fall through
        case BASE:
        {
            unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
            switch (type->un.base.enc)
            {
                case DW_ATE_boolean:
                    return &ffi_type_uint8;
                case DW_ATE_address:
                case DW_ATE_unsigned:
                case DW_ATE_unsigned_char:
                    if (size == 1) return &ffi_type_uint8;
                    if (size == 2) return &ffi_type_uint16;
                    if (size == 4) return &ffi_type_uint32;
                    if (size == 8) return &ffi_type_uint64;
                    return NULL;
                case DW_ATE_signed:
                case DW_ATE_signed_char:
                    if (size == 1) return &ffi_type_sint8;
                    if (size == 2) return &ffi_type_sint16;
                    if (size == 4) return &ffi_type_sint32;
                    if (size == 8) return &ffi_type_sint64;
                    return NULL;
                case DW_ATE_float:
                    if (size == 4) return &ffi_type_float;
                    if (size == 8) return &ffi_type_double;
                    if (size == 16) return &ffi_type_longdouble;
                    return NULL;
#ifdef FFI_TARGET_HAS_COMPLEX_TYPE
                case DW_ATE_complex_float:
                    if (size == 8) return &ffi_type_complex_float;
                    if (size == 16) return &ffi_type_complex_double;
                    if (size == 32) return &ffi_type_complex_longdouble;
                    return NULL;
#endif
                default:
                    return NULL;
            }
        }
        case COMPOSITE:
        {
            // We must compute and store the ffi_type's of the struct fields
            // This is required to comply with complicated ABI rules like in
            // AMD64 ELF ABI.

            int nb_fields = type->un.composite.nmemb;
            ffi_type **field_ffi_types = PyMem_Malloc((1+nb_fields) * sizeof(ffi_type *));
            for (int i = 0; i < nb_fields; ++i)
            {
                field_ffi_types[i] = ffi_type_for_uniqtype(type->related[i].un.t.ptr);
                if (!field_ffi_types[i])
                {
                    free_ffi_type_arr(field_ffi_types);
                    return NULL;
                }
            }
            field_ffi_types[nb_fields] = NULL;

            ffi_type *ffityp = PyMem_Malloc(sizeof(ffi_type));
            *ffityp = (ffi_type){
                .type = FFI_TYPE_STRUCT,
                .elements = field_ffi_types,
            };

            // v The line below requires a recent version of libffi v
            //if (ffi_get_struct_offsets(FFI_DEFAULT_ABI, ffityp) != FFI_OK)
            ffi_cif dummy;
            if (ffi_prep_cif(&dummy, FFI_DEFAULT_ABI, 0, ffityp, NULL) != FFI_OK)
            {
                free_ffi_type(ffityp);
                return NULL;
            }

            // We may have to patch size and alignment to handle unions
            if (ffityp->size != UNIQTYPE_SIZE_IN_BYTES(type))
            {
                ffityp->size = UNIQTYPE_SIZE_IN_BYTES(type);
                // Be conservative about alignment if size is modified
                for (int i = 0; i < nb_fields; ++i)
                {
                    if (field_ffi_types[i]->alignment > ffityp->alignment)
                    {
                        ffityp->alignment = field_ffi_types[i]->alignment;
                    }
                }
            }

            return ffityp;
        }
        case SUBRANGE:
        default:
            return NULL; // not handled
    }
}

/* foreignfun_setup_cif returns a negative value and sets a Python exception
 * on failure */
static int foreignfun_setup_cif(ForeignFunctionObject *self)
{
    if (self->ff_cif) return 0;
    const struct uniqtype *type = self->ff_type;

    if (type->un.subprogram.nret != 1)
    {
        PyErr_SetString(PyExc_ImportError, "Foreign functions not having exactly one return value are not supported");
        return -1;
    }
    ffi_type *ret_type = ffi_type_for_uniqtype(type->related[0].un.t.ptr);
    if (!ret_type)
    {
        PyErr_SetString(PyExc_ImportError, "Cannot get ABI encoding for return value");
        free_ffi_type(ret_type);
        return -1;
    }

    ffi_type **arg_types = NULL;
    unsigned narg = type->un.subprogram.narg;
    if (narg > 0)
    {
        arg_types = PyMem_Malloc((1+narg) * sizeof(ffi_type *));
        arg_types[narg] = NULL;
    }
    for (int i = 0 ; i < narg ; ++i)
    {
        arg_types[i] = ffi_type_for_uniqtype(type->related[i+1].un.t.ptr);
        if (!arg_types[i])
        {
            PyErr_Format(PyExc_ImportError, "Cannot get ABI encoding for argument %d", i);
            free_ffi_type(ret_type);
            free_ffi_type_arr(arg_types);
            return -1;
        }
    }

    ffi_cif *cif = PyMem_Malloc(sizeof(ffi_cif));
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, narg, ret_type, arg_types) == FFI_OK)
    {
        self->ff_cif = cif;
        return 0;
    }
    else
    {
        PyMem_Free(cif);
        free_ffi_type(ret_type);
        free_ffi_type_arr(arg_types);
        PyErr_Format(PyExc_ImportError, "Failure in call information initialization for '%s'", self->ff_symname);
        return -1;
    }
}

static PyObject *foreignfun_call(ForeignFunctionObject *self, PyObject *args, PyObject *kwds)
{
    if (foreignfun_setup_cif(self) < 0) return NULL;
    unsigned narg = self->ff_type->un.subprogram.narg;

    if (PySequence_Fast_GET_SIZE(args) != narg)
    {
        PyErr_Format(PyExc_TypeError,
                     "'%s' takes exactly %d argument%s (%d given)",
                     self->ff_symname, narg, narg == 1 ? "" : "s",
                     PySequence_Fast_GET_SIZE(args));
        return NULL;
    }

    unsigned argsize = 0;
    for (int i = 0 ; i < narg ; ++i)
    {
        const struct uniqtype *arg_type = self->ff_type->related[i+1].un.t.ptr;
        argsize += UNIQTYPE_SIZE_IN_BYTES(arg_type);
    }

    // Using libffi to make calls is probably highly inefficient as each
    // argument will be pushed to the stack twice.
    void *ff_args[narg];
    char argvals[argsize];
    void *cur_arg = argvals;
    for (int i = 0 ; i < narg ; ++i)
    {
        // TODO: Use kwds arguments too
        PyObject *py_arg = PySequence_Fast_GET_ITEM(args, i);

        const struct uniqtype *arg_type = self->ff_type->related[i+1].un.t.ptr;
        if (store_pyobject_as_type(py_arg, cur_arg, arg_type))
        {
            return NULL;
        }
        ff_args[i] = cur_arg;
        cur_arg += UNIQTYPE_SIZE_IN_BYTES(arg_type);
    }

    const struct uniqtype *ret_type = self->ff_type->related[0].un.t.ptr;
    unsigned retsize = UNIQTYPE_SIZE_IN_BYTES(ret_type);
    // Return values can be widened by libffi up to sizeof(ffi_arg)
    if (sizeof(ffi_arg) > retsize) retsize = sizeof(ffi_arg);
    char retval[retsize];

    ffi_call(self->ff_cif, self->ff_funptr, retval, ff_args);

    PyObject *typdict = ForeignLibraryLoader_GetUniqtypeDict(self->ff_dlloader);

    // FIXME: On big-endian architectures, we need to shift retval pointer if
    // it has been widened by libffi. For the moment assume we are little-endian
    return pyobject_from_type(retval, ret_type, typdict);
}

static PyObject *foreignfun_repr(ForeignFunctionObject *self)
{
    const struct uniqtype *type = self->ff_type;

    int nret = type->un.subprogram.nret;
    int narg = type->un.subprogram.narg;

    PyObject *ret_list = PyList_New(0);
    PyObject *arg_list = PyList_New(0);
    for (int a = 0; a < nret + narg ; ++a)
    {
        struct uniqtype *argtype = type->related[a].un.t.ptr;
        const char *arg_name;
        if (argtype) arg_name = __liballocs_uniqtype_name(argtype);
        else arg_name = "<unknown>";
        PyObject *arg_name_obj = PyUnicode_FromString(arg_name);
        PyList_Append(a < nret ? ret_list : arg_list, arg_name_obj);
        Py_DECREF(arg_name_obj);
    }
    PyObject *sep = PyUnicode_FromString(", ");
    PyObject *ret_str = PyUnicode_Join(sep, ret_list);
    Py_DECREF(ret_list);
    PyObject *arg_str = PyUnicode_Join(sep, arg_list);
    Py_DECREF(arg_list);
    Py_DECREF(sep);

    PyObject *funsig = PyUnicode_FromFormat("<foreign function '%U %s(%U)' at %p>", ret_str,
        self->ff_symname, arg_str, self->ff_funptr);

    Py_DECREF(ret_str);
    Py_DECREF(arg_str);

    return funsig;
}

static void foreignfun_dealloc(ForeignFunctionObject *self)
{
    Py_DECREF(self->ff_dlloader);
    if (self->ff_cif)
    {
        free_ffi_type(self->ff_cif->rtype);
        free_ffi_type_arr(self->ff_cif->arg_types);
        PyMem_Free(self->ff_cif);
    }
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyTypeObject ForeignFunction_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignFunction",
    .tp_doc = "Callable function from a foreign library",
    .tp_basicsize = sizeof(ForeignFunctionObject),
    .tp_itemsize = 0,
    .tp_repr = (reprfunc) foreignfun_repr,
    .tp_call = (ternaryfunc) foreignfun_call,
    .tp_dealloc = (destructor) foreignfun_dealloc,
};

PyObject *ForeignFunction_New(const char *symname, void *funptr, PyObject *dlloader)
{
    const struct uniqtype *type = __liballocs_get_alloc_type(funptr);
    if (!type || !UNIQTYPE_IS_SUBPROGRAM_TYPE(type))
    {
        PyErr_Format(PyExc_ImportError, "Retrieving type information for foreign function '%s' failed", symname);
        return NULL;
    }

    ForeignFunctionObject *obj;
    obj = PyObject_New(ForeignFunctionObject, &ForeignFunction_Type);
    obj->ff_symname = symname;
    obj->ff_funptr = funptr;
    obj->ff_type = type;
    obj->ff_cif = NULL;
    Py_INCREF(dlloader);
    obj->ff_dlloader = dlloader;
    return (PyObject *) obj;
}

// Warning : This function does not typecheck the given object
const struct uniqtype *ForeignFunction_GetType(PyObject *func)
{
    ForeignFunctionObject *ffobj = (ForeignFunctionObject *) func;
    return ffobj->ff_type;
}
