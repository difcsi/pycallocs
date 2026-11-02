#include "foreign_library.h"
#include <liballocs.h>
#include <ffi.h>
#include <dwarf.h>
#include <ffitypes.h> /* shared uniqtype -> ffi_type mapping (vmextensions/common) */

typedef struct {
    PyTypeObject tp_base;
    const struct uniqtype *ff_type;
    ffi_cif *ff_cif;
    ForeignTypeObject **ff_argtypes;
    ForeignTypeObject *ff_rettype;
    PyTypeObject *ff_closure_type;
} FunctionProxyTypeObject;

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
        /* plain malloc: released via the common free_ffi_type_arr (free) */
        ffi_arg_types = malloc((1+narg) * sizeof(ffi_type *));
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
    Py_XDECREF(self->ff_closure_type);
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

    // TODO: Handle keywords arguments (need liballocs support)

    // Using libffi to make calls is probably highly inefficient as some
    // arguments will be pushed to the stack twice.
    void *ff_args[narg];
    unsigned argsize = 0;
    for (int i = 0 ; i < narg ; ++i)
    {
        ForeignTypeObject *arg_ftype = type->ff_argtypes[i];
        void *data_ptr = NULL;
        if (arg_ftype->ft_getdataptr)
        {
            PyObject *py_arg = PySequence_Fast_GET_ITEM(args, i);

            data_ptr = arg_ftype->ft_getdataptr(py_arg, arg_ftype);
            ff_args[i] = data_ptr;
        }
        /* The argument was not extractable using getdataptr, therefore allocate
         * stack space for storing it */
        if (!data_ptr) argsize += UNIQTYPE_SIZE_IN_BYTES(arg_ftype->ft_type);
    }

    // Proxies materialised on the fly for plain (non-proxy) arguments that are
    // lazily crossed into C by reference. They are kept alive across ffi_call
    // (their backing C data is what the callee sees) and released afterwards.
    PyObject *arg_tmps[narg];
    for (int i = 0 ; i < narg ; ++i) arg_tmps[i] = NULL;

    char argvals[argsize];
    void *cur_arg = argvals;
    for (int i = 0 ; i < narg ; ++i)
    {
        ForeignTypeObject *arg_ftype = type->ff_argtypes[i];
        if (arg_ftype->ft_getdataptr && ff_args[i]) continue;

        PyObject *py_arg = PySequence_Fast_GET_ITEM(args, i);

        // Lazy proxy translation: a plain Python object passed where a
        // pointer-to-T is expected has no proxy yet. Materialise a backing T,
        // copy the object into it and pass a pointer to it. The temporary proxy
        // is released after the call (its data has been copied into C for the
        // call's duration). We do not use the dest-keyed GC retention here: the
        // argument slot lives on the C stack, so a stack-keyed entry would never
        // be cleared.
        if (UNIQTYPE_KIND(arg_ftype->ft_type) == ADDRESS)
        {
            ProxyObject *conv = AddressProxy_MaterializePointee(py_arg, arg_ftype);
            if (conv)
            {
                arg_tmps[i] = (PyObject *) conv;
                *(void **) cur_arg = conv->p_ptr;
                ff_args[i] = cur_arg;
                cur_arg += UNIQTYPE_SIZE_IN_BYTES(arg_ftype->ft_type);
                continue;
            }
            if (PyErr_Occurred()) goto fail; // conversion attempted and failed
            // else: not materialisable, fall through to ft_storeinto so it can
            // raise the canonical "expected reference to ..." type error.
        }

        if (arg_ftype->ft_storeinto(py_arg, cur_arg, arg_ftype) < 0)
        {
            goto fail;
        }
        ff_args[i] = cur_arg;
        cur_arg += UNIQTYPE_SIZE_IN_BYTES(arg_ftype->ft_type);
    }

    const struct uniqtype *ret_type = type->ff_type->related[0].un.t.ptr;
    ForeignTypeObject *ret_ftype = type->ff_rettype;

    // Scope the retval VLA so its lifetime ends before the `fail:` label;
    // otherwise `goto fail` would jump into the scope of a variably-modified type.
    PyObject *result;
    // Defer GC frees across the call AND the return-value wrapping below: if the
    // callee overwrites a C slot that held the only reference to a chunk it then
    // returns, the delref must not free it before ft_copyfrom re-adopts it
    // (struct_global_swap). Released right after the result is built.
    Proxy_BeginDeferFrees();
    {
        unsigned natret = UNIQTYPE_SIZE_IN_BYTES(ret_type);
        unsigned retsize = natret;
        // Return values can be widened by libffi up to sizeof(ffi_arg)
        if (sizeof(ffi_arg) > retsize) retsize = sizeof(ffi_arg);
        char retval[retsize];

        ffi_call(type->ff_cif, self->p_ptr, retval, ff_args);

        // libffi widens *integral* return values (ints, enums, pointers) smaller
        // than ffi_arg up to a full ffi_arg; floats and by-value aggregates are
        // not widened. On big-endian the meaningful low-order bytes land at the
        // end of that slot, so shift the pointer the wrappers read from.
        char *retp = retval;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        if (natret < sizeof(ffi_arg) &&
            (UNIQTYPE_IS_ENUM_TYPE(ret_type) || UNIQTYPE_IS_POINTER_TYPE(ret_type) ||
             (UNIQTYPE_IS_BASE_TYPE(ret_type) &&
              ret_type->un.base.enc != 0x4 /* DW_ATE_float */)))
        {
            retp += sizeof(ffi_arg) - natret;
        }
#endif
#ifdef PYCALLOCS_HAVE_SPECIALISE
        // Fast path: hand a by-value struct return back as a plain Python object
        // (types.SimpleNamespace) via a specialised T_to_PyObject<T>, the mirror
        // of the by-pointer argument path. rc>0 means unsupported -> fall back to
        // ft_copyfrom (a proxy); rc<0 leaves result NULL with an exception set.
        result = NULL;
        int spec_rc = (UNIQTYPE_KIND(ret_type) == COMPOSITE)
            ? Specialise_FromValue(retp, ret_ftype, &result) : 1;
        if (spec_rc > 0) result = ret_ftype->ft_copyfrom(retp, ret_ftype);
#else
        result = ret_ftype->ft_copyfrom(retp, ret_ftype);
#endif
    }
    Proxy_EndDeferFrees();
    for (int i = 0 ; i < narg ; ++i) Py_XDECREF(arg_tmps[i]);
    return result;

fail:
    for (int i = 0 ; i < narg ; ++i) Py_XDECREF(arg_tmps[i]);
    return NULL;
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

static PyObject *closureproxy_ctor(PyObject *args, PyObject *kwds, ForeignTypeObject *ftype)
{
    FunctionProxyTypeObject *fun_proxy_type =
        (FunctionProxyTypeObject *) ftype->ft_proxy_type;
    PyTypeObject *closure_type = fun_proxy_type->ff_closure_type;

    if (!closure_type || funproxytype_setup(fun_proxy_type) < 0) return NULL;

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

    ClosureProxyObject *obj = PyObject_New(ClosureProxyObject, closure_type);
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
        Proxy_Register_To_Dict((ProxyObject *) obj);
        // Do not release the manual allocation because our deallocation is
        // special. If liballocs supports custom deallocator we should use them
        // instead of this workaround.
        // FIXME: Seems that this is not working because closures are not
        // located in the heap but in mmap'ed section.
    }
    return (PyObject *) obj;
}

static void closureproxy_dealloc(ClosureProxyObject *self)
{
    Proxy_Unregister_From_Dict((ProxyObject *) self);

    ffi_closure_free(self->fc_closure);
    Py_DECREF(self->fc_callable);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyTypeObject *closureproxy_newtype(PyTypeObject *funproxytype)
{
    PyTypeObject *clostype = PyObject_GC_NewVar(PyTypeObject, &PyType_Type, 0);

    *clostype = (PyTypeObject){
        .ob_base = clostype->ob_base,
        .tp_name = "<foreign closure type>",
        .tp_basicsize = sizeof(ClosureProxyObject),
        .tp_base = funproxytype,
        .tp_dealloc = (destructor) closureproxy_dealloc,
        .tp_flags = Py_TPFLAGS_DEFAULT,
    };

    if (PyType_Ready(clostype) < 0)
    {
        PyObject_GC_Del(clostype);
        return NULL;
    }

    return clostype;
}

static PyTypeObject *funproxy_newproxytype(const struct uniqtype *type)
{
    FunctionProxyTypeObject *htype =
        PyObject_GC_NewVar(FunctionProxyTypeObject, &FunctionProxy_Metatype, 0);

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base,
        .tp_name = UNIQTYPE_NAME(type), // Maybe find a better name ?
        .tp_base = &Proxy_Type,
        .tp_call = (ternaryfunc) funproxy_call,
        .tp_repr = (reprfunc) funproxy_repr,
    };
    htype->ff_type = type;
    htype->ff_cif = NULL;

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        PyObject_GC_Del(htype);
        return NULL;
    }

    htype->ff_closure_type = closureproxy_newtype(&htype->tp_base);
    return (PyTypeObject *) htype;
}

ForeignTypeObject *FunctionProxy_NewType(const struct uniqtype *type)
{
    PyTypeObject *fun_type = funproxy_newproxytype(type);
    if (!fun_type) return NULL;
    ForeignTypeObject *ftype = Proxy_NewType(type, fun_type);
    ftype->ft_constructor = closureproxy_ctor;
    return ftype;
}
