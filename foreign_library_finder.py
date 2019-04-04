import allocs
import importlib
import importlib.abc
import importlib.machinery
import sys

# These can be modified by the user to change the finder search paths
lib_extension = ".so"

# The blank search path has a special meaning as dlopen is using its own search 
# paths when the file name does not contain any /
search_paths = [""]

class ForeignLibraryFinder(importlib.abc.MetaPathFinder):
    """
        Meta path finder searching foreign libraries usable
        with allocs.ForeignLibraryLoader.
    """

    def find_spec(fullname, path, target):
        if path is not None:
            # Should we support submodules ?
            return None
        
        for base_path in search_paths:
            try:
                filename = base_path + fullname + lib_extension
                loader = allocs.ForeignLibraryLoader(filename)
                return importlib.machinery.ModuleSpec(fullname, loader, origin=filename)
            except ImportError:
                continue

        return None

sys.meta_path.append(ForeignLibraryFinder)
