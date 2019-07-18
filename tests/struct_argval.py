import elflib
elflib.__path__.append("libs/")
from elflib import composite as m

a = m.hello_world()
a.hello = 42
a.world = 9.99
m.print_hw(a)
