import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import nested_struct as m

o = m.outstruct()
a = o.a
del o

a.data1 = 1
a.data2 = 2
print(a)
