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
    unsigned ff_argsize;
} FunctionProxyTypeObject;

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
    switch (UNIQTYPE_KIND(type))
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

/* funproxytype_setup returns a negative value and sets a Python exception
 * on failure */
static int funproxytype_setup(FunctionProxyTypeObject *self)
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
    self->ff_argsize = 0;
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
        if (!self->ff_argtypes[i]->ft_getdataptr)
        {
            // We cannot directly get a data pointer without conversion for
            // this type. Give it some argument stack space.
            self->ff_argsize += UNIQTYPE_SIZE_IN_BYTES(arg_type);
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

static PyObject *funproxytype_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign function proxy types");
    return NULL;
}

static void funproxytype_dealloc(FunctionProxyTypeObject *self)
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

PyTypeObject FunctionProxy_Metatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.FunctionProxyType",
    .tp_doc = "Metatype for foreign function proxies",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(FunctionProxyTypeObject),
    .tp_new = funproxytype_new,
    .tp_dealloc = (destructor) funproxytype_dealloc,
};

static PyObject *funproxy_call(ProxyObject *self, PyObject *args, PyObject *kwds)
{
    FunctionProxyTypeObject *type = (FunctionProxyTypeObject *) Py_TYPE(self);
    if (funproxytype_setup(type) < 0) return NULL;

    unsigned narg = type->ff_type->un.subprogram.narg;
    if (PySequence_Fast_GET_SIZE(args) != narg)
    {
        PyErr_Format(PyExc_TypeError,
                     "This function takes exactly %d argument%s (%d given)",
                     narg, narg == 1 ? "" : "s", PySequence_Fast_GET_SIZE(args));
        return NULL;
    }

    // Using libffi to make calls is probably highly inefficient as some
    // arguments will be pushed to the stack twice.
    void *ff_args[narg];
    char argvals[type->ff_argsize];
    void *cur_arg = argvals;
    for (int i = 0 ; i < narg ; ++i)
    {
        // TODO: Use kwds arguments too
        PyObject *py_arg = PySequence_Fast_GET_ITEM(args, i);

        const struct uniqtype *arg_type = type->ff_type->related[i+1].un.t.ptr;
        ForeignTypeObject *arg_ftype = type->ff_argtypes[i];
        if (arg_ftype->ft_getdataptr)
        {
            void *data_ptr = arg_ftype->ft_getdataptr(py_arg, arg_ftype);
            if (!data_ptr) return NULL;
            ff_args[i] = data_ptr;
        }
        else
        {
            if (arg_ftype->ft_storeinto(py_arg, cur_arg, arg_ftype) < 0)
            {
                return NULL;
            }
            ff_args[i] = cur_arg;
            cur_arg += UNIQTYPE_SIZE_IN_BYTES(arg_type);
        }
    }

    const struct uniqtype *ret_type = type->ff_type->related[0].un.t.ptr;
    ForeignTypeObject *ret_ftype = type->ff_rettype;

    unsigned retsize = UNIQTYPE_SIZE_IN_BYTES(ret_type);
    // Return values can be widened by libffi up to sizeof(ffi_arg)
    if (sizeof(ffi_arg) > retsize) retsize = sizeof(ffi_arg);
    char retval[retsize];

    ffi_call(type->ff_cif, self->p_ptr, retval, ff_args);

    // FIXME: On big-endian architectures, we need to shift retval pointer if
    // it has been widened by libffi. For the moment assume we are little-endian
    return ret_ftype->ft_copyfrom(retval, ret_ftype);
}

static PyObject *funproxy_repr(ProxyObject *self)
{
    FunctionProxyTypeObject *proxytype = (FunctionProxyTypeObject *) Py_TYPE(self);
    const struct uniqtype *type = proxytype->ff_type;

    const char *symname = "<unknown>";
    Dl_info dlinfo;
    if(dladdr(self->p_ptr, &dlinfo)) symname = dlinfo.dli_sname;

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
            ret_str, symname, arg_str, self->p_ptr);

    Py_DECREF(ret_str);
    Py_DECREF(arg_str);

    return funsig;
}

static PyObject *funproxy_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign function");
    return NULL;
}

static PyTypeObject *funproxy_newproxytype(const struct uniqtype *type)
{
    FunctionProxyTypeObject *htype =
        PyObject_GC_New(FunctionProxyTypeObject, &FunctionProxy_Metatype);

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base,
        .tp_name = UNIQTYPE_NAME(type), // Maybe find a better name ?
        .tp_base = &Proxy_Type,
        .tp_new = funproxy_new,
        .tp_call = (ternaryfunc) funproxy_call,
        .tp_repr = (reprfunc) funproxy_repr,
    };
    htype->ff_type = type;
    htype->ff_cif = NULL;

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        Py_DECREF(htype);
        return NULL;
    }

    return (PyTypeObject *) htype;
}

typedef struct {
    ProxyObject ff_base;
    ffi_closure *fc_closure;
    PyObject *fc_callable;
} ClosureProxyObject;

typedef void (*ffi_closure_func)(ffi_cif *, void *, void **, void*);

static void closureproxy_call(ffi_cif *cif, void *ret, void **args, ClosureProxyObject *closure)
{
    // FIXME: Will break if subclassing (but what's the point in doing this anyway...)
    FunctionProxyTypeObject *fun_type =
        (FunctionProxyTypeObject *) Py_TYPE(closure)->tp_base;

    unsigned nargs = cif->nargs;
    PyObject *pargs = PyTuple_New(nargs);
    for (unsigned i = 0; i < nargs; ++i)
    {
        ForeignTypeObject *arg_type = fun_type->ff_argtypes[i];
        // Copy to ensure that we are in the heap (so arguments can live after
        // the function call).
        PyTuple_SET_ITEM(pargs, i, arg_type->ft_copyfrom(args[i], arg_type));
    }

    PyObject *ret_obj = PyObject_CallObject(closure->fc_callable, pargs);

    ForeignTypeObject *ret_type = fun_type->ff_rettype;
    ret_type->ft_storeinto(ret_obj, ret, ret_type);
}

static PyObject *closureproxy_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // FIXME: Will break if subclassing
    FunctionProxyTypeObject *fun_proxy_type =
        (FunctionProxyTypeObject *) type->tp_base;

    if (funproxytype_setup(fun_proxy_type) < 0) return NULL;

    static char *keywords[] = { "callable", NULL };
    PyObject *callable;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", keywords, &callable))
    {
        return NULL;
    }
    if (!PyCallable_Check(callable))
    {
        PyErr_SetString(PyExc_TypeError,
                "Cannot create foreign closure over a non callable object");
        return NULL;
    }

    ClosureProxyObject *obj = (ClosureProxyObject *) type->tp_alloc(type, 0);
    if (obj)
    {
        obj->fc_closure = ffi_closure_alloc(sizeof(ffi_closure), &obj->ff_base.p_ptr);
        Py_INCREF(callable);
        obj->fc_callable = callable;

        if (ffi_prep_closure_loc(obj->fc_closure, fun_proxy_type->ff_cif,
                (ffi_closure_func) closureproxy_call, obj,
                obj->ff_base.p_ptr) != FFI_OK)
        {
            PyErr_SetString(PyExc_ValueError, "Failed to create closure for callable object");
            Py_DECREF(obj);
            return NULL;
        }

        // Register the proxy
        Proxy_Register((ProxyObject *) obj);
        // Do not release the manual allocation because our deallocation is
        // special. If liballocs supports custom deallocator we should use them
        // instead of this workaround.
        // FIXME: Seems that this is not working because closures are not
        // located in the heap by in mmap'ed section.
    }
    return (PyObject *) obj;
}

static void closureproxy_dealloc(ClosureProxyObject *self)
{
    Proxy_Unregister((ProxyObject *) self);

    ffi_closure_free(self->fc_closure);
    Py_DECREF(self->fc_callable);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyTypeObject *closureproxy_newtype(PyTypeObject *funproxytype)
{
    PyTypeObject *clostype = PyObject_GC_New(PyTypeObject, &PyType_Type);

    *clostype = (PyTypeObject){
        .ob_base = clostype->ob_base,
        .tp_name = "<foreign closure type>",
        .tp_basicsize = sizeof(ClosureProxyObject),
        .tp_base = funproxytype,
        .tp_new = closureproxy_new,
        .tp_dealloc = (destructor) closureproxy_dealloc,
        .tp_flags = Py_TPFLAGS_DEFAULT,
    };

    if (PyType_Ready(clostype) < 0)
    {
        Py_DECREF(clostype);
        return NULL;
    }

    return clostype;
}

ForeignTypeObject *FunctionProxy_NewType(const struct uniqtype *type)
{
    PyTypeObject *fun_type = funproxy_newproxytype(type);
    ForeignTypeObject *ftype = Proxy_NewType(type, fun_type);
    ftype->ft_constructor = (PyObject *) closureproxy_newtype(fun_type);
    return ftype;
}
