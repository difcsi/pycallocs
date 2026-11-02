// Host-side prologue for a Python HOST (LINKPY_HOST_LANG=python). The auditor
// appends this right after the CALLEE template, bringing the Python marshaller
// into scope for the generated host wrappers (linkpy_wrap_*) that follow the
// trampolines.
//
// Deliberately NO constructor and NO VM lifecycle here: the host interpreter
// belongs to the process that dlopen'ed us (e.g. `python app.py`) and is
// already running. For the same reason the linkstub build must NOT link
// libpython -- every Py* reference stays undefined and resolves against the
// host binary's exported symbols (CPython links -export-dynamic) -- mirroring
// the vmextensions rule that an extension never links its own copy of the VM.
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pycpputils.hpp>
#include <stdexcept>
#include <cstdio>
