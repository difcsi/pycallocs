/* Hand-written CPython extension: the performance ceiling.
 *
 * What a competent author would write by hand to expose libbench to Python, with
 * no FFI layer in between -- METH_FASTCALL entry points that unpack the Python
 * args, call the real C function, and pack the result. The gap from any FFI
 * column down to this one is the irreducible overhead that FFI layer adds.
 *
 * Returns structs to Python as tuples (the natural hand-written idiom); it does
 * not model Python-attribute field access, so the field_read/field_write and
 * array_index workloads have no native cell (N/A in the report). */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "bench.h"

static PyObject *nb_triple(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    if (n != 1) { PyErr_SetString(PyExc_TypeError, "triple(x)"); return NULL; }
    long x = PyLong_AsLong(args[0]);
    if (x == -1 && PyErr_Occurred()) return NULL;
    return PyLong_FromLong(bench_triple((int) x));
}

static PyObject *nb_mul(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    if (n != 2) { PyErr_SetString(PyExc_TypeError, "mul(a, b)"); return NULL; }
    double a = PyFloat_AsDouble(args[0]);
    double b = PyFloat_AsDouble(args[1]);
    if (PyErr_Occurred()) return NULL;
    return PyFloat_FromDouble(bench_mul(a, b));
}

static PyObject *nb_make_point(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    if (n != 2) { PyErr_SetString(PyExc_TypeError, "make_point(x, y)"); return NULL; }
    int x = (int) PyLong_AsLong(args[0]);
    int y = (int) PyLong_AsLong(args[1]);
    if (PyErr_Occurred()) return NULL;
    struct point p = make_point(x, y);
    return Py_BuildValue("(ii)", p.x, p.y);
}

static PyObject *nb_consume_point(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    if (n != 2) { PyErr_SetString(PyExc_TypeError, "consume_point(x, y)"); return NULL; }
    struct point p = { (int) PyLong_AsLong(args[0]), (int) PyLong_AsLong(args[1]) };
    if (PyErr_Occurred()) return NULL;
    return PyLong_FromLong(consume_point(p));
}

static PyObject *nb_sum_point(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    if (n != 2) { PyErr_SetString(PyExc_TypeError, "sum_point(x, y)"); return NULL; }
    struct point p = { (int) PyLong_AsLong(args[0]), (int) PyLong_AsLong(args[1]) };
    if (PyErr_Occurred()) return NULL;
    return PyLong_FromLong(sum_point(&p));
}

static PyObject *nb_make_wide(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    if (n != 1) { PyErr_SetString(PyExc_TypeError, "make_wide(base)"); return NULL; }
    int base = (int) PyLong_AsLong(args[0]);
    if (PyErr_Occurred()) return NULL;
    struct wide w = make_wide(base);
    return Py_BuildValue("(iiiiiiiiiiii)",
        w.a, w.b, w.c, w.d, w.e, w.f, w.g, w.h, w.i, w.j, w.k, w.l);
}

static PyMethodDef methods[] = {
    {"triple",        (PyCFunction) nb_triple,        METH_FASTCALL, NULL},
    {"mul",           (PyCFunction) nb_mul,           METH_FASTCALL, NULL},
    {"make_point",    (PyCFunction) nb_make_point,    METH_FASTCALL, NULL},
    {"consume_point", (PyCFunction) nb_consume_point, METH_FASTCALL, NULL},
    {"sum_point",     (PyCFunction) nb_sum_point,     METH_FASTCALL, NULL},
    {"make_wide",     (PyCFunction) nb_make_wide,     METH_FASTCALL, NULL},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "benchnative",
    "hand-written C-extension baseline for the pycallocs benchmark suite",
    -1,
    methods,
    NULL, NULL, NULL, NULL,
};

PyMODINIT_FUNC PyInit_benchnative(void)
{
    return PyModule_Create(&moduledef);
}
