import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import composite as m

a = m.make_dead(3)
print(a.dead)

a = m.make_alive(3.14)
print(a.alive)
