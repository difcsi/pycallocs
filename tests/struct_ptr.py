import elflib
elflib.__path__.append("libs/")
from elflib import composite
from elflib import bintree

hw = composite.hello_world(666)
composite.compl_hw(hw)
print(hw)

bt = bintree.bintree(5)
bintree.bst_insert(bt, 1)
bintree.bst_insert(bt, 44)
bintree.bst_insert(bt, -8)
bintree.bst_insert(bt, 3)
bintree.bst_insert(bt, 98)
bintree.bst_insert(bt, 8)
print(bt)
bintree.print_bintree(bt)
