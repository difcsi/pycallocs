#include <stdio.h>
#include "mytest.py.h"

static int expect(const char *step, int got, int want)
{
    if (got != want)
    {
        printf("after %s: value() returned %d, expected %d\n", step, got, want);
        return 0;
    }
    return 1;
}

int main(void)
{
    int ok = expect("import", value(), 1);
    rebind();
    ok &= expect("rebind", value(), 2);
    replace();
    ok &= expect("replace", value(), 3);
    printf("rebind ok=%d\n", ok);
    return ok ? 0 : 1;
}
