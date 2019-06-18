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

    PyObject *ft_constructor; // Callable constructor (may be a class)

    // Use the data pointer as a reference for the content if possible.
    // Can extend the lifetime of the data if necessary.
    // Same as ft_copyfrom for objects without proxy.
    PyObject *(*ft_getfrom)(void *data, struct ForeignTypeObject *type);

    // Copies the contents inside data into a newly created object.
    // These copies are shallow (i.e. pointers or arrays still refer to their
    // original pointee values).
    PyObject *(*ft_copyfrom)(void *data, struct ForeignTypeObject *type);

    int (*ft_storeinto)(PyObject *obj, void *dest, struct ForeignTypeObject *type);

    // Returns a pointer to an existing data chunk of the current type.
    // Return NULL on failure, and set Python exception.
    // This field can be NULL if object of this type do not hold a pointer to
    // the native representation.
    // This is used instead of ft_storeinto when available to optimize libffi 
    // function calls.
    void *(*ft_getdataptr)(PyObject *obj, struct ForeignTypeObject *type);
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
ForeignTypeObject *Proxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype);

extern PyTypeObject LibraryLoader_Type;

ForeignTypeObject *ForeignBaseType_New(const struct uniqtype *type);

extern PyTypeObject FunctionProxy_Metatype;
ForeignTypeObject *FunctionProxy_NewType(const struct uniqtype *type);

extern PyTypeObject CompositeProxy_Metatype;
ForeignTypeObject *CompositeProxy_NewType(const struct uniqtype *type);
void CompositeProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);

extern PyTypeObject AddressProxy_Metatype;
ForeignTypeObject *AddressProxy_NewType(const struct uniqtype *type);
ForeignTypeObject *ArrayProxy_NewType(const struct uniqtype *type);

#endif
