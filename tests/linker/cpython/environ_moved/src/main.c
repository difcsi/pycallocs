#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include "envmod.py.h"

extern char **environ;

int main(void)
{
    /* Overwrite every environment string in place, the way a server reuses
     * that memory for its process title, before the init library is loaded. */
    for (char **e = environ; *e != NULL; e++) {
        memset(*e, 'x', strlen(*e));
    }

    void *lib = dlopen("libinit.so", RTLD_NOW | RTLD_GLOBAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    int (*fn)(void) = (int (*)(void)) dlsym(lib, "answer");
    if (fn == NULL) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        return 1;
    }
    int r = fn();
    printf("answer: %d\n", r);
    return r == 42 ? 0 : 1;
}
