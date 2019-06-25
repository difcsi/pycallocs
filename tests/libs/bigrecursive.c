#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bigrecursive
{
    struct bigrecursive *ptr;
    char data[0x1000];
};

struct bigrecursive *copy_bigrec(struct bigrecursive *orig_br)
{
    struct bintree *new_br = malloc(sizeof(struct bigrecursive));
    memcpy(new_br, orig_br, sizeof(struct bigrecursive));
    return new_br;
}
