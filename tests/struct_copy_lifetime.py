import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import bintree

bt = bintree.bintree(0)
bt.left = bintree.bintree(-1)
bt.right = bintree.bintree(1)
bt_cpy = bintree.bintree(bt)
del bt
bintree.bst_insert(bt_cpy, 2)
bintree.print_bintree(bt_cpy)

