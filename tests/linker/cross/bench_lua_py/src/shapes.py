# The Python callee of the Lua -> Python per-call benchmark
# (tests/linker/bench/cross_bench.py). Every route calls this one file: the
# cross fixtures through their linkstub, the baselines under
# tests/linker/bench/lua_py/ through PYTHONPATH.


def divide(a: int, b: int) -> float:
    return a / b


def describe(p) -> int:
    return p.x + p.y


def bump(p) -> None:
    # By-reference routes only: a write the host must see, which proves the
    # object is shared and not copied.
    p.x = p.x + 100
