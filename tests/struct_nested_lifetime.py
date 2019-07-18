import elflib
elflib.__path__.append("libs/")
from elflib import nested_struct as m

o = m.outstruct()
a = o.a
del o

a.data1 = 1
a.data2 = 2
print(a)
