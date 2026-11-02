-- Loader for linkpy cross-language modules, Lua-host side (linkpy.py's twin).
-- Run the host program under the CALLEE language's LD_AUDIT auditor with
-- LINKPY_HOST_LANG=lua and HOST_TEMPLATE_PATH set; then:
--
--   local linkpy = require("linkpy")
--   local mytest = linkpy.load("mytest")
--   print(mytest.divide(6, 3))
--
-- package.loadlib dlopens the stub init library (the auditor redirects it to
-- the JIT-compiled liblinkstub.so) and returns its luaopen_linkstub, which we
-- call with the host's lua_State; it registers every foreign module in
-- package.loaded (so plain require() works afterwards too) and returns
-- { modname = table, ... }.

local M = {}

local reg

function M.load(name, path)
    path = path or "./libinit.so"
    if not reg then
        -- Link with RTLD_GLOBAL first ("*" mode): the linkstub's dependency
        -- (the callee VM's library) must be globally visible so extensions the
        -- callee VM dlopens later resolve their VM symbols -- e.g. the callee
        -- Python's `allocs` extension resolving Py* on the by-reference path.
        local ok, gerr = package.loadlib(path, "*")
        if not ok then
            -- Without the auditor the load may fail outright, or succeed on
            -- the abort-stub libinit.so -- the next loadlib catches that case.
            error("linkpy: cannot load " .. path .. ": " .. tostring(gerr) ..
                  " (is LD_AUDIT set to the callee auditor, with " ..
                  "LINKPY_HOST_LANG=lua and HOST_TEMPLATE_PATH?)")
        end
        local f, err = package.loadlib(path, "luaopen_linkstub")
        if not f then
            error("linkpy: no luaopen_linkstub in " .. path .. ": " ..
                  tostring(err) .. " (is LD_AUDIT set to the callee auditor?)")
        end
        reg = f()
    end
    local mod = reg[name]
    if not mod then
        error("linkpy: no cross-language module '" .. tostring(name) .. "'")
    end
    return mod
end

return M
