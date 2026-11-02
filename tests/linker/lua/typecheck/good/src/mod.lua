-- Fully LuaLS-annotated module: every linked signature is present and correct,
-- so this is a good candidate even under --strict. Mirror of the Python
-- tests/typecheck/good fixture.

local M = {}

---@class Point
---@field x integer
---@field y integer

---@param a integer
---@param b integer
---@return integer
function M.add(a, b)
    return a + b
end

---@param x integer
---@param y integer
---@return Point
function M.make_point(x, y)
    return { x = x, y = y }
end

---@param p Point
---@return nil
function M.bump(p)
    p.x = p.x + 1
end

return M
