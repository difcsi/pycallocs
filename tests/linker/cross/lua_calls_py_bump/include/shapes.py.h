struct Point {
    int x;
    int y;
};

/* By-reference ACROSS two VMs (LINKPY_CROSS_BYREF=1), reversed direction: the
 * pointer is extracted from the host's luaproxy proxy, crosses the C
 * trampoline raw, and is re-proxied by the callee's pycallocs -- both VMs see
 * the same live C object, whose layout is this struct. */
extern void bump(struct Point *p);
