-- Several incompatibilities with include/mod.lua.h: a conflicting field type, a
-- wrong-arity / wrong-type function, and a C function with no Lua definition.
-- Mirror of the Python tests/typecheck/bad fixture; expected to fail.

local M = {}

---@class Point
---@field x integer
---@field y string   -- conflicts with C `int y;`

---@param a string   -- wrong type, and wrong arity (1 vs 2)
---@return integer   -- wrong return type (C is float)
function M.divide(a)
    return 0
end

---@param p Point
---@return integer
function M.describe(p)
    return p.x
end

return M
