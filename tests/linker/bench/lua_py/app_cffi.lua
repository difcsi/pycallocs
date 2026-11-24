-- Lua -> Python benchmark, route "cffi":
-- cffishim.so is a Lua C module that calls the C functions of a library cffi
-- generated in embedding mode (cffi_build.py); only the Lua half is by hand.
local here = arg[0]:match("^(.*/)")
package.path = here .. "?.lua;" .. package.path
local bench = require("benchloop")

local dir = assert(os.getenv("BENCH_MODULE_DIR"), "BENCH_MODULE_DIR is not set")
-- Bind the module, and with it libpython, with RTLD_GLOBAL first: extension
-- modules the embedded interpreter imports resolve their Py* symbols against
-- the global scope (linkpy.lua does the same for the linkstub).
assert(package.loadlib(dir .. "/cffishim.so", "*"))
package.cpath = dir .. "/?.so;" .. package.cpath
local shim = require("cffishim")

bench.run2("divide", 2.0, shim.divide, 6, 3)
bench.run1("describe", 7, shim.describe, {x = 3, y = 4})
os.exit(0)
