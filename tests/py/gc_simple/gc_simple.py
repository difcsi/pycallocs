from pyc_testlib import add_libs
add_libs("bigrecursive")
from elflib import bigrecursive

# Should memleak if next two lines are decommented
#import gc
#gc.disable()

NB_ITER = 10**6

# Create a lot of cyclic object and drop them
for _ in range(NB_ITER):
    br = bigrecursive.bigrecursive()
    br.ptr = br

