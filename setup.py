from distutils.core import setup, Extension
from os import environ

try:
    LIBALLOCS_DIR = environ['LIBALLOCS_DIR']
except KeyError:
    raise EnvironmentError("You must set the value of the environement variable LIBALLOCS_DIR")

allocs = Extension('allocs',
                   include_dirs = [LIBALLOCS_DIR+'/include'],
                   libraries = ['dl', 'ffi'],
                   library_dirs = [LIBALLOCS_DIR+'/lib'],
                   sources = ['allocs_module.c', 'library_loader.c',
                       'proxy.c', 'foreign_type.c', 'foreign_basetype.c',
                       'function_proxy.c', 'composite_proxy.c',
                       'address_proxy.c'],
                   extra_compile_args = ["-O0"])

setup (name = 'Liballocs FFI',
       version = '0.0',
       description = 'Python invisible FFI using liballocs meta-information',
       author = 'Guillaume Bertholon',
       author_email = 'guillaume.bertholon@ens.fr',
       ext_modules = [allocs],
       py_modules = ['foreign_library_finder'])
