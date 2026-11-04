from types import SimpleNamespace

CHUNK = 65536

_BODIES = {n: b"X" * n for n in (0, 100, CHUNK)}
_HEADERS = b"Content-Length\0" + str(CHUNK).encode() + b"\0"


def take(req, c):
    return c.len


def give(req, n):
    data = _BODIES[n]
    return SimpleNamespace(data=data, len=len(data))


def ask(req, m):
    return SimpleNamespace(kind=1, status=200, origin=SimpleNamespace(x=3, y=4),
                           headers_len=len(_HEADERS), headers=_HEADERS)
