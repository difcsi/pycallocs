-- By-value struct ARGUMENT: C passes a `struct Point` by value; the trampoline
-- marshals it into a Lua table field-by-field (reflection). The mirror of
-- lua_struct, and the analogue of tests/struct_arg.

local M = {}

---@class Point
---@field x integer
---@field y integer

---@param p Point
---@return integer
function M.describe(p)
    return p.x + p.y
end

return M
