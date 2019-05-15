#include <stdlib.h>
#include <string.h>

int *make_int_array(int nb_elems)
{
    return malloc(nb_elems * sizeof(int));
}

int *get_index(int *arr, int i)
{
    return &arr[i];
}

struct named
{
    char name[20];
    void *ptr;
};

struct open_struct
{
    int nb_elems;
    struct named elems[];
};

struct open_struct *make_empty()
{
    struct open_struct *s = malloc(sizeof(struct open_struct));
    s->nb_elems = 0;
    return s;
}

struct named *make_named_array(int nb_elems)
{
    return malloc(nb_elems * sizeof(struct named));
}

struct open_struct *make_full(int nb_elems, struct named arr[])
{
    struct open_struct *s = malloc(sizeof(struct open_struct) + nb_elems * sizeof(struct named));
    s->nb_elems = nb_elems;
    memcpy(s->elems, arr, nb_elems * sizeof(struct named));
    return s;
}

