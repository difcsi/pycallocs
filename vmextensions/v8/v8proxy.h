#ifndef V8PROXY_H
#define V8PROXY_H

/* v8proxy: liballocs-driven C interop for JavaScript as a Node.js native
 * addon, mirroring vmextensions/cpython (ForeignTypeObject over PyObject*)
 * and vmextensions/lua (LuaForeignType over the Lua stack). Foreign data is
 * described by V8ForeignType, a per-uniqtype vtable that is the whole
 * marshalling protocol; proxies are JS objects carrying a malloc'd V8Proxy
 * record in an internal field.
 *
 * Error convention: no C++ exceptions (-fno-exceptions). A failing vtable
 * call returns an empty MaybeLocal / false with a V8 exception pending on
 * g_isolate. Only outermost FunctionCallbacks let it propagate to JS; ffi
 * trampolines and weak callbacks bracket with v8::TryCatch / avoid V8 APIs
 * per the weak-callback rules (see proxy.cc).
 */

#include <cstddef>
#include <cstdint>

#include <v8.h>

extern "C" {
#include <liballocs.h>
#include <minicrunch.h>
#include <handle_query.h>
#include <ffitypes.h>
#include <dlsym_iter.h>
}

/* mirror of include/foreign_library.h / luaproxy.h */
#define totally_malloc(x) malloc(x)
#define totally_free(ptr) __liballocs_detach_manual_dealloc_policy((ptr))

struct V8ForeignType;

/* Malloc'd payload owned by the proxy JS object (internal field 1; field 0
 * is the brand tag). Outlives the JS object into the weak callbacks. */
struct V8Proxy {
    void *p_ptr;                 /* underlying addresslike; NULL after release */
    V8ForeignType *pr_type;
    size_t ap_length;            /* element count for address/array proxies */
    /* True iff this proxy is the registered base proxy for p_ptr (owns a
     * lifetime-policy attach that finalization must release). Lives HERE:
     * the weak identity cache cannot answer this inside a finalizer. */
    bool pr_registered;
    v8::Global<v8::Object> pr_self; /* weak self-handle; the SetWeak target */
};

struct V8ForeignType {
    const struct uniqtype *ft_type;
    /* Proxy class template for this type; empty for scalar (by-value only)
     * kinds. Instances get 2 internal fields (brand tag, V8Proxy*). */
    v8::Global<v8::FunctionTemplate> ft_template;

    /* argc/argv rather than FunctionCallbackInfo so conversion fallbacks can
     * synthesise single-argument constructor calls. */
    v8::MaybeLocal<v8::Value> (*ft_constructor)(int argc,
            const v8::Local<v8::Value> *argv, V8ForeignType *ft);
    v8::MaybeLocal<v8::Value> (*ft_getfrom)(void *data, V8ForeignType *ft);
    v8::MaybeLocal<v8::Value> (*ft_copyfrom)(void *data, V8ForeignType *ft);
    bool (*ft_storeinto)(v8::Local<v8::Value> v, void *dest, V8ForeignType *ft);
    /* Raw pointer to existing data of this type held by v, or NULL; never
     * throws (libffi zero-copy argument path). */
    void *(*ft_getdataptr)(v8::Local<v8::Value> v, V8ForeignType *ft);

    V8ForeignType() : ft_type(nullptr), ft_constructor(nullptr),
        ft_getfrom(nullptr), ft_copyfrom(nullptr), ft_storeinto(nullptr),
        ft_getdataptr(nullptr) {}
};

/* Kind-specific extensions (aggregation mirrors the Lua port). */
struct V8FieldInfo {
    const char *name;      /* borrowed from the uniqtype metadata */
    V8ForeignType *type;   /* NULL when the field's type is unhandled */
    size_t offset;
};

struct V8CompositeType {
    V8ForeignType ft;
    unsigned nfields;
    V8FieldInfo *fields;   /* malloc'd; types immortal */
};

struct V8AddressType {
    V8ForeignType ft;
    V8ForeignType *pointee_type; /* element type for ARRAY kinds too */
    bool shares_template;        /* ARRAY aliasing the pointer template */
};

struct V8FunctionType {
    V8ForeignType ft;
    ffi_cif *ff_cif;             /* lazily prepared */
    V8ForeignType **ff_argtypes;
    V8ForeignType *ff_rettype;
    v8::Global<v8::FunctionTemplate> ff_closure_template;
};

/* Single-VM-instance discipline: liballocs GC-policy callbacks carry no
 * context, so v8proxy serves exactly one Isolate (node's main isolate).
 * NULL after environment teardown; every callback guards on it. */
extern v8::Isolate *g_isolate;
extern v8::Global<v8::Context> g_context;

/* Throw helper: sets a pending TypeError on g_isolate; returns empty. */
v8::MaybeLocal<v8::Value> Throwf(const char *fmt, ...);

/* ---- proxy.cc ---- */
void Proxy_Init(v8::Isolate *isolate, v8::Local<v8::Context> context);
void Proxy_Teardown();
/* NULL if v is not a v8proxy object (brand check on internal field 0). */
V8Proxy *Proxy_Untyped(v8::Local<v8::Value> v);
/* Create the proxy FunctionTemplate for ft. `configure` may add interceptors
 * and prototype methods; the common brand/internal-field/weakness setup is
 * done here. */
void Proxy_CreateTemplate(V8ForeignType *ft,
        void (*configure)(v8::Isolate *, v8::Local<v8::FunctionTemplate>, V8ForeignType *));
/* Instantiate an unregistered proxy of ft over addr (sets up payload +
 * SetWeak). Empty on failure (pending exception). */
v8::MaybeLocal<v8::Object> Proxy_NewObject(void *addr, V8ForeignType *ft);
/* Register obj as the base proxy for its address: identity-cache insert +
 * liballocs lifetime-policy attach. */
void Proxy_Register(v8::Local<v8::Object> obj);
/* The registered base proxy for the allocation containing addr, creating and
 * registering it if liballocs can type it. Empty if none can exist. */
v8::MaybeLocal<v8::Object> Proxy_GetOrCreateBase(void *addr);
/* Generic vtable for proxy-kinded types. */
v8::MaybeLocal<v8::Value> Proxy_GetFrom(void *data, V8ForeignType *ft);
v8::MaybeLocal<v8::Value> Proxy_CopyFrom(void *data, V8ForeignType *ft);
bool Proxy_StoreInto(v8::Local<v8::Value> v, void *dest, V8ForeignType *ft);
void *Proxy_GetDataPtr(v8::Local<v8::Value> v, V8ForeignType *ft);
void Proxy_SetupType(V8ForeignType *ft);
bool Proxy_TypeAcceptable(V8ForeignType *have, V8ForeignType *want);
/* Defer-free window bracketing foreign calls (struct_global_swap). */
void Proxy_BeginDeferFrees();
void Proxy_EndDeferFrees();
/* After a pointer store, keep the stored-from proxy alive while the slot
 * references it (values liballocs cannot track, e.g. ffi trampolines). */
void Proxy_RetainStoredPtr(v8::Local<v8::Value> val, const void **dest, const void *val_ptr);
/* proxy.release(): clears tracked pointer slots, detaches the policy,
 * poisons the proxy -- the manual escape hatch for host-C cycles. */
void Proxy_ReleaseCallback(const v8::FunctionCallbackInfo<v8::Value> &args);
/* Common prototype toString/inspect wiring. */
void Proxy_AddInspect(v8::Isolate *isolate, v8::Local<v8::FunctionTemplate> tmpl,
        v8::FunctionCallback tostring);

/* ---- foreign_type.cc ---- */
V8ForeignType *ForeignType_GetOrCreate(const struct uniqtype *type);
void ForeignType_Teardown();
bool ForeignType_IsTriviallyCopiable(const V8ForeignType *ft);
/* The (cached) JS wrapper object for a foreign type (callable: T(...)). */
v8::MaybeLocal<v8::Object> ForeignType_Wrapper(V8ForeignType *ft);
/* One-element out-parameter cell of elem_ft, optionally initialised from
 * init (empty handle => zero/NULL-init). */
v8::MaybeLocal<v8::Value> ForeignType_NewCell(V8ForeignType *elem_ft,
        v8::Local<v8::Value> init);
v8::MaybeLocal<v8::Value> ForeignType_CellCtor(int argc,
        const v8::Local<v8::Value> *argv, V8ForeignType *ft);

/* ---- foreign_basetype.cc ---- */
bool ForeignBaseType_Init(V8ForeignType *ft);  /* false: unsupported (no throw) */
bool ForeignEnumType_Init(V8ForeignType *ft);
bool ForeignBaseType_IsChar(const struct uniqtype *type);

/* ---- composite_proxy.cc ---- */
V8ForeignType *CompositeProxy_NewType(const struct uniqtype *type);
void CompositeProxy_InitType(V8ForeignType *ft);
bool CompositeProxy_FindField(V8ForeignType *composite_ft, const char *key,
        V8ForeignType **out_ft, size_t *out_off);

/* ---- address_proxy.cc ---- */
V8ForeignType *AddressProxy_NewType(const struct uniqtype *type);
void AddressProxy_InitType(V8ForeignType *ft);
V8ForeignType *ArrayProxy_NewType(const struct uniqtype *type);
void ArrayProxy_InitType(V8ForeignType *ft);

/* ---- function_proxy.cc ---- */
V8ForeignType *FunctionProxy_NewType(const struct uniqtype *type);
void FunctionProxy_InitType(V8ForeignType *ft);

/* ---- library_loader.cc ---- */
void LibraryLoader_Load(const v8::FunctionCallbackInfo<v8::Value> &args);

#endif /* V8PROXY_H */
