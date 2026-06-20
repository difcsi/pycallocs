from pyc_testlib import add_libs
add_libs("composite")
from elflib import composite as m

# A plain (non-proxy) Python object passed where `struct hello_world *` is
# expected. The interop layer lazily materialises a backing C struct, copies the
# object into it, and passes a pointer to it (Figure: lazy proxy translation).
class Hw:
    def __init__(self, h, w):
        self.hello = h
        self.world = w

print(m.sum_hw(Hw(42, 8.0)))
print(m.sum_hw(Hw(7, 0.25)))
