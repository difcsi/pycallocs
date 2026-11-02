-- By-reference: p arrives from C as a luaproxy proxy over the live C struct,
-- so reading p.x reads C memory and writing p.x writes straight back to it.
-- The Lua mirror of ../../cpython/ptr_arg's mytest.py.

local M = {}

---@param p Point
function M.bump(p)
    print("Lua got Point with x =", p.x, "y =", p.y)
    p.x = p.x + 100
end

return M
