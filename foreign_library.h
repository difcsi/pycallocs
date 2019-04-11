#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>

// All the functions declared here do not NULL check or typecheck their arguments

int store_pyobject_as_type(PyObject *obj, void *dest, const struct uniqtype *type, PyObject *dlloader);
PyObject *pyobject_from_type(void *data, const struct uniqtype *type, PyObject *dlloader);

PyTypeObject ForeignLibraryLoader_Type;
PyTypeObject *ForeignLibraryLoader_GetPyTypeForUniqtype(PyObject *self, const struct uniqtype *type);

PyTypeObject ForeignFunction_Type;
PyObject *ForeignFunction_New(const char *symname, void *funptr, PyObject *dlloader);
const struct uniqtype *ForeignFunction_GetType(PyObject *func);

PyTypeObject ForeignCompositeType_Type;
PyObject *ForeignHandler_NewCompositeType(const struct uniqtype *type);
PyObject *ForeignHandler_FromDataAndType(void *data, PyTypeObject *type);
void *ForeignHandler_GetDataAddr(PyObject *self);

#endif
