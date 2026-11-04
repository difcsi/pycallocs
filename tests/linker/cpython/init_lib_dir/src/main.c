#include <stdio.h>
#include "beside.py.h"

int main(void)
{
    int v = value_beside();
    printf("value_beside: %d\n", v);
    return v == 5 ? 0 : 1;
}
