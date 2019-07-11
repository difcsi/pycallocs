import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

a = m.hello_world(42, 9.99)
m.save_hw(a)
del a

b = m.hello_world(0, 0.0)

print(m.swap_saved_hw(b))
print(m.swap_saved_hw(None))

