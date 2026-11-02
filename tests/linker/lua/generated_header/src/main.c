#include <stdio.h>
#include "mymod.lua.h"

int main(void)
{
    int s = add(2, 5);
    printf("add(2, 5) = %d\n", s);
    struct Point p = make_point(10, 20);
    printf("make_point(10, 20) = {%d, %d}\n", p.x, p.y);
    return (s == 7 && p.x == 10 && p.y == 20) ? 0 : 1;
}
