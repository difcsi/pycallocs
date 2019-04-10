#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>

int store_pyobject_as_type(PyObject *obj, void *mem, const struct uniqtype *type);
PyObject *pyobject_from_type(void *data, const struct uniqtype *type, PyObject *typdict);

PyTypeObject ForeignLibraryLoader_Type;
PyObject *ForeignLibraryLoader_GetUniqtypeDict(PyObject *self); // No type checking

PyTypeObject ForeignFunction_Type;
PyObject *ForeignFunction_New(const char *symname, void *funptr, PyObject *dlloader);
const struct uniqtype *ForeignFunction_GetType(PyObject *func); // No type checking

PyTypeObject ForeignCompositeType_Type;
PyObject *ForeignHandler_NewCompositeType(const struct uniqtype *type);
PyObject *ForeignHandler_FromDataAndType(void *data, PyTypeObject *type);

#endif
