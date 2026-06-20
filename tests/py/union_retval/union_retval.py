from pyc_testlib import add_libs
add_libs("composite")
import allocs
from elflib import composite as m

# The SPECIALISE_CONVERSION fast path cannot represent a union (its fields
# overlap), so a by-value union return always comes back as a proxy -- even when
# the fast path is on, it falls back to the generic converter. Assert that, so a
# regression that wrongly specialises a union (and returns garbage) is caught.
a = m.make_dead(3)
if allocs.SPECIALISE_CONVERSION:
    assert isinstance(a, allocs.Proxy)
print(a.dead)

a = m.make_alive(3.14)
if allocs.SPECIALISE_CONVERSION:
    assert isinstance(a, allocs.Proxy)
print(a.alive)
