-- Scalar marshalling: integer args in, number (float) out. The Lua analogue of
-- tests/simple_divide. `require("mytest")` returns this table of functions.

local M = {}

---@param a integer
---@param b integer
---@return number
function M.divide(a, b)
    return a / b
end

return M
