struct Point {
    int x;
    int y;
};

extern int add(int a, int b);
extern struct Point make_point(int x, int y);
extern void bump(struct Point *p);
