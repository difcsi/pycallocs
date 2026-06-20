from pyc_testlib import add_libs
add_libs("basic")
from elflib import basic as m

char = m.signed_char__8

int_arr = m.uint__16.array([1,2,3,4,5])
int_arr02 = int_arr[0:2]
int_arr25 = int_arr[2:5]
del int_arr
char_arr = char.array("why not ?")

print(int_arr02)
del int_arr02
print(int_arr25)
del int_arr25
print(char_arr[-1])
