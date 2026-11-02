-- Lua HOST, Python callee, with a user-defined C struct as the ABI pivot:
-- make_point's Python object crosses -> struct Point (shapes.py.h) -> Lua
-- table; describe takes it back the same way.
local linkpy = require("linkpy")
local m = linkpy.load("shapes")
local p = m.make_point(3, 4)
assert(p.x == 3 and p.y == 4, "make_point fields wrong")
local s = m.describe(p)
print("lua got Point(", p.x, ",", p.y, "), describe ->", s)
assert(s == 7, "describe(p) ~= 7")
os.exit(0)
