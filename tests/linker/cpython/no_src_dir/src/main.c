#include <stdio.h>
#include "nosrc.py.h"

int main(void)
{
    int relative = relative_src_on_path();
    printf("relative 'src' on sys.path: %d\n", relative);
    return relative ? 1 : 0;
}
