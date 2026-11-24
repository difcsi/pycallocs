-- Lua -> Python benchmark, route "hand-written bridge" (the baseline):
-- handbridge.so is a Lua C module written against the CPython embedding API.
local here = arg[0]:match("^(.*/)")
package.path = here .. "?.lua;" .. package.path
local bench = require("benchloop")

local dir = assert(os.getenv("BENCH_MODULE_DIR"), "BENCH_MODULE_DIR is not set")
-- Bind the module, and with it libpython, with RTLD_GLOBAL first: extension
-- modules the embedded interpreter imports resolve their Py* symbols against
-- the global scope (linkpy.lua does the same for the linkstub).
assert(package.loadlib(dir .. "/handbridge.so", "*"))
package.cpath = dir .. "/?.so;" .. package.cpath
local bridge = require("handbridge")

bench.run2("divide", 2.0, bridge.divide, 6, 3)
bench.run1("describe", 7, bridge.describe, {x = 3, y = 4})
os.exit(0)
