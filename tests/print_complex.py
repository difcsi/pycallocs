import elflib
elflib.__path__.append("libs/")
from elflib import complex as m

m.print_complex(0)

# There seems to be a bug in libffi here...
m.print_complex(31j + 12)
