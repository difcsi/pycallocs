#include <stdio.h>
#include "mymod.py.h" // We will automatically generate this at build time for this test

// mymod.py ships no hand-written header; mymod.py.h is auto-generated from its
// type hints by tools/pyhgen.py. Exercises a scalar function and a struct return.
int main()
{
    int s = add(20, 22);
    printf("add returned: %d\n", s);

    struct Point p = make_point(3, 4);
    printf("make_point returned: {x=%d, y=%d}\n", p.x, p.y);
    print_point(p);

    return (s == 42 && p.x == 3 && p.y == 4) ? 0 : 1;
}