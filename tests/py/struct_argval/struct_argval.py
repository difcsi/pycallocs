from pyc_testlib import add_libs
add_libs("composite")
from elflib import composite as m

a = m.hello_world()
a.hello = 42
a.world = 9.99
m.print_hw(a)
