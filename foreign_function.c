#include "foreign_library.h"
#include <stdbool.h>
#include <liballocs.h>
#include <ffi.h>
#include <dwarf.h>

#ifdef FFI_TARGET_HAS_COMPLEX_TYPE
# include <complex.h>
#endif

typedef struct {
    PyObject_HEAD
    const char *ff_symname;
    void *ff_funptr;
    const struct uniqtype *ff_type;
    ffi_cif *ff_cif;
    // Ensure that the library is never unloaded while the function is callable
    PyObject *ff_dlloader;
} ForeignFunctionObject;

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
            // TODO : manage struct and unions (need to create ffi_type's)
        case SUBRANGE:
        default:
            return NULL; // not handled
    }
}

/* All the `foreignfun_setup_*` functions are returning a negative value and set
 * a Python exception on failure */

static int foreignfun_setup_type(ForeignFunctionObject *self)
{
    if (self->ff_type) return 0;

    const struct uniqtype *type = __liballocs_get_alloc_type(self->ff_funptr);
    if (!type || !UNIQTYPE_IS_SUBPROGRAM_TYPE(type))
    {
        PyErr_Format(PyExc_ImportError, "Retrieving type information for foreign function '%s' failed", self->ff_symname);
        return -1;
    }

    self->ff_type = type;
    return 0;
}

static int foreignfun_setup_cif(ForeignFunctionObject *self)
{
    if (self->ff_cif) return 0;
    if (foreignfun_setup_type(self) < 0) return -1;
    const struct uniqtype *type = self->ff_type;

    if (type->un.subprogram.nret != 1)
    {
        PyErr_SetString(PyExc_ImportError, "Function not having exactly one return value are not supported");
        return -1;
    }
    ffi_type *ret_type = ffi_type_for_uniqtype(type->related[0].un.t.ptr);

    ffi_type **arg_types = NULL;
    unsigned narg = type->un.subprogram.narg;
    if (narg > 0) arg_types = PyMem_Malloc(narg * sizeof(ffi_type *));
    for (int i = 0 ; i < narg ; ++i)
    {
        arg_types[i] = ffi_type_for_uniqtype(type->related[i+1].un.t.ptr);
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
        PyMem_Free(arg_types);
        PyErr_Format(PyExc_ImportError, "Failure in call information initialization for '%s'", self->ff_symname);
        return -1;
    }
}

// Adapted from builtin_ord
static PyObject *char_to_long(PyObject* c)
{
    long ord;
    Py_ssize_t size;

    if (PyBytes_Check(c)) {
        size = PyBytes_GET_SIZE(c);
        if (size == 1) {
            ord = (long)((unsigned char)*PyBytes_AS_STRING(c));
            return PyLong_FromLong(ord);
        }
    }
    else if (PyUnicode_Check(c)) {
        if (PyUnicode_READY(c) == -1)
            return NULL;
        size = PyUnicode_GET_LENGTH(c);
        if (size == 1) {
            ord = (long)PyUnicode_READ_CHAR(c, 0);
            return PyLong_FromLong(ord);
        }
    }
    else if (PyByteArray_Check(c)) {
        /* XXX Hopefully this is temporary */
        size = PyByteArray_GET_SIZE(c);
        if (size == 1) {
            ord = (long)((unsigned char)*PyByteArray_AS_STRING(c));
            return PyLong_FromLong(ord);
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "expected string of length 1 as argument, but " \
                     "%.200s found", c->ob_type->tp_name);
        return NULL;
    }

    PyErr_Format(PyExc_TypeError,
                 "expected a character as argument, "
                 "but string of length %zd found",
                 size);
    return NULL;
}

#define STORE_AS(val, type) do {\
    type *tmem = mem; \
    *tmem = (type)(val); \
    return 0; \
} while(0)

static int store_pyobject_as_type(PyObject *obj, void *mem, const struct uniqtype *type)
{
    switch (type->un.info.kind)
    {
        case VOID:
            // Means that we have a void parameter... should not be possible
            PyErr_SetString(PyExc_TypeError, "This foreign function has a void argument, \
                this should never happen, please consider making a bug report");
            return -1;
        case ARRAY:
        case ADDRESS:
        case SUBPROGRAM:
        case ENUMERATION:
            // TODO
            return -1;
        case BASE:
        {
            unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
            bool decrobj = false;
            switch (type->un.base.enc)
            {
                case DW_ATE_boolean:
                {
                    int is_true = PyObject_IsTrue(obj);
                    if (is_true < 0) return -1;
                    STORE_AS(is_true, uint8_t);
                }

                case DW_ATE_unsigned_char:
                    obj = char_to_long(obj);
                    if (!obj) return -1;
                    decrobj = true;
                    // fall through
                case DW_ATE_address:
                case DW_ATE_unsigned:
                {
                    unsigned long long u = PyLong_AsUnsignedLongLong(obj);
                    if (decrobj) Py_DECREF(obj);
                    if (PyErr_Occurred()) return -1;

                    // Check for overflows
                    unsigned long long max_val = (1ULL << (8*size)) - 1;
                    if (u > max_val)
                    {
                        PyErr_Format(PyExc_OverflowError, "argument does not fit into a %d byte unsigned integer", size);
                        return -1;
                    }

                    if (size == 1) STORE_AS(u, uint8_t);
                    if (size == 2) STORE_AS(u, uint16_t);
                    if (size == 4) STORE_AS(u, uint32_t);
                    if (size == 8) STORE_AS(u, uint64_t);

                    PyErr_Format(PyExc_TypeError, "Unsupported foreign unsigned integer parameter");
                    return -1;
                }
                case DW_ATE_signed_char:
                    obj = char_to_long(obj);
                    if (!obj) return -1;
                    decrobj = true;
                    // fall through
                case DW_ATE_signed:
                {
                    long long i = PyLong_AsLongLong(obj);
                    if (decrobj) Py_DECREF(obj);
                    if (PyErr_Occurred()) return -1;

                    // Check for overflows & underflows
                    long long max_val = (long long)((1ULL << (8*size-1)) - 1);
                    long long min_val = -max_val - 1;
                    if (i < min_val || i > max_val)
                    {
                        PyErr_Format(PyExc_OverflowError, "argument does not fit into a %d byte signed integer", size);
                        return -1;
                    }

                    if (size == 1) STORE_AS(i, int8_t);
                    if (size == 2) STORE_AS(i, int16_t);
                    if (size == 4) STORE_AS(i, int32_t);
                    if (size == 8) STORE_AS(i, int64_t);

                    PyErr_SetString(PyExc_TypeError, "Unsupported foreign integer parameter");
                    return -1;
                }
                    
                case DW_ATE_float:
                {
                    double f = PyFloat_AsDouble(obj);
                    if (PyErr_Occurred()) return -1;

                    if (size == 4) STORE_AS(f, float);
                    if (size == 8) STORE_AS(f, double);
                    if (size == 16) STORE_AS(f, long double);
                    
                    PyErr_SetString(PyExc_TypeError, "Unsupported foreign float parameter");
                    return -1;
                }
#ifdef FFI_TARGET_HAS_COMPLEX_TYPE
                case DW_ATE_complex_float:
                {
                    Py_complex pz = PyComplex_AsCComplex(obj);
                    if (PyErr_Occurred()) return -1;

                    complex double z = CMPLX(pz.real, pz.imag);

                    if (size == 8) STORE_AS(z, complex float);
                    if (size == 16) STORE_AS(z, complex double);
                    if (size == 32) STORE_AS(z, complex long double);

                    PyErr_SetString(PyExc_TypeError, "Unsupported foreign complex parameter");
                    return -1;
                }
#endif
                default:
                    PyErr_SetString(PyExc_TypeError, "Unsupported foreign base type parameter");
                    return -1;
            }
        }
        case COMPOSITE:
            // TODO : manage struct and unions
        case SUBRANGE:
        default:
            return -1; // not handled
    }
}

PyObject *pyobject_from_type(void *data, const struct uniqtype *type)
{
    switch (type->un.info.kind)
    {
        case VOID:
            Py_RETURN_NONE;
        case ARRAY:
        case ADDRESS:
        case SUBPROGRAM:
        case ENUMERATION:
            Py_RETURN_NOTIMPLEMENTED;
        case BASE:
        {
            unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
            switch (type->un.base.enc)
            {
                case DW_ATE_boolean:
                {
                    bool b = *(uint8_t*) data;
                    if (b) Py_RETURN_TRUE;
                    else Py_RETURN_FALSE;
                }
                case DW_ATE_address:
                case DW_ATE_unsigned:
                {
                    unsigned long long u;
                    if (size == 1) u = *(uint8_t*) data;
                    else if (size == 2) u = *(uint16_t*) data;
                    else if (size == 4) u = *(uint32_t*) data;
                    else if (size == 8) u = *(uint64_t*) data;
                    else
                    {
                        PyErr_SetString(PyExc_TypeError, "Unsupported foreign unsigned integer return value");
                        return NULL;
                    }
                    return PyLong_FromUnsignedLongLong(u);
                }
                case DW_ATE_signed:
                {
                    long long i;
                    if (size == 1) i = *(int8_t*) data;
                    else if (size == 2) i = *(int16_t*) data;
                    else if (size == 4) i = *(int32_t*) data;
                    else if (size == 8) i = *(int64_t*) data;
                    else
                    {
                        PyErr_SetString(PyExc_TypeError, "Unsupported foreign integer return value");
                        return NULL;
                    }
                    return PyLong_FromLongLong(i);
                }
                case DW_ATE_unsigned_char:
                case DW_ATE_signed_char:
                    // Should we make strings or bytes here ???
                    return PyBytes_FromStringAndSize(data, size);
                case DW_ATE_float:
                {
                    double f;
                    if (size == 4) f = *(float*) data;
                    else if (size == 8) f = *(double*) data;
                    else if (size == 16) f = *(long double*) data;
                    else
                    {
                        PyErr_SetString(PyExc_TypeError, "Unsupported foreign float return value");
                        return NULL;
                    }
                    return PyFloat_FromDouble(f);
                }
#ifdef FFI_TARGET_HAS_COMPLEX_TYPE
                case DW_ATE_complex_float:
                {
                    complex double z;
                    if (size == 8) z = *(complex float*) data;
                    else if (size == 16) z = *(complex double*) data;
                    else if (size == 32) z = *(complex long double*) data;
                    else
                    {
                        PyErr_SetString(PyExc_TypeError, "Unsupported foreign complex return value");
                        return NULL;
                    }
                    return PyComplex_FromDoubles(creal(z), cimag(z));
                }
#endif
                default:
                    PyErr_SetString(PyExc_TypeError, "Unsupported foreign base type return value");
                    return NULL;
            }
        }
        case COMPOSITE:
            // TODO : manage struct and unions
        case SUBRANGE:
        default:
            Py_RETURN_NOTIMPLEMENTED;
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
    char retval[UNIQTYPE_SIZE_IN_BYTES(ret_type)];

    ffi_call(self->ff_cif, self->ff_funptr, retval, ff_args);

    return pyobject_from_type(retval, ret_type);
}

static PyObject *foreignfun_repr(ForeignFunctionObject *self)
{
    if (foreignfun_setup_type(self) < 0)
    {
        PyErr_Clear();
        return PyUnicode_FromFormat(
            "<foreign function '%s' without type information at %p>",
            self->ff_symname, self->ff_funptr);
    }
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
        PyMem_Free(self->ff_cif->arg_types);
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
    ForeignFunctionObject *obj;
    obj = PyObject_New(ForeignFunctionObject, &ForeignFunction_Type);
    obj->ff_symname = symname;
    obj->ff_funptr = funptr;
    obj->ff_type = NULL;
    obj->ff_cif = NULL;
    Py_INCREF(dlloader);
    obj->ff_dlloader = dlloader;
    return (PyObject *) obj;
}
