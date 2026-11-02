#include <string>
#include <unistd.h>
#include <Python.h>
#include <string.h>
#include <pycpputils.hpp>


#define DEBUG

#ifdef DEBUG
#include <iostream>
#include <stdio.h>
#endif

__attribute__((constructor)) void init_vms()
{
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    PyConfig_Clear(&config);

    init_py();

#ifdef DEBUG
    std::cout << "PyVM Initialized from linkstub" << std::endl;
#endif
}

__attribute__((destructor)) void cleanup_vms()
{
    Py_Finalize();
#ifdef DEBUG
    std::cout << "PyVM Cleaned Up from linkstub" << std::endl;
#endif
}