#include <stdio.h>
#include "meta.py.h"

int main(void)
{
    struct meta m = { .name = "alice", .note = NULL, .n = 3 };
    int r = describe(m);
    printf("describe returned: %d\n", r);
    return r == 8 ? 0 : 1;
}
