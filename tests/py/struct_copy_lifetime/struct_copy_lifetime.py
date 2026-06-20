from pyc_testlib import add_libs
add_libs("bintree")
from elflib import bintree

bt = bintree.bintree(0)
bt.left = bintree.bintree(-1)
bt.right = bintree.bintree(1)
bt_cpy = bintree.bintree(bt)
del bt
bintree.bst_insert(bt_cpy, 2)
bintree.print_bintree(bt_cpy)

