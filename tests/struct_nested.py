import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import nested_struct as m

o = m.outstruct()
a = o.a
a.data1 = 1
o.a.data2 = 2

b = m.instruct()
b.data1 = 3
b.data2 = 6
o.b = b
o.b.data2 = 4

b.data1 = 5

print(o)
print(a)
print(b)

m.print_outstruct(o)
