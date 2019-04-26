#include "foreign_library.h"
#include <liballocs.h>
#include <ffi.h>
#include <dwarf.h>

typedef struct {
    PyTypeObject tp_base;
    const struct uniqtype *ff_type;
    ffi_cif *ff_cif;
    ForeignTypeObject **ff_argtypes;
    ForeignTypeObject *ff_rettype;
} ForeignFunctionProxyTypeObject;

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
static ffi_type *ffi_type_for_uniqtype(const struct uniqtype *type)
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

/* foreignfuntype_setup returns a negative value and sets a Python exception
 * on failure */
static int foreignfuntype_setup(ForeignFunctionProxyTypeObject *self)
{
    if (self->ff_cif) return 0;
    const struct uniqtype *type = self->ff_type;

    if (type->un.subprogram.nret != 1)
    {
        PyErr_SetString(PyExc_ImportError, "Foreign functions not having exactly one return value are not supported");
        return -1;
    }
    const struct uniqtype *ret_type = type->related[0].un.t.ptr;
    self->ff_rettype = ForeignType_GetOrCreate(ret_type);
    if (!self->ff_rettype) return -1;
    ffi_type *ffi_ret_type = ffi_type_for_uniqtype(ret_type);
    if (!ffi_ret_type)
    {
        PyErr_SetString(PyExc_ImportError, "Cannot get ABI encoding for return value");
        goto err_rettype;
    }

    ffi_type **ffi_arg_types = NULL;
    unsigned narg = type->un.subprogram.narg;
    if (narg > 0)
    {
        ffi_arg_types = PyMem_Malloc((1+narg) * sizeof(ffi_type *));
        self->ff_argtypes = PyMem_Malloc((1+narg) * sizeof(ForeignTypeObject *));

        ffi_arg_types[narg] = NULL;
        self->ff_argtypes[narg] = NULL;
    }
    for (int i = 0 ; i < narg ; ++i)
    {
        const struct uniqtype *arg_type = type->related[i+1].un.t.ptr;
        ffi_arg_types[i] = ffi_type_for_uniqtype(arg_type);
        self->ff_argtypes[i] = ForeignType_GetOrCreate(arg_type);
        if (!ffi_arg_types[i] || !self->ff_argtypes[i])
        {
            if (!ffi_arg_types[i])
            {
                PyErr_Format(PyExc_ImportError, "Cannot get ABI encoding for argument %d", i);
            }
            ffi_arg_types[i+1] = NULL;
            self->ff_argtypes[i+1] = NULL;
            goto err_argtype;
        }
    }

    ffi_cif *cif = PyMem_Malloc(sizeof(ffi_cif));
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, narg, ffi_ret_type, ffi_arg_types) == FFI_OK)
    {
        self->ff_cif = cif;
        return 0;
    }

    PyErr_Format(PyExc_ImportError, "Failure in call information initialization");
    PyMem_Free(cif);
err_argtype:
    if (narg > 0)
    {
        for (unsigned i = 0; self->ff_argtypes[i]; ++i)
        {
            Py_DECREF(self->ff_argtypes[i]);
        }
        PyMem_Free(self->ff_argtypes);
        free_ffi_type_arr(ffi_arg_types);
    }
    free_ffi_type(ffi_ret_type);
err_rettype:
    Py_DECREF(self->ff_rettype);
    return -1;
}

static PyObject *foreignfuntype_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign function proxy types");
    return NULL;
}

static void foreignfuntype_dealloc(ForeignFunctionProxyTypeObject *self)
{
    if (self->ff_cif)
    {
        free_ffi_type(self->ff_cif->rtype);
        free_ffi_type_arr(self->ff_cif->arg_types);
        PyMem_Free(self->ff_cif);

        for (unsigned i = 0; self->ff_argtypes[i]; ++i)
        {
            Py_DECREF(self->ff_argtypes[i]);
        }
        PyMem_Free(self->ff_argtypes);

        Py_DECREF(self->ff_rettype);
    }
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyTypeObject ForeignFunction_ProxyMetatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignFunctionProxyType",
    .tp_doc = "Metatype for foreign function proxies",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(ForeignFunctionProxyTypeObject),
    .tp_new = foreignfuntype_new,
    .tp_dealloc = (destructor) foreignfuntype_dealloc,
};

static PyObject *foreignfun_call(ForeignProxyObject *self, PyObject *args, PyObject *kwds)
{
    ForeignFunctionProxyTypeObject *type = (ForeignFunctionProxyTypeObject *) Py_TYPE(self);
    if (foreignfuntype_setup(type) < 0) return NULL;

    unsigned narg = type->ff_type->un.subprogram.narg;
    if (PySequence_Fast_GET_SIZE(args) != narg)
    {
        PyErr_Format(PyExc_TypeError,
                     "This function takes exactly %d argument%s (%d given)",
                     narg, narg == 1 ? "" : "s", PySequence_Fast_GET_SIZE(args));
        return NULL;
    }

    unsigned argsize = 0;
    for (int i = 0 ; i < narg ; ++i)
    {
        const struct uniqtype *arg_type = type->ff_type->related[i+1].un.t.ptr;
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

        const struct uniqtype *arg_type = type->ff_type->related[i+1].un.t.ptr;
        ForeignTypeObject *arg_ftype = type->ff_argtypes[i];
        if (arg_ftype->ft_storeinto(py_arg, cur_arg, arg_ftype) < 0)
        {
            return NULL;
        }
        ff_args[i] = cur_arg;
        cur_arg += UNIQTYPE_SIZE_IN_BYTES(arg_type);
    }

    const struct uniqtype *ret_type = type->ff_type->related[0].un.t.ptr;
    ForeignTypeObject *ret_ftype = type->ff_rettype;

    unsigned retsize = UNIQTYPE_SIZE_IN_BYTES(ret_type);
    // Return values can be widened by libffi up to sizeof(ffi_arg)
    if (sizeof(ffi_arg) > retsize) retsize = sizeof(ffi_arg);
    char retval[retsize];

    ffi_call(type->ff_cif, self->fpo_ptr, retval, ff_args);

    // FIXME: On big-endian architectures, we need to shift retval pointer if
    // it has been widened by libffi. For the moment assume we are little-endian
    return ret_ftype->ft_getfrom(retval, ret_ftype, NULL);
}

static PyObject *foreignfun_repr(ForeignProxyObject *self)
{
    ForeignFunctionProxyTypeObject *proxytype = (ForeignFunctionProxyTypeObject *) Py_TYPE(self);
    const struct uniqtype *type = proxytype->ff_type;

    const char *symname = "<unknown>";
    Dl_info dlinfo;
    if(dladdr(self->fpo_ptr, &dlinfo)) symname = dlinfo.dli_sname;

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

    PyObject *funsig = PyUnicode_FromFormat("<foreign function '%U %s(%U)' at %p>",
            ret_str, symname, arg_str, self->fpo_ptr);

    Py_DECREF(ret_str);
    Py_DECREF(arg_str);

    return funsig;
}

static PyObject *foreignfun_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign function");
    return NULL;
}

ForeignTypeObject *ForeignFunction_NewType(const struct uniqtype *type)
{
    ForeignFunctionProxyTypeObject *htype =
        PyObject_GC_New(ForeignFunctionProxyTypeObject, &ForeignFunction_ProxyMetatype);

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base,
        .tp_name = UNIQTYPE_NAME(type), // Maybe find a better name ?
        .tp_base = &ForeignProxy_Type,
        .tp_new = foreignfun_new,
        .tp_call = (ternaryfunc) foreignfun_call,
        .tp_repr = (reprfunc) foreignfun_repr,
    };
    htype->ff_type = type;
    htype->ff_cif = NULL;

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        Py_DECREF(htype);
        return NULL;
    }

    return ForeignProxy_NewType(type, (PyTypeObject *) htype);
}
