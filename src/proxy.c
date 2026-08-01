#include "foreign_library.h"

// FIXME: Use more efficient representation than PyDict
static PyObject *proxy_dict; // Weak reference over values (using PyLong to store refs)
static PyObject *proxy_pointing_addr_dict; // Strong refs over values

// Defer-free window (see Proxy_BeginDeferFrees in foreign_library.h). While
// proxy_defer_depth > 0, proxy_delref parks the proxy whose last reference it is
// about to drop onto proxy_deferred_list, keeping it (and its foreign chunk) alive
// until the outermost foreign call returns. The list is created lazily on first
// park and dropped (releasing all parked proxies) when the depth returns to 0.
static int proxy_defer_depth = 0;
static PyObject *proxy_deferred_list = NULL;

void Proxy_BeginDeferFrees(void)
{
    ++proxy_defer_depth;
}

void Proxy_EndDeferFrees(void)
{
    if (--proxy_defer_depth == 0 && proxy_deferred_list)
    {
        PyObject *parked = proxy_deferred_list;
        proxy_deferred_list = NULL;
        Py_DECREF(parked); // drops every parked proxy; any not re-adopted now dies
    }
}


// Get's the Proxjy object for a given addresslike.
// Returns NULL if no proxy is registered for that addresslike.
static ProxyObject *lookup_proxydict(const void *addrlike)
{
    PyObject *key = PyLong_FromVoidPtr((void *) addrlike);
    PyObject *proxy_ptr = PyDict_GetItem(proxy_dict, key);
    Py_DECREF(key);
    if (!proxy_ptr) return NULL;
    return PyLong_AsVoidPtr(proxy_ptr);
}

void Proxy_AddRefTo(ProxyObject *target_proxy, const void **from)
{
    PyObject *from_key = PyLong_FromVoidPtr(from);
    assert(!PyDict_Contains(proxy_pointing_addr_dict, from_key));
    // This effectively incref target_proxy which is the desired behaviour
    if(PyDict_SetItem(proxy_pointing_addr_dict, from_key, (PyObject *) target_proxy) < 0)
        abort();
    Py_DECREF(from_key);
}

// increase the reference count of a proxy if a pointer to it's pointee is stored somewhere
// FIXME: this is horrendous and not needed under the following assumptions:
// 1. The allocscc compiles the foreign code with alaska
// 2. Target is a handle

// These callbacks only ever see raw (non-handle) objects: Python's interest in
// a handle-backed object is an Alaska handle refcount (Proxy_Register_To_Dict),
// not a liballocs lifetime policy, so liballocs never tracks handle writes here.
static void proxy_addref(const void *target, const void **from)
{
    ProxyObject *target_proxy = lookup_proxydict(target);
    // We should only be called if a proxy is registered for target.
    // liballocs' GC callbacks operate in raw (translated) address space,
    assert(target_proxy);
    assert(target_proxy->p_ptr == target);

    Proxy_AddRefTo(target_proxy, from);
}

static void proxy_delref(const void *target, const void **from)
{
    /* We can ignore the target and just delete the entry in
     * proxy_pointing_addr_dict. */

    PyObject *from_key = PyLong_FromVoidPtr(from);

#ifndef NDEBUG
    // Sanity check to be sure that we are effectively deleting what we think
    if (target && PyDict_Contains(proxy_pointing_addr_dict, from_key) == 1)
    {
        ProxyObject *proxy =
            (ProxyObject *) PyDict_GetItem(proxy_pointing_addr_dict, from_key);
        assert(proxy->p_ptr == target);
    }
#endif

    /* We can observe spurious calls from inexistant location.
     * Just ignore them => No check for existing key
     * Deleting the item from dict decref the proxy object and can trigger
     * deletion. */
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback); // Save the current exception
    // Inside a foreign-call defer window, park the proxy this slot points at so the
    // DelItem below doesn't free it mid-call -- the callee may be returning it.
    if (proxy_defer_depth > 0)
    {
        PyObject *parked = PyDict_GetItem(proxy_pointing_addr_dict, from_key); // borrowed
        if (parked)
        {
            if (!proxy_deferred_list) proxy_deferred_list = PyList_New(0);
            if (!proxy_deferred_list || PyList_Append(proxy_deferred_list, parked) < 0)
                PyErr_Clear();
        }
    }
    if (PyDict_DelItem(proxy_pointing_addr_dict, from_key) < 0)
    {
        PyErr_Clear();
    }
    PyErr_Restore(type, value, traceback);
    Py_DECREF(from_key);
}

static int proxy_gc_policy_id = -1;

void Proxy_InitGCPolicy()
{
    proxy_dict = PyDict_New();
    proxy_pointing_addr_dict = PyDict_New();
    proxy_gc_policy_id = __liballocs_register_gc_policy(proxy_addref, proxy_delref);
}

// Registers a proxy into the proxy dict to keep it alive.
void Proxy_Register_To_Dict(ProxyObject *proxy)
{
    // Keyed by base handle when p_ptr is an Alaska handle (relocation-stable), raw
    // base otherwise (
    PyObject *proxy_key = PyLong_FromVoidPtr(proxy->p_ptr);
    assert(!PyDict_Contains(proxy_dict, proxy_key));

    // proxy_dict must only have a 'weak' reference
    // So use a PyLong for storing it instead of storing the object
    PyObject *proxy_ptr = PyLong_FromVoidPtr(proxy);
    PyDict_SetItem(proxy_dict, proxy_key, proxy_ptr);
    Py_DECREF(proxy_ptr);
    Py_DECREF(proxy_key);

    // Python's interest in the pointee. A handle-backed object (from
    // Alaska-transformed code) gets one handle refcount owned by the live
    // proxy -- Alaska's own barrier cannot count this store, because the
    // extension is never compiled with the Alaska transform. A raw object
    // (e.g. malloc'd by pycallocs itself, or any object when running without
    // Alaska) falls back to the liballocs lifetime policy. ss_is_handle() and
    // ss_inc_refcount() compile to no-ops without SS_HAVE_ALASKA, so only the
    // policy branch exists there.
    if (ss_is_handle(proxy->p_ptr))
        ss_inc_refcount(proxy->p_ptr);
    else
        __liballocs_attach_lifetime_policy(proxy_gc_policy_id, ss_translate(proxy->p_ptr));
    // Start tracking the object with the cycle GC
    if (PyType_IS_GC(Py_TYPE(proxy))) PyObject_GC_Track(proxy);
}

// Does nothing if obj has not been registered before
// Call free on the underlying foreign object if we are the last lifetime policy
void Proxy_Unregister_From_Dict(ProxyObject *proxy)
{
    PyObject *proxy_key = PyLong_FromVoidPtr(proxy->p_ptr);
    PyObject *proxy_ptr = PyDict_GetItem(proxy_dict, proxy_key);
    if (proxy_ptr && PyLong_AsVoidPtr(proxy_ptr) == proxy)
    {
        // Stop tracking the object with the cycle GC
        if (PyType_IS_GC(Py_TYPE(proxy))) PyObject_GC_UnTrack(proxy);
        // Mirror of Proxy_Register_To_Dict: drop the handle reference we took
        // there, or detach the liballocs lifetime policy for raw objects.
        if (ss_is_handle(proxy->p_ptr))
            ss_dec_refcount(proxy->p_ptr);
        else
            __liballocs_detach_lifetime_policy(proxy_gc_policy_id, ss_translate(proxy->p_ptr));
        PyDict_DelItem(proxy_dict, proxy_key);
    }
    Py_DECREF(proxy_key);
}

// After a pointer `val` (taken from proxy `valobj`) has been written into the C
// slot at `dest` and __notify_ptr_write has run, make sure `valobj` is kept alive
// for as long as that slot references it -- even when liballocs cannot track the
// value's lifetime. The motivating case is libffi closures: their code pointer
// lives in mmap'd executable memory (from ffi_closure_alloc), which has no
// liballocs lifetime insert, so __notify_ptr_write's GC addref is a silent no-op.
// Without this, `cs.fun = closures.void.fun()(cb)` drops the only Python reference
// to the closure right after the store, freeing the trampoline that the struct
// still points at -> calling cs.fun() jumps into freed code (closure_lifetime).
//
// For liballocs-tracked values __notify_ptr_write already recorded a strong ref
// (an entry in proxy_pointing_addr_dict keyed by `dest`); we detect that and skip
// to avoid double-counting. We only retain registered base proxies, so ordinary
// C function pointers (never registered) are left untouched.
void Proxy_RetainStoredPtr(const void **dest, PyObject *valobj, const void *val)
{
    if (!val || !valobj || !PyObject_TypeCheck(valobj, &Proxy_Type)) return;
    ProxyObject *proxy = (ProxyObject *) valobj;
    if (proxy->p_ptr != val) return;

    PyObject *dest_key = PyLong_FromVoidPtr((void *) dest);
    int already_tracked = PyDict_Contains(proxy_pointing_addr_dict, dest_key);
    Py_DECREF(dest_key);
    if (already_tracked) return; // liballocs' GC policy is already managing this slot

    // Look up by the dict's key convention: base handle under Alaska, raw base
    // otherwise. `val` is the proxy's p_ptr (a handle, or a raw non-handle pointer
    // such as a closure trampoline -- for which the key is the pointer itself).
    if (lookup_proxydict((void *) val) == proxy) Proxy_AddRefTo(proxy, dest);
}

// Under Alaska, a foreign object adopted by pycallocs (e.g. a struct returned from a
// a C library) may already hold pointers to objects pycallocs proxies
// The barrier that would record those edges -- __notify_copy in the copy's
// own (fixture) code -- may be a silent no-op under Alaska, when liballocs cannot type
// a fixture-allocated chunk FIXME: But why?

//  __notify_copy bails with "no type information". The result is a use-after-free: when the original is cleared,
// the only references to the shared children go with it.

// Re-establish those references from a type the *caller* supplies (the address proxy's
// static pointee type, since liballocs itself can't type the chunk), walking it like
// notify_copy_for_type. `region` is the raw (translated) base. Idempotent: handle
// fields just take an Alaska refcount; raw fields skip slots already tracked, so a
// reference __notify_copy did manage to record is never doubled.
void Proxy_AdoptPtrFields(void *region, struct uniqtype *t)
{
    if (UNIQTYPE_IS_POINTER_TYPE(t))
    {
        const void **slot = (const void **) region;
        void *val = (void *) *slot;
        if (!val) return;
        // FIXME: What?
        // Handle pointees are kept alive by Alaska's own refcount;
        // We only need to recover the edges liballocs' lifetime policy would have tracked for raw pointees.
        if (ss_is_handle(val)) return;
        PyObject *k = PyLong_FromVoidPtr((void *) slot);
        int tracked = PyDict_Contains(proxy_pointing_addr_dict, k);
        Py_DECREF(k);
        if (tracked) return;
        // Only keep registered base proxies alive (mirrors Proxy_RetainStoredPtr).
        ProxyObject *p = lookup_proxydict(val);
        if (p) Proxy_AddRefTo(p, slot);
    }
    else if (UNIQTYPE_IS_ARRAY_TYPE(t))
    {
        struct uniqtype *et = UNIQTYPE_ARRAY_ELEMENT_TYPE(t);
        if (!et) return;
        unsigned long esz = UNIQTYPE_SIZE_IN_BYTES(et);
        unsigned n = UNIQTYPE_ARRAY_LENGTH(t);
        for (unsigned i = 0; i < n; ++i)
            Proxy_AdoptPtrFields((char *) region + i * esz, et);
    }
    else if (UNIQTYPE_IS_COMPOSITE_TYPE(t))
    {
        unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(t);
        for (unsigned i = 0; i < nmemb; ++i)
            Proxy_AdoptPtrFields((char *) region + t->related[i].un.memb.off,
                                   t->related[i].un.memb.ptr);
    }
}

// Create a base proxy for the foreign object at `addr`
// Return NULL or a new reference
ProxyObject *Proxy_GetOrCreateBase(void *addr)
{
    SS_ASSERT_PINNED(addr); // We expect the caller to have pinned the handle, otherwise the proxy may be invalid

    // Prevent recursion inside ourself
    static bool creating_base = false;
    if (creating_base) return NULL;

    
    struct allocator *allocator;
    const void *alloc_start;
    struct uniqtype *alloc_type;
    struct liballocs_err* err;
    
    void *raw = ss_translate(addr);
    err = __liballocs_get_alloc_info(raw, &allocator, &alloc_start, NULL,
            &alloc_type, NULL);
    if (err || !ALLOCATOR_HANDLE_LIFETIME_INSERT(allocator) || !alloc_type)
    {
        return NULL;
    }

    // liballocs answers in raw (translated) address space, but the proxy must hold the Alaska *handle* for its base --
    // Recover the base handle from the pointer results
    void *base_handle = (char *)addr - ((char *)raw - (char *)alloc_start);

    ProxyObject *proxy = lookup_proxydict(base_handle);
    if (proxy)
    {
        Py_INCREF(proxy);
        return proxy;
    }

    ForeignTypeObject *ftyp = ForeignType_GetOrCreate(alloc_type);
    assert(ftyp);
    if (!ftyp->ft_proxy_type)
    {
        Py_DECREF(ftyp);
        return NULL;
    }
    creating_base = true;
    proxy = (ProxyObject *) ftyp->ft_getfrom(base_handle, ftyp);
    creating_base = false;
    Py_DECREF(ftyp);
    assert(proxy);

    // Register the base proxy
    Proxy_Register_To_Dict(proxy);
    return proxy;
}

static void proxy_dealloc(ProxyObject *self)
{
    // Drop the Alaska reference this proxy took when it adopted its handle
    // A  *registered base* proxy took it in Proxy_Register_To_Dict and drops it in
    // Proxy_Unregister_From_Dict (below), so skip those here
    // Any *other* proxy that holds the handle 
    // is not seen by Unregister
    if (lookup_proxydict(self->p_ptr) != self)
        ss_dec_refcount(self->p_ptr);

    Proxy_Unregister_From_Dict(self);

    // Notify deletion of the reference
    proxy_delref(NULL, (const void **) &self->p_ptr);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static int proxy_is_gc(ProxyObject *self)
{
    // Should only be called for GC'd subtypes
    assert(PyType_IS_GC(Py_TYPE(self)));

    // We are GC iff we are a registered base proxy.
    PyObject *proxy_key = PyLong_FromVoidPtr(self->p_ptr);
    PyObject *proxy_ptr = PyDict_GetItem(proxy_dict, proxy_key);
    int res = proxy_ptr && PyLong_AsVoidPtr(proxy_ptr) == self;
    Py_DECREF(proxy_key);
    return res;
}

PyTypeObject Proxy_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.Proxy",
    .tp_doc = "Base type for foreign proxy objects",
    .tp_basicsize = sizeof(ProxyObject),
    .tp_itemsize = 0, // Size of the underlying object
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_dealloc = (destructor) proxy_dealloc,
    .tp_is_gc = (inquiry) proxy_is_gc,
};

#define PyObject_MaybeGC_New(TYPE, typobj) \
    (PyType_IS_GC(typobj) ? PyObject_GC_New(TYPE, typobj) : PyObject_New(TYPE, typobj))

PyObject *Proxy_GetFrom(void *data, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;

    ProxyObject *base_proxy = Proxy_GetOrCreateBase(data);
    if (base_proxy && Py_TYPE(base_proxy) == type->ft_proxy_type &&
            base_proxy->p_ptr == data)
    {
        // We can reuse the base_proxy as the result
        return (PyObject *) base_proxy;
    }

    ProxyObject *obj = PyObject_MaybeGC_New(ProxyObject, proxy_type);
    if (obj)
    {
        if (base_proxy) Proxy_AddRefTo(base_proxy, (const void **) &obj->p_ptr);
        obj->p_ptr = data;
    }
    Py_XDECREF(base_proxy);
    return (PyObject *) obj;
}

PyObject *Proxy_CopyFrom(void *src, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    ProxyObject *obj = PyObject_MaybeGC_New(ProxyObject, proxy_type);
    if (obj)
    {
        obj->p_ptr = totally_malloc(UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        __liballocs_set_alloc_type(obj->p_ptr, type->ft_type);
        // Should memcpy copy type information ?
        // Re-derive raw backing from the handle(s) for the libc memcpy (not Alaska-
        // instrumented); no-op when already raw / Alaska off.
        memcpy(obj->p_ptr, ss_translate(src), UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        Proxy_Register_To_Dict(obj);
        totally_free(obj->p_ptr);  // remains kept alive by proxy object
    }
    return (PyObject *) obj;
}

int Proxy_StoreInto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    if (PyObject_TypeCheck(obj, proxy_type))
    {
        ProxyObject *proxy = (ProxyObject *) obj;
        memcpy(ss_translate(dest), ss_translate(proxy->p_ptr), UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        return 0;
    }

    // We must try to convert the given object to the requested foreign
    // representation whenever possible
    PyObject *tmpobj = NULL;
    if (type->ft_constructor) tmpobj = type->ft_constructor(obj, NULL, type);
    if (tmpobj)
    {
        ProxyObject *proxy = (ProxyObject *) tmpobj;
        memcpy(ss_translate(dest), ss_translate(proxy->p_ptr), UNIQTYPE_SIZE_IN_BYTES(type->ft_type));
        Py_DECREF(tmpobj);
        return 0;
    }
    else
    {
        if (!PyErr_Occurred())
        {
            PyErr_Format(PyExc_TypeError, "expected value of type %s, got %s",
                proxy_type->tp_name, Py_TYPE(obj)->tp_name);
        }
        return -1;
    }
}

void *Proxy_GetDataPtr(PyObject *obj, ForeignTypeObject *type)
{
    PyTypeObject *proxy_type = type->ft_proxy_type;
    if (!PyObject_TypeCheck(obj, proxy_type)) return NULL;
    return ((ProxyObject *) obj)->p_ptr;
}

int Proxy_ClearRef(PyObject *object, void *arg)
{
    // This function should never be called, instrad Proxy_TraverseRef
    // should detect its address and remove the reference.
    abort();
}

int Proxy_TraverseRef(void *data, visitproc visit, void *arg, ForeignTypeObject* type)
{
    if (visit == Proxy_ClearRef)
    {
        // Drop the strong ref we hold for this pointer slot...
        proxy_delref(NULL, (const void **) data);
        // ...and clear the C pointer field itself. Otherwise, when this (garbage)
        // object's chunk is later freed, liballocs' __notify_free would follow the
        // still-populated field to delref the pointee -- but during cyclic-GC
        // teardown that pointee may already have been freed, so get_info() /
        // malloc_usable_size() on it segfaults (gc_struct_cycle). Nulling the slot
        // makes the later free a no-op for it; the delref above already accounts
        // for the dropped reference. Safe because the object is unreachable garbage.
        *((void **) data) = NULL;
        return 0;
    }
    // Find the base proxy referenced by the data pointer, if any
    PyObject *key = PyLong_FromVoidPtr(data);
    PyObject *target_base_proxy = PyDict_GetItem(proxy_pointing_addr_dict, key);
    Py_DECREF(key);
    Py_VISIT(target_base_proxy);
    return 0;
}

// Steals reference to proxytype
ForeignTypeObject *Proxy_NewType(const struct uniqtype *type, PyTypeObject *proxytype)
{
    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_proxy_type = proxytype;
    ftype->ft_constructor = NULL;
    ftype->ft_getfrom = Proxy_GetFrom;
    ftype->ft_copyfrom = Proxy_CopyFrom;
    ftype->ft_storeinto = Proxy_StoreInto;
    ftype->ft_getdataptr = Proxy_GetDataPtr;
    ftype->ft_traverse = NULL;
    return ftype;
}
