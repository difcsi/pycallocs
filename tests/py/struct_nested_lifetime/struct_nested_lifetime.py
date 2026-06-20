from pyc_testlib import add_libs
add_libs("nested_struct")
from elflib import nested_struct as m

o = m.outstruct()
a = o.a
del o

a.data1 = 1
a.data2 = 2
print(a)
