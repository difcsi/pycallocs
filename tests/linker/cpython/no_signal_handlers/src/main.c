#include <signal.h>
#include <stdio.h>
#include "sigs.py.h"

int main(void)
{
    struct sigaction before, after;
    sigaction(SIGINT, NULL, &before);

    nothing();      /* the first call starts the interpreter */

    sigaction(SIGINT, NULL, &after);
    int same = before.sa_handler == after.sa_handler
               && before.sa_flags == after.sa_flags;
    printf("SIGINT disposition unchanged by starting Python: %d\n", same);
    return same ? 0 : 1;
}
