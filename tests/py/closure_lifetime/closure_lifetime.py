from pyc_testlib import add_libs
add_libs("closures")
from elflib import closures

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
