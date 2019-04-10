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
                   sources = ['allocsmodule.c', 'foreign_library_loader.c',
                       'foreign_function.c'],
                   extra_compile_args = ["-O0"])

setup (name = 'Liballocs FFI',
       version = '0.0',
       description = 'Python invisible FFI using liballocs meta-information',
       author = 'Guillaume Bertholon',
       author_email = 'guillaume.bertholon@ens.fr',
       ext_modules = [allocs],
       py_modules = ['foreign_library_finder'])
