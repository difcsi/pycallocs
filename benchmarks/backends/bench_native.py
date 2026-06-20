"""Native backend adapter: drives the hand-written `benchnative` C extension.

The ceiling. Implements only the workloads a function-style C extension exposes
naturally (scalars, by-value/by-pointer struct calls); field access and array
indexing have no native idiom here and are left N/A.

`benchnative` is built next to this file (native/benchnative*.so) by CMake, or by
benchmarks/build.sh for a standalone run.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))          # benchmarks/
sys.path.insert(0, os.path.join(_HERE, "native"))   # for benchnative*.so
import benchlib
import workloads

import benchnative as nb


def _scalar_int():
    f = nb.triple
    return lambda: f(7)


def _scalar_fp():
    f = nb.mul
    return lambda: f(1.5, 2.0)


def _byval_ret():
    f = nb.make_point

    def run():
        p = f(3, 4)
        return p[0] + p[1]
    return run


def _byval_arg():
    f = nb.consume_point
    return lambda: f(3, 4)


def _byptr_arg():
    f = nb.sum_point
    return lambda: f(3, 4)


def _bigstruct_ret():
    f = nb.make_wide

    def run():
        return sum(f(0))
    return run


RUNNERS = {
    "scalar_int": _scalar_int,
    "scalar_fp": _scalar_fp,
    "byval_ret": _byval_ret,
    "byval_arg": _byval_arg,
    "byptr_arg": _byptr_arg,
    "bigstruct_ret": _bigstruct_ret,
}

if __name__ == "__main__":
    benchlib.run_backend("native", RUNNERS, workloads.EXPECTED)
