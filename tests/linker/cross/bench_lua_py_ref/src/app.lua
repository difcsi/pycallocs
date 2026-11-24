-- Lua -> Python benchmark, the two by-reference routes. One typed C object
-- (from the allocscc-built points.so) is handed to Python by address.
--
--   "PolyLink by reference": stock Lua, the object is a luaproxy userdata.
--   "PolyLua native view":   the contrib/lua fork, whose built-in `allocs`
--                            library makes the object a native foreign value.
--
-- The generated host wrapper asks require("luaproxy") for the address of its
-- argument; the fork's `allocs` has the same surface, so on the fork it stands
-- in for luaproxy and luaproxy.so is never loaded. Which of the two ran is
-- printed (`info view=...`) for the driver to check against the route label.
package.path = arg[0]:match("^(.*/)") .. "../../../bench/lua_py/?.lua;" .. package.path
local bench = require("benchloop")
local is_native, native = pcall(require, "allocs")
if is_native then package.loaded.luaproxy = native end
local allocs = require("luaproxy")
bench.info("view", is_native and "native" or "proxy")

local p = allocs.load("./points.so").new_point(3, 4)
local shapes = require("linkpy").load("shapes")

-- Shared, not copied: a write made in Python must be visible here.
shapes.bump(p)
assert(p.x == 103 and p.y == 4, "by-reference write made in Python is not visible in Lua")
p.x = 3

bench.run1("describe", 7, shapes.describe, p)
os.exit(0)
