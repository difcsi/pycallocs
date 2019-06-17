import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import closures

def cat():
    print("Meow.")

def dog():
    print("Woof!")

cs = closures.closure_struct()
cs.fun1 = closures.__FUN_FROM___FUN_TO_void(cat)
cs.fun2 = closures.__FUN_FROM___FUN_TO_void(dog)
cs.fun3 = closures.__FUN_FROM___FUN_TO_void(cat)
del cat

closures.call_closure_struct(cs);
