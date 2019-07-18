import elflib
elflib.__path__.append("libs/")
from elflib import composite as m

a = m.make_hw(3, 5.5)
print(a)
