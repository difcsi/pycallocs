#include <stdio.h>
#include <unistd.h>
#include "bg.py.h"

int main(void)
{
    start();
    usleep(300 * 1000);     /* C-side work; no trampoline is running */
    int n = count();
    printf("worker advanced %d times while C slept\n", n);
    /* With the GIL parked on the loading thread the worker gets at most an
     * iteration or two (inside start() itself); released, it gets dozens. */
    return n >= 20 ? 0 : 1;
}
