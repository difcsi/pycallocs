import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import closures

def hi():
    print("Hi")

def add_one(i):
    return i+1

closures.repeat(3, closures.__FUN_FROM___FUN_TO_void(hi))

add_one_closure = closures.__FUN_FROM___ARG0_int_32__FUN_TO_int_32(add_one)
print(closures.fold_int(5, add_one_closure))
print(closures.fold_int(42, add_one_closure))

print(closures.fold_int(10, closures.__FUN_FROM___ARG0_int_32__FUN_TO_int_32(lambda x: (x*2)+1)))
