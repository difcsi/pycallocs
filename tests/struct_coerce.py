import elflib
elflib.__path__.append("libs/")
from elflib import composite as m

class Hw:
    def __init__(self, h, w):
        self.hello = h
        self.world = w

class Hcw(Hw):
    def __init__(self, h, c, w):
        Hw.__init__(self, h, w)
        self.crazy = c

a = Hw(42, 9.99)
m.print_hw(a)
b = Hcw(-1, "foo", float('nan'))
m.print_hw(b)
m.print_hw((49,3))
m.print_hw({'hello':0, 'world':2.71828})
m.print_hw({'hello':1, 'crazy':None, 'world':3.14159})

try:
    m.print_hw((a, b))
    exit(1)
except TypeError:
    pass

try:
    m.print_hw((-1, "foo", float('nan')))
    exit(1)
except TypeError:
    pass
