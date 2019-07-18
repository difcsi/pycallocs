import elflib
elflib.__path__.append("libs/")
from elflib import inheritance as m

b = m.base()
b.id = 0
m.dynamic_print(b)

d = m.derivated()
d.id = 1
d.data = 0
m.dynamic_print(d)

l = m.leaf()
l.id = 2
l.data = 3.14
l.character = 'l'
m.dynamic_print(l)
