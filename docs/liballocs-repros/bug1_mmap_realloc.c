/* Bug 1 (docs/LIBALLOCS_BUGS.md): realloc of an mmap-served glibc
 * chunk crashes the malloc index.
 *
 * glibc serves requests above the mmap threshold (~128 KiB) via mmap
 * and implements realloc of such chunks with mremap, which can move
 * the mapping without a malloc-hook event the index understands. The
 * growth loop below mimics what Lua's stack reallocation does (the
 * original crash was a 188392 -> 279032 byte grow) and crashes in
 * arena_for_userptr (generic_malloc_index.h) under
 * __liballocs_malloc_post_nonnull_nonzero_realloc.
 *
 * Expected without the bug: prints every step and "no crash".
 * Observed: SIGSEGV once the chunk is mmap-served and gets moved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    size_t sz = 100 * 1024;  /* start below the mmap threshold */
    char *p = malloc(sz);
    if (!p) return 2;
    memset(p, 0xAA, sz);

    while (sz < 8u * 1024 * 1024) {
        size_t nsz = sz + sz / 2;   /* Lua-style 1.5x growth */
        char *np;
        fprintf(stderr, "realloc %zu -> %zu\n", sz, nsz);
        np = realloc(p, nsz);
        if (!np) return 2;
        p = np;
        memset(p, 0xBB, nsz);
        sz = nsz;
    }
    free(p);
    puts("no crash (bug not reproduced)");
    return 0;
}
