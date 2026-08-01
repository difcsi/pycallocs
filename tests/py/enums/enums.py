from pyc_testlib import add_libs
add_libs("enums")
from elflib import enums as m

# Enum global is read as its underlying integer value (GREEN == 5).
print(m.favourite)
# Enum passed as argument and returned (brighten(RED) -> 1).
print(m.brighten(0))
# Enum returned then passed back in as an argument.
print(m.as_int(m.favourite))
# Larger enum value round-trips through a libffi call.
print(m.brighten(m.favourite))
