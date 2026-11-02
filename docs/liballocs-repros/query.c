/* Drives liballocs the way a real client does: force a typed query so
 * the meta-DSO / dlbind machinery loads (this is what makes the
 * startup/noise bugs in docs/LIBALLOCS_BUGS.md observable; a bare
 * malloc is not enough). See run.sh for how it is invoked. */

#include <stdio.h>
#include <stdlib.h>

#include <liballocs.h>

int main(void)
{
    void *p = malloc(64);
    struct allocator *a = NULL;
    const void *base = NULL;
    unsigned long sz = 0;
    struct uniqtype *t = NULL;
    const void *site = NULL;

    __liballocs_get_alloc_info(p, &a, &base, &sz, &t, &site);

    free(p);
    puts("reached main (bug not reproduced)");
    return 0;
}
