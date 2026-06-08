from setuptools import setup, Extension
from os import environ
from pathlib import Path
import os

ROOT = Path(__file__).parent.absolute()

# liballocs and alaska are submodules of stackscan, not of pycallocs. The
# CMake build locates the pre-built copies and passes their paths via the
# environment; fall back to the in-tree stackscan submodule when building
# setup.py directly.
LIBALLOCS = Path(environ.get('LIBALLOCS',
                             ROOT / 'contrib/stackscan/contrib/liballocs'))

# Set allocscc as the compiler to generate uniqtype symbols (only if enabled)
use_allocscc = environ.get('USE_ALLOCSCC', 'no').lower() in ('yes', '1', 'true')
if use_allocscc:
    allocscc_path = str(LIBALLOCS / 'tools/lang/c/bin/allocscc')
    os.environ['CC'] = allocscc_path

DEBUG = environ.get('DEBUG')

INCLUDE_PATHS = list(map(str, [
    LIBALLOCS / 'include',
    LIBALLOCS / 'contrib/libsystrap/contrib/librunt/include',
    LIBALLOCS / 'contrib/liballocstool/include',
    ROOT / 'include'
]))

LIBRARY_PATHS = list(map(str, [
    LIBALLOCS / 'lib'
]))
compile_args = [
    '-DLIFETIME_POLICIES',
    '-gdwarf-4',
]

# CIL (used by allocscc) doesn't understand some C23/native types, so map them
# to standard ones. These are only needed on the allocscc path; on plain
# clang/gcc (e.g. the alaska path) they are both unnecessary and harmful --
# alaska's compiler wrapper re-splits arguments on spaces, breaking the
# `=long double` macro values.
if use_allocscc:
    compile_args += [
        '-D_Float64=double',
        '-D_Float128=long double',
        '-D_Float64x=long double',
        '-D__float128=long double',
        '-Dnullptr=NULL',
        '-Dtrue=1',
        '-Dfalse=0',
    ]

if DEBUG:
    compile_args.append("-O0")

# Add RPATH so the module can find liballocs at runtime
link_args = [
    f'-Wl,-rpath,{LIBALLOCS / "lib"}'
]

allocs = Extension('allocs',
                   include_dirs = INCLUDE_PATHS,
                   libraries = ['dl', 'ffi', 'allocs'],
                   library_dirs = LIBRARY_PATHS,
                   sources = ['allocs_module.c', 'library_loader.c',
                       'proxy.c', 'foreign_type.c', 'foreign_basetype.c',
                       'function_proxy.c', 'composite_proxy.c',
                       'address_proxy.c', 'minicrunch.c'],
                   extra_compile_args = compile_args,
                   extra_link_args = link_args,
                   undef_macros = ["NDEBUG"] if DEBUG else [])

setup (name = 'Liballocs FFI',
       version = '0.1',
       description = 'Python invisible FFI using liballocs meta-information',
       author = 'Guillaume Bertholon & Zoltan Meszaros',
       author_email = 'zoltan.meszaros@kcl.ac.uk',
       ext_modules = [allocs],
       py_modules = ['elflib'])
