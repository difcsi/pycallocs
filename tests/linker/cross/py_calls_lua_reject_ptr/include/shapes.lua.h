/* Negative fixture: bump takes a pointer, which cannot cross a language
 * boundary by value. The auditor must reject the whole link map with
 * "by-reference does not cross language boundaries" before JIT-compiling. */
struct Point {
    int x;
    int y;
};

extern void bump(struct Point *p);
