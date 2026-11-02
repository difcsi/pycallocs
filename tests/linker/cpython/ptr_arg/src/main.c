#include <stdio.h>
#include <stdlib.h>
#include "mytest.py.h"

/* Compiled with allocscc so the heap allocation below carries a liballocs
 * type. That is what lets the linkpy trampoline turn `&Point` into a pycallocs
 * proxy: Proxy_GetOrCreateBase asks liballocs for the type at the address. */
int main(void)
{
    struct Point *p = malloc(sizeof *p);
    p->x = 1;
    p->y = 2;

    bump(p);   /* by-reference call into Python via linkpy */

    printf("after bump: x=%d y=%d\n", p->x, p->y);
    int ok = (p->x == 101 && p->y == 2);  /* Python's write must be visible here */
    free(p);
    return ok ? 0 : 1;
}
