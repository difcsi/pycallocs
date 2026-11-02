#!/usr/bin/env python3
"""Generate C ``*.py.h`` stub headers from the type hints in Python sources.

LinkPy links C against Python by reflecting on C function/struct types and
generating marshalling shims. The C side of that contract is a ``<mod>.py.h``
header declaring the ``extern`` functions (and any ``struct`` types they use).
Those headers used to be written by hand; this tool derives them from the type
hints already present in the corresponding ``<mod>.py`` sources.

For every ``<mod>.py`` under ``--src``:

  * if ``--hand/<mod>.py.h`` exists, it is copied verbatim into ``--out``
    (a hand-written header always wins and is never parsed), otherwise
  * the module is parsed with the stdlib ``ast`` and a header is emitted to
    ``--out/<mod>.py.h``.

The C-side rendering (struct ordering, ``extern`` emission, ``<stdbool.h>``) is
shared with the Lua generator via ``tools/linktypes.py``; this file is the thin
Python layer that maps Python annotations to C types. If a module needs a header
but its signatures are not fully annotated (or use a type we cannot map to C),
generation fails with a precise message naming the offending
function/parameter/field, rather than letting a later build step fail opaquely.
"""

import argparse
import ast
import os
import shutil
import sys

# linktypes (the shared vocabulary/model module) lives in ../common, which is
# not on sys.path when this tool is run by absolute path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))

import linktypes
from linktypes import UnsupportedType

# Python annotation -> C type, for the scalars marshalled by PyObject_to_T /
# T_to_PyObject in include/pycpputils.hpp. CPython floats are C doubles. ``str``
# crosses as a C string; ``None`` is only meaningful as a (void) return.
SCALAR_C_TYPES = {
    "int": "int",
    "float": "double",
    "bool": "bool",
    "str": "const char *",
}
NONE_NAMES = {"None"}


def _name_of(node):
    """Return the identifier for a bare-name annotation node, else None.

    Handles the two ways ``None`` shows up as an annotation: ``ast.Name`` (older
    grammars) and ``ast.Constant(None)``.
    """
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Constant) and node.value is None:
        return "None"
    return None


def _scalar_or_struct(node, classes, where):
    """Resolve an annotation node to a C type spelling.

    ``classes`` maps class name -> ClassDef for the structs defined in this
    module. A class reference resolves to ``struct <Name>`` and the name is
    returned alongside so the caller can mark the struct as used.

    Returns ``(c_type, referenced_class_name_or_None)``.
    Raises UnsupportedType (with ``where`` for context) if it cannot be mapped.
    """
    name = _name_of(node)
    if name is None:
        raise UnsupportedType(
            f"{where}: unsupported type annotation "
            f"`{ast.unparse(node)}` (only int/float/bool/str and local "
            f"classes are supported)")
    if name in SCALAR_C_TYPES:
        return SCALAR_C_TYPES[name], None
    if name in classes:
        return f"struct {name}", name
    if name in NONE_NAMES:
        raise UnsupportedType(f"{where}: `None` is only valid as a return type")
    raise UnsupportedType(
        f"{where}: unknown type `{name}` (not a builtin scalar or a class "
        f"defined in this module)")


def _struct_fields(cls, classes):
    """Return [(c_type, field_name, referenced_class_or_None), ...] for a class.

    Fields are the annotated class-body assignments (``a: int``), in source
    order. A class with no annotated fields cannot be a struct.
    """
    fields = []
    for stmt in cls.body:
        if isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name):
            field = stmt.target.id
            c_type, ref = _scalar_or_struct(
                stmt.annotation, classes, f"field `{cls.name}.{field}`")
            fields.append((c_type, field, ref))
    if not fields:
        raise UnsupportedType(
            f"class `{cls.name}` has no annotated fields, so it cannot be "
            f"emitted as a C struct (annotate them, e.g. `a: int`)")
    return fields


def _function_signature(fn, classes):
    """Resolve a FunctionDef to (return_c_type, [(c_type, name), ...], used_classes).

    Requires the return and every parameter to be annotated.
    """
    used = set()
    args = fn.args
    if (args.vararg or args.kwarg or args.posonlyargs or args.kwonlyargs):
        raise UnsupportedType(
            f"function `{fn.name}`: only plain positional parameters are "
            f"supported (no *args/**kwargs/keyword-only/positional-only)")

    params = []
    for a in args.args:
        if a.annotation is None:
            raise UnsupportedType(
                f"function `{fn.name}`: parameter `{a.arg}` has no type hint")
        c_type, ref = _scalar_or_struct(
            a.annotation, classes, f"parameter `{fn.name}({a.arg})`")
        if ref:
            used.add(ref)
        params.append((c_type, a.arg))

    if fn.returns is None:
        raise UnsupportedType(
            f"function `{fn.name}`: missing return type hint "
            f"(use `-> None` for void)")
    ret_name = _name_of(fn.returns)
    if ret_name in NONE_NAMES:
        ret_c_type = "void"
    else:
        ret_c_type, ref = _scalar_or_struct(
            fn.returns, classes, f"return type of `{fn.name}`")
        if ref:
            used.add(ref)

    return ret_c_type, params, used


def generate_header(py_path):
    """Parse a .py file and return the text of its .py.h header.

    Raises UnsupportedType with a user-facing message if it cannot be generated.
    """
    with open(py_path, "r") as f:
        source = f.read()
    tree = ast.parse(source, filename=py_path)

    classes = {n.name: n for n in tree.body if isinstance(n, ast.ClassDef)}
    functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)]

    if not functions:
        raise UnsupportedType(
            f"{os.path.basename(py_path)} defines no top-level functions to "
            f"export")

    fns = []           # (name, ret_c_type, [(c_type, argname)]) in source order
    used_classes = set()
    for fn in functions:
        ret_c_type, params, used = _function_signature(fn, classes)
        used_classes |= used
        fns.append((fn.name, ret_c_type, params))

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
        "pyhgen.py", os.path.basename(py_path), fns, structs)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", required=True,
                    help="directory of Python sources (<mod>.py)")
    ap.add_argument("--hand", default=None,
                    help="optional directory of hand-written headers; when a "
                         "<mod>.py.h exists there it is copied verbatim instead "
                         "of being generated (hand-written wins)")
    ap.add_argument("--out", required=True,
                    help="output directory for the resulting headers")
    args = ap.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)

    py_files = sorted(f for f in os.listdir(args.src) if f.endswith(".py"))
    for py in py_files:
        module = py[:-3]  # strip ".py"
        header = f"{module}.py.h"
        out_path = os.path.join(args.out, header)

        hand_path = os.path.join(args.hand, header) if args.hand else None
        if hand_path and os.path.exists(hand_path):
            shutil.copyfile(hand_path, out_path)
            continue

        try:
            text = generate_header(os.path.join(args.src, py))
        except (UnsupportedType, SyntaxError) as e:
            print(f"pyhgen: cannot generate {header}: {e}", file=sys.stderr)
            return 1
        with open(out_path, "w") as f:
            f.write(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
