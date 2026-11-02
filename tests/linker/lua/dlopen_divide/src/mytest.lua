-- Same module as the Lua simple_divide; the difference is in main.c, which
-- resolves divide() at runtime via dlopen + dlsym rather than linking it at load
-- time. `require("mytest")` returns this table of functions.

local M = {}

---@param a integer
---@param b integer
---@return number
function M.divide(a, b)
    return a / b
end

return M
