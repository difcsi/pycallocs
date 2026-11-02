-- Forked-VM milestone: foreign values are interned one per
-- (address, type, length), so equality, rawequal and table-key
-- identity all work by C address with no metamethods involved.
local allocs = allocs
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

local hw = c.make_hw(4, 8.5)
local t = {}
t[hw] = "found by identity"

-- the same object handed back through C is the same Lua value
c.save_hw(hw)
local back = c.retrieve_hw()
print("eq:", back == hw)
print("rawequal:", rawequal(back, hw))
print("table key:", t[back])

-- distinct allocations of equal content stay distinct
local other = c.make_hw(4, 8.5)
print("distinct:", other ~= hw, t[other] == nil)
