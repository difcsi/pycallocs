import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import print as m

m.hello()
m.print_int(42)
m.print_bool(True)
m.print_char("a")
m.print_float(3.14)
m.print_float(2)
m.print_multiargs(165, 12., 0x465)
