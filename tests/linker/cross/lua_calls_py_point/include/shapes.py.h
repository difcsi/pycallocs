/* The headline cross-language case, reversed direction: the user-defined C
 * struct is the binary layout both VMs marshal through. */
struct Point {
    int x;
    int y;
};

extern struct Point make_point(int x, int y);
extern int describe(struct Point p);
