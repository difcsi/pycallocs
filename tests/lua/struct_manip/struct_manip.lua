-- Phase 2: struct/union construction and manipulation from Lua, using the
-- types the composite fixture publishes (through its function signatures).
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

-- the fixture's types are published even though its functions are not yet
-- callable (they sit in __unsupported until the libffi phase)
print(c.hello_world)

-- positional construction
local hw = c.hello_world(5, 2.5)
print(hw)
print(hw.hello, hw.world)

-- field writes
hw.hello = 7
hw.world = 0.5
print(hw.hello, hw.world)

-- named (table) construction; absent fields are zero-initialised
local hw2 = c.hello_world{world = 1.5}
print(hw2.hello, hw2.world)

-- copy construction: same values, distinct object
local hw3 = c.hello_world(hw)
print(hw3.hello, hw3.world)
print(hw3 == hw)
hw3.hello = 100
print(hw.hello, hw3.hello)

-- zero-initialising constructor
local z = c.hello_world()
print(z.hello, z.world)

-- unions: fields alias the same storage
local qc = c.quantum_cat()
qc.dead = 1
print(qc.dead)
qc.alive = 0.25
print(qc.alive)
