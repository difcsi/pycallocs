"""pycallocs backend (the system under test).

Note what is NOT here: no struct layout, no restype/argtypes, no cdef. The types
come from the DWARF in bench.so (built by allocscc), discovered at load. That
absence is the whole point of the project; the timing columns say what it costs.

Must run under the liballocs/linkpy environment (LD_PRELOAD + LD_AUDIT + the
static-TLS tunable) -- the orchestrator sets it. Loads bench.so via elflib.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import benchlib
import workloads

import elflib

_LIBDIR = os.path.abspath(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                       "libs", "bench"))
elflib.__path__.append(_LIBDIR)
from elflib import bench as m   # noqa: E402  (must follow the __path__ append)

_COLS = "abcdefghijkl"


class _PL:
    """A plain Python object pycallocs lazily crosses into `struct point *`
    (the same coercion tests/py/struct_ptr_coerce exercises)."""
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y


def _new_point(x, y):
    """A real composite proxy with C backing (for the field-access workloads)."""
    p = m.point()
    p.x = x
    p.y = y
    return p


def _scalar_int():
    f = m.bench_triple
    return lambda: f(7)


def _scalar_fp():
    f = m.bench_mul
    return lambda: f(1.5, 2.0)


def _byval_ret():
    f = m.make_point

    def run():
        p = f(3, 4)
        return p.x + p.y
    return run


def _byval_arg():
    # A tuple coerces positionally into `struct point` by value (struct_coerce).
    f = m.consume_point
    return lambda: f((3, 4))


def _byptr_arg():
    # A plain object is lazily materialised behind a `struct point *`.
    f = m.sum_point
    return lambda: f(_PL(3, 4))


def _bigstruct_ret():
    f = m.make_wide

    def run():
        w = f(0)
        return sum(getattr(w, c) for c in _COLS)
    return run


def _field_read():
    p = _new_point(3, 4)
    return lambda: p.x + p.y


def _field_write():
    p = _new_point(0, 0)

    def run():
        p.x = 10
        p.y = 20
        return p.x + p.y
    return run


def _array_index():
    n = workloads.ARRAY_N
    arr = m.make_int_array(n)

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
    benchlib.run_backend("pycallocs", RUNNERS, workloads.EXPECTED)
