import elflib
elflib.__path__.append("libs/")
from elflib import basic as m

char = m.signed_char_8

int_arr = m.uint_16.array([1,2,3,4,5])
print(int_arr[:2])
print(int_arr[2:])
print(int_arr[:])
print(int_arr[-2:0])

char_arr = char.array("why not ?")
print(char_arr[:3])
print(char_arr[4:7])
print(char_arr[-1:100])
print(char_arr[0:0])
