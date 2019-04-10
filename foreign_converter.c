#include "foreign_library.h"
#include <stdbool.h>
#include <dwarf.h>

#ifndef __STDC_NO_COMPLEX__
# include <complex.h>
#endif

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
                     "expected string of length 1, but %.200s found",
                     c->ob_type->tp_name);
        return NULL;
    }

    PyErr_Format(PyExc_TypeError,
                 "expected a character, but string of length %zd found",
                 size);
    return NULL;
}

#define STORE_AS(val, type) do {\
    type *tmem = mem; \
    *tmem = (type)(val); \
    return 0; \
} while(0)

int store_pyobject_as_type(PyObject *obj, void *mem, const struct uniqtype *type)
{
    switch (type->un.info.kind)
    {
        case VOID:
            // Means that we have a void parameter/field... should not be possible
            PyErr_SetString(PyExc_TypeError, "This foreign function has a void argument, "
                "this should never happen, please consider making a bug report");
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
#ifndef __STDC_NO_COMPLEX__
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
        {
            // Check that types are compatible
            // Copy data
        }
        case SUBRANGE:
        default:
            return -1; // not handled
    }
}

PyObject *pyobject_from_type(void *data, const struct uniqtype *type, PyObject *typdict)
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
#ifndef __STDC_NO_COMPLEX__
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
        {
            if (!typdict) return NULL; // TODO: temporary
            PyObject *typkey = PyLong_FromVoidPtr((void *) type);
            PyObject *ptype = PyDict_GetItem(typdict, typkey);
            Py_DECREF(typkey);
            if (!ptype)
            {
                // We should only be here if the composite type importation have failed
                PyErr_SetString(PyExc_TypeError, "Unsupported foreign composite return value");
                return NULL;
            }
            return ForeignHandler_FromDataAndType(data, (PyTypeObject *) ptype);
        }
        case SUBRANGE:
        default:
            Py_RETURN_NOTIMPLEMENTED;
    }
}


