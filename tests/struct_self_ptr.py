import elflib
elflib.__path__.append("libs/")
from elflib import bintree

bt = bintree.bintree(1)
bt.left = bt
bt.right = bt
print(bt)
