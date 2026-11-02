-- Phase 1 milestone: load fixture libraries, read scalar and enum globals,
-- inspect published foreign types. (Struct access is exercised from Phase 2
-- on, once values of composite type can be obtained from the fixtures.)
local allocs = allocs or require "luaproxy"

local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

-- scalar globals
local alpha = allocs.load(fixtures .. "/alphanum/alphanum.so")
print(alpha.NB_LETTERS, alpha.NB_DIGITS)

-- enum global: reads as its integer value
local enums = allocs.load(fixtures .. "/enums/enums.so")
print(enums.favourite)

-- module-level base types are callable foreign types
print(allocs.int)
print(allocs.double)
print(allocs.char)

-- functions/pointers are not materialised yet; they are reported, not fatal
local unsupported = alpha.__unsupported ~= nil
print("has __unsupported table:", unsupported)
