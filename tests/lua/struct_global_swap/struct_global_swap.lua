-- Phase 4: the defer-free window. swap_saved_hw overwrites the C global that
-- holds the only reference to the 42-object and returns that object; without
-- Begin/EndDeferFrees around ffi_call the delref would free it mid-call
-- (mirror of py/struct_global_swap).
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

do
    local a = c.hello_world(42, 9.99)
    c.save_hw(a)
end
collectgarbage(); collectgarbage()

local b = c.hello_world(0, 0.0)

print(c.swap_saved_hw(b))
print(c.swap_saved_hw(nil))
