/* See bench.h for the rationale. Keep every function leaf and allocation-free
 * except make_int_array (whose heap block is what liballocs types as int[]). */
#include <stdlib.h>
#include "bench.h"

int bench_triple(int x) { return 3 * x; }

double bench_mul(double a, double b) { return a * b; }

struct point make_point(int x, int y)
{
    struct point p = { x, y };
    return p;
}

long consume_point(struct point p)
{
    return (long) p.x + (long) p.y;
}

long sum_point(struct point *p)
{
    return (long) p->x + (long) p->y;
}

void scale_point(struct point *p, int k)
{
    p->x *= k;
    p->y *= k;
}

struct wide make_wide(int base)
{
    struct wide w = {
        base + 0,  base + 1,  base + 2,  base + 3,
        base + 4,  base + 5,  base + 6,  base + 7,
        base + 8,  base + 9,  base + 10, base + 11,
    };
    return w;
}

long sum_wide(struct wide *w)
{
    return (long) w->a + w->b + w->c + w->d + w->e + w->f
         + w->g + w->h + w->i + w->j + w->k + w->l;
}

/* Mirrors tests/libs/arrays.c:make_int_array -- the return-type-driven malloc
 * site is what liballocs types as int[], giving the pycallocs proxy its length.
 * Filled 0..n-1 so array_index has a deterministic checkable sum. */
int *make_int_array(int nb_elems)
{
    int *arr = malloc((size_t) nb_elems * sizeof *arr);
    for (int i = 0; i < nb_elems; i++) arr[i] = i;
    return arr;
}

void free_int_array(int *arr) { free(arr); }
