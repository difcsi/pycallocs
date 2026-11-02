-- By-value struct RETURN: Lua returns a table, the trampoline marshals it into a
-- C `struct HelloWorld` field-by-field (reflection). Analogue of tests/struct.

local M = {}

---@class HelloWorld
---@field a integer

---@return HelloWorld
function M.phelloworld2()
    print("Hello World from lua")
    return { a = 13 }
end

return M
