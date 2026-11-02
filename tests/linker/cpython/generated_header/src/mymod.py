# Fully type-annotated module with NO hand-written include/mymod.py.h: the build
# must derive the C header from these hints via tools/pyhgen.py.


class Point:
    x: int
    y: int

    def __init__(self, x, y):
        self.x = x
        self.y = y


def add(a: int, b: int) -> int:
    return a + b


def make_point(x: int, y: int) -> Point:
    return Point(x, y)

def print_point(p: Point) -> None:
    print(f"Point({p.x}, {p.y})")