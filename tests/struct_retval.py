import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

a = m.make_hw(3, 5.5)
print(a)
