#ifndef FOREIGN_LIBRARY_H
#define FOREIGN_LIBRARY_H

#include <Python.h>
#include <liballocs.h>
#include <stdbool.h>
#include <minicrunch.h>
// Route every address-indexed liballocs query through stackscan's
// translate-then-index glue: an Alaska handle (non-canonical, bit 63 set) must
// be translated to its backing pointer before any liballocs lookup, otherwise
// the query indexes a top-bit-set address and faults. ss_translate() is a no-op
// on non-handles, so wrapping every pointer is always safe.
#include <handle_query.h>

#ifdef ALLOCS_HAVE_ALASKA
// HACK: This might be buggy. In that case, replace macro with an always-inline halloc wrapper and inc ref in it
#define totally_malloc(x) halloc(x)
// TODO: Improve performance by directly detaching the policy instead of interposing through hfree.
#define totally_free(ptr) hfree(ptr)
#else
#define totally_malloc(x) malloc(x)
#define totally_free(ptr) __liballocs_detach_manual_dealloc_policy((ptr))
#endif

// Workaround for CIL compatibility issues
#ifndef nullptr
#define nullptr NULL
#endif
#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

// The specialised PyObject_to_T / T_to_PyObject fast path can be supplied by
// either back end (string-templated specialise.c or the P3294 token-injection
// draft specialise_inject.c). Code that only cares *whether* the fast path is
// present -- call sites and the exported `SPECIALISE_CONVERSION` module flag --
// keys off this single derived macro instead of the two back-end macros.
#if defined(PYCALLOCS_SPECIALISE_CONVERSION) || defined(PYCALLOCS_INJECT_CONVERSION)
#define PYCALLOCS_HAVE_SPECIALISE 1
#endif

// All the functions declared here do not NULL check or typecheck their arguments

typedef struct {
    PyObject_HEAD
    void *p_ptr; // ZMTODO: VERY IMPORTANT: THIS IS THE UNDERLYING ADDRESSLIKE. THIS IS A HANDLE
} ProxyObject;
extern PyTypeObject Proxy_Type;

// The ForeignTypeObject struct is the Python-level representation of a uniqtype.
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
// One-element out-parameter "cells" (see foreign_type.c). NewCell builds a
// length-1 array proxy of elem_ftype initialised from init; the CellCtor*
// variants are ft_constructor implementations for base (zero-initialised)
// and pointer (NULL-initialised) types respectively.
PyObject *ForeignType_NewCell(ForeignTypeObject *elem_ftype, PyObject *init);
PyObject *ForeignType_CellCtorZero(PyObject *args, PyObject *kwargs, ForeignTypeObject *type);
PyObject *ForeignType_CellCtorNull(PyObject *args, PyObject *kwargs, ForeignTypeObject *type);

void Proxy_InitGCPolicy();
void Proxy_Register_To_Dict(ProxyObject *proxy);
void Proxy_Unregister_From_Dict(ProxyObject *proxy);
ProxyObject *Proxy_GetOrCreateBase(void *addr);
void Proxy_AddRefTo(ProxyObject *target_proxy, const void **from);
void Proxy_RetainStoredPtr(const void **dest, PyObject *valobj, const void *val);

// Re-establish lifetime references for the pointer fields a just-adopted foreign
// object already holds, walking uniqtype `t` over the raw region. See proxy.c.
void Proxy_AdoptPtrFields(void *region, struct uniqtype *t);

// Bracket a foreign (C) call: while the defer count is non-zero, a GC delref that
// would drop a proxy's last reference instead parks that proxy until the outermost
// call returns. This lets a pointer the callee returns -- after internally
// overwriting the slot that kept it alive -- be re-adopted by the return-value
// wrapper before it is freed (struct_global_swap).
void Proxy_BeginDeferFrees(void);
void Proxy_EndDeferFrees(void);
PyObject *Proxy_GetFrom(void *data, ForeignTypeObject *type);
PyObject *Proxy_CopyFrom(void *data, ForeignTypeObject *type);
int Proxy_StoreInto(PyObject *obj, void *dest, ForeignTypeObject *type);
void *Proxy_GetDataPtr(PyObject *obj, ForeignTypeObject *type);
int Proxy_ClearRef(PyObject *obj, void *arg);
int Proxy_TraverseRef(void *data, visitproc visit, void *arg, ForeignTypeObject *type);
ForeignTypeObject *Proxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype);

extern PyTypeObject LibraryLoader_Type;

ForeignTypeObject *ForeignBaseType_New(const struct uniqtype *type);
ForeignTypeObject *ForeignEnumType_New(const struct uniqtype *type);
bool ForeignBaseType_IsChar(const struct uniqtype *type);

extern PyTypeObject FunctionProxy_Metatype;
ForeignTypeObject *FunctionProxy_NewType(const struct uniqtype *type);

extern PyTypeObject CompositeProxy_Metatype;
ForeignTypeObject *CompositeProxy_NewType(const struct uniqtype *type);
void CompositeProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);

extern PyTypeObject AddressProxy_Metatype;
ForeignTypeObject *AddressProxy_NewType(const struct uniqtype *type);
void AddressProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);
ProxyObject *AddressProxy_MaterializePointee(PyObject *obj, ForeignTypeObject *type);
ForeignTypeObject *ArrayProxy_NewType(const struct uniqtype *type);
void ArrayProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type);

// JIT-compiled fast_path conversions: PyObject <-> C struct by-value
#if defined(PYCALLOCS_SPECIALISE_CONVERSION) || defined(PYCALLOCS_INJECT_CONVERSION)

// Try to convert `obj` into a freshly registered proxy of the foreign type `pointee`
// Returns 0 on success (*out is a NEW reference), -1 on failure (Python exception
// set), or a positive value when the type is unsupported by this fast path and
// the caller should fall back to the generic converter.
int Specialise_Convert(PyObject *obj, ForeignTypeObject *pointee, PyObject **out);

// Inverse of Specialise_Convert: convert a by-value C struct at `src` (of foreign
// type `type`) into a plain Python object (types.SimpleNamespace)
int Specialise_FromValue(void *src, ForeignTypeObject *type, PyObject **out);
#endif

#endif
