-- Native-GC milestone (forked VM only): host<->C reference cycles are
-- collected by the VM's own traversal over foreign memory -- the leak
-- gc_struct_cycle documents for the proxy extension, fixed natively.
local allocs = allocs
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local m = allocs.load(fixtures .. "/bigrecursive/bigrecursive.so")

collectgarbage(); collectgarbage()
local baseline = allocs._interncount()

-- self-cycle: the C self-pointer no longer pins the object
do
    local br = m.bigrecursive()
    br.ptr = br
end
collectgarbage(); collectgarbage()
print("self-cycle collected:", allocs._interncount() == baseline)

-- two-chunk cycle
do
    local a = m.bigrecursive()
    local b = m.bigrecursive()
    a.ptr = b
    b.ptr = a
end
collectgarbage(); collectgarbage()
print("two-chunk cycle collected:", allocs._interncount() == baseline)

-- reachable cycles stay alive, and their fields stay intact
local keep = m.bigrecursive()
keep.ptr = keep
collectgarbage(); collectgarbage()
print("live cycle kept:", allocs._interncount() == baseline + 1)
print("field intact:", keep.ptr == keep)
