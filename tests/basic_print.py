import elflib
elflib.__path__.append("libs/")
from elflib import print as m

m.hello()
m.print_int(42)
m.print_bool(True)
m.print_char("a")
m.print_float(3.14)
m.print_float(2)
m.print_multiargs(165, 12., 0x465)
