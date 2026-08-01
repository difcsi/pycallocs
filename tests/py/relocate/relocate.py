from pyc_testlib import add_libs
add_libs("relocate")
from elflib import relocate as m

p = m.make_point(3, 7)
before = m.backing_addr(p)
print(p.x, p.y)

m.relocate_point(p)
after = m.backing_addr(p)

print(p.x, p.y)
p.x = 42
print(p.x, p.y)
print(m.backing_addr(p) == after)
print(before != after)
