-- Fully ---@-annotated module with NO hand-written include/mymod.lua.h: the build
-- derives the C header from these LuaLS hints via tools/luahgen.py, then runs the
-- normal pipeline (pystubgen -> stub lib -> DWARF -> audit_lua). The Lua analogue
-- of tests/generated_header, exercising a scalar function and a struct return.

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

return M
