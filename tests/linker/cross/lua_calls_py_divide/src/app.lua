-- Lua HOST calling a Python function through linkpy (scalar crossing).
local linkpy = require("linkpy")
local m = linkpy.load("mytest")
local r = m.divide(6, 3)
print("lua got", r)
assert(math.abs(r - 2.0) < 1e-6, "divide(6,3) ~= 2.0")
os.exit(0)
