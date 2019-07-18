import importlib
import importlib.abc
import importlib.machinery
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

        for base_path in path:
            try:
                filename = base_path + name + lib_extension
                loader = LibraryLoader(filename)
                return importlib.machinery.ModuleSpec(fullname, loader, origin=filename)
            except ImportError:
                continue

        return None

# We must register ourselves before the default meta path finder to prevent exception
# ImportError: dynamic module does not define module export function (PyInit_%)
sys.meta_path.insert(0, LibraryFinder)
