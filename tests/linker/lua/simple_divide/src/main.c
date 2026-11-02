#include <stdio.h>
#include "mytest.lua.h"

int main(void)
{
    printf("Hello from main!\n");
    int a = 6, b = 3;
    printf("Calling divide(%d, %d)\n", a, b);
    double result = divide(a, b);
    printf("Result: %f\n", result);
    // 6 / 3 == 2.0 in Lua; fail the test if the marshalled value is wrong.
    return (result > 1.999 && result < 2.001) ? 0 : 1;
}
