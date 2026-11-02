"""Cross-language BY-REFERENCE, Python host -> Lua callee.

The host mints a pycallocs proxy over a typed C struct (from the
allocscc-built points.so via elflib); passing it to the Lua callee's
bump(struct Point *) hands the SAME C object to Lua as a luaproxy proxy.
Lua's write must be visible through the Python proxy afterwards.
"""
import sys

import linkpy
from elflib import points

p = points.new_point(3, 4)
shapes = linkpy.load("shapes")
shapes.bump(p)
print("python sees x =", p.x, "y =", p.y)
sys.exit(0 if (p.x == 103 and p.y == 4) else 1)
