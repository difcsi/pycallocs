#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>
#include <stdbool.h>

// All the functions declared here do not NULL check or typecheck their arguments

typedef struct {
    PyObject_HEAD
    void *fpo_ptr;
    PyObject *fpo_allocator;
} ForeignProxyObject;
extern PyTypeObject ForeignProxy_Type;

typedef struct ForeignTypeObject {
    PyObject_HEAD
    const struct uniqtype* ft_type;

    // Type of proxies to foreign objects of this type. Can be NULL.
    // If set, this must be a subtype of ForeignProxy_Type.
    PyTypeObject *ft_proxy_type;

    PyObject *ft_constructor; // Callable constructor (may be a class)

    // Data is copied into the created Python proxy except if allocator is not
    // NULL. These copies are shallow (i.e. pointers or arrays still refer to their
    // original pointee values).
    // With allocator set to non-NULL, proxies will use the data pointer as
    // a reference for their content.
    PyObject *(*ft_getfrom)(void *data, struct ForeignTypeObject *type, PyObject *allocator);

    int (*ft_storeinto)(PyObject *obj, void *dest, struct ForeignTypeObject *type);
} ForeignTypeObject;
extern PyTypeObject ForeignType_Type;

ForeignTypeObject *ForeignType_GetOrCreate(const struct uniqtype *type);
bool ForeignType_IsTriviallyCopiable(const ForeignTypeObject *type);
ForeignTypeObject *ForeignProxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype);

extern PyTypeObject ForeignLibraryLoader_Type;

ForeignTypeObject *ForeignBaseType_New(const struct uniqtype *type);

extern PyTypeObject ForeignFunction_ProxyMetatype;
ForeignTypeObject *ForeignFunction_NewType(const struct uniqtype *type);

extern PyTypeObject ForeignComposite_ProxyMetatype;
ForeignTypeObject *ForeignComposite_NewType(const struct uniqtype *type);
void ForeignComposite_InitType(ForeignTypeObject *self, const struct uniqtype *type);

extern PyTypeObject ForeignAddress_ProxyMetatype;
ForeignTypeObject *ForeignAddress_NewType(const struct uniqtype *type);

#endif
