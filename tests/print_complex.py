import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import complex as m

m.print_complex(0)

# There seems to be a bug in libffi here...
m.print_complex(31j + 12)
