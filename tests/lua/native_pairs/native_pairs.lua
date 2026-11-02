-- Forked-VM milestone: pairs() iterates foreign values natively --
-- structs yield (field, value) in declaration order, arrays yield
-- (index, element).
local allocs = allocs
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local c = allocs.load(fixtures .. "/composite/composite.so")

local hw = c.make_hw(4, 8.5)
for k, v in pairs(hw) do
    print(k, v)
end

local arr = allocs.int.array{10, 20, 30}
for i, v in pairs(arr) do
    io.write(i > 1 and " " or "", i, "=", v)
end
print()
