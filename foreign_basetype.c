#include "foreign_library.h"
#include <stdbool.h>
#include <stdint.h>
#include <dwarf.h>

#ifndef __STDC_NO_COMPLEX__
# include <complex.h>
#endif

static PyObject *bool_getfrom(void *data, ForeignTypeObject *type)
{
    bool b = *(uint8_t*) data;
    if (b) Py_RETURN_TRUE;
    else Py_RETURN_FALSE;
}
static int bool_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    int is_true = PyObject_IsTrue(obj);
    if (is_true < 0) return -1;
    *(uint8_t *)dest = is_true;
    return 0;
}

#define DEFINE_UINT_GETSTORE(size) \
static PyObject *uint##size##_getfrom(void *data, ForeignTypeObject *type)\
{\
    unsigned long long u;\
    u = *(uint##size##_t *) data;\
    return PyLong_FromUnsignedLongLong(u);\
}\
static int uint##size##_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)\
{\
    unsigned long long u = PyLong_AsUnsignedLongLong(obj);\
    if (PyErr_Occurred()) return -1;\
\
    /* Check for overflows */\
    if (u > UINT##size##_MAX)\
    {\
        PyErr_SetString(PyExc_OverflowError, "argument does not fit into a " #size " bit unsigned integer");\
        return -1;\
    }\
\
    *(uint##size##_t *)dest = (uint##size##_t) u;\
    return 0;\
}
DEFINE_UINT_GETSTORE(8)
DEFINE_UINT_GETSTORE(16)
DEFINE_UINT_GETSTORE(32)
DEFINE_UINT_GETSTORE(64)

#define DEFINE_INT_GETSTORE(size) \
static PyObject *int##size##_getfrom(void *data, ForeignTypeObject *type)\
{\
    long long i;\
    i = *(int##size##_t *) data;\
    return PyLong_FromLongLong(i);\
}\
static int int##size##_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)\
{\
    long long i = PyLong_AsLongLong(obj);\
    if (PyErr_Occurred()) return -1;\
\
    /* Check for overflows & underflows */ \
    if (i < INT##size##_MIN || i > INT##size##_MAX)\
    {\
        PyErr_SetString(PyExc_OverflowError, "argument does not fit into a " #size " bit signed integer");\
        return -1;\
    }\
\
    *(int##size##_t *)dest = (int##size##_t) i;\
    return 0;\
}
DEFINE_INT_GETSTORE(8)
DEFINE_INT_GETSTORE(16)
DEFINE_INT_GETSTORE(32)
DEFINE_INT_GETSTORE(64)

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
#define DEFINE_CHAR_GETSTORE(size) \
static PyObject *char##size##_getfrom(void *data, ForeignTypeObject *type)\
{\
    return PyBytes_FromStringAndSize(data, size/8);\
}\
static int uchar##size##_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)\
{\
    obj = char_to_long(obj);\
    if (!obj) return -1;\
    int ret = uint##size##_storeinto(obj, dest, type);\
    Py_DECREF(obj);\
    return ret;\
}\
static int schar##size##_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)\
{\
    obj = char_to_long(obj);\
    if (!obj) return -1;\
    int ret = int##size##_storeinto(obj, dest, type);\
    Py_DECREF(obj);\
    return ret;\
}
DEFINE_CHAR_GETSTORE(8)
DEFINE_CHAR_GETSTORE(16)
DEFINE_CHAR_GETSTORE(32)
DEFINE_CHAR_GETSTORE(64)

#define DEFINE_FLOAT_GETSTORE(t) \
static PyObject *t##_getfrom(void *data, ForeignTypeObject *type)\
{\
    double f;\
    f = *(t *) data;\
    return PyFloat_FromDouble(f);\
}\
static int t##_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)\
{\
    double f = PyFloat_AsDouble(obj);\
    if (PyErr_Occurred()) return -1;\
    *(t *)dest = (t) f;\
    return 0;\
}
DEFINE_FLOAT_GETSTORE(float)
DEFINE_FLOAT_GETSTORE(double)
typedef long double long_double;
DEFINE_FLOAT_GETSTORE(long_double)

#ifndef __STDC_NO_COMPLEX__
#define DEFINE_COMPLEX_GETSTORE(t) \
static PyObject *t##_getfrom(void *data, ForeignTypeObject *type)\
{\
    complex double z;\
    z = *(t *) data;\
    return PyComplex_FromDoubles(creal(z), cimag(z));\
}\
static int t##_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)\
{\
    Py_complex pz = PyComplex_AsCComplex(obj);\
    if (PyErr_Occurred()) return -1;\
    complex double z = CMPLX(pz.real, pz.imag);\
    *(t *)dest = (t) z;\
    return 0;\
}
typedef complex float cmplx_float;
DEFINE_COMPLEX_GETSTORE(cmplx_float)
typedef complex double cmplx_double;
DEFINE_COMPLEX_GETSTORE(cmplx_double)
typedef complex long double cmplx_long_double;
DEFINE_COMPLEX_GETSTORE(cmplx_long_double)
#endif

#define CHECK_SIZE(sz, getfrom, storeinto) \
if (size == sz)\
{\
    obj->ft_getfrom = getfrom;\
    obj->ft_copyfrom = getfrom;\
    obj->ft_storeinto = storeinto;\
    break;\
}

// type must be of kind BASE, return a new reference
ForeignTypeObject *ForeignBaseType_New(const struct uniqtype *type)
{
    ForeignTypeObject *obj = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    obj->ft_type = type;
    obj->ft_proxy_type = NULL;
    obj->ft_constructor = NULL;
    obj->ft_getdataptr = NULL;
    obj->ft_traverse = NULL;

    unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);

    switch (type->un.base.enc)
    {
        case DW_ATE_boolean:
            obj->ft_getfrom = bool_getfrom;
            obj->ft_copyfrom = bool_getfrom;
            obj->ft_storeinto = bool_storeinto;
            break;

        case DW_ATE_address:
        case DW_ATE_unsigned:
            CHECK_SIZE(1, uint8_getfrom, uint8_storeinto)
            CHECK_SIZE(2, uint16_getfrom, uint16_storeinto)
            CHECK_SIZE(4, uint32_getfrom, uint32_storeinto)
            CHECK_SIZE(8, uint64_getfrom, uint64_storeinto)

            PyErr_SetString(PyExc_TypeError, "Unsupported foreign unsigned integer parameter");
            break;

        case DW_ATE_signed:
            CHECK_SIZE(1, int8_getfrom, int8_storeinto)
            CHECK_SIZE(2, int16_getfrom, int16_storeinto)
            CHECK_SIZE(4, int32_getfrom, int32_storeinto)
            CHECK_SIZE(8, int64_getfrom, int64_storeinto)

            PyErr_SetString(PyExc_TypeError, "Unsupported foreign signed integer parameter");
            break;

        case DW_ATE_unsigned_char:
            CHECK_SIZE(1, char8_getfrom, uchar8_storeinto)
            CHECK_SIZE(2, char16_getfrom, uchar16_storeinto)
            CHECK_SIZE(4, char32_getfrom, uchar32_storeinto)
            CHECK_SIZE(8, char64_getfrom, uchar64_storeinto)

            PyErr_SetString(PyExc_TypeError, "Unsupported foreign unsigned character parameter");
            break;

        case DW_ATE_signed_char:
            CHECK_SIZE(1, char8_getfrom, schar8_storeinto)
            CHECK_SIZE(2, char16_getfrom, schar16_storeinto)
            CHECK_SIZE(4, char32_getfrom, schar32_storeinto)
            CHECK_SIZE(8, char64_getfrom, schar64_storeinto)

            PyErr_SetString(PyExc_TypeError, "Unsupported foreign signed character parameter");
            break;

        case DW_ATE_float:
            CHECK_SIZE(4, float_getfrom, float_storeinto)
            CHECK_SIZE(8, double_getfrom, double_storeinto)
            CHECK_SIZE(16, long_double_getfrom, long_double_storeinto)

            PyErr_SetString(PyExc_TypeError, "Unsupported foreign float parameter");
            break;

#ifndef __STDC_NO_COMPLEX__
        case DW_ATE_complex_float:
            CHECK_SIZE(8, cmplx_float_getfrom, cmplx_float_storeinto)
            CHECK_SIZE(16, cmplx_double_getfrom, cmplx_double_storeinto)
            CHECK_SIZE(32, cmplx_long_double_getfrom, cmplx_long_double_storeinto)

            PyErr_SetString(PyExc_TypeError, "Unsupported foreign complex parameter");
            break;
#endif

        default:
            PyErr_SetString(PyExc_TypeError, "Unsupported foreign base type parameter");
            break;
    }

    if (PyErr_Occurred())
    {
        Py_DECREF(obj);
        return NULL;
    }
    return obj;
}
