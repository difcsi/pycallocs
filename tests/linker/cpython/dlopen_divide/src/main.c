#include <stdio.h>
#include <dlfcn.h>

// dlopen variant of simple_divide. Instead of linking libinit.so at load time,
// main resolves divide() at RUNTIME: dlopen the init library, then dlsym it. The
// auditor's la_objsearch intercepts the dlopen and redirects "libinit.so" to the
// JIT-compiled liblinkstub.so, so the dlsym'd `divide` is the trampoline that
// marshals into Python. The "./" path form also proves the redirect matches by
// basename -- an exact-string match on "libinit.so" would miss it and fall
// through to the real (aborting) stub library on disk.
int main(void)
{
    printf("Hello from dlopen main!\n");

    void *h = dlopen("./libinit.so", RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    float (*divide)(int, int) = (float (*)(int, int)) dlsym(h, "divide");
    if (!divide)
    {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }

    int a = 6, b = 3;
    printf("Calling divide(%d, %d)\n", a, b);
    float result = divide(a, b);
    printf("Result: %f\n", result);

    // 6 / 3 == 2.0; fail the test if the marshalled value is wrong.
    return (result > 1.999f && result < 2.001f) ? 0 : 1;
}
