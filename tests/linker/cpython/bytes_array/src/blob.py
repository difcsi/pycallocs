from types import SimpleNamespace


def echo(b):
    # The array arrives whole: a bytes as long as the C array, with the
    # meaningful prefix given by the struct's own len field. Embedded NULs
    # must survive, which a char* could never carry.
    assert isinstance(b.data, bytes), type(b.data)
    assert len(b.data) == 16, len(b.data)
    payload = b.data[:b.len]
    assert payload == b"ab\x00cd", payload
    return SimpleNamespace(data=payload[::-1], len=len(payload))


def label(n):
    return SimpleNamespace(name=b"tag%d" % n, n=n)
