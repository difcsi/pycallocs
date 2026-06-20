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

# Optional: specialised PyObject_to_T<T> conversion translators (compile-time,
# default off; set via the SPECIALISE_CONVERSION CMake option). When enabled, the
# extension #embed's linkpy's pycpputils.hpp (located via --embed-dir, which is
# how both gcc and clang resolve #embed -- not -I), and at runtime generates,
# compiles and dlopens a specialised converter per type with g++-16 (-std=c++26
# -freflection). The embed needs a C23-capable C compiler (gcc>=15 / clang>=19)
# to build the extension; -std=gnu23 enables both #embed and the C extensions the
# codebase already relies on.
if environ.get('SPECIALISE_CONVERSION'):
    compile_args += [
        '-DPYCALLOCS_SPECIALISE_CONVERSION',
        '-DPYCALLOCS_TRANSLATE_CXX="g++-16"',
        '-std=gnu23',
        f'--embed-dir={ROOT / "contrib/linkpy/include"}',
    ]

# DRAFT alternative to SPECIALISE_CONVERSION: synthesise the same translators via
# P3294 token-sequence injection (src/specialise_inject.c) instead of by printing
# C++ source from C (src/specialise.c). Mutually exclusive with SPECIALISE_CONVERSION
# -- each back end guards its body on its own macro, so defining only one keeps the
# entry points single-defined. Both pycpputils.hpp (PyObject_to_T) and pyc_inject.hpp
# (the injector) are #embed'd, so two --embed-dir entries are needed.
elif environ.get('INJECT_CONVERSION'):
    compile_args += [
        '-DPYCALLOCS_INJECT_CONVERSION',
        '-DPYCALLOCS_TRANSLATE_CXX="g++-16"',
        '-std=gnu23',
        f'--embed-dir={ROOT / "contrib/linkpy/include"}',  # pycpputils.hpp
        f'--embed-dir={ROOT / "include"}',                 # pyc_inject.hpp
    ]

# Add RPATH so the module can find liballocs at runtime
link_args = [
    f'-Wl,-rpath,{LIBALLOCS / "lib"}'
]

allocs = Extension('allocs',
                   include_dirs = INCLUDE_PATHS,
                   libraries = ['dl', 'ffi', 'allocs'],
                   library_dirs = LIBRARY_PATHS,
                   sources = [str(p) for p in sorted((ROOT / 'src').glob('*.c'))],
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
