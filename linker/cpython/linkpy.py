"""Loader for linkpy cross-language modules, Python-host side.

Run the host program under the CALLEE language's LD_AUDIT auditor with
LINKPY_HOST_LANG=python and HOST_TEMPLATE_PATH set; then:

    import linkpy
    mytest = linkpy.load("mytest")
    mytest.divide(6, 3)

load() dlopens the stub init library; the auditor redirects that load to the
JIT-compiled liblinkstub.so, whose constructor registers one Python module per
foreign scripting module (each backed by the callee VM embedded in the
linkstub, with the user's C header types as the by-value ABI pivot).
"""

import ctypes
import importlib
import sys

_handles = {}


def load(name, path="./libinit.so"):
    """Return the foreign module `name` provided by the init library `path`."""
    if path not in _handles:
        # RTLD_GLOBAL: the linkstub's dependency (the callee VM's library) must
        # be globally visible so extensions the callee VM dlopens later resolve
        # their VM symbols -- e.g. luaproxy.so resolving lua_* on the
        # by-reference path. Keeping the handle keeps the linkstub (and its
        # embedded VM) loaded.
        _handles[path] = ctypes.CDLL(path, mode=ctypes.RTLD_GLOBAL)
    if name not in sys.modules:
        # Without the auditor the real (abort-stub) libinit.so loads fine but
        # registers nothing -- the likeliest cause of landing here.
        raise ImportError(
            f"linkpy: '{name}' was not registered by {path} (is LD_AUDIT set "
            "to the callee auditor, with LINKPY_HOST_LANG=python and "
            "HOST_TEMPLATE_PATH?)")
    return importlib.import_module(name)
