#!/usr/bin/env python3
"""Scan Lua sources for LuaLS / EmmyLua ``---@`` type annotations.

Lua has no native type syntax, so LinkPy reads types from the de-facto-standard
LuaLS doc comments that sit immediately above each definition:

    ---@class Point
    ---@field x integer
    ---@field y integer

    ---@param a integer
    ---@param b integer
    ---@return number
    function M.divide(a, b) return a / b end

There is no Lua parser in the Python stdlib, so this is a focused line scanner,
not a grammar: it associates the contiguous block of ``---@param`` / ``---@return``
comment lines preceding a ``function`` definition with that function (matching
params by name), and gathers ``---@field`` lines following a ``---@class`` into a
struct. A blank line ends a block (LuaLS requires the annotations to be adjacent
to what they document).

It returns a *raw* model -- the Lua type spellings as written -- leaving the
mapping to C (``luahgen``) or to the neutral check model (``luatypecheck``) to
the callers, which is the only language-neutral part and lives in
``tools/linktypes.py``.

Returned model:
  * ``functions``: ``{name: LuaFunc}`` in source order, where each param/return
    type is ``None`` (no annotation) or ``(spelling, is_bare)`` -- ``is_bare`` is
    True only for a single identifier (e.g. ``integer``, ``Point``); a union /
    array / generic / function type (``string|nil``, ``integer[]``) is carried
    with ``is_bare`` False so the caller treats it as unverifiable.
  * ``classes``: ``{name: LuaClass}`` whose ``fields`` are ``(name, spelling,
    is_bare)`` in declaration order.
"""

import re


class LuaFunc:
    """A scanned Lua function. ``name`` has any ``M.``/``M:`` prefix stripped."""
    __slots__ = ("name", "params", "ret", "has_varargs")

    def __init__(self, name, params, ret, has_varargs):
        self.name = name
        self.params = params            # [(argname, None | (spelling, is_bare))]
        self.ret = ret                  # None | (spelling, is_bare)
        self.has_varargs = has_varargs


class LuaClass:
    """A scanned ``---@class`` with its ``---@field`` declarations."""
    __slots__ = ("name", "fields")

    def __init__(self, name, fields):
        self.name = name
        self.fields = fields            # [(field_name, spelling, is_bare)]


_ANN_RE = re.compile(r"^---@(\w+)\b\s*(.*)$")
# function [local] name / dotted.path / colon:method (params)
_FUNC_RE = re.compile(
    r"^\s*(?:local\s+)?function\s+"
    r"([A-Za-z_]\w*(?:[.:][A-Za-z_]\w*)*)\s*\(([^)]*)\)")
# [local] lhs = function(params)
_ASSIGN_FUNC_RE = re.compile(
    r"^\s*(?:local\s+)?([A-Za-z_]\w*(?:[.:][A-Za-z_]\w*)*)\s*=\s*"
    r"function\s*\(([^)]*)\)")
_BARE_RE = re.compile(r"[A-Za-z_][\w.]*")
_LASTSEG_RE = re.compile(r"[.:]([A-Za-z_]\w*)\Z")
_FIELD_SCOPES = {"public", "private", "protected", "package"}
# A bare type followed immediately by one of these is actually a compound type
# (union/array/generic/optional/tuple), so it is not a verifiable bare name.
_COMPOUND_LEAD = ("|", "[", "<", "?", "(", ",")


def _split_type(rest):
    """Reduce the text after a name to ``(spelling, is_bare)`` or ``None``.

    The spelling is the first whitespace-delimited token (the LuaLS type, with
    any trailing description dropped). ``is_bare`` is True only when that token
    is a single dotted identifier with no compound-type continuation.
    """
    s = rest.strip()
    if not s:
        return None
    tok = re.match(r"\S+", s).group(0)
    after = s[len(tok):].lstrip()
    is_bare = re.fullmatch(r"[A-Za-z_][\w.]*", tok) is not None
    if is_bare and after[:1] in _COMPOUND_LEAD:
        is_bare = False
    return tok, is_bare


def _params(param_str):
    """Split a Lua parameter list into ([names], saw_vararg)."""
    names = []
    vararg = False
    for p in param_str.split(","):
        p = p.strip()
        if not p:
            continue
        if p == "...":
            vararg = True
            continue
        names.append(p)
    return names, vararg


def parse_lua_source(text):
    """Scan Lua source text into ``(functions, classes)`` (see module docstring)."""
    functions = {}
    classes = {}

    cur_class = None
    pending_params = {}
    pending_ret = None
    pending_vararg = False

    def reset():
        nonlocal cur_class, pending_params, pending_ret, pending_vararg
        cur_class = None
        pending_params = {}
        pending_ret = None
        pending_vararg = False

    for raw in text.splitlines():
        line = raw.strip()

        if not line:
            reset()
            continue

        am = _ANN_RE.match(line)
        if am:
            tag, rest = am.group(1), am.group(2)
            if tag == "class":
                cm = _BARE_RE.match(rest)
                if cm:
                    cur_class = cm.group(0)
                    classes.setdefault(cur_class, LuaClass(cur_class, []))
            elif tag == "field" and cur_class is not None:
                toks = rest.split(None, 1)
                if toks:
                    fname = toks[0]
                    remainder = toks[1] if len(toks) > 1 else ""
                    if fname in _FIELD_SCOPES:  # ---@field <scope> name type
                        toks = remainder.split(None, 1)
                        if toks:
                            fname = toks[0]
                            remainder = toks[1] if len(toks) > 1 else ""
                    if re.fullmatch(r"[A-Za-z_]\w*", fname):
                        t = _split_type(remainder)
                        if t is not None:
                            classes[cur_class].fields.append((fname, t[0], t[1]))
            elif tag == "param":
                toks = rest.split(None, 1)
                if toks:
                    pname = toks[0]
                    remainder = toks[1] if len(toks) > 1 else ""
                    if pname == "...":
                        pending_vararg = True
                    else:
                        pending_params[pname] = _split_type(remainder)
            elif tag == "return":
                if pending_ret is None:
                    t = _split_type(rest)
                    if t is not None:
                        pending_ret = t
            elif tag == "vararg":
                pending_vararg = True
            # other tags (e.g. @type, @generic, @overload) are ignored.
            continue

        if line.startswith("--"):
            continue  # plain comment/description -- stays within the block

        fm = _FUNC_RE.match(line) or _ASSIGN_FUNC_RE.match(line)
        if fm:
            full, param_str = fm.group(1), fm.group(2)
            mlast = _LASTSEG_RE.search(full)
            if mlast:
                name = mlast.group(1)
                is_method = full[mlast.start()] == ":"
            else:
                name = full
                is_method = False
            names, sig_vararg = _params(param_str)
            params = []
            if is_method:
                params.append(("self", None))  # implicit self on `:` methods
            for pn in names:
                params.append((pn, pending_params.get(pn)))
            functions[name] = LuaFunc(
                name, params, pending_ret, pending_vararg or sig_vararg)

        reset()  # any code line ends the current annotation block

    return functions, classes
