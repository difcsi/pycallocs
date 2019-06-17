from distutils.core import setup, Extension
from os import environ

LIBALLOCS_DIR = environ['LIBALLOCS']
LIBCRUNCH_DIR = environ['LIBCRUNCH']
DEBUG = environ.get('DEBUG')

allocs = Extension('allocs',
                   include_dirs = [LIBALLOCS_DIR+'/include', LIBCRUNCH_DIR+'/include'],
                   libraries = ['dl', 'ffi'],
                   library_dirs = [],
                   sources = ['allocs_module.c', 'library_loader.c',
                       'proxy.c', 'foreign_type.c', 'foreign_basetype.c',
                       'function_proxy.c', 'composite_proxy.c',
                       'address_proxy.c'],
                   extra_compile_args = ["-O0"] if DEBUG else [],
                   undef_macros = ["NDEBUG"] if DEBUG else [])

setup (name = 'Liballocs FFI',
       version = '0.0',
       description = 'Python invisible FFI using liballocs meta-information',
       author = 'Guillaume Bertholon',
       author_email = 'guillaume.bertholon@ens.fr',
       ext_modules = [allocs],
       py_modules = ['foreign_library_finder'])
