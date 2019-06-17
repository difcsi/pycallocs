import foreign_library_finder
foreign_library_finder.search_paths.append("libs/")
import bintree

bt = bintree.bintree(1)
bt.left = bt
bt.right = bt
print(bt)
