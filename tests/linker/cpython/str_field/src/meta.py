def describe(m):
    assert m.name == "alice", m.name
    assert m.note is None, m.note
    assert m.n == 3, m.n
    return len(m.name) + m.n
