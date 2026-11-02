#include <stdio.h>
#include "mytest.lua.h"

int main(void)
{
    struct HelloWorld hw = phelloworld2();
    printf("HelloWorld: %d\n", hw.a);
    return hw.a == 13 ? 0 : 1;
}
