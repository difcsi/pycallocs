# By-reference from ANOTHER VM: p is a pycallocs proxy over the same C struct
# the Lua host holds a luaproxy proxy for. The write goes straight to C
# memory, where the host observes it.


def bump(p) -> None:
    p.x = p.x + 100
