import importlib
import importlib.abc
import importlib.machinery
import os
import sys
# Re-export everything from the C extension module
from allocs import *

# These can be modified by the user to change the finder search paths
lib_extension = ".so"

# The blank search path has a special meaning as dlopen is using its own search 
# paths when the file name does not contain any /
__path__ = ["./", ""]

class LibraryFinder(importlib.abc.MetaPathFinder):
    """
        Meta path finder searching foreign libraries usable
        with allocs.ForeignLibraryLoader.
    """

    def find_spec(fullname, path, target):
        if path is None:
            # We are defining __path__ so it should never be None
            return None

        # We only try to load modules below ourselves
        modpath = fullname.split(".")
        if len(modpath) != 2 or modpath[0] != __name__:
            return None
        name = modpath[1]

        # The blank "" entry has a special meaning: os.path.join("", x) == x, so the
        # loader gets a bare filename and dlopen falls back to its own (system) library
        # search paths. That is a last resort and must be tried only AFTER every
        # explicit directory on the path -- otherwise a bare-name match (a stale or
        # empty library found by the system search) can shadow the real fixture, which
        # then loads without its symbols (e.g. `module 'elflib.arrays' has no attribute
        # 'make_int_array'`). Explicit entries keep their relative order; "" goes last.
        ordered = [p for p in path if p != ""] + [p for p in path if p == ""]
        for base_path in ordered:
            try:
                # os.path.join tolerates base paths with or without a trailing
                # separator. The blank "" entry is preserved verbatim (join("", x) == x).
                filename = os.path.join(base_path, name + lib_extension)
                loader = LibraryLoader(filename)
                return importlib.machinery.ModuleSpec(fullname, loader, origin=filename)
            except ImportError:
                continue

        return None

# We must register ourselves before the default meta path finder to prevent exception
# ImportError: dynamic module does not define module export function (PyInit_%)
sys.meta_path.insert(0, LibraryFinder)
