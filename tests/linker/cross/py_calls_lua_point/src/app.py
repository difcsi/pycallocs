"""Python HOST, Lua callee, with a user-defined C struct as the ABI pivot.

make_point's result crosses Lua table -> struct Point (the C layout from
shapes.lua.h) -> Python SimpleNamespace; describe sends it back the same way.
"""
import sys

import linkpy

m = linkpy.load("shapes")
p = m.make_point(3, 4)
s = m.describe(p)
print("python got Point(", p.x, ",", p.y, "), describe ->", s)
sys.exit(0 if (p.x == 3 and p.y == 4 and s == 7) else 1)
