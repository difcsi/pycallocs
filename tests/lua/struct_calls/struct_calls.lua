-- Phase 3: structs and unions across call boundaries -- by value, by
-- pointer, and out-parameter cells (mirror of py/struct_argval, struct_retval,
-- union_argval/retval and parts of struct_ptr).
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

-- struct return by value
local hw = c.make_hw(4, 8.5)
print(hw.hello, hw.world)

-- struct passed by value
c.print_hw(hw)

-- struct passed by pointer: callee mutates through the pointer
c.compl_hw(hw)
print(hw.world)

-- read-only by-pointer accessor
print(c.sum_hw(hw))

-- a plain table is coerced by value where a struct is expected
c.print_hw({hello = 1, world = 0.25})

-- unions across the boundary
local qc = c.make_dead(7)
print(qc.dead)

-- scalar out-parameter cell: C sees int*, Lua reads the result back
local w = allocs.int(5)
print(#w, w[1])

-- pointer round-trip: C keeps the pointer, hands the same object back
c.save_hw(hw)
local back = c.retrieve_hw()
print(back == hw)
print(back.hello)
back.hello = 11
print(hw.hello)
