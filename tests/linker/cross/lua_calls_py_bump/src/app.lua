-- Cross-language BY-REFERENCE, Lua host -> Python callee.
--
-- The host mints a luaproxy proxy over a typed C struct (from the
-- allocscc-built points.so); passing it to the Python callee's
-- bump(struct Point *) hands the SAME C object to Python as a pycallocs
-- proxy. Python's write must be visible through the Lua proxy afterwards.
local linkpy = require("linkpy")
local allocs = require("luaproxy")
local pts = allocs.load("./points.so")
local p = pts.new_point(3, 4)
local shapes = linkpy.load("shapes")
shapes.bump(p)
print("lua sees x =", p.x, "y =", p.y)
assert(p.x == 103 and p.y == 4, "Python's by-ref write not visible in Lua")
os.exit(0)
