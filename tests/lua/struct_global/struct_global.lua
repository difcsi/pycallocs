-- Phase 4: a C global pointer keeps a Lua-constructed struct alive after the
-- last Lua reference is dropped and collected (mirror of py/struct_global).
-- The trapptrwrites barrier in the fixture drives the liballocs GC-policy
-- callbacks, which maintain the strong slot->proxy table.
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

print(c.retrieve_hw())

do
    local a = c.hello_world()
    a.hello = 42
    a.world = 9.99
    c.save_hw(a)
end
collectgarbage(); collectgarbage()
print(c.retrieve_hw())

do
    local b = c.hello_world()
    b.hello = 0
    b.world = 0.0
    c.save_hw(b)
end
collectgarbage(); collectgarbage()
print(c.retrieve_hw())
