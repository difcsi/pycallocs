#include <stdio.h>

struct instruct {
    int data1;
    int data2;
};

struct outstruct {
    struct instruct a;
    struct instruct b;
};

struct outstruct make_outstruct(int a1, int a2, int b1, int b2)
{
    struct outstruct res;
    res.a.data1 = a1;
    res.a.data2 = a2;
    res.b.data1 = b1;
    res.b.data2 = b2;
    return res;
}

void print_instruct(struct instruct s)
{
    printf("{ %d, %d }", s.data1, s.data2);
}

void print_outstruct(struct outstruct s)
{
    printf("{ ");
    print_instruct(s.a);
    printf(", ");
    print_instruct(s.b);
    printf(" }\n");
}

