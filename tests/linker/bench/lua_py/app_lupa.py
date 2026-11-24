#!/usr/bin/env python3
"""Lua -> Python benchmark, route "lupa".

lupa embeds Lua in Python, so here the process is Python and Lua is the
guest; what is timed is unchanged: benchloop.lua, running in Lua, calls the
callee's Python functions (shapes.py, on PYTHONPATH). `describe` receives
lupa's wrapper of the Lua table, which is the same table, not a copy.

lupa brings its own Lua: its version is printed as an `info` line.
"""
import os

import lupa

import shapes

HERE = os.path.dirname(os.path.abspath(__file__))

CHUNK = """
function(dir, lupa_version, divide, describe)
    package.path = dir .. "/?.lua;" .. package.path
    local bench = require("benchloop")
    bench.info("lupa", lupa_version)
    bench.info("lua", (_VERSION:gsub(" ", "-")))
    bench.run2("divide", 2.0, divide, 6, 3)
    bench.run1("describe", 7, describe, {x = 3, y = 4})
end
"""


def main():
    runtime = lupa.LuaRuntime()
    runtime.eval(CHUNK)(HERE, lupa.__version__, shapes.divide, shapes.describe)


if __name__ == "__main__":
    main()
