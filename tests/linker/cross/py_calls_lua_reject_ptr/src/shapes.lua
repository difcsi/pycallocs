-- Never reached: the auditor rejects the pointer signature before any call.

local M = {}

---@param p Point
function M.bump(p)
    p.x = p.x + 1
end

return M
