-- Phase 3: calling foreign functions through libffi (mirror of py/basic_ops).
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local m = allocs.load(fixtures .. "/basic/basic.so")

print(m.triple(3))
print(m.inv(true))
print(m.mul(1.02, 5))
print(m.a())
print(m.triple)
