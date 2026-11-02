#!/usr/bin/env python3
"""Generate C ``*.lua.h`` stub headers from the LuaLS hints in Lua sources.

The Lua analogue of ``tools/pyhgen.py``: it derives the C side of the link
contract (a ``<mod>.lua.h`` declaring the ``extern`` functions and any ``struct``
types they use) from the ``---@param`` / ``---@return`` / ``---@class`` /
``---@field`` annotations in ``<mod>.lua``.

For every ``<mod>.lua`` under ``--src``:

  * if ``--hand/<mod>.lua.h`` exists, it is copied verbatim into ``--out``
    (hand-written wins, never parsed), otherwise
  * the module is scanned with ``luaparse`` and a header is emitted to
    ``--out/<mod>.lua.h``.

The C-side rendering (struct ordering, ``extern`` emission, ``<stdbool.h>``) is
shared with pyhgen via ``tools/linktypes.py``; this file maps Lua annotations to
C types. The mapping mirrors pyhgen's, in Lua's vocabulary:
``integer -> int``, ``number -> double``, ``boolean -> bool``,
``string -> const char *``, no/``nil`` return ``-> void``, and an annotated
``---@class X`` referenced by a signature ``-> struct X``.

A function is emitted only when every parameter has a ``---@param`` of a
supported type (the return may be omitted -- absent means ``void``, which is the
idiomatic Lua spelling). An unannotated parameter or an unsupported/compound
type fails with a precise message naming the offender, rather than producing a
wrong header.
"""

import argparse
import os
import shutil
import sys

# linktypes (the shared vocabulary/model module) lives in ../common, which is
# not on sys.path when this tool is run by absolute path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))

import linktypes
import luaparse
from linktypes import UnsupportedType

# LuaLS annotation -> C type, for the scalars marshalled by luacpputils.hpp.
# Lua numbers come in two subtypes: `integer` is a C int, `number` a C double.
SCALAR_C_TYPES = {
    "integer": "int",
    "number": "double",
    "boolean": "bool",
    "string": "const char *",
}
NIL_NAMES = {"nil"}


def _resolve(t, classes, where, *, allow_nil_void=False):
    """Resolve a scanned Lua type ``(spelling, is_bare)`` to a C type.

    Returns ``(c_type, referenced_class_or_None)``. With ``allow_nil_void`` a
    ``nil`` (or absent type) resolves to ``void`` (return position only). Raises
    UnsupportedType (with ``where``) when it cannot be mapped.
    """
    if t is None:
        if allow_nil_void:
            return "void", None
        raise UnsupportedType(f"{where}: missing type annotation")
    spelling, is_bare = t
    if not is_bare:
        raise UnsupportedType(
            f"{where}: unsupported type `{spelling}` (only integer/number/"
            f"boolean/string and local classes are supported, not unions/"
            f"arrays/generics)")
    if spelling in SCALAR_C_TYPES:
        return SCALAR_C_TYPES[spelling], None
    if spelling in classes:
        return f"struct {spelling}", spelling
    if spelling in NIL_NAMES:
        if allow_nil_void:
            return "void", None
        raise UnsupportedType(f"{where}: `nil` is only valid as a return type")
    raise UnsupportedType(
        f"{where}: unknown type `{spelling}` (not a builtin scalar or a class "
        f"defined in this module)")


def _struct_fields(lua_class, classes):
    """Resolve a class's ``---@field`` list to [(c_type, field, ref), ...]."""
    fields = []
    for fname, spelling, is_bare in lua_class.fields:
        c_type, ref = _resolve(
            (spelling, is_bare), classes, f"field `{lua_class.name}.{fname}`")
        fields.append((c_type, fname, ref))
    if not fields:
        raise UnsupportedType(
            f"class `{lua_class.name}` has no `---@field` annotations, so it "
            f"cannot be emitted as a C struct")
    return fields


def generate_header(lua_path):
    """Scan a .lua file and return the text of its .lua.h header.

    Raises UnsupportedType with a user-facing message if it cannot be generated.
    """
    with open(lua_path, "r") as f:
        functions, classes = luaparse.parse_lua_source(f.read())

    if not functions:
        raise UnsupportedType(
            f"{os.path.basename(lua_path)} defines no functions to export "
            f"(expected `function name(...)` with `---@param`/`---@return`)")

    fns = []           # (name, ret_c_type, [(c_type, argname)]) in source order
    used_classes = set()
    for name, fn in functions.items():
        if fn.has_varargs:
            raise UnsupportedType(
                f"function `{name}`: variadic (`...`) signatures are not "
                f"supported")
        params = []
        for argname, t in fn.params:
            c_type, ref = _resolve(
                t, classes, f"parameter `{name}({argname})`")
            if ref:
                used_classes.add(ref)
            params.append((c_type, argname))
        ret_c_type, ref = _resolve(
            fn.ret, classes, f"return type of `{name}`", allow_nil_void=True)
        if ref:
            used_classes.add(ref)
        fns.append((name, ret_c_type, params))

    # Resolve every used struct (and its transitive dependencies) to fields.
    struct_table = {}
    def add_struct(name):
        if name in struct_table:
            return
        fields = _struct_fields(classes[name], classes)
        struct_table[name] = fields
        for _c, _f, ref in fields:
            if ref:
                add_struct(ref)
    for name in used_classes:
        add_struct(name)

    ordered = linktypes.order_structs(used_classes, struct_table)
    structs = [(name, [(t, f) for t, f, _ in struct_table[name]])
               for name in ordered]

    return linktypes.render_header(
        "luahgen.py", os.path.basename(lua_path), fns, structs)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="directory of Lua sources (<mod>.lua)")
    ap.add_argument("--hand", default=None,
                    help="optional directory of hand-written headers; when a "
                         "<mod>.lua.h exists there it is copied verbatim instead "
                         "of being generated (hand-written wins)")
    ap.add_argument("--out", required=True,
                    help="output directory for the resulting headers")
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)

    lua_files = sorted(f for f in os.listdir(args.src) if f.endswith(".lua"))
    for lua in lua_files:
        module = lua[:-4]  # strip ".lua"
        header = f"{module}.lua.h"
        out_path = os.path.join(args.out, header)

        hand_path = os.path.join(args.hand, header) if args.hand else None
        if hand_path and os.path.exists(hand_path):
            shutil.copyfile(hand_path, out_path)
            continue

        try:
            text = generate_header(os.path.join(args.src, lua))
        except UnsupportedType as e:
            print(f"luahgen: cannot generate {header}: {e}", file=sys.stderr)
            return 1
        with open(out_path, "w") as f:
            f.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
