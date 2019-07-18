import elflib
elflib.__path__.append("libs/")
from elflib import bigrecursive

# Should memleak if next two lines are decommented
#import gc
#gc.disable()

NB_ITER = 10**6
br = bigrecursive.bigrecursive()

# Create a lot of cyclic object and drop them
for _ in range(NB_ITER):
    brarr = bigrecursive.bigrecursive.array([br, br])
    brarr[1].ptr = brarr

