/* A python launcher linked against the shared libpython of the LinkPy build:
 * `pyrun script.py` is `python3 script.py` on the interpreter that the
 * hand-written bridge, the cffi library and the linkstub embed. The lupa
 * route runs on it so that every route of the benchmark executes the callee
 * on the same CPython build; a distribution's python3 binary may be linked
 * statically, which is a different (faster) build of the same version.
 *
 * Prints `info python=shared` or `info python=static`, by where Py_BytesMain
 * was found, for the driver to check. */

#define _GNU_SOURCE
#include <Python.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    Dl_info where;
    int shared = dladdr((void *)Py_BytesMain, &where) && where.dli_fname
                 && strstr(where.dli_fname, "libpython") != NULL;
    printf("info python=%s\n", shared ? "shared" : "static");
    fflush(stdout);
    return Py_BytesMain(argc, argv);
}
