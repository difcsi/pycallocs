/* Lua -> Python benchmark, by value: struct Point is copied through this
 * layout on every call. */
struct Point {
    int x;
    int y;
};

extern float divide(int a, int b);
extern int describe(struct Point p);
