#include <stdio.h>
#include "mytest.py.h"

int main()
{
    struct HelloWorld hw = { .a = 21 };
    int r = describe(hw);
    printf("describe returned: %d\n", r);
    return r == 42 ? 0 : 1;
}
