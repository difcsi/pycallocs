/* Proxy identity cache, lifetime bookkeeping and the generic proxy vtable:
 * the V8 port of vmextensions/lua/proxy.c. The CPython/Lua table machinery
 * becomes C++ maps of v8::Global handles:
 *   g_cache      - weak identity: address -> V8Proxy* (whose pr_self is a
 *                  weak Global; entries erased in the first weak pass)
 *   g_slot_refs  - strong: C slot address -> Global<Object>, so a C pointer
 *                  field keeps the proxy (and its chunk) alive; maintained
 *                  by the liballocs GC-policy callbacks
 *   g_defer_list - strong parking for proxies doomed inside a foreign call
 *
 * V8 weak-callback rules shape finalization: the FIRST pass may only Reset
 * the triggering handle (plus plain C work); V8 API use goes in the SECOND
 * pass (still no JavaScript). The lifetime-policy detach can free the chunk,
 * which re-enters lp_delref -- map + Global operations only, hence legal in
 * the second pass. Host-C reference cycles are uncollectable in this model;
 * release() is the documented escape hatch. */

#include "v8proxy.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

v8::Isolate *g_isolate = nullptr;
v8::Global<v8::Context> g_context;

static char g_brand_tag; /* address = the proxy brand */

static std::unordered_map<uintptr_t, V8Proxy *> g_cache;
static std::unordered_map<uintptr_t, v8::Global<v8::Object>> g_slot_refs;
static std::vector<v8::Global<v8::Object>> g_defer_list;
static int g_policy_id = -1;
static int g_defer_depth = 0;

static uintptr_t addr_key(const void *p) { return (uintptr_t)p; }

v8::MaybeLocal<v8::Value> Throwf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_isolate)
        g_isolate->ThrowException(v8::Exception::TypeError(
                v8::String::NewFromUtf8(g_isolate, buf).ToLocalChecked()));
    return v8::MaybeLocal<v8::Value>();
}

/* ---- liballocs GC-policy callbacks -------------------------------------- */
/* Fire synchronously on the JS thread from tracked pointer writes in fixture
 * code (never during a V8 GC, except through the second-pass detach chain --
 * both only use map and Global operations, which need no HandleScope). */

extern "C" void v8proxy_lp_addref(const void *target, const void **from)
{
    if (!g_isolate) return;
    auto it = g_cache.find(addr_key(target));
    if (it == g_cache.end()) return;
    g_slot_refs[addr_key(from)] =
        v8::Global<v8::Object>(g_isolate, it->second->pr_self);
}

extern "C" void v8proxy_lp_delref(const void *target, const void **from)
{
    (void)target; /* spurious calls from inexistent locations are expected */
    if (!g_isolate) return;
    auto it = g_slot_refs.find(addr_key(from));
    if (it == g_slot_refs.end()) return;
    /* Inside a foreign-call defer window, park the proxy this slot points at
     * so dropping the slot ref cannot free it mid-call (the callee may be
     * returning that very chunk: struct_global_swap). */
    if (g_defer_depth > 0)
        g_defer_list.emplace_back(std::move(it->second));
    g_slot_refs.erase(it);
}

void Proxy_BeginDeferFrees() { ++g_defer_depth; }

void Proxy_EndDeferFrees()
{
    if (--g_defer_depth == 0)
        g_defer_list.clear(); /* Global dtors Reset; anything unadopted dies at next GC */
}

void Proxy_Init(v8::Isolate *isolate, v8::Local<v8::Context> context)
{
    g_isolate = isolate;
    g_context.Reset(isolate, context);
    if (g_policy_id < 0)
        g_policy_id = __liballocs_register_gc_policy(v8proxy_lp_addref, v8proxy_lp_delref);
}

void Proxy_Teardown()
{
    g_slot_refs.clear();
    g_defer_list.clear();
    g_context.Reset();
    g_isolate = nullptr; /* late liballocs callbacks become no-ops */
}

/* ---- proxy objects ------------------------------------------------------- */

V8Proxy *Proxy_Untyped(v8::Local<v8::Value> v)
{
    if (v.IsEmpty() || !v->IsObject()) return nullptr;
    v8::Local<v8::Object> obj = v.As<v8::Object>();
    if (obj->InternalFieldCount() < 2) return nullptr;
    if (obj->GetAlignedPointerFromInternalField(0) != &g_brand_tag) return nullptr;
    return static_cast<V8Proxy *>(obj->GetAlignedPointerFromInternalField(1));
}

/* Weak finalization, two passes (see file header). */
static void proxy_weak_second_pass(const v8::WeakCallbackInfo<V8Proxy> &data)
{
    V8Proxy *p = data.GetParameter();
    /* V8 API legal here, JS forbidden. The detach may free the chunk ->
     * lp_delref for its tracked slots (map + Global ops only). */
    if (p->pr_registered && p->p_ptr)
    {
        if (ss_is_handle(p->p_ptr))
            ss_dec_refcount(p->p_ptr);
        else
            __liballocs_detach_lifetime_policy(g_policy_id, ss_translate(p->p_ptr));
    }
    delete p;
}

static void proxy_weak_first_pass(const v8::WeakCallbackInfo<V8Proxy> &data)
{
    V8Proxy *p = data.GetParameter();
    p->pr_self.Reset(); /* mandatory: reset the triggering handle */
    auto it = g_cache.find(addr_key(p->p_ptr));
    if (it != g_cache.end() && it->second == p) g_cache.erase(it);
    if (p->pr_registered)
        data.SetSecondPassCallback(proxy_weak_second_pass);
    else
        delete p;
}

v8::MaybeLocal<v8::Object> Proxy_NewObject(void *addr, V8ForeignType *ft)
{
    v8::Isolate *isolate = g_isolate;
    v8::Local<v8::Context> ctx = g_context.Get(isolate);
    if (ft->ft_template.IsEmpty())
    {
        Throwf("foreign type '%s' has no proxy representation",
                UNIQTYPE_NAME(ft->ft_type));
        return v8::MaybeLocal<v8::Object>();
    }
    v8::Local<v8::Object> obj;
    if (!ft->ft_template.Get(isolate)->InstanceTemplate()
            ->NewInstance(ctx).ToLocal(&obj))
        return v8::MaybeLocal<v8::Object>();

    V8Proxy *p = new V8Proxy();
    p->p_ptr = addr;
    p->pr_type = ft;
    p->ap_length = 0;
    p->pr_registered = false;
    obj->SetAlignedPointerInInternalField(0, &g_brand_tag);
    obj->SetAlignedPointerInInternalField(1, p);
    p->pr_self.Reset(isolate, obj);
    p->pr_self.SetWeak(p, proxy_weak_first_pass, v8::WeakCallbackType::kParameter);
    return obj;
}

void Proxy_Register(v8::Local<v8::Object> obj)
{
    V8Proxy *p = Proxy_Untyped(obj);
    if (!p || !p->p_ptr) return;
    g_cache[addr_key(p->p_ptr)] = p;
    if (ss_is_handle(p->p_ptr))
        ss_inc_refcount(p->p_ptr);
    else
        __liballocs_attach_lifetime_policy(g_policy_id, ss_translate(p->p_ptr));
    p->pr_registered = true;
}

v8::MaybeLocal<v8::Object> Proxy_GetOrCreateBase(void *addr)
{
    static bool creating_base = false;
    if (creating_base) return v8::MaybeLocal<v8::Object>();

    struct allocator *allocator = nullptr;
    const void *alloc_start = nullptr;
    struct uniqtype *alloc_type = nullptr;
    void *raw = ss_translate(addr);
    struct liballocs_err *err = __liballocs_get_alloc_info(raw, &allocator,
            &alloc_start, nullptr, &alloc_type, nullptr);
    if (err || !allocator || !ALLOCATOR_HANDLE_LIFETIME_INSERT(allocator) || !alloc_type)
        return v8::MaybeLocal<v8::Object>();

    void *base = (char *)addr - ((char *)raw - (char *)alloc_start);

    auto it = g_cache.find(addr_key(base));
    if (it != g_cache.end())
        return it->second->pr_self.Get(g_isolate);

    /* Base creation is best-effort (mirrors the Lua port): swallow anything
     * thrown in here. 13.6 has no Isolate::ClearPendingException; a TryCatch
     * that goes out of scope un-rethrown is the sanctioned swallow. */
    v8::TryCatch tc(g_isolate);
    V8ForeignType *ft = ForeignType_GetOrCreate(alloc_type);
    if (!ft) return v8::MaybeLocal<v8::Object>();
    if (ft->ft_template.IsEmpty()) return v8::MaybeLocal<v8::Object>();

    creating_base = true;
    v8::MaybeLocal<v8::Value> maybe = ft->ft_getfrom(base, ft);
    creating_base = false;
    v8::Local<v8::Value> v;
    if (!maybe.ToLocal(&v) || !v->IsObject())
        return v8::MaybeLocal<v8::Object>();
    v8::Local<v8::Object> obj = v.As<v8::Object>();
    Proxy_Register(obj);
    return obj;
}

/* ---- generic vtable ------------------------------------------------------ */

static v8::Local<v8::Private> base_anchor_key()
{
    return v8::Private::ForApi(g_isolate,
            v8::String::NewFromUtf8Literal(g_isolate, "v8proxy.base"));
}

v8::MaybeLocal<v8::Value> Proxy_GetFrom(void *data, V8ForeignType *ft)
{
    v8::Local<v8::Object> base;
    if (Proxy_GetOrCreateBase(data).ToLocal(&base))
    {
        V8Proxy *bp = Proxy_Untyped(base);
        if (bp && bp->pr_type == ft && bp->p_ptr == data)
            return base; /* the base proxy is the result */

        v8::Local<v8::Object> obj;
        if (!Proxy_NewObject(data, ft).ToLocal(&obj))
            return v8::MaybeLocal<v8::Value>();
        /* Derived/interior proxy: anchor the base (GC-traced private). */
        obj->SetPrivate(g_context.Get(g_isolate), base_anchor_key(), base).Check();
        return obj;
    }
    v8::Local<v8::Object> obj;
    if (!Proxy_NewObject(data, ft).ToLocal(&obj))
        return v8::MaybeLocal<v8::Value>();
    return obj;
}

v8::MaybeLocal<v8::Value> Proxy_CopyFrom(void *data, V8ForeignType *ft)
{
    size_t size = UNIQTYPE_SIZE_IN_BYTES(ft->ft_type);
    void *chunk = totally_malloc(size);
    if (!chunk) return Throwf("out of memory copying foreign value");
    __liballocs_set_alloc_type(ss_translate(chunk), ft->ft_type);
    memcpy(ss_translate(chunk), ss_translate(data), size);
    v8::Local<v8::Object> obj;
    if (!Proxy_NewObject(chunk, ft).ToLocal(&obj))
    {
        free(chunk);
        return v8::MaybeLocal<v8::Value>();
    }
    Proxy_Register(obj);
    totally_free(chunk); /* the proxy's policy reference keeps the chunk alive */
    return obj;
}

bool Proxy_TypeAcceptable(V8ForeignType *have, V8ForeignType *want)
{
    if (have == want) return true;
    const struct uniqtype *t = have->ft_type;
    while (UNIQTYPE_IS_COMPOSITE_TYPE(t) && UNIQTYPE_COMPOSITE_MEMBER_COUNT(t) > 0
            && t->related[0].un.memb.off == 0)
    {
        t = t->related[0].un.memb.ptr;
        if (t == want->ft_type) return true;
    }
    return false;
}

bool Proxy_StoreInto(v8::Local<v8::Value> v, void *dest, V8ForeignType *ft)
{
    V8Proxy *p = Proxy_Untyped(v);
    if (p && p->p_ptr && Proxy_TypeAcceptable(p->pr_type, ft))
    {
        memcpy(ss_translate(dest), ss_translate(p->p_ptr),
                UNIQTYPE_SIZE_IN_BYTES(ft->ft_type));
        return true;
    }

    /* Convert through the type's constructor (object literals etc.). */
    if (ft->ft_constructor)
    {
        v8::Local<v8::Value> arg = v;
        v8::Local<v8::Value> converted;
        if (!ft->ft_constructor(1, &arg, ft).ToLocal(&converted))
            return false;
        V8Proxy *tmp = Proxy_Untyped(converted);
        if (tmp && tmp->p_ptr)
        {
            memcpy(ss_translate(dest), ss_translate(tmp->p_ptr),
                    UNIQTYPE_SIZE_IN_BYTES(ft->ft_type));
            return true;
        }
    }

    Throwf("expected value of foreign type '%s'", UNIQTYPE_NAME(ft->ft_type));
    return false;
}

void *Proxy_GetDataPtr(v8::Local<v8::Value> v, V8ForeignType *ft)
{
    V8Proxy *p = Proxy_Untyped(v);
    if (!p || !p->p_ptr || !Proxy_TypeAcceptable(p->pr_type, ft)) return nullptr;
    return p->p_ptr;
}

void Proxy_SetupType(V8ForeignType *ft)
{
    ft->ft_constructor = nullptr;
    ft->ft_getfrom = Proxy_GetFrom;
    ft->ft_copyfrom = Proxy_CopyFrom;
    ft->ft_storeinto = Proxy_StoreInto;
    ft->ft_getdataptr = Proxy_GetDataPtr;
}

void Proxy_RetainStoredPtr(v8::Local<v8::Value> val, const void **dest, const void *val_ptr)
{
    if (!val_ptr) return;
    V8Proxy *p = Proxy_Untyped(val);
    if (!p || p->p_ptr != (void *)val_ptr) return;
    if (g_slot_refs.count(addr_key(dest))) return; /* GC policy already tracks it */
    auto it = g_cache.find(addr_key(val_ptr));
    if (it != g_cache.end() && it->second == p)
        g_slot_refs[addr_key(dest)] = v8::Global<v8::Object>(g_isolate, p->pr_self);
}

/* Recursive walk clearing tracked pointer slots (release() support). */
static void clear_tracked_slots(void *region, struct uniqtype *t)
{
    if (!t) return;
    if (UNIQTYPE_IS_POINTER_TYPE(t))
    {
        const void **slot = (const void **)region;
        if (*slot)
        {
            v8proxy_lp_delref(nullptr, slot);
            *slot = nullptr;
        }
    }
    else if (UNIQTYPE_IS_ARRAY_TYPE(t))
    {
        struct uniqtype *et = UNIQTYPE_ARRAY_ELEMENT_TYPE(t);
        if (!et) return;
        unsigned long esz = UNIQTYPE_SIZE_IN_BYTES(et);
        unsigned n = UNIQTYPE_ARRAY_LENGTH(t);
        for (unsigned i = 0; i < n; ++i)
            clear_tracked_slots((char *)region + i * esz, et);
    }
    else if (UNIQTYPE_IS_COMPOSITE_TYPE(t))
    {
        unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(t);
        for (unsigned i = 0; i < nmemb; ++i)
            clear_tracked_slots((char *)region + t->related[i].un.memb.off,
                    t->related[i].un.memb.ptr);
    }
}

void Proxy_ReleaseCallback(const v8::FunctionCallbackInfo<v8::Value> &args)
{
    V8Proxy *p = Proxy_Untyped(args.This());
    if (!p)
    {
        Throwf("release: not a foreign proxy");
        return;
    }
    if (!p->p_ptr) return; /* already released */

    void *raw = ss_translate(p->p_ptr);
    struct uniqtype *t = ss_alloc_get_type(p->p_ptr);
    if (!t) t = (struct uniqtype *)p->pr_type->ft_type;
    clear_tracked_slots(raw, t);

    if (p->pr_registered)
    {
        if (ss_is_handle(p->p_ptr))
            ss_dec_refcount(p->p_ptr);
        else
            __liballocs_detach_lifetime_policy(g_policy_id, ss_translate(p->p_ptr));
        auto it = g_cache.find(addr_key(p->p_ptr));
        if (it != g_cache.end() && it->second == p) g_cache.erase(it);
        p->pr_registered = false;
    }
    p->p_ptr = nullptr; /* poison: later access raises instead of dangling */
}

void Proxy_CreateTemplate(V8ForeignType *ft,
        void (*configure)(v8::Isolate *, v8::Local<v8::FunctionTemplate>, V8ForeignType *))
{
    v8::Isolate *isolate = g_isolate;
    v8::EscapableHandleScope hs(isolate);
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);
    const char *tyname = UNIQTYPE_NAME(ft->ft_type);
    if (tyname)
        tmpl->SetClassName(v8::String::NewFromUtf8(isolate, tyname).ToLocalChecked());
    tmpl->InstanceTemplate()->SetInternalFieldCount(2);
    tmpl->PrototypeTemplate()->Set(isolate, "release",
            v8::FunctionTemplate::New(isolate, Proxy_ReleaseCallback));
    if (configure) configure(isolate, tmpl, ft);
    ft->ft_template.Reset(isolate, tmpl);
}

void Proxy_AddInspect(v8::Isolate *isolate, v8::Local<v8::FunctionTemplate> tmpl,
        v8::FunctionCallback tostring)
{
    v8::Local<v8::FunctionTemplate> fn = v8::FunctionTemplate::New(isolate, tostring);
    tmpl->PrototypeTemplate()->Set(isolate, "toString", fn);
    /* console.log/util.inspect determinism: reuse toString */
    tmpl->PrototypeTemplate()->Set(
            v8::Symbol::For(isolate, v8::String::NewFromUtf8Literal(isolate,
                    "nodejs.util.inspect.custom")),
            fn);
}
