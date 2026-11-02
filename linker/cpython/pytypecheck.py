#!/usr/bin/env python3
"""Type-check Python sources against the C types LinkPy links them to.

This is the dual of ``tools/pyhgen.py``. Where pyhgen *derives* a C ``*.py.h``
stub header from a module's Python type hints, this tool goes the other way: it
takes the C side of the contract -- the recovered function/struct types -- and
checks that a Python module is type-compatible with it. It answers the question
the LinkPy auditor implicitly asks every time it marshals a call: *does the
Python I loaded actually match the C signature I am about to bind it to?*

The C-side machinery (header/lmap parsing, type classification, the check
grading) lives in ``tools/linktypes.py`` and is shared with the Lua tools; this
file is the thin Python layer: it reduces a ``.py`` module's ``ast`` to the
neutral model ``linktypes`` checks, and supplies the Python annotation
vocabulary (``int``/``float``/``bool``/``str``/``None``).

Two sources of C signatures are supported, mirroring the two LinkPy already uses:

  * ``--include DIR`` / ``--header FILE`` -- hand-written or generated ``*.py.h``
    stub headers, parsed with the same grammar as ``pystubgen``. Header mode
    also recovers struct layouts, so class fields are checked too.

  * ``--lmap -`` (stdin) -- the colon-delimited signature format produced by
    ``FunLinkEntry::to_string()`` in ``src/linkmap.cpp``, i.e. the exact
    representation the DWARF path (``dwarfmap.cpp``) and the auditor build in
    memory. The C++ ``linkpychk`` runner (``src/pytypecheck_runner.cpp``)
    recovers signatures from a stub library's DWARF and pipes them here. Only
    function signatures travel this path; struct-field checks require header mode.

The C->Python type mapping is the inverse of pyhgen's:

    int -> int,  double/float -> float,  bool -> bool,
    const char * -> str,  void (return) -> None,
    struct X -> class X (by value),  struct X * -> class X (by reference).

Findings are classified by how they affect *linkability*:

  * ERROR   -- a genuine incompatibility that would break the binding: a C
               function with no matching Python def, an arity mismatch, or a
               present-but-contradictory type hint / struct-field hint.
  * WARNING -- a gap that is not provably wrong: a missing hint where C has a
               concrete type (gradual typing), or a Python def the C side never
               declares.

A module is a *good candidate* iff it has zero ERRORs. The process exit code is
0 for a good candidate, 1 if any ERROR was found, 2 on a usage/parse failure.
With ``--strict`` every WARNING is promoted to an ERROR. ``--json`` emits the
full structured result on stdout for programmatic consumers.
"""

import argparse
import ast
import os
import sys

# linktypes (the shared vocabulary/model module) lives in ../common, which is
# not on sys.path when this tool is run by absolute path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "common"))

import linktypes
from linktypes import ABSENT, Ann, FnModel, Vocab

# C->Python annotation vocabulary for the neutral kinds in linktypes.
PY_VOCAB = Vocab("Python", int_name="int", float_name="float",
                 bool_name="bool", str_name="str", void_name="None")


def _ann(node):
    """Reduce a Python annotation node to an ``Ann``, or ``ABSENT`` if absent.

    A bare ``ast.Name`` and ``None`` (written as ``ast.Constant``) are verifiable
    bare names; anything else (``list[int]``, ``Optional[X]``) is present but
    unverifiable, so the checker treats it as a non-fatal "cannot verify".
    """
    if node is None:
        return ABSENT
    if isinstance(node, ast.Name):
        return Ann(node.id)
    if isinstance(node, ast.Constant) and node.value is None:
        return Ann("None")
    return Ann(ast.unparse(node), verifiable=False)


def parse_python_source(py_path):
    """Parse a .py file into the neutral (functions, classes) model."""
    with open(py_path) as f:
        tree = ast.parse(f.read(), filename=py_path)
    funcs, classes = {}, {}
    for n in tree.body:
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)):
            a = n.args
            positional = list(a.posonlyargs) + list(a.args)
            params = [(arg.arg, _ann(arg.annotation)) for arg in positional]
            has_varargs = a.vararg is not None or len(a.kwonlyargs) > 0
            funcs[n.name] = FnModel(n.name, params, _ann(n.returns), has_varargs)
        elif isinstance(n, ast.ClassDef):
            field_anns = {}
            for stmt in n.body:
                if isinstance(stmt, ast.AnnAssign) and \
                        isinstance(stmt.target, ast.Name):
                    field_anns[stmt.target.id] = _ann(stmt.annotation)
            classes[n.name] = field_anns
    return funcs, classes


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    linktypes.add_typecheck_args(ap, src_ext=".py", header_suffix=".py.h")
    args = ap.parse_args(argv)
    return linktypes.run_typecheck(
        args, vocab=PY_VOCAB, src_ext=".py", header_suffix=".py.h",
        parse_source=parse_python_source, tool_name="pytypecheck")


if __name__ == "__main__":
    sys.exit(main())
