import elflib
elflib.__path__.append("libs/")
from elflib import composite as m

a = m.quantum_cat()
a.dead = 42
m.print_cat(a)

a.alive = 9.99
m.print_cat(a)
