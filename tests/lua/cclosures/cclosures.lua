-- Phase 5: Lua functions as C function pointers (mirror of py/cclosures).
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local closures = allocs.load(fixtures .. "/closures/closures.so")

local function hi()
    print("Hi")
end

local function add_one(i)
    return i + 1
end

-- `repeat` is a Lua keyword, so the C symbol is reached by string key
closures["repeat"](3, allocs.void.fun()(hi))

local add_one_closure = allocs.int.fun(allocs.int)(add_one)
print(closures.fold_int(5, add_one_closure))
print(closures.fold_int(42, add_one_closure))

print(closures.fold_int(10, allocs.int.fun(allocs.int)(function(x) return x * 2 + 1 end)))
