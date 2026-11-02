#!/usr/bin/env python3
"""Language-neutral core shared by LinkPy's per-language tools.

LinkPy links C against a scripting language by reflecting on the C side of a
contract (the recovered function/struct types) and either *generating* that
contract from a module's type annotations (``*hgen``) or *checking* a module
against it (``*typecheck``). The C side of that work — parsing C stub headers,
classifying C type spellings, the colon-delimited ``FunLinkEntry`` lmap format,
the check grading, and the ``.h`` rendering — is identical regardless of the
scripting language. It lives here so the Python tools (``pyhgen``/
``pytypecheck``) and the Lua tools (``luahgen``/``luatypecheck``) share one
source of truth and grow matching coverage, rather than drifting apart.

Each language tool supplies only the language-specific pieces:

  * a parser that turns its source into the neutral model used below
    (functions: ``{name: FnModel}``; classes: ``{name: {field: Ann}}``), and
  * a :class:`Vocab` mapping the neutral C "kind" to that language's annotation
    spellings (``int`` -> Python ``int`` / Lua ``integer``; ``void`` -> Python
    ``None`` / Lua ``nil``; etc.).

The neutral C "kind" of a type is one of ``"int"``, ``"float"``, ``"bool"``,
``"str"``, ``"void"``, ``("struct", Name)``, or ``None`` (opaque -- no checkable
constraint, e.g. ``void *`` and unknown scalars).
"""

import os
import re
import sys


# --- C type model ---------------------------------------------------------

# C scalar spellings, by neutral kind. Mirrors the scalars handled by
# PyObject_to_T / T_to_PyObject (include/pycpputils.hpp) and luacpputils.hpp.
# Width-variant integers all cross as one scripting integer; CPython/Lua floats
# are C doubles.
_INT_SPELLINGS = {
    "int", "unsigned", "unsigned int", "signed", "signed int",
    "short", "short int", "unsigned short", "long", "long int",
    "unsigned long", "long long", "long long int", "unsigned long long",
    "char", "signed char", "unsigned char",
    "size_t", "ssize_t", "ptrdiff_t",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
}
_FLOAT_SPELLINGS = {"float", "double", "long double"}
_BOOL_SPELLINGS = {"bool", "_Bool"}


def normalize_c_type(c_type):
    """Split a C type spelling into (base, star_count).

    Strips ``const``/``volatile`` and collapses whitespace, counting pointer
    levels regardless of where the ``*`` was written. ``"const char *"`` ->
    ``("char", 1)``; ``"struct Point*"`` -> ``("struct Point", 1)``.
    """
    spaced = c_type.replace("*", " * ")
    toks = [t for t in spaced.split() if t not in ("const", "volatile")]
    stars = toks.count("*")
    base = " ".join(t for t in toks if t != "*")
    return base, stars


def classify_c_type(c_type, is_return):
    """Neutral "kind" of a C type, or None when it imposes no checkable constraint.

    Returns ``"int" | "float" | "bool" | "str" | "void" | ("struct", Name)``, or
    ``None`` (opaque -- e.g. ``void *`` and other pointers, unknown scalars). A
    bare ``void`` is a kind only as a return type. This is the language-neutral
    refactor of the old ``c_acceptable_py``; a :class:`Vocab` turns the kind into
    the acceptable annotation spellings for a given language.
    """
    base, stars = normalize_c_type(c_type)
    if base == "void":
        if stars == 0:
            return "void" if is_return else None
        return None  # void* / opaque pointer -- no type constraint
    if base.startswith("struct "):
        # By value (stars==0) or by reference (stars>=1) both bind to the class.
        return ("struct", base.split(None, 1)[1])
    if stars == 0:
        if base in _INT_SPELLINGS:
            return "int"
        if base in _FLOAT_SPELLINGS:
            return "float"
        if base in _BOOL_SPELLINGS:
            return "bool"
        return None  # unknown scalar (typedef etc.) -- can't check
    if stars == 1 and base in ("char", "signed char", "unsigned char"):
        return "str"  # const char * <-> string (by value, does not escape)
    return None  # other pointers -- opaque to the type check


class CFunc:
    """A C function signature: ``ret name(type pname, ...)``."""
    __slots__ = ("module", "name", "ret", "params")

    def __init__(self, module, name, ret, params):
        self.module = module
        self.name = name
        self.ret = ret
        self.params = params  # list of (c_type, param_name)


class CStruct:
    """A C struct layout: ``struct name { type field; ... }``."""
    __slots__ = ("name", "fields")

    def __init__(self, name, fields):
        self.name = name
        self.fields = fields  # list of (c_type, field_name)


_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT_RE = re.compile(r"//.*")
# extern <ret...> <name>(<params>);  -- same shape pystubgen matches.
_FUNC_RE = re.compile(r"extern\s+([\w\*\s]+?)\s+(\w+)\s*\(([^;{]*)\)\s*;")
_STRUCT_RE = re.compile(r"struct\s+(\w+)\s*\{([^}]*)\}\s*;")


def _strip_comments(text):
    return _LINE_COMMENT_RE.sub("", _COMMENT_RE.sub("", text))


def _split_param(decl):
    """Split one parameter declaration into (c_type, name).

    The name is the last identifier; everything before it (incl. any ``*``) is
    the type. ``"struct Point *p"`` -> ``("struct Point *", "p")``. A bare type
    with no name (e.g. ``void``) yields ``(decl, "")``.
    """
    decl = decl.strip()
    m = re.search(r"(\w+)\s*$", decl)
    if not m:
        return decl, ""
    name = m.group(1)
    c_type = decl[:m.start(1)].strip()
    if not c_type:
        # bare type, no name (e.g. "int") -- treat the whole thing as the type
        return name, ""
    return c_type, name


def _parse_params(param_str):
    """Parse a parameter list into [(c_type, name), ...]. ``void``/empty -> []."""
    param_str = param_str.strip()
    if param_str == "" or param_str == "void":
        return []
    params = []
    for piece in param_str.split(","):
        piece = piece.strip()
        if not piece:
            continue
        params.append(_split_param(piece))
    return params


def parse_header_text(text, module):
    """Parse one stub header's text into (functions, structs)."""
    text = _strip_comments(text)
    funcs = []
    for m in _FUNC_RE.finditer(text):
        ret, name, params = m.group(1).strip(), m.group(2), m.group(3)
        funcs.append(CFunc(module, name, ret, _parse_params(params)))
    structs = []
    for m in _STRUCT_RE.finditer(text):
        name, body = m.group(1), m.group(2)
        fields = []
        for stmt in body.split(";"):
            stmt = stmt.strip()
            if not stmt:
                continue
            c_type, fname = _split_param(stmt)
            if fname:
                fields.append((c_type, fname))
        structs.append(CStruct(name, fields))
    return funcs, structs


def parse_lmap_text(text):
    """Parse the colon-delimited lmap signature format into [CFunc, ...].

    Format (FunLinkEntry::to_string): ``module:name:ret[:ptype:pname]...``.
    Blank lines and ``#`` comments are ignored. No struct layouts are carried
    in this format, so struct-field checks are unavailable in lmap mode.
    """
    funcs = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(":")
        if len(parts) < 3:
            continue
        module, name, ret = parts[0], parts[1], parts[2]
        rest = parts[3:]
        params = []
        for i in range(0, len(rest) - 1, 2):
            params.append((rest[i], rest[i + 1]))
        funcs.append(CFunc(module, name, ret, params))
    return funcs


# --- Header rendering (shared by *hgen) -----------------------------------

class UnsupportedType(Exception):
    """A type annotation that cannot be mapped to a C type.

    Raised by the language ``*hgen`` layer with a user-facing message naming the
    offending function/parameter/field, so generation fails precisely rather
    than letting a later build step fail opaquely.
    """


def order_structs(used, struct_table, _seen=None, _out=None):
    """Topologically order the used struct names so dependencies come first.

    ``struct_table`` maps a struct name to ``[(c_type, field, ref_or_None)]``,
    where ``ref`` is the name of another struct the field references.
    """
    if _seen is None:
        _seen, _out = set(), []
    for name in used:
        if name in _seen:
            continue
        _seen.add(name)
        deps = {ref for _, _, ref in struct_table[name] if ref}
        order_structs(deps, struct_table, _seen, _out)
        _out.append(name)
    return _out


def render_header(generator, source_filename, functions, structs):
    """Render a C stub header from a neutral model.

    ``functions``: ``[(name, ret_c_type, [(c_type, argname)]), ...]`` in source
    order. ``structs``: ``[(name, [(c_type, field)]), ...]`` already
    topologically ordered (dependencies first). Emits the structs ahead of the
    ``extern`` declarations and pulls in ``<stdbool.h>`` only when a ``bool`` is
    actually emitted.
    """
    c_types_used = set()

    decls = []
    for name, ret_c_type, params in functions:
        c_types_used.add(ret_c_type)
        c_types_used.update(t for t, _ in params)
        param_list = ", ".join(f"{t} {n}" for t, n in params) or "void"
        decls.append(f"extern {ret_c_type} {name}({param_list});")

    struct_blocks = []
    for name, fields in structs:
        lines = [f"struct {name} {{"]
        for c_type, field in fields:
            c_types_used.add(c_type)
            lines.append(f"    {c_type} {field};")
        lines.append("};")
        struct_blocks.append("\n".join(lines))

    needs_stdbool = "bool" in c_types_used

    out = [f"/* Auto-generated by {generator} from {source_filename}. Do not edit. */"]
    if needs_stdbool:
        out.append("#include <stdbool.h>")
    out.append("")
    out.extend(b + "\n" for b in struct_blocks)
    out.extend(decls)
    return "\n".join(out) + "\n"


# --- Checking (shared by *typecheck) --------------------------------------

# Sentinel: a parameter/return position that carries no annotation at all
# (distinct from an annotation that is present but not reducible to a bare name).
ABSENT = object()


class Ann:
    """A source annotation reduced for checking.

    ``spelling`` is the annotation as written (for messages). ``verifiable`` is
    False when it is present but cannot be reduced to a bare name (e.g. Python
    ``list[int]``), which the checker treats as an unverifiable hint rather than
    a contradiction.
    """
    __slots__ = ("spelling", "verifiable")

    def __init__(self, spelling, verifiable=True):
        self.spelling = spelling
        self.verifiable = verifiable


class FnModel:
    """A scripting function reduced for checking.

    ``params``: ``[(argname, ABSENT | Ann), ...]`` (positional). ``ret``:
    ``ABSENT | Ann``. ``has_varargs``: True if the function soaks up extra
    positional arguments (so an arity check against C is not meaningful).
    """
    __slots__ = ("name", "params", "ret", "has_varargs")

    def __init__(self, name, params, ret, has_varargs=False):
        self.name = name
        self.params = params
        self.ret = ret
        self.has_varargs = has_varargs


class SourceParseError(Exception):
    """A language ``parse_source`` could not parse a module."""


class Vocab:
    """A scripting language's annotation vocabulary for the neutral C kinds."""

    def __init__(self, lang, *, int_name, float_name, bool_name, str_name,
                 void_name):
        self.lang = lang  # "Python" / "Lua", used in messages
        self._by_kind = {
            "int": {int_name}, "float": {float_name}, "bool": {bool_name},
            "str": {str_name}, "void": {void_name},
        }

    def names_for_kind(self, kind):
        """Acceptable annotation spellings for a neutral kind."""
        if isinstance(kind, tuple):  # ("struct", Name) -> the type's own name
            return {kind[1]}
        return self._by_kind.get(kind, set())


def finding(severity, module, where, message):
    return {"severity": severity, "module": module, "where": where,
            "message": message}


def _check_type(c_type, ann, is_return, vocab):
    """Compare a C type against a reduced annotation (ABSENT | Ann).

    Returns ``(severity, message)`` for a problem, or ``None`` if compatible /
    not checkable.
    """
    kind = classify_c_type(c_type, is_return)
    if kind is None:
        return None  # opaque C type -- no constraint to check
    acceptable = vocab.names_for_kind(kind)
    want = " or ".join(sorted(acceptable))
    if ann is ABSENT:
        k = "return type" if is_return else "type"
        return ("warning", f"missing {k} hint (C is `{c_type}`, expected "
                f"`{want}`)")
    if not ann.verifiable:
        return ("warning", f"cannot verify hint `{ann.spelling}` against C "
                f"`{c_type}` (expected `{want}`)")
    if ann.spelling in acceptable:
        return None
    return ("error", f"hint `{ann.spelling}` is incompatible with C `{c_type}` "
            f"(expected `{want}`)")


def check_function(cfunc, fn, vocab):
    """Check one C function against the matching scripting function (or None)."""
    out = []
    if fn is None:
        out.append(finding(
            "error", cfunc.module, cfunc.name,
            f"C declares `{cfunc.name}` but no top-level {vocab.lang} function "
            f"defines it (cannot bind)"))
        return out

    # Arity: the C side calls with exactly len(params) positional arguments.
    if not fn.has_varargs and len(fn.params) != len(cfunc.params):
        out.append(finding(
            "error", cfunc.module, cfunc.name,
            f"arity mismatch: C passes {len(cfunc.params)} argument(s) but "
            f"{vocab.lang} `{cfunc.name}` takes {len(fn.params)}"))

    # Per-parameter type compatibility (positional pairing).
    for i, (c_type, _cname) in enumerate(cfunc.params):
        if i >= len(fn.params):
            continue  # absorbed by varargs, or arity error already reported
        argname, ann = fn.params[i]
        res = _check_type(c_type, ann, is_return=False, vocab=vocab)
        if res:
            sev, msg = res
            out.append(finding(sev, cfunc.module, f"{cfunc.name}({argname})", msg))

    # Return type.
    res = _check_type(cfunc.ret, fn.ret, is_return=True, vocab=vocab)
    if res:
        sev, msg = res
        out.append(finding(sev, cfunc.module, f"{cfunc.name} -> return", msg))

    return out


def check_struct(cstruct, field_anns, vocab):
    """Check a C struct layout against the matching class's field annotations.

    ``field_anns`` is ``None`` when no class matches, or ``{field: Ann}`` with
    only the annotated fields present (an unannotated field is skipped, not a
    warning -- it is set dynamically at runtime).
    """
    out = []
    if not field_anns:
        return out  # no class, or class sets fields dynamically -- skip
    for c_type, fname in cstruct.fields:
        ann = field_anns.get(fname)
        if ann is None:
            continue  # field not annotated as a class attribute -- can't check
        res = _check_type(c_type, ann, is_return=False, vocab=vocab)
        if res:
            sev, msg = res
            out.append(finding(sev, None, f"struct {cstruct.name}.{fname}", msg))
    return out


def check_module(module, cfuncs, cstructs, model, declared_only, vocab):
    """Check all C signatures of one module against a parsed source model.

    ``model`` is ``(functions{name: FnModel}, classes{name: {field: Ann}})``.
    ``declared_only`` skips the "module defines a function C never declares"
    advisory (used in lmap mode, where the signature set may be a subset).
    """
    funcs, classes = model
    findings = []
    for cf in cfuncs:
        findings.extend(check_function(cf, funcs.get(cf.name), vocab))
    for cs in cstructs:
        findings.extend(check_struct(cs, classes.get(cs.name), vocab))
        # struct findings carry module=None; stamp it for reporting.
    for f in findings:
        if f["module"] is None:
            f["module"] = module

    if not declared_only:
        declared = {cf.name for cf in cfuncs}
        for name in funcs:
            if name not in declared:
                findings.append(finding(
                    "warning", module, name,
                    f"{vocab.lang} defines `{name}` but no C header declares it "
                    f"(not part of the link contract)"))
    return findings


# --- Driver (shared by *typecheck) ----------------------------------------

def _gather_c_signatures(args, header_suffix):
    """Build {module: (cfuncs, cstructs)} from the chosen C signature source.

    Returns ``(by_module, declared_only)``.
    """
    by_module = {}

    def slot(mod):
        return by_module.setdefault(mod, ([], []))

    if args.lmap is not None:
        text = sys.stdin.read() if args.lmap == "-" else open(args.lmap).read()
        for cf in parse_lmap_text(text):
            slot(cf.module)[0].append(cf)
        return by_module, True  # lmap -> declared_only

    headers = []
    if args.header:
        headers = [args.header]
    elif args.include:
        for root, _dirs, files in os.walk(args.include):
            for fn in sorted(files):
                if fn.endswith(header_suffix):
                    headers.append(os.path.join(root, fn))

    for hpath in headers:
        module = os.path.basename(hpath)[:-len(header_suffix)]
        funcs, structs = parse_header_text(open(hpath).read(), module)
        f, s = slot(module)
        f.extend(funcs)
        s.extend(structs)
    return by_module, False


def add_typecheck_args(ap, *, src_ext, header_suffix):
    """Register the common typecheck CLI on an ArgumentParser."""
    ap.add_argument("--src", required=True,
                    help=f"directory of sources (<mod>{src_ext})")
    src_group = ap.add_mutually_exclusive_group(required=True)
    src_group.add_argument("--include",
                           help=f"directory of C stub headers (*{header_suffix})")
    src_group.add_argument("--header",
                           help=f"a single C stub header (*{header_suffix}) to "
                                f"check against")
    src_group.add_argument("--lmap",
                           help="colon-delimited lmap signatures (FunLinkEntry "
                                "format); `-` reads stdin. This is the dwarfmap/"
                                "linker integration path.")
    ap.add_argument("--strict", action="store_true",
                    help="promote warnings to errors (require full, correct "
                         "annotations on every linked signature)")
    ap.add_argument("--json", action="store_true",
                    help="emit the structured result as JSON on stdout")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="suppress the human-readable report (exit code only)")


def run_typecheck(args, *, vocab, src_ext, header_suffix, parse_source,
                  tool_name):
    """Run the shared typecheck given parsed args and a language adapter.

    ``parse_source(path)`` returns ``(functions{name: FnModel},
    classes{name: {field: Ann}})`` or raises ``SourceParseError``/``SyntaxError``/
    ``OSError``. Returns the process exit code (0 candidate / 1 errors / 2 usage).
    """
    try:
        by_module, declared_only = _gather_c_signatures(args, header_suffix)
    except OSError as e:
        print(f"{tool_name}: {e}", file=sys.stderr)
        return 2

    if not by_module:
        print(f"{tool_name}: no C signatures found to check against",
              file=sys.stderr)
        return 2

    all_findings = []
    for module, (cfuncs, cstructs) in sorted(by_module.items()):
        src_path = os.path.join(args.src, module + src_ext)
        if not os.path.exists(src_path):
            all_findings.append(finding(
                "error", module, src_path,
                f"C declares module `{module}` but `{src_path}` does not exist"))
            continue
        try:
            model = parse_source(src_path)
        except (SourceParseError, SyntaxError, OSError) as e:
            all_findings.append(finding(
                "error", module, src_path,
                f"cannot parse {vocab.lang} source: {e}"))
            continue
        all_findings.extend(
            check_module(module, cfuncs, cstructs, model, declared_only, vocab))

    if args.strict:
        for f in all_findings:
            if f["severity"] == "warning":
                f["severity"] = "error"

    errors = [f for f in all_findings if f["severity"] == "error"]
    warnings = [f for f in all_findings if f["severity"] == "warning"]
    candidate = not errors

    result = {
        "candidate": candidate,
        "findings": all_findings,
        "summary": {
            "modules": len(by_module),
            "errors": len(errors),
            "warnings": len(warnings),
        },
    }

    if args.json:
        import json
        json.dump(result, sys.stdout, indent=2)
        sys.stdout.write("\n")
    elif not args.quiet:
        for f in all_findings:
            tag = "error" if f["severity"] == "error" else "warning"
            print(f"{tool_name}: {tag}: {f['module']}: {f['where']}: "
                  f"{f['message']}", file=sys.stderr)
        verdict = "good candidate" if candidate else "NOT a candidate"
        print(f"{tool_name}: {verdict} "
              f"({len(errors)} error(s), {len(warnings)} warning(s))",
              file=sys.stderr)

    return 0 if candidate else 1
