from setuptools import setup, Extension
from os import environ
from pathlib import Path
import os

ROOT = Path(__file__).parent.absolute()

# Set allocscc as the compiler to generate uniqtype symbols (only if enabled)
use_allocscc = environ.get('USE_ALLOCSCC', 'no').lower() in ('yes', '1', 'true')
if use_allocscc:
    allocscc_path = str(ROOT / 'contrib/liballocs/tools/lang/c/bin/allocscc')
    os.environ['CC'] = allocscc_path

DEBUG = environ.get('DEBUG')

INCLUDE_PATHS = list(map(str, [
    ROOT / 'contrib/liballocs/include',
    ROOT / 'contrib/liballocs/contrib/libsystrap/contrib/librunt/include',
    ROOT / 'contrib/liballocs/contrib/liballocstool/include',
    ROOT / 'include'
]))

LIBRARY_PATHS = list(map(str, [
    ROOT / 'contrib/liballocs/lib'
]))
compile_args = [
    '-DLIFETIME_POLICIES',
    '-gdwarf-4',
    # Map unsupported CIL float types to standard types
    '-D_Float64=double',
    '-D_Float128=long double',
    '-D_Float64x=long double',
    '-D__float128=long double',
    # CIL doesn't understand nullptr (C23/C++) or true/false constants
    '-Dnullptr=NULL',
    '-Dtrue=1',
    '-Dfalse=0',
]

if DEBUG:
    compile_args.append("-O0")

# Add RPATH so the module can find liballocs at runtime
link_args = [
    f'-Wl,-rpath,{ROOT / "contrib/liballocs/lib"}'
]

allocs = Extension('allocs',
                   include_dirs = INCLUDE_PATHS,
                   libraries = ['dl', 'ffi', 'allocs'],
                   library_dirs = LIBRARY_PATHS,
                   sources = ['allocs_module.c', 'library_loader.c',
                       'proxy.c', 'foreign_type.c', 'foreign_basetype.c',
                       'function_proxy.c', 'composite_proxy.c',
                       'address_proxy.c'],
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
