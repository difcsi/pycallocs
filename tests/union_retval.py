import elflib
elflib.__path__.append("libs/")
from elflib import composite as m

a = m.make_dead(3)
print(a.dead)

a = m.make_alive(3.14)
print(a.alive)
