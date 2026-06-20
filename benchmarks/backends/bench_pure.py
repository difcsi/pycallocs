"""Pure-Python backend: the floor.

No C crossing at all -- it reimplements each workload's observable computation in
Python. This frames every other column: the gap from `pure` to a real FFI is the
cost of crossing the boundary; the gap from `pure` to `native` is what CPython
itself charges for the equivalent object churn.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import benchlib
import workloads


class _Pt:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y


def _scalar_int():
    return lambda: 3 * 7


def _scalar_fp():
    return lambda: 1.5 * 2.0


def _byval_ret():
    def run():
        p = (3, 4)
        return p[0] + p[1]
    return run


def _byval_arg():
    def run():
        p = _Pt(3, 4)
        return p.x + p.y
    return run


def _byptr_arg():
    def run():
        p = _Pt(3, 4)
        return p.x + p.y
    return run


def _bigstruct_ret():
    def run():
        w = tuple(range(12))
        return sum(w)
    return run


def _field_read():
    p = _Pt(3, 4)
    return lambda: p.x + p.y


def _field_write():
    p = _Pt(0, 0)

    def run():
        p.x = 10
        p.y = 20
        return p.x + p.y
    return run


def _array_index():
    n = workloads.ARRAY_N
    a = list(range(n))

    def run():
        s = 0
        for i in range(n):
            s += a[i]
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
    benchlib.run_backend("pure", RUNNERS, workloads.EXPECTED)
