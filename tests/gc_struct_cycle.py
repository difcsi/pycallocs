import elflib
elflib.__path__.append("libs/")
from elflib import bigrecursive

# Should memleak if next two lines are decommented
#import gc
#gc.disable()

NB_ITER = 10**6

# Create a lot of cyclic object and drop them
for _ in range(NB_ITER):
    br1 = bigrecursive.bigrecursive()
    br2 = bigrecursive.bigrecursive()
    br3 = bigrecursive.bigrecursive()
    br1.ptr = br2
    br2.ptr = br3
    br3.ptr = br1

