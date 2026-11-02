struct Point {
    int x;
    int y;
};

/* By-reference: the argument is a pointer, so linkpy passes a luaproxy proxy
 * wrapping the live C struct (not a by-value table copy). Writes the Lua
 * function makes through the proxy propagate back into this C memory. The
 * Lua mirror of ../../cpython/ptr_arg. */
extern void bump(struct Point *p);
