import elflib
elflib.__path__.append("libs/")
from elflib import bintree

bt = bintree.bintree(0)
bt.left = bintree.bintree(-1)
bt.right = bintree.bintree(1)
bt_cpy = bintree.bst_copy_node(bt)
del bt
bintree.bst_insert(bt_cpy, 2)
bintree.print_bintree(bt_cpy)

