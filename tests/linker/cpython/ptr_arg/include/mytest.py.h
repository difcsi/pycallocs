struct Point {
    int x;
    int y;
};

/* By-reference: the argument is a pointer, so linkpy passes a pycallocs proxy
 * wrapping the live C struct (not a by-value SimpleNamespace copy). Writes the
 * Python function makes through the proxy propagate back into this C memory. */
extern void bump(struct Point *p);
