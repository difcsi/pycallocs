from setuptools import setup, Extension
from os import environ
from pathlib import Path

ROOT = Path(__file__).parent.absolute()


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
    '-DLIFETIME_POLICIES'
]

if DEBUG:
    compile_args.append("-O0")

allocs = Extension('allocs',
                   include_dirs = INCLUDE_PATHS,
                   libraries = ['dl', 'ffi', 'allocs'],
                   library_dirs = LIBRARY_PATHS,
                   sources = ['allocs_module.c', 'library_loader.c',
                       'proxy.c', 'foreign_type.c', 'foreign_basetype.c',
                       'function_proxy.c', 'composite_proxy.c',
                       'address_proxy.c'],
                   extra_compile_args = compile_args,
                   undef_macros = ["NDEBUG"] if DEBUG else [])

setup (name = 'Liballocs FFI',
       version = '0.1',
       description = 'Python invisible FFI using liballocs meta-information',
       author = 'Guillaume Bertholon & Zoltan Meszaros',
       author_email = 'zoltan.meszaros@kcl.ac.uk',
       ext_modules = [allocs],
       py_modules = ['elflib'])
