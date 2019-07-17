#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>
#include <stdbool.h>

// All the functions declared here do not NULL check or typecheck their arguments

typedef struct {
    PyObject_HEAD
    void *p_ptr;
} ProxyObject;
extern PyTypeObject Proxy_Type;

typedef struct ForeignTypeObject {
    PyObject_HEAD
    const struct uniqtype* ft_type;

    // Type of proxies to foreign objects of this type. Can be NULL.
    // If set, this must be a subtype of Proxy_Type.
    PyTypeObject *ft_proxy_type;

    // Function called to construct an object of this type.
    // Is also called with non tuple args and NULL kwds when trying to convert
    // arbitrary Python object to the foreign representation.
    // Must return an object with a type matching ft_proxy_type
    PyObject *(*ft_constructor)(PyObject *args, PyObject *kwds, struct ForeignTypeObject *type);

    // Use the data pointer as a reference for the content if possible.
    // Can extend the lifetime of the data if necessary.
    // Same as ft_copyfrom for objects without proxy.
    PyObject *(*ft_getfrom)(void *data, struct ForeignTypeObject *type);

    // Copies the contents inside data into a newly created object.
    // These copies are shallow (i.e. pointers or arrays still refer to their
    // original pointee values).
    PyObject *(*ft_copyfrom)(void *data, struct ForeignTypeObject *type);

    // Store the contents of obj inside dest using the current foreign type
    // as the representation.
    // Return a negative value on failure and set a Python exception.
    // This function can (and should) try to do its best to convert any
    // given compatible object to the foreign type.
    int (*ft_storeinto)(PyObject *obj, void *dest, struct ForeignTypeObject *type);

    // Returns a pointer to an existing data chunk of the current type.
    // Return NULL on failure, but never set Python exception.
    // This field can be NULL if objects of this type never hold pointers to
    // the native representation.
    // This is used before ft_storeinto when available to optimize libffi
    // function calls.
    void *(*ft_getdataptr)(PyObject *obj, struct ForeignTypeObject *type);

    // Set for types that can be involved in reference cycles, correspond to
    // standard CPython counterpart.
    // Needed because we cannot create proxies for subobjects when checking for
    // cycles.
    int (*ft_traverse)(void *data, visitproc visit, void *arg, struct ForeignTypeObject *type);
} ForeignTypeObject;
extern PyTypeObject ForeignType_Type;

ForeignTypeObject *ForeignType_GetOrCreate(const struct uniqtype *type);
bool ForeignType_IsTriviallyCopiable(const ForeignTypeObject *type);

void Proxy_InitGCPolicy();
void Proxy_Register(ProxyObject *proxy);
void Proxy_Unregister(ProxyObject *proxy);
ProxyObject *Proxy_GetOrCreateBase(void *addr);
void Proxy_AddRefTo(ProxyObject *target_proxy, const void **from);
PyObject *Proxy_GetFrom(void *data, ForeignTypeObject *type);
PyObject *Proxy_CopyFrom(void *data, ForeignTypeObject *type);
int Proxy_StoreInto(PyObject *obj, void *dest, ForeignTypeObject *type);
void *Proxy_GetDataPtr(PyObject *obj, ForeignTypeObject *type);
int Proxy_ClearRef(PyObject *obj, void *arg);
int Proxy_TraverseRef(void *data, visitproc visit, void *arg, ForeignTypeObject *type);
ForeignTypeObject *Proxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype);

extern PyTypeObject LibraryLoader_Type;

ForeignTypeObject *ForeignBaseType_New(const struct uniqtype *type);
bool ForeignBaseType_IsChar(const struct uniqtype *type);

extern PyTypeObject FunctionProxy_Metatype;
ForeignTypeObject *FunctionProxy_NewType(const struct uniqtype *type);

extern PyTypeObject CompositeProxy_Metatype;
ForeignTypeObject *CompositeProxy_NewType(const struct uniqtype *type);
void CompositeProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);

extern PyTypeObject AddressProxy_Metatype;
ForeignTypeObject *AddressProxy_NewType(const struct uniqtype *type);
void AddressProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);
ForeignTypeObject *ArrayProxy_NewType(const struct uniqtype *type);
void ArrayProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);

#endif
