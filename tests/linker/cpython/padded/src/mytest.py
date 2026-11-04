from types import SimpleNamespace


def make_padded():
    return SimpleNamespace(c=1, x=2, d=3, y=4)


def sum_padded(p):
    return p.c + p.x + p.d + p.y
