/* Lua -> Python benchmark, by reference (LINKPY_CROSS_BYREF=1): the host's
 * view of one live struct Point crosses as its address and Python works on
 * the same object. The callee is the by-value fixture's shapes.py. */
struct Point {
    int x;
    int y;
};

extern int describe(struct Point *p);
extern void bump(struct Point *p);
