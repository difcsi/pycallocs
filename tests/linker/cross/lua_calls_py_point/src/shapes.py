# Struct crossing, Lua host -> Python callee: Point is a Python object here, a
# Lua table on the host side, and struct Point (shapes.py.h) on the wire.


class Point:
    x: int
    y: int

    def __init__(self, x, y):
        self.x = x
        self.y = y


def make_point(x: int, y: int) -> Point:
    return Point(x, y)


def describe(p: Point) -> int:
    # p arrives by value as a SimpleNamespace mirroring struct Point's fields.
    return p.x + p.y
