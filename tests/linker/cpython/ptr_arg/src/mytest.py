def bump(p) -> None:
    # p arrives from C by reference: it is a pycallocs proxy over the C struct,
    # so reading p.x reads C memory and writing p.x writes straight back to it.
    print("Python got Point with x =", p.x, "y =", p.y)
    p.x = p.x + 100
