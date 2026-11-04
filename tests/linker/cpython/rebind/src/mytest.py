"""Each call must see the module as it is at that moment: a function rebound
in the module's namespace, and a module object replaced in sys.modules."""
import sys
import types


def value():
    return 1


def rebind():
    # What importlib.reload() does to every function: the module object stays,
    # the attribute is rebound.
    global value
    value = lambda: 2


def replace():
    # A fresh module object under the same name, as a test harness might install.
    mod = types.ModuleType(__name__)
    mod.value = lambda: 3
    mod.rebind = rebind
    mod.replace = replace
    sys.modules[__name__] = mod
