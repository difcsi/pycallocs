from pyc_testlib import add_libs
add_libs("complex")
from elflib import complex as m

m.print_complex(0)

# There seems to be a bug in libffi here...
m.print_complex(31j + 12)
