#include <stdio.h>
#include "mytest.lua.h"

int main(void)
{
    struct Point p = { .x = 3, .y = 4 };
    int sum = describe(p);
    printf("describe({3, 4}) = %d\n", sum);
    return sum == 7 ? 0 : 1;
}
