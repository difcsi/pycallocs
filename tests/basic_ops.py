import elflib
elflib.__path__.append("libs/")
from elflib import basic as m

print(m.triple(3))
print(m.inv(True))
print(m.mul(1.02, 5))
print(m.a())
