/* Benchmark: the fixed cost of one trampoline call (two ints in, a float out).
 * Prints the microseconds per call; LINKPY_BENCH_CALLS sets the iteration
 * count (default 200000). Passes on exit code, so it runs as an ordinary test:
 *   ctest -R python_bench_divide -V */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "mytest.py.h"

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    const char *env = getenv("LINKPY_BENCH_CALLS");
    int n = env != NULL ? atoi(env) : 200000;
    if (n < 1)
        n = 1;
    volatile double sink = 0;
    for (int i = 0; i < 1000; i++)
        sink += divide(6, 3);
    double t0 = now();
    for (int i = 0; i < n; i++)
        sink += divide(6, 3);
    double per_call = (now() - t0) / n * 1e6;
    printf("bench divide(6,3): %.3f us/call (%d calls)\n", per_call, n);
    return sink > 0 ? 0 : 1;
}
