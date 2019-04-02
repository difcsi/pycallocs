from distutils.core import setup, Extension

allocs = Extension('allocs',
                   include_dirs = ['../liballocs/include'],
                   libraries = ['dl', 'ffi'],
                   library_dirs = ['../liballocs/lib'],
                   sources = ['allocsmodule.c', 'foreign_library.c', 
                       'foreign_function.c'],
                   extra_compile_args = ["-O0"])

setup (name = 'Allocs',
       version = '0.0',
       description = 'Python invisible FFI using liballocs meta-information',
       author = 'Guillaume Bertholon',
       author_email = 'guillaume.bertholon@ens.fr',
       ext_modules = [allocs])
