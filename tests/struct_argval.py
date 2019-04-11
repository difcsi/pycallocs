import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

a = m.hello_world()
a.hello = 42
a.world = 9.99
m.print_hw(a)
