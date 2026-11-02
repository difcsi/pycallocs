-- Forked-VM milestone: pointer arithmetic and ordering on
-- address-class foreign values, via the shared metatable.
local allocs = allocs

local a = allocs.int.array{1, 2, 3, 4, 5}

local p = a + 1
print(#a, #p, p[1])

local q = a + 3
print(q - a, a[4] == q[1])

print(a < q, q <= a, a <= a + 0)

-- writes through a view land in the source array
p[1] = 22
print(a[2])

-- ordering across different allocations is an error
local b = allocs.int.array{9}
local ok = pcall(function() return a < b end)
print("cross-alloc compare fails:", not ok)
