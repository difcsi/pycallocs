"""cffi backend, ABI / dlopen mode.

ABI mode (ffi.cdef + ffi.dlopen) is the fair head-to-head with ctypes and
pycallocs: no compiler is invoked, the types are declared by hand from the same
header knowledge. (cffi's API/set_source mode compiles a wrapper and runs closer
to native, but then it is no longer a runtime-only FFI -- left out to keep the
comparison apples-to-apples.) Loads libbench_plain.so.

If cffi is not installed this import fails; the orchestrator records the backend
as unavailable and continues with the others.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import benchlib
import workloads

import cffi

_COLS = "abcdefghijkl"

ffi = cffi.FFI()
ffi.cdef("""
struct point { int x; int y; };
struct wide { int a, b, c, d, e, f, g, h, i, j, k, l; };
int    bench_triple(int);
double bench_mul(double, double);
struct point make_point(int, int);
long   consume_point(struct point);
long   sum_point(struct point *);
struct wide make_wide(int);
int   *make_int_array(int);
""")

_LIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "libs", "bench", "libbench_plain.so")
lib = ffi.dlopen(os.path.abspath(_LIB))


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
    new = ffi.new

    def run():
        return f(new("struct point *", [3, 4])[0])
    return run


def _byptr_arg():
    f = lib.sum_point
    new = ffi.new

    def run():
        return f(new("struct point *", [3, 4]))
    return run


def _bigstruct_ret():
    f = lib.make_wide

    def run():
        w = f(0)
        return sum(getattr(w, c) for c in _COLS)
    return run


def _field_read():
    p = ffi.new("struct point *", [3, 4])
    return lambda: p.x + p.y


def _field_write():
    p = ffi.new("struct point *", [0, 0])

    def run():
        p.x = 10
        p.y = 20
        return p.x + p.y
    return run


def _array_index():
    n = workloads.ARRAY_N
    arr = lib.make_int_array(n)

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
    benchlib.run_backend("cffi", RUNNERS, workloads.EXPECTED)
