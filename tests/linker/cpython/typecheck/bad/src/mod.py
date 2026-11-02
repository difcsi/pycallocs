class Point:
    x: int
    y: str  # conflicts with C `int y;`


def divide(a: str) -> int:  # wrong arity (1 vs 2), wrong param + return types
    return 0


def describe(p: Point) -> int:
    return p.x
