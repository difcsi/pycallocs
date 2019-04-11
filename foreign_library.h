#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>

// All the functions declared here do not NULL check or typecheck their arguments

int store_pyobject_as_type(PyObject *obj, void *dest, const struct uniqtype *type, PyObject *dlloader);

// Data is copied into the created Python object except if allocator is not
// NULL. These copies are shallow (i.e. pointers or arrays still refer to their
// original pointee values).
// With allocator set to non-NULL, struct handlers will use the data pointer as
// a reference for their content.
PyObject *pyobject_from_type(void *data, const struct uniqtype *type, PyObject *dlloader, PyObject *allocator);

PyTypeObject ForeignLibraryLoader_Type;
PyTypeObject *ForeignLibraryLoader_GetPyTypeForUniqtype(PyObject *self, const struct uniqtype *type);

PyTypeObject ForeignFunction_Type;
PyObject *ForeignFunction_New(const char *symname, void *funptr, PyObject *dlloader);
const struct uniqtype *ForeignFunction_GetType(PyObject *func);

PyTypeObject ForeignCompositeType_Type;
PyObject *ForeignHandler_NewCompositeType(const struct uniqtype *type, PyObject *dlloader);
PyObject *ForeignHandler_FromData(void *data, PyTypeObject *type);
PyObject *ForeignHandler_FromPtr(void *ptr, PyTypeObject *type, PyObject *allocator);
void *ForeignHandler_GetDataAddr(PyObject *self);

#endif
