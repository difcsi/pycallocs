/* Pointer and array proxies: V8 port of the Lua address_proxy.c. One shared
 * proxy shape serves both kinds (V8AddressType; arrays alias the
 * pointer-to-element template), giving:
 *   p[i], p[i] = v   0-based element access (stores only for trivially
 *                    copiable element types); out-of-range reads -> undefined
 *   p.length         inferred or static element count (Array.from works)
 *   p.slice(i, j)    interior slice proxy (0-based, end-exclusive), anchored
 *   p.str()          char pointee bytes as a JS string (NUL-trimmed)
 *   p.field          direct field access when the pointee is a composite
 * Length inference for bare T*: liballocs bounds via minicrunch, then a
 * NUL-scan for char pointees capped by known bounds, then 1. */

#include "v8proxy.h"

#include <cstring>
#include <string>

using v8::Intercepted;
using v8::Local;
using v8::MaybeLocal;
using v8::Value;

/* liballocs lifetime hook (trapptrwrites CIL pass in fixture code). */
extern "C" void __notify_ptr_write(const void **dest, const void *val);

static Local<v8::Context> ctx() { return g_context.Get(g_isolate); }

static V8AddressType *addr_type(V8Proxy *p) { return (V8AddressType *)p->pr_type; }

static size_t elem_size(V8AddressType *at)
{
    return at->pointee_type ? UNIQTYPE_SIZE_IN_BYTES(at->pointee_type->ft_type) : 0;
}

/* ---- length inference ---------------------------------------------------- */

static void addrproxy_initlength(V8Proxy *p, V8AddressType *at)
{
    if (!at->pointee_type) { p->ap_length = 0; return; }
    const struct uniqtype *ptyp = at->pointee_type->ft_type;
    if (UNIQTYPE_SIZE_IN_BYTES(ptyp) == 0 || !UNIQTYPE_HAS_KNOWN_LENGTH(ptyp))
    {
        p->ap_length = 0;
        return;
    }

    void *ptr = ss_translate(p->p_ptr);
    Bounds bounds = __fetch_bounds_internal(ptr, ptyp);
    unsigned long esz = UNIQTYPE_SIZE_IN_BYTES(ptyp);

    if ((void *)bounds.base == ptr && bounds.size != 1)
        p->ap_length = bounds.size / esz;
    else if (ForeignBaseType_IsChar(ptyp) && esz == 1)
    {
        /* char pointer with unknown extent: C length = NUL terminator,
         * scan capped by whatever bound we have */
        unsigned long cap = 0;
        if (bounds.base && bounds.size > 1
                && (char *)ptr >= (char *)bounds.base
                && (char *)ptr < (char *)bounds.base + bounds.size)
            cap = bounds.size - (unsigned long)((char *)ptr - (char *)bounds.base);
        if (cap)
        {
            unsigned long n = 0;
            while (n < cap && ((const char *)ptr)[n] != '\0') ++n;
            p->ap_length = n;
        }
        else p->ap_length = 1;
    }
    else
        p->ap_length = 1; /* interior pointer to a single element */
}

/* ---- interceptors -------------------------------------------------------- */

static Intercepted indexed_get(uint32_t index,
        const v8::PropertyCallbackInfo<Value> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    V8AddressType *at = addr_type(p);
    if (!p->p_ptr) { Throwf("access to a released foreign proxy"); return Intercepted::kYes; }
    if (!at->pointee_type)
    { Throwf("pointee foreign datatype is not handled"); return Intercepted::kYes; }
    if (index >= p->ap_length)
        return Intercepted::kYes; /* undefined: JS-idiomatic bounds behaviour */
    void *item = (char *)ss_translate(p->p_ptr) + (size_t)index * elem_size(at);
    Local<Value> v;
    if (at->pointee_type->ft_getfrom(item, at->pointee_type).ToLocal(&v))
        info.GetReturnValue().Set(v);
    return Intercepted::kYes;
}

static Intercepted indexed_set(uint32_t index, Local<Value> value,
        const v8::PropertyCallbackInfo<void> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    V8AddressType *at = addr_type(p);
    if (!p->p_ptr) { Throwf("access to a released foreign proxy"); return Intercepted::kYes; }
    if (!at->pointee_type)
    { Throwf("pointee foreign datatype is not handled"); return Intercepted::kYes; }
    if (!ForeignType_IsTriviallyCopiable(at->pointee_type))
    { Throwf("cannot assign elements of this type (not trivially copiable)"); return Intercepted::kYes; }
    if (index >= p->ap_length)
    {
        g_isolate->ThrowException(v8::Exception::RangeError(
                v8::String::NewFromUtf8Literal(g_isolate, "index out of range")));
        return Intercepted::kYes;
    }
    void *item = (char *)ss_translate(p->p_ptr) + (size_t)index * elem_size(at);
    at->pointee_type->ft_storeinto(value, item, at->pointee_type);
    return Intercepted::kYes;
}

static Intercepted addr_named_get(Local<v8::Name> name,
        const v8::PropertyCallbackInfo<Value> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    V8AddressType *at = addr_type(p);
    v8::String::Utf8Value key(g_isolate, name);
    if (!*key) return Intercepted::kNo;

    if (strcmp(*key, "length") == 0)
    {
        info.GetReturnValue().Set(v8::Number::New(g_isolate, (double)p->ap_length));
        return Intercepted::kYes;
    }

    /* direct field access on a composite pointee */
    if (at->pointee_type && UNIQTYPE_IS_COMPOSITE_TYPE(at->pointee_type->ft_type))
    {
        V8ForeignType *fft;
        size_t off;
        if (CompositeProxy_FindField(at->pointee_type, *key, &fft, &off))
        {
            if (!p->p_ptr)
            { Throwf("access to a released foreign proxy"); return Intercepted::kYes; }
            if (!fft)
            { Throwf("field '%s': foreign datatype is not handled", *key); return Intercepted::kYes; }
            void *field = (char *)ss_translate(p->p_ptr) + off;
            Local<Value> v;
            if (fft->ft_getfrom(field, fft).ToLocal(&v))
                info.GetReturnValue().Set(v);
            return Intercepted::kYes;
        }
    }
    return Intercepted::kNo; /* methods resolve on the prototype */
}

static Intercepted addr_named_set(Local<v8::Name> name, Local<Value> value,
        const v8::PropertyCallbackInfo<void> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    V8AddressType *at = addr_type(p);
    v8::String::Utf8Value key(g_isolate, name);
    if (!*key) return Intercepted::kNo;

    if (at->pointee_type && UNIQTYPE_IS_COMPOSITE_TYPE(at->pointee_type->ft_type))
    {
        V8ForeignType *fft;
        size_t off;
        if (CompositeProxy_FindField(at->pointee_type, *key, &fft, &off))
        {
            if (!p->p_ptr)
            { Throwf("access to a released foreign proxy"); return Intercepted::kYes; }
            if (!fft)
            { Throwf("field '%s': foreign datatype is not handled", *key); return Intercepted::kYes; }
            if (!ForeignType_IsTriviallyCopiable(fft))
            { Throwf("cannot assign to field '%s'", *key); return Intercepted::kYes; }
            void *field = (char *)ss_translate(p->p_ptr) + off;
            fft->ft_storeinto(value, field, fft);
            return Intercepted::kYes;
        }
        Throwf("no field '%s' in foreign type '%s'", *key,
                UNIQTYPE_NAME(at->pointee_type->ft_type));
        return Intercepted::kYes;
    }
    return Intercepted::kNo;
}

/* ---- prototype methods --------------------------------------------------- */

static void addr_slice(const v8::FunctionCallbackInfo<Value> &args)
{
    V8Proxy *p = Proxy_Untyped(args.This());
    if (!p) { Throwf("slice: not an address proxy"); return; }
    if (!p->p_ptr) { Throwf("access to a released foreign proxy"); return; }
    V8AddressType *at = addr_type(p);
    int64_t i = 0, j = (int64_t)p->ap_length;
    if (args.Length() >= 1 && args[0]->IsNumber()) i = (int64_t)args[0].As<v8::Number>()->Value();
    if (args.Length() >= 2 && args[1]->IsNumber()) j = (int64_t)args[1].As<v8::Number>()->Value();
    if (i < 0) i = 0;
    if (j > (int64_t)p->ap_length) j = (int64_t)p->ap_length;
    if (j < i) j = i;

    Local<v8::Object> obj;
    if (!Proxy_NewObject((char *)p->p_ptr + i * elem_size(at), p->pr_type).ToLocal(&obj))
        return;
    Proxy_Untyped(obj)->ap_length = (size_t)(j - i);
    /* anchor the source proxy (and its chain) */
    obj->SetPrivate(ctx(), v8::Private::ForApi(g_isolate,
            v8::String::NewFromUtf8Literal(g_isolate, "v8proxy.base")),
            args.This()).Check();
    args.GetReturnValue().Set(obj);
}

static void addr_str(const v8::FunctionCallbackInfo<Value> &args)
{
    V8Proxy *p = Proxy_Untyped(args.This());
    if (!p) { Throwf("str: not an address proxy"); return; }
    if (!p->p_ptr) { Throwf("access to a released foreign proxy"); return; }
    V8AddressType *at = addr_type(p);
    if (!at->pointee_type || !ForeignBaseType_IsChar(at->pointee_type->ft_type))
    { Throwf("str: only available for char pointees"); return; }
    const char *s = (const char *)ss_translate(p->p_ptr);
    size_t n = p->ap_length;
    while (n > 0 && s[n - 1] == '\0') --n;
    args.GetReturnValue().Set(v8::String::NewFromUtf8(g_isolate, s,
            v8::NewStringType::kNormal, (int)n).ToLocalChecked());
}

static void addr_tostring(const v8::FunctionCallbackInfo<Value> &args)
{
    V8Proxy *p = Proxy_Untyped(args.This());
    if (!p) return;
    if (!p->p_ptr)
    {
        args.GetReturnValue().Set(
                v8::String::NewFromUtf8Literal(g_isolate, "<released foreign proxy>"));
        return;
    }
    V8AddressType *at = addr_type(p);

    std::string out;
    if (at->pointee_type && ForeignBaseType_IsChar(at->pointee_type->ft_type)
            && elem_size(at) == 1)
    {
        const char *s = (const char *)ss_translate(p->p_ptr);
        size_t n = p->ap_length;
        while (n > 0 && s[n - 1] == '\0') --n;
        out = "<'" + std::string(s, n) + "'>";
    }
    else
    {
        out = "<[";
        for (size_t i = 0; i < p->ap_length; ++i)
        {
            if (i > 0) out += ", ";
            void *item = (char *)ss_translate(p->p_ptr) + i * elem_size(at);
            Local<Value> v;
            if (!at->pointee_type->ft_getfrom(item, at->pointee_type).ToLocal(&v))
                return;
            Local<v8::String> s;
            if (v->ToDetailString(ctx()).ToLocal(&s))
            {
                v8::String::Utf8Value u(g_isolate, s);
                out += *u ? *u : "?";
            }
        }
        out += "]>";
    }
    args.GetReturnValue().Set(
            v8::String::NewFromUtf8(g_isolate, out.c_str()).ToLocalChecked());
}

static void addr_configure(v8::Isolate *isolate,
        Local<v8::FunctionTemplate> tmpl, V8ForeignType *ft)
{
    v8::IndexedPropertyHandlerConfiguration indexed(
            indexed_get, indexed_set, nullptr, nullptr, nullptr, Local<Value>());
    tmpl->InstanceTemplate()->SetHandler(indexed);
    v8::NamedPropertyHandlerConfiguration named(
            addr_named_get, addr_named_set, nullptr, nullptr, nullptr,
            Local<Value>(), v8::PropertyHandlerFlags::kOnlyInterceptStrings);
    tmpl->InstanceTemplate()->SetHandler(named);
    tmpl->PrototypeTemplate()->Set(isolate, "slice",
            v8::FunctionTemplate::New(isolate, addr_slice));
    tmpl->PrototypeTemplate()->Set(isolate, "str",
            v8::FunctionTemplate::New(isolate, addr_str));
    Proxy_AddInspect(isolate, tmpl, addr_tostring);
    (void)ft;
}

/* ---- vtable: ADDRESS ----------------------------------------------------- */

static MaybeLocal<Value> addrproxy_getfrom(void *data, V8ForeignType *ft)
{
    V8AddressType *at = (V8AddressType *)ft;
    void *ptr = *(void **)data; /* data is the address of a pointer */

    if (!ptr) return v8::Null(g_isolate).As<Value>();

    Local<v8::Object> base;
    if (Proxy_GetOrCreateBase(ptr).ToLocal(&base))
    {
        V8Proxy *bp = Proxy_Untyped(base);
        if (at->pointee_type
                && UNIQTYPE_KIND(at->pointee_type->ft_type) == VOID)
            return base;
        bool same_elem_class = bp
                && (UNIQTYPE_KIND(bp->pr_type->ft_type) == ADDRESS
                    || UNIQTYPE_KIND(bp->pr_type->ft_type) == ARRAY)
                && ((V8AddressType *)bp->pr_type)->pointee_type == at->pointee_type;
        if (bp && bp->p_ptr == ptr && at->pointee_type
                && (bp->pr_type == ft || same_elem_class
                    || Proxy_TypeAcceptable(bp->pr_type, at->pointee_type)))
            return base;

        Local<v8::Object> obj;
        if (!Proxy_NewObject(ptr, ft).ToLocal(&obj))
            return MaybeLocal<Value>();
        obj->SetPrivate(ctx(), v8::Private::ForApi(g_isolate,
                v8::String::NewFromUtf8Literal(g_isolate, "v8proxy.base")),
                base).Check();
        addrproxy_initlength(Proxy_Untyped(obj), at);
        return obj;
    }

    Local<v8::Object> obj;
    if (!Proxy_NewObject(ptr, ft).ToLocal(&obj))
        return MaybeLocal<Value>();
    addrproxy_initlength(Proxy_Untyped(obj), at);
    return obj;
}

static bool addrproxy_typecheck(Local<Value> v, V8AddressType *at)
{
    V8Proxy *p = Proxy_Untyped(v);
    if (!p || !p->p_ptr) return false;

    /* same element class: pointer/array/slice over the same element type */
    const struct uniqtype *k = p->pr_type->ft_type;
    if ((UNIQTYPE_KIND(k) == ADDRESS || UNIQTYPE_KIND(k) == ARRAY)
            && ((V8AddressType *)p->pr_type)->pointee_type == at->pointee_type
            && at->pointee_type)
        return true;

    if (!at->pointee_type) return false;
    if (UNIQTYPE_KIND(at->pointee_type->ft_type) == VOID) return true;
    if (Proxy_TypeAcceptable(p->pr_type, at->pointee_type)) return true;
    return false;
}

static bool addrproxy_storeinto(Local<Value> v, void *dest, V8ForeignType *ft)
{
    V8AddressType *at = (V8AddressType *)ft;

    if (v->IsNullOrUndefined())
    {
        ss_dec_refcount(*(void **)dest);
        __notify_ptr_write((const void **)dest, nullptr);
        *(void **)dest = nullptr;
        return true;
    }

    if (!addrproxy_typecheck(v, at))
    {
        Throwf("expected reference to object of type %s",
                at->pointee_type ? UNIQTYPE_NAME(at->pointee_type->ft_type) : "?");
        return false;
    }

    V8Proxy *p = Proxy_Untyped(v);
    void *val = p->p_ptr;
    /* exact barrier ordering of the Lua/Python ports */
    ss_dec_refcount(*(void **)dest);
    ss_inc_refcount(val);
    __notify_ptr_write((const void **)dest, val);
    *(void **)dest = val;
    if (!ss_is_handle(val))
        Proxy_RetainStoredPtr(v, (const void **)dest, val);
    return true;
}

static void *addrproxy_getdataptr(Local<Value> v, V8ForeignType *ft)
{
    V8AddressType *at = (V8AddressType *)ft;

    if (v->IsNullOrUndefined())
    {
        static void *np = nullptr;
        return &np;
    }
    if (addrproxy_typecheck(v, at))
        return &Proxy_Untyped(v)->p_ptr;

    /* one-element pointer cell passed where T* is expected: hand over the
     * pointer stored IN the cell (out-param/handle idiom) */
    V8Proxy *p = Proxy_Untyped(v);
    if (p && p->p_ptr && p->ap_length == 1)
    {
        const struct uniqtype *k = p->pr_type->ft_type;
        if (UNIQTYPE_KIND(k) == ADDRESS || UNIQTYPE_KIND(k) == ARRAY)
        {
            V8AddressType *pat = (V8AddressType *)p->pr_type;
            if (pat->pointee_type == &at->ft
                    || (pat->pointee_type && pat->pointee_type->ft_type == ft->ft_type))
                return p->p_ptr;
        }
    }
    return nullptr;
}

V8ForeignType *AddressProxy_NewType(const struct uniqtype *type)
{
    const struct uniqtype *pointee = type->related[0].un.t.ptr;
    if (!pointee)
    {
        Throwf("pointer type with no pointee");
        return nullptr;
    }
    V8AddressType *at = new V8AddressType();
    at->ft.ft_type = type;
    at->pointee_type = nullptr;
    at->shares_template = false;
    at->ft.ft_getfrom = addrproxy_getfrom;
    at->ft.ft_copyfrom = addrproxy_getfrom;
    at->ft.ft_storeinto = addrproxy_storeinto;
    at->ft.ft_getdataptr = addrproxy_getdataptr;
    at->ft.ft_constructor = ForeignType_CellCtor; /* T_ptr(): T** out-param cell */
    return &at->ft;
}

void AddressProxy_InitType(V8ForeignType *ft)
{
    V8AddressType *at = (V8AddressType *)ft;
    const struct uniqtype *pointee = ft->ft_type->related[0].un.t.ptr;
    {
        v8::TryCatch tc(g_isolate); /* swallow: degraded (opaque) class */
        at->pointee_type = ForeignType_GetOrCreate(pointee);
    }
    Proxy_CreateTemplate(ft, addr_configure);
}

/* ---- vtable: ARRAY ------------------------------------------------------- */

static bool array_fill(V8Proxy *p, Local<v8::Object> init /* empty => zero */)
{
    V8AddressType *at = addr_type(p);
    size_t esz = elem_size(at);
    void *raw = ss_translate(p->p_ptr);

    if (init.IsEmpty())
    {
        memset(raw, 0, p->ap_length * esz);
        return true;
    }
    uint32_t arglen = 0;
    {
        Local<Value> lv;
        if (!init->Get(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "length"))
                .ToLocal(&lv) || !lv->IsNumber())
        {
            Throwf("array initialiser must be array-like");
            return false;
        }
        arglen = (uint32_t)lv.As<v8::Number>()->Value();
    }
    if (arglen > p->ap_length)
    {
        Throwf("sequence too long to be stored at this address (only %zu items)",
                p->ap_length);
        return false;
    }
    for (uint32_t i = 0; i < arglen; ++i)
    {
        Local<Value> v;
        if (!init->Get(ctx(), i).ToLocal(&v)) return false;
        if (!at->pointee_type->ft_storeinto(v, (char *)raw + (size_t)i * esz,
                at->pointee_type))
            return false;
    }
    if (arglen < p->ap_length)
        memset((char *)raw + (size_t)arglen * esz, 0, (p->ap_length - arglen) * esz);
    return true;
}

static MaybeLocal<Value> arrayproxy_ctor(int argc, const Local<Value> *argv,
        V8ForeignType *ft)
{
    V8AddressType *at = (V8AddressType *)ft;
    if (!at->pointee_type)
        return Throwf("array element foreign datatype is not handled");
    if (argc != 1)
        return Throwf("array constructor takes exactly one argument (length or array)");

    int64_t len;
    Local<v8::Object> init;
    if (argv[0]->IsArray() || (argv[0]->IsObject() && !Proxy_Untyped(argv[0])))
    {
        init = argv[0].As<v8::Object>();
        Local<Value> lv;
        if (!init->Get(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "length"))
                .ToLocal(&lv) || !lv->IsNumber())
            return Throwf("array constructor argument must be array-like or a length");
        len = (int64_t)lv.As<v8::Number>()->Value();
    }
    else if (argv[0]->IsNumber())
        len = (int64_t)argv[0].As<v8::Number>()->Value();
    else
        return Throwf("array constructor argument must be array-like or a length");
    if (len < 0) return Throwf("array length cannot be negative");

    size_t esz = elem_size(at);
    bool is_char = ForeignBaseType_IsChar(at->pointee_type->ft_type);
    void *chunk = totally_malloc((len + (is_char ? 1 : 0)) * esz);
    if (!chunk) return Throwf("out of memory constructing foreign array");
    if (is_char) memset((char *)chunk + len * esz, 0, esz);
    __liballocs_set_alloc_type(ss_translate(chunk),
            __liballocs_get_or_create_array_type(
                (struct uniqtype *)at->pointee_type->ft_type, (unsigned)len));

    Local<v8::Object> obj;
    if (!Proxy_NewObject(chunk, ft).ToLocal(&obj))
    {
        free(chunk);
        return MaybeLocal<Value>();
    }
    V8Proxy *p = Proxy_Untyped(obj);
    p->ap_length = (size_t)len;
    Proxy_Register(obj);
    totally_free(chunk);

    if (!array_fill(p, init)) return MaybeLocal<Value>();
    return obj;
}

static MaybeLocal<Value> arrayproxy_getfrom(void *data, V8ForeignType *ft)
{
    Local<Value> v;
    if (!Proxy_GetFrom(data, ft).ToLocal(&v)) return MaybeLocal<Value>();
    V8Proxy *p = Proxy_Untyped(v);
    if (p)
    {
        if (UNIQTYPE_HAS_KNOWN_LENGTH(ft->ft_type))
            p->ap_length = UNIQTYPE_ARRAY_LENGTH(ft->ft_type);
        else
            addrproxy_initlength(p, (V8AddressType *)ft);
    }
    return v;
}

static bool arrayproxy_storeinto(Local<Value>, void *, V8ForeignType *)
{
    Throwf("cannot copy full arrays (for now)");
    return false;
}

V8ForeignType *ArrayProxy_NewType(const struct uniqtype *type)
{
    V8AddressType *at = new V8AddressType();
    at->ft.ft_type = type;
    at->pointee_type = nullptr;
    at->shares_template = true;
    at->ft.ft_constructor = arrayproxy_ctor;
    at->ft.ft_getfrom = arrayproxy_getfrom;
    at->ft.ft_copyfrom = nullptr;
    at->ft.ft_storeinto = arrayproxy_storeinto;
    at->ft.ft_getdataptr = Proxy_GetDataPtr;
    return &at->ft;
}

void ArrayProxy_InitType(V8ForeignType *ft)
{
    V8AddressType *at = (V8AddressType *)ft;
    const struct uniqtype *elem = ft->ft_type->related[0].un.t.ptr;
    if (!elem) return;

    /* Arrays share the pointer-to-element proxy class (what makes one-element
     * cells usable as out-params). */
    const struct uniqtype *addr_uniqtype = __liballocs_get_or_create_address_type(elem);
    V8ForeignType *addr_ft = nullptr;
    {
        v8::TryCatch tc(g_isolate); /* swallow on failure */
        if (addr_uniqtype) addr_ft = ForeignType_GetOrCreate(addr_uniqtype);
    }
    if (!addr_ft)
    {
        at->pointee_type = nullptr;
        return;
    }
    at->pointee_type = ((V8AddressType *)addr_ft)->pointee_type;
    at->ft.ft_template.Reset(g_isolate, addr_ft->ft_template.Get(g_isolate));
}
