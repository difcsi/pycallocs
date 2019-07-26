import elflib
elflib.__path__.append("libs/")
from elflib import closures

def hi():
    print("Hi")

def add_one(i):
    return i+1

closures.repeat(3, elflib.void.fun()(hi))

add_one_closure = elflib.int.fun(elflib.int)(add_one)
print(closures.fold_int(5, add_one_closure))
print(closures.fold_int(42, add_one_closure))

print(closures.fold_int(10, elflib.int.fun(elflib.int)(lambda x: (x*2)+1)))
