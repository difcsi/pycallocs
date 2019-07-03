import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import closures

def cat():
    print("Meow.")

def dog():
    print("Woof!")

cs = closures.closure_struct()
cs.fun1 = closures.void.fun()(cat)
cs.fun2 = closures.void.fun()(dog)
cs.fun3 = closures.void.fun()(cat)
del cat

closures.call_closure_struct(cs);
