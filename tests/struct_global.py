import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

print(m.retrieve_hw())

a = m.hello_world()
a.hello = 42
a.world = 9.99
m.save_hw(a)
del a

print(m.retrieve_hw())

b = m.hello_world()
b.hello = 0
b.world = 0.0
m.save_hw(b)

print(m.retrieve_hw())

