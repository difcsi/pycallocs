#include <stdio.h>
#include <stdlib.h>

struct base
{
    int id;
};

struct derivated
{
    struct base base;
    float data;
};

struct leaf
{
    struct derivated derivated;
    char character;
};

void print_base(const struct base b)
{
    printf("Base: %d\n", b.id);
}

void print_derivated(const struct derivated *d)
{
    printf("Derivated: %d, %f\n", d->base.id, d->data);
}

void print_leaf(const struct leaf *l)
{
    printf("Leaf: %d, %f, %c\n",
            l->derivated.base.id, l->derivated.data, l->character);
}

void dynamic_print(void *obj)
{
    switch (((struct base *)obj)->id)
    {
        case 0:
            print_base(*(struct base *) obj);
            break;
        case 1:
            print_derivated(obj);
            break;
        case 2:
            print_leaf(obj);
            break;
        default:
            printf("Unrecognized object\n");
            break;
    }
}

struct base *create_object(int id)
{
    struct base *ob;
    switch (id)
    {
        case 0:
            ob = malloc(sizeof(struct base));
            break;
        case 1:
            ob = (struct base *) malloc(sizeof(struct derivated));
            break;
        case 2:
            ob = (struct base *) malloc(sizeof(struct leaf));
            break;
        default:
            return NULL;
    }
    ob->id = id;
    return ob;
}
