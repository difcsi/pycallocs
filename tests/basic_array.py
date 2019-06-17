import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import basic as m

char = m.signed_char_8

int_arr = m.uint_16.array([1,2,3,4,5])
print(int_arr)

char_arr = char.array("why not ?")
print(char_arr)
byte_recons = bytes()
for elt in char_arr:
    byte_recons += elt
print(byte_recons)

