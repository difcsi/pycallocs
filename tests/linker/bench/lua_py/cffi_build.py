#!/usr/bin/env python3
"""Build libshapes_cffi.so, the "cffi" route of the Lua -> Python benchmark.

cffi's embedding mode turns Python functions into a C library: the C side
calls `divide` and `describe` as ordinary functions, and the library starts
the interpreter and runs the init code below on the first call. Here the init
code registers the callee's own functions (shapes.py, found on PYTHONPATH at
run time) with no wrapper in between; `describe` receives the by-value
`struct Point` as a cffi object, whose `.x` and `.y` it reads as it would any
other object's.

    cffi_build.py --out <dir>      writes <dir>/libshapes_cffi.so
"""
import argparse
import os

import cffi

STRUCT = "struct Point { int x; int y; };"

API = STRUCT + """
float divide(int a, int b);
int describe(struct Point p);
"""

INIT = """
from shapes_cffi import ffi
import shapes

ffi.def_extern()(shapes.divide)
ffi.def_extern()(shapes.describe)
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True, help="directory for the library and cffi's build files")
    args = ap.parse_args()

    builder = cffi.FFI()
    builder.embedding_api(API)
    builder.set_source("shapes_cffi", STRUCT, extra_compile_args=["-O2", "-g"])
    builder.embedding_init_code(INIT)
    os.makedirs(args.out, exist_ok=True)
    builder.compile(tmpdir=args.out, target="libshapes_cffi.so")


if __name__ == "__main__":
    main()
