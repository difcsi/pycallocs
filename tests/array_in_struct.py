import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import arrays as m

print(m.make_empty()[0])

na = m.make_named_array(2)
frodo = na[0].first_name
frodo.__init__("Frodo")
na[0].first_name.__init__("Frodo")
na[0].last_name.__init__("Baggins")
na[1].first_name.__init__("Samsaget")
na[1].last_name.__init__("Gamgie")
print(na)
