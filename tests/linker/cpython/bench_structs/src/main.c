/* Benchmark: by-value structs carrying fixed-capacity arrays, each direction,
 * and a flat struct of strings as an argument. Prints microseconds per call
 * for each shape; LINKPY_BENCH_CALLS sets the iteration count (default
 * 20000). Passes on exit code (the results are checked), so it runs as an
 * ordinary test:  ctest -R python_bench_structs -V */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shapes.py.h"

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define BENCH(label, n, stmt)                                                   \
    do {                                                                        \
        for (int i = 0; i < 100; i++) { stmt; }                                 \
        double t0 = now();                                                      \
        for (int i = 0; i < (n); i++) { stmt; }                                 \
        printf("bench %s: %.3f us/call (%d calls)\n", label,                    \
               (now() - t0) / (n) * 1e6, (n));                                  \
    } while (0)

int main(void)
{
    const char *env = getenv("LINKPY_BENCH_CALLS");
    int n = env != NULL ? atoi(env) : 20000;
    if (n < 1)
        n = 1;
    int ok = 1;

    struct chunk *in = malloc(sizeof *in), *out = malloc(sizeof *out);
    struct answer *ans = malloc(sizeof *ans);
    memset(in->data, 'U', sizeof in->data);

    in->len = CHUNK;
    BENCH("take(64 KiB)", n, ok &= take(1, *in) == CHUNK);
    in->len = 100;
    BENCH("take(100 B)", n, ok &= take(1, *in) == 100);
    BENCH("give(64 KiB)", n, *out = give(1, CHUNK));
    ok &= out->len == CHUNK && out->data[0] == 'X' && out->data[CHUNK - 1] == 'X';
    BENCH("give(0)", n, *out = give(1, 0));
    ok &= out->len == 0;

    struct meta m = { 7, 0, "GET", "/", "/", "", "HTTP/1.1",
                      "127.0.0.1", "127.0.0.1", "8080", "127.0.0.1" };
    BENCH("ask(meta)", n, *ans = ask(1, m));
    ok &= ans->kind == 1 && ans->status == 200 && ans->origin.x == 3 && ans->origin.y == 4
          && ans->headers_len == 21 && memcmp(ans->headers, "Content-Length\0" "65536\0", 21) == 0
          && ans->headers[21] == 0 && ans->headers[HEAD_MAX - 1] == 0;

    free(in);
    free(out);
    free(ans);
    printf("bench results %s\n", ok ? "ok" : "WRONG");
    return ok ? 0 : 1;
}
