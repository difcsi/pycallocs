from pyc_testlib import add_libs
add_libs("composite")
import allocs
from elflib import composite as m

a = m.make_hw(3, 5.5)
# When the SPECIALISE_CONVERSION fast path is compiled in, a by-value struct
# return comes back as a types.SimpleNamespace (via T_to_PyObject) rather than a
# proxy. Either way the fields are the same; normalise the printed form so the
# expected output matches in both builds.
if allocs.SPECIALISE_CONVERSION:
    assert not isinstance(a, allocs.Proxy)
    print(f"(hello_world){{hello: {a.hello}, world: {a.world}}}")
else:
    print(a)
