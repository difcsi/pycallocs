from pyc_testlib import add_libs
add_libs("basic")
from elflib import basic as m

print(m.triple(3))
print(m.inv(True))
print(m.mul(1.02, 5))
print(m.a())
