"""ctypes backend (stdlib FFI).

The fair-comparison cost includes what ctypes actually makes you do: declare the
struct layout (`Point`, `Wide`) and the per-function restype/argtypes by hand.
That declaration is exactly the boilerplate pycallocs eliminates via DWARF.
Loads the plain gcc build of the fixture (libbench_plain.so).
"""
import ctypes
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import benchlib
import workloads

_LIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "libs", "bench", "libbench_plain.so")
lib = ctypes.CDLL(os.path.abspath(_LIB))

_COLS = "abcdefghijkl"


class Point(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int)]


class Wide(ctypes.Structure):
    _fields_ = [(c, ctypes.c_int) for c in _COLS]


lib.bench_triple.restype = ctypes.c_int
lib.bench_triple.argtypes = [ctypes.c_int]
lib.bench_mul.restype = ctypes.c_double
lib.bench_mul.argtypes = [ctypes.c_double, ctypes.c_double]
lib.make_point.restype = Point
lib.make_point.argtypes = [ctypes.c_int, ctypes.c_int]
lib.consume_point.restype = ctypes.c_long
lib.consume_point.argtypes = [Point]
lib.sum_point.restype = ctypes.c_long
lib.sum_point.argtypes = [ctypes.POINTER(Point)]
lib.make_wide.restype = Wide
lib.make_wide.argtypes = [ctypes.c_int]
lib.make_int_array.restype = ctypes.POINTER(ctypes.c_int)
lib.make_int_array.argtypes = [ctypes.c_int]


def _scalar_int():
    f = lib.bench_triple
    return lambda: f(7)


def _scalar_fp():
    f = lib.bench_mul
    return lambda: f(1.5, 2.0)


def _byval_ret():
    f = lib.make_point

    def run():
        p = f(3, 4)
        return p.x + p.y
    return run


def _byval_arg():
    f = lib.consume_point
    return lambda: f(Point(3, 4))


def _byptr_arg():
    f = lib.sum_point
    byref = ctypes.byref

    def run():
        p = Point(3, 4)
        return f(byref(p))
    return run


def _bigstruct_ret():
    f = lib.make_wide

    def run():
        w = f(0)
        return sum(getattr(w, c) for c in _COLS)
    return run


def _field_read():
    p = Point(3, 4)
    return lambda: p.x + p.y


def _field_write():
    p = Point(0, 0)

    def run():
        p.x = 10
        p.y = 20
        return p.x + p.y
    return run


def _array_index():
    n = workloads.ARRAY_N
    arr = ctypes.cast(lib.make_int_array(n), ctypes.POINTER(ctypes.c_int))

    def run():
        s = 0
        for i in range(n):
            s += arr[i]
        return s
    return run


RUNNERS = {
    "scalar_int": _scalar_int,
    "scalar_fp": _scalar_fp,
    "byval_ret": _byval_ret,
    "byval_arg": _byval_arg,
    "byptr_arg": _byptr_arg,
    "bigstruct_ret": _bigstruct_ret,
    "field_read": _field_read,
    "field_write": _field_write,
    "array_index": _array_index,
}

if __name__ == "__main__":
    benchlib.run_backend("ctypes", RUNNERS, workloads.EXPECTED)
