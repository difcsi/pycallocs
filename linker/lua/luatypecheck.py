#!/usr/bin/env python3
"""Type-check Lua sources against the C types LinkPy links them to.

The Lua analogue of ``tools/pytypecheck.py`` and the dual of ``tools/luahgen.py``:
it takes the C side of the contract (recovered function/struct types) and checks
that a Lua module's LuaLS ``---@`` annotations are compatible with it -- the
static form of the question the auditor asks at marshal time.

The C-side machinery (header/lmap parsing, type classification, check grading)
is shared with the Python tools via ``tools/linktypes.py``; this file is the thin
Lua layer: it reduces a ``.lua`` module's scanned annotations (``tools/luaparse``)
to the neutral model and supplies the Lua annotation vocabulary
(``integer``/``number``/``boolean``/``string``/``nil``).

Two sources of C signatures are supported, exactly as for Python:

  * ``--include DIR`` / ``--header FILE`` -- ``*.lua.h`` stub headers (also
    recovers struct layouts so class fields are checked); and
  * ``--lmap -`` -- the colon-delimited ``FunLinkEntry::to_string()`` lines the
    DWARF path produces, piped on stdin by the ``lualinkchk`` runner.

The C->Lua mapping (the inverse of luahgen's): ``int -> integer``,
``double/float -> number``, ``bool -> boolean``, ``const char * -> string``,
``void`` return ``-> nil``, ``struct X`` / ``struct X *`` ``-> class X``.
Findings are graded by linkability (error vs warning), a module is a good
candidate iff it has zero errors, and ``--strict`` promotes warnings to errors.
Exit code: 0 candidate / 1 errors / 2 usage.
"""

import argparse
import os
import sys

# linktypes (the shared vocabulary/model module) lives in ../common, which is
# not on sys.path when this tool is run by absolute path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))

import linktypes
import luaparse
from linktypes import ABSENT, Ann, FnModel, Vocab

# C->Lua annotation vocabulary for the neutral kinds in linktypes.
LUA_VOCAB = Vocab("Lua", int_name="integer", float_name="number",
                  bool_name="boolean", str_name="string", void_name="nil")


def _ann(t):
    """Reduce a scanned Lua type ``None | (spelling, is_bare)`` to the model.

    ``None`` -> ABSENT (no annotation). A bare spelling is verifiable; a
    compound type (union/array/generic) is present-but-unverifiable.
    """
    if t is None:
        return ABSENT
    spelling, is_bare = t
    return Ann(spelling, verifiable=is_bare)


def parse_lua_source(lua_path):
    """Parse a .lua file into the neutral (functions, classes) model."""
    with open(lua_path) as f:
        functions, classes = luaparse.parse_lua_source(f.read())
    funcs = {}
    for name, fn in functions.items():
        params = [(argname, _ann(t)) for argname, t in fn.params]
        funcs[name] = FnModel(name, params, _ann(fn.ret), fn.has_varargs)
    cls = {}
    for name, lc in classes.items():
        cls[name] = {fname: Ann(spelling, verifiable=is_bare)
                     for fname, spelling, is_bare in lc.fields}
    return funcs, cls


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    linktypes.add_typecheck_args(ap, src_ext=".lua", header_suffix=".lua.h")
    args = ap.parse_args(argv)
    return linktypes.run_typecheck(
        args, vocab=LUA_VOCAB, src_ext=".lua", header_suffix=".lua.h",
        parse_source=parse_lua_source, tool_name="luatypecheck")


if __name__ == "__main__":
    sys.exit(main())
