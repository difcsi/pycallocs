-- Phase 5 demo, NOT a parity test: host-C reference cycles are uncollectable
-- in the Lua port. Lua's GC has no traverse/clear protocol over foreign
-- memory, so a C struct whose pointer field keeps its own proxy alive (via
-- the strong slot->proxy table) can never be reclaimed automatically -- the
-- Python port breaks such cycles with tp_traverse/tp_clear, which has no Lua
-- counterpart. The documented policy: such cycles leak unless broken by hand
-- with proxy:release().
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local m = allocs.load(fixtures .. "/bigrecursive/bigrecursive.so")

-- this one leaks (by design): the C self-pointer pins the proxy forever
do
    local br = m.bigrecursive()
    br.ptr = br
end
collectgarbage(); collectgarbage()
print("cycle dropped (leaks by design)")

-- release() breaks the cycle by hand: slots cleared, chunk freed
do
    local br = m.bigrecursive()
    br.ptr = br
    br:release()
end
collectgarbage(); collectgarbage()
print("released cycle reclaimed")
