#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>

// All the functions declared here do not NULL check or typecheck their arguments

typedef struct ForeignTypeObject {
    PyObject_HEAD
    const struct uniqtype* ft_type;

    PyObject *ft_constructor; // Callable constructor (may be a class)

    // Data is copied into the created Python object except if allocator is not
    // NULL. These copies are shallow (i.e. pointers or arrays still refer to their
    // original pointee values).
    // With allocator set to non-NULL, struct handlers will use the data pointer as
    // a reference for their content.
    PyObject *(*ft_getfrom)(void *data, struct ForeignTypeObject *type, PyObject *allocator);

    int (*ft_storeinto)(PyObject *obj, void *dest, struct ForeignTypeObject *type);
} ForeignTypeObject;

extern PyTypeObject ForeignType_Type;
ForeignTypeObject *ForeignType_GetOrCreate(const struct uniqtype *type);

extern PyTypeObject ForeignLibraryLoader_Type;

extern PyTypeObject ForeignFunction_Type;
PyObject *ForeignFunction_New(const char *symname, void *funptr, PyObject *dlloader);
const struct uniqtype *ForeignFunction_GetType(PyObject *func);

extern PyTypeObject ForeignComposite_HandlerMetatype;

#endif
