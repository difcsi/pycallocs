-- Struct crossing, Python host -> Lua callee: Point travels as a Lua table on
-- this side and a struct Point (shapes.lua.h) on the wire.

local M = {}

---@class Point
---@field x integer
---@field y integer

---@param x integer
---@param y integer
---@return Point
function M.make_point(x, y)
    return { x = x, y = y }
end

---@param p Point
---@return integer
function M.describe(p)
    return p.x + p.y
end

return M
