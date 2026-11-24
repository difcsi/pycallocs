-- The timed loop shared by every route of the Lua -> Python per-call
-- benchmark (tests/linker/bench/cross_bench.py): the cross fixtures
-- tests/linker/cross/bench_lua_py*, the baselines in this directory, and the
-- Lua runtime lupa brings (so nothing here needs more than Lua 5.1).
--
--   local bench = require("benchloop")
--   bench.run2("divide", 2.0, f, 6, 3)      -- f(6, 3), expected 2.0
--   bench.run1("describe", 7, g, point)     -- g(point), expected 7
--   bench.info("view", "proxy")             -- a fact about the route
--
-- A run makes WARMUP untimed calls, checking each result, then N timed calls
-- (LINKPY_BENCH_CALLS, default 200000) and prints
--   bench <name>: <microseconds> us/call (<N> calls)
-- the line format of the python_bench_* fixtures. A wrong result is an
-- error: a route cannot be fast by computing something else.

local M = {}

local WARMUP = 1000

local function calls()
    local env = os.getenv("LINKPY_BENCH_CALLS")
    if env == nil or env == "" then return 200000 end
    local n = tonumber(env)
    if not n or n < 1 or n ~= math.floor(n) then
        error("benchloop: LINKPY_BENCH_CALLS must be a positive integer, got '" .. env .. "'", 0)
    end
    return n
end

local function check(name, expect, got)
    local ok
    if type(expect) == "number" then
        ok = type(got) == "number" and math.abs(got - expect) < 1e-6
    else
        ok = got == expect
    end
    if not ok then
        error(string.format("benchloop: %s returned %s, expected %s",
                            name, tostring(got), tostring(expect)), 0)
    end
end

local function report(name, n, t0, t1)
    print(string.format("bench %s: %.3f us/call (%d calls)", name, (t1 - t0) * 1e6 / n, n))
    io.stdout:flush()
end

function M.run1(name, expect, f, a)
    local n = calls()
    for _ = 1, WARMUP do check(name, expect, f(a)) end
    local r
    local t0 = os.clock()
    for _ = 1, n do r = f(a) end
    local t1 = os.clock()
    check(name, expect, r)
    report(name, n, t0, t1)
end

function M.run2(name, expect, f, a, b)
    local n = calls()
    for _ = 1, WARMUP do check(name, expect, f(a, b)) end
    local r
    local t0 = os.clock()
    for _ = 1, n do r = f(a, b) end
    local t1 = os.clock()
    check(name, expect, r)
    report(name, n, t0, t1)
end

function M.info(key, value)
    print(string.format("info %s=%s", key, tostring(value)))
    io.stdout:flush()
end

return M
