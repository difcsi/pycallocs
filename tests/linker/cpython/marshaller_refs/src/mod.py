import sys


def boom():
    raise ValueError("boom")


def echo(s):
    return None


def refs():
    m = sys.modules[__name__]
    return sys.getrefcount(m) + sys.getrefcount(m.boom) + sys.getrefcount(m.echo)
