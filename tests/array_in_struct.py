import elflib
elflib.__path__.append("libs/")
from elflib import arrays as m

print(m.make_empty())

na = m.make_named_array(2)
frodo = na[0].first_name
frodo.__init__("Frodo")
na[0].last_name.__init__("Baggins")
na[1].first_name.__init__("Samsaget")
na[1].last_name.__init__("Gamgie")
print(na)
