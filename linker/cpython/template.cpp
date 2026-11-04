#include <mutex>
#include <pthread.h>
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
static std::once_flag linkpy_vm_once;
static bool linkpy_vm_started = false;
static pthread_t linkpy_vm_thread;

static void linkpy_start_vm()
{
    linkpy::py::init_py();
#ifdef LINKPY_INIT_LIB_DIR
    // Emitted by the auditor: the stub library's directory, searched for
    // modules after `src` so a language module's Python half can live beside
    // its init library.
    PyList_Append(PySys_GetObject("path"), PyUnicode_FromString(LINKPY_INIT_LIB_DIR));
#endif
    linkpy_main_thread_state = PyEval_SaveThread();
    linkpy_vm_thread = pthread_self();
    linkpy_vm_started = true;
#ifdef DEBUG
    std::cout << "PyVM Initialized from linkstub" << std::endl;
#endif
}

// Threads that make their first call together all wait for the one start.
void linkpy::py::ensure_vm()
{
    std::call_once(linkpy_vm_once, linkpy_start_vm);
}

__attribute__((destructor)) void cleanup_vms()
{
    // Py_Finalize belongs to the thread that started the interpreter; a
    // process exiting from another thread keeps the interpreter as it is.
    if (!linkpy_vm_started || !pthread_equal(pthread_self(), linkpy_vm_thread))
        return;
    PyEval_RestoreThread(linkpy_main_thread_state);
    Py_Finalize();
#ifdef DEBUG
    std::cout << "PyVM Cleaned Up from linkstub" << std::endl;
#endif
}