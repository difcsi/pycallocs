#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include "late.py.h"

int main(void)
{
    const char *module_dir = getenv("LATE_MODULE_DIR");
    if (module_dir == NULL) {
        fprintf(stderr, "LATE_MODULE_DIR must name the directory holding late.py\n");
        return 2;
    }

    void *lib = dlopen("libinit.so", RTLD_NOW | RTLD_GLOBAL);
    if (lib == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    /* Only now does the host know where the application lives. */
    setenv("PYTHONPATH", module_dir, 1);

    int (*fn)(void) = (int (*)(void)) dlsym(lib, "answer_late");
    if (fn == NULL) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        return 1;
    }
    int r = fn();
    printf("answer_late: %d\n", r);
    return r == 7 ? 0 : 1;
}
