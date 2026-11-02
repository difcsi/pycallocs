-- Phase 4: proxies and their backing chunks are reclaimed when unreferenced;
-- bulk churn must not accumulate (mirror of py/gc_simple, scaled down --
-- verify with the liballocs exit-time summary on stderr if in doubt).
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

for i = 1, 100000 do
    local hw = c.hello_world(i % 100, 0.5)
end
collectgarbage(); collectgarbage()
print("done")

-- a released proxy is inert
local hw = c.hello_world(1, 2.0)
hw:release()
print(tostring(hw))
local ok, err = pcall(function() return hw.hello end)
print(ok, err:match("released foreign proxy") ~= nil)
