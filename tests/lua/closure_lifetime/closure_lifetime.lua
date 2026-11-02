-- Phase 5: a C struct field is the only thing keeping a closure alive
-- (mirror of py/closure_lifetime). The closure trampoline lives in mmap'd
-- memory liballocs cannot track, so the retention comes from
-- LuaProxy_RetainStoredPtr, not the GC-policy barrier.
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local closures = allocs.load(fixtures .. "/closures/closures.so")

local function cat() print("Meow.") end
local function dog() print("Woof!") end

local cs = closures.closure_struct()
cs.fun1 = allocs.void.fun()(cat)
cs.fun2 = allocs.void.fun()(dog)
cs.fun3 = allocs.void.fun()(cat)
cat = nil
dog = nil
collectgarbage(); collectgarbage()

closures.call_closure_struct(cs)
