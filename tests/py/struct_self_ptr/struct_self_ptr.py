from pyc_testlib import add_libs
add_libs("bintree")
from elflib import bintree

bt = bintree.bintree(1)
bt.left = bt
bt.right = bt
print(bt)
