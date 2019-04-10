import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

a = m.quantum_cat()
a.dead = 42
print(a)

a.alive = 9.99
print(a)
