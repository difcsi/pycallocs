/* Benchmark fixture library for pycallocs.
 *
 * Deliberately tiny, leaf, side-effect-free functions: the work each one does
 * is negligible next to the Python<->C crossing, so a timing loop measures the
 * FFI layer rather than the C body. (Contrast tests/libs/composite.c, whose
 * print_hw/print_cat do stdout I/O that would swamp a benchmark.)
 *
 * Compiled two ways from the same source (see CMakeLists.txt):
 *   bench.so          via allocscc  -> DWARF + uniqtypes, loaded by pycallocs
 *   libbench_plain.so via gcc -O2   -> loaded by ctypes / cffi / the native ext
 */
#ifndef PYCALLOCS_BENCH_H
#define PYCALLOCS_BENCH_H

struct point { int x; int y; };

/* A wider aggregate so by-value conversion cost can be seen to scale with the
 * field count (bigstruct_ret workload). */
struct wide { int a, b, c, d, e, f, g, h, i, j, k, l; };

int    bench_triple(int x);            /* scalar_int  */
double bench_mul(double a, double b);  /* scalar_fp   */

struct point make_point(int x, int y);    /* byval_ret  : struct returned by value */
long         consume_point(struct point p); /* byval_arg : struct passed by value  */
long         sum_point(struct point *p);    /* byptr_arg : struct passed by pointer */
void         scale_point(struct point *p, int k); /* field write helper            */

struct wide make_wide(int base);       /* bigstruct_ret */
long        sum_wide(struct wide *w);

int  *make_int_array(int nb_elems);    /* array_index: heap int[] typed by liballocs */
void  free_int_array(int *arr);

#endif /* PYCALLOCS_BENCH_H */
