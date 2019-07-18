import elflib
elflib.__path__.append("libs/")
from elflib import arrays as m

arr = m.make_int_array(3)
print(len(arr))

for i in range(len(arr)):
    arr[i] = i

print(arr)

print(m.get_index(arr, 1))

try:
    arr[42]
    exit(1)
except IndexError:
    pass

arr = m.make_int_array(0)
print(arr)
