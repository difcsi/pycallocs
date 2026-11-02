-- Scalar crossing, Python host -> Lua callee: integer args in, number out.
-- The cross twin of tests/linker/lua/simple_divide.

local M = {}

---@param a integer
---@param b integer
---@return number
function M.divide(a, b)
    return a / b
end

return M
