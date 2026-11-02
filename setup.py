from setuptools import setup, Extension
from os import environ
from pathlib import Path

ROOT = Path(__file__).parent.absolute()

# liballocs and alaska are submodules under contrib/. The CMake build passes
# their paths via the environment; fall back to the in-tree submodules when
# building setup.py directly.
LIBALLOCS = Path(environ.get('LIBALLOCS', ROOT / 'contrib/liballocs'))

# The extension is ALWAYS built with the system compiler -- never with
# liballocs' allocscc nor alaska's clang (yukon). Those toolchains apply only
# to the test fixtures. Refuse an environment that tries to sneak one in.
_cc = environ.get('CC', '')
if any(tok in _cc for tok in ('allocscc', 'alaska', 'yukon')):
    raise SystemExit(
        f"CC={_cc!r}: the VM extensions must be built with the system "
        "compiler, not allocscc/alaska (yukon). Unset CC and retry.")

DEBUG = environ.get('DEBUG')

# The CPython extension lives in vmextensions/cpython; VM-agnostic code shared
# with the other VM extensions (minicrunch, dlsym_iter, ffitypes, and the
# handle_query.h translate-then-index liballocs glue) in vmextensions/common.
CPYTHON_EXT = ROOT / 'vmextensions/cpython'
VMEXT_COMMON = ROOT / 'vmextensions/common'

INCLUDE_PATHS = list(map(str, [
    LIBALLOCS / 'include',
    LIBALLOCS / 'contrib/libsystrap/contrib/librunt/include',
    LIBALLOCS / 'contrib/liballocstool/include',
    CPYTHON_EXT,
    VMEXT_COMMON,
]))

LIBRARY_PATHS = list(map(str, [
    LIBALLOCS / 'lib'
]))
compile_args = [
    '-DLIFETIME_POLICIES',
    '-gdwarf-4',
]

if DEBUG:
    compile_args.append("-O0")

# Alaska is a configure-time option (SS_HAVE_ALASKA=1, set by the CMake build when
# USE_ALASKA is on). handle_query.h keys off this macro to decide whether
# ss_translate()/ss_pin() bind to the Alaska runtime or degrade to a compile-time
# passthrough/no-op. liballocs itself is always assumed present, so it needs no gate.
#
# The extension is NOT handle-transformed (it calls ss_translate explicitly); it
# just needs to link libalaska so those runtime symbols resolve. Alaska lives under
# the dir passed in ALASKA (opt/alaska-anchorage/lib holds libalaska.so).
extra_libraries = []
extra_objects = []
if environ.get('SS_HAVE_ALASKA'):
    compile_args.append('-DSS_HAVE_ALASKA')
    ALASKA = Path(environ.get('ALASKA', ROOT / 'contrib/alaska'))
    # Which opt/ tree to link against (must match the tree the fixtures are
    # compiled with); the CMake build passes ALASKA_OPT_TREE through.
    alaska_lib = ALASKA / 'opt' / environ.get('ALASKA_OPT_TREE', 'alaska-anchorage') / 'lib'
    LIBRARY_PATHS.append(str(alaska_lib))
    extra_libraries.append('alaska')
    # alaska_translate()/pin/unpin are inlined by the Alaska transform and thus not
    # exported by libalaska. The extension is not transformed, so link a native
    # object compiled from Alaska's alaska_translate.bc (path supplied by CMake).
    translate_obj = environ.get('ALASKA_TRANSLATE_OBJ')
    if translate_obj:
        extra_objects.append(translate_obj)

# Optional: specialised PyObject_to_T<T> conversion translators (compile-time,
# default off; set via the SPECIALISE_CONVERSION CMake option). When enabled, the
# extension #embed's the shared common/pycpputils.hpp (located via --embed-dir,
# which is how both gcc and clang resolve #embed -- not -I), and at runtime generates,
# compiles and dlopens a specialised converter per type with g++-16 (-std=c++26
# -freflection). The embed needs a C23-capable C compiler (gcc>=15 / clang>=19)
# to build the extension; -std=gnu23 enables both #embed and the C extensions the
# codebase already relies on.
if environ.get('SPECIALISE_CONVERSION'):
    compile_args += [
        '-DPYCALLOCS_SPECIALISE_CONVERSION',
        '-DPYCALLOCS_TRANSLATE_CXX="g++-16"',
        '-std=gnu23',
        f'--embed-dir={ROOT / "common"}',
    ]

# DRAFT alternative to SPECIALISE_CONVERSION: synthesise the same translators via
# P3294 token-sequence injection (src/specialise_inject.c) instead of by printing
# C++ source from C (src/specialise.c). Mutually exclusive with SPECIALISE_CONVERSION
# -- each back end guards its body on its own macro, so defining only one keeps the
# entry points single-defined. pycpputils.hpp (PyObject_to_T), tokenject.hpp (the
# shared consteval machinery) and pyc_inject.hpp (the injector) are all #embed'd,
# so two --embed-dir entries are needed.
elif environ.get('INJECT_CONVERSION'):
    compile_args += [
        '-DPYCALLOCS_INJECT_CONVERSION',
        '-DPYCALLOCS_TRANSLATE_CXX="g++-16"',
        '-std=gnu23',
        f'--embed-dir={ROOT / "common"}',                # pycpputils.hpp, tokenject.hpp
        f'--embed-dir={ROOT / "vmextensions/cpython"}',  # pyc_inject.hpp
    ]

# Add RPATH so the module can find liballocs (and libalaska, when enabled) at runtime
link_args = [
    f'-Wl,-rpath,{LIBALLOCS / "lib"}'
]
if environ.get('SS_HAVE_ALASKA'):
    # Must point at the same opt/ tree we linked against (ALASKA_OPT_TREE).
    link_args.append(f'-Wl,-rpath,{alaska_lib}')

allocs = Extension('allocs',
                   include_dirs = INCLUDE_PATHS,
                   libraries = ['dl', 'ffi', 'allocs'] + extra_libraries,
                   library_dirs = LIBRARY_PATHS,
                   sources = [str(p) for p in
                              sorted(CPYTHON_EXT.glob('*.c')) + sorted(VMEXT_COMMON.glob('*.c'))],
                   extra_objects = extra_objects,
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
