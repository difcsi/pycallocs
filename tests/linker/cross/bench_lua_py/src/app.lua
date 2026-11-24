-- Lua -> Python benchmark, route "PolyLink by value": the argument table is
-- copied into struct Point (shapes.py.h) and arrives in Python as a copy.
package.path = arg[0]:match("^(.*/)") .. "../../../bench/lua_py/?.lua;" .. package.path
local bench = require("benchloop")
local linkpy = require("linkpy")
local shapes = linkpy.load("shapes")
bench.run2("divide", 2.0, shapes.divide, 6, 3)
bench.run1("describe", 7, shapes.describe, {x = 3, y = 4})
os.exit(0)
