#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "mytest.py.h"

int main(void)
{
    struct padded p;
    memset(&p, 0xAA, sizeof p);   /* the return slot starts out dirty */
    p = make_padded();

    int members_ok = p.c == 1 && p.x == 2 && p.d == 3 && p.y == 4;
    int padding_ok = 1;
    const unsigned char *b = (const unsigned char *) &p;
    for (size_t i = 0; i < sizeof p; i++)
    {
        int is_member = i == offsetof(struct padded, c)
                        || (i >= offsetof(struct padded, x) && i < offsetof(struct padded, x) + sizeof p.x)
                        || i == offsetof(struct padded, d)
                        || (i >= offsetof(struct padded, y) && i < offsetof(struct padded, y) + sizeof p.y);
        if (!is_member && b[i] != 0)
        {
            printf("padding byte %zu is 0x%02x, expected 0\n", i, b[i]);
            padding_ok = 0;
        }
    }
    long s = sum_padded(p);
    printf("padded: members ok=%d padding ok=%d sum=%ld\n", members_ok, padding_ok, s);
    return (members_ok && padding_ok && s == 10) ? 0 : 1;
}
