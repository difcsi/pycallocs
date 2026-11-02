-- By-reference from ANOTHER VM: p is a luaproxy proxy over the same C struct
-- the Python host holds a pycallocs proxy for. The write goes straight to C
-- memory, where the host observes it.

local M = {}

---@param p Point
function M.bump(p)
    p.x = p.x + 100
end

return M
