#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>

PyTypeObject ForeignLibraryLoader_Type;

PyTypeObject ForeignFunction_Type;
PyObject *ForeignFunction_New(const char *symname, void *funptr);

#endif
