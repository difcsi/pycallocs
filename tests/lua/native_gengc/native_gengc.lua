-- Native-GC milestone (forked VM only): cycle collection must also
-- work under the generational collector -- foreign objects are
-- re-traversed in every minor collection (the thread pattern), so C
-- memory gaining references to young objects is never missed.
local allocs = allocs
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local m = allocs.load(fixtures .. "/bigrecursive/bigrecursive.so")

collectgarbage("generational")
collectgarbage(); collectgarbage()
local baseline = allocs._interncount()

do
    local br = m.bigrecursive()
    br.ptr = br
end
collectgarbage(); collectgarbage()
print("gen self-cycle collected:", allocs._interncount() == baseline)

do
    local a = m.bigrecursive()
    local b = m.bigrecursive()
    a.ptr = b
    b.ptr = a
    -- minor steps in between must neither free nor miss foreign edges
    collectgarbage("step"); collectgarbage("step")
    print("still reachable mid-steps:", a.ptr == b and b.ptr == a)
end
collectgarbage(); collectgarbage()
print("gen two-chunk collected:", allocs._interncount() == baseline)

local keep = m.bigrecursive()
keep.ptr = keep
collectgarbage(); collectgarbage()
print("gen live cycle kept:", allocs._interncount() == baseline + 1,
      keep.ptr == keep)
