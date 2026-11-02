-- Phase 2: array/pointer proxies over fixture globals -- lengths, 1-based
-- element access, ipairs, writes, slices, char arrays as strings.
local allocs = allocs or require "luaproxy"
local fixtures = os.getenv("PYCALLOCS_TEST_LIBS")

local a = allocs.load(fixtures .. "/alphanum/alphanum.so")

-- char array global (static length includes the NUL element)
print(#a.ALPHABET)
print(a.ALPHABET:str())
print(tostring(a.ALPHABET))
print(a.ALPHABET[1], a.ALPHABET[26])

-- int array global
print(#a.DIGITS)
print(a.DIGITS[1], a.DIGITS[10])
print(a.DIGITS)

-- iteration stops past the end (out-of-range reads yield nil)
local sum = 0
for _, v in ipairs(a.DIGITS) do sum = sum + v end
print(sum)

-- element writes hit the real memory
a.DIGITS[1] = 42
print(a.DIGITS[1])
a.DIGITS[1] = 0

-- slices are interior proxies anchored to their source
local mid = a.DIGITS:sub(3, 5)
print(#mid, mid[1], mid[3])
mid[1] = 20
print(a.DIGITS[3])
mid[1] = 2
