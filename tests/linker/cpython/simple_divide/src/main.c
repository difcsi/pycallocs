#include <stdio.h>
#include "mytest.py.h"

#define FLOAT false

int main(int argc, char **argv)
{
    printf("Hello from main!\n");
    // phelloworld();
    int a = 6, b = 3;
    printf("Calling %d,%d\n", a, b);
#if FLOAT
    float result = divide(a, b);
    printf("Result: %f\n", result);
#else
    int result = divide(a, b);
    printf("Result: %d\n", result);
#endif

    return 0;
}