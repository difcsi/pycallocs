/* The headline cross-language case: this user-defined C struct is the binary
 * layout both VMs marshal through. Python objects and Lua tables never meet --
 * each converts to/from `struct Point` at its side of the C trampoline. */
struct Point {
    int x;
    int y;
};

extern struct Point make_point(int x, int y);
extern int describe(struct Point p);
