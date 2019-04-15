import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

a = m.hello_world()
print(a)
a.hello = 42
a.world = 9.99
print(a)

b = m.hello_world(1, 1)
print(b)

c = m.hello_world(hello = 2, world = float('nan'))
print(c)

d = m.hello_world(5)
print(d)

e = m.hello_world(d)
d.world = float('inf')
print(d)
print(e)
