#include <string>
#include <unistd.h>
#include <Python.h>
#include <string.h>
#include <pycpputils.hpp>



#ifdef DEBUG
#include <iostream>
#include <stdio.h>
#endif

// The interpreter starts at the first call, not when the linkstub is loaded:
// a host may configure the environment it should see (PYTHONPATH, PYTHONHOME)
// between loading the library and calling into it. Once up, the GIL is
// parked: the host is a C program that may run for a long time between calls,
// and Python threads an application starts must keep running meanwhile. Each
// trampoline takes the GIL for the duration of its call (GilGuard in
// run_function) and lets go on the way out.
static PyThreadState *linkpy_main_thread_state = nullptr;
static bool linkpy_vm_started = false;

void linkpy::py::ensure_vm()
{
    if (linkpy_vm_started)
        return;
    linkpy_vm_started = true;
    linkpy::py::init_py();
    linkpy_main_thread_state = PyEval_SaveThread();
#ifdef DEBUG
    std::cout << "PyVM Initialized from linkstub" << std::endl;
#endif
}

__attribute__((destructor)) void cleanup_vms()
{
    if (!linkpy_vm_started)
        return;
    if (linkpy_main_thread_state != nullptr)
        PyEval_RestoreThread(linkpy_main_thread_state);
    Py_Finalize();
#ifdef DEBUG
    std::cout << "PyVM Cleaned Up from linkstub" << std::endl;
#endif
}