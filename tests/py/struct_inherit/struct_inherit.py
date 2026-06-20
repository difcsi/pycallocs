from pyc_testlib import add_libs
add_libs("inheritance")
from elflib import inheritance as m

b = m.base(0)
m.print_base(b)
m.dynamic_print(b)

d = m.derivated((1,), 0)
m.print_base(d)
m.print_derivated(d)
m.dynamic_print(d)

l = m.leaf(((2,), 3.14), 'l')
m.print_base(l)
m.print_derivated(l)
m.print_leaf(l)
m.dynamic_print(l)

m.dynamic_print(m.int__32.array([3]))
