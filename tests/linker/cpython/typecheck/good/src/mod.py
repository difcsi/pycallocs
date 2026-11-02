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


def bump(p: Point) -> None:
    p.x = p.x + 1
