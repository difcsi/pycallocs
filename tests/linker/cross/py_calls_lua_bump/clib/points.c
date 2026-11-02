#include <stdlib.h>

/* Typed-allocation helper for the cross-language by-reference fixtures.
 * Compiled with allocscc (with META_BASE set), so the malloc site carries
 * `struct Point` and the exported symbol gets liballocs metadata -- that is
 * what lets the host VM's library loader mint a live proxy from new_point's
 * result. */

struct Point {
    int x;
    int y;
};

struct Point *new_point(int x, int y)
{
    struct Point *p = malloc(sizeof *p);
    p->x = x;
    p->y = y;
    return p;
}
