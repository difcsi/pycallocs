/* Struct/union proxies: V8 port of the Lua composite_proxy.c. Field access
 * goes through named property interceptors (the __index/__newindex analogue);
 * the enumerator/query callbacks make Object.keys and `in` work over the C
 * struct's fields. The offset-0 first-member chain is walked on lookup miss
 * (C "struct inheritance"). */

#include "v8proxy.h"

#include <cstring>
#include <string>
#include <vector>

using v8::Intercepted;
using v8::Local;
using v8::MaybeLocal;
using v8::Value;

static Local<v8::Context> ctx() { return g_context.Get(g_isolate); }

bool CompositeProxy_FindField(V8ForeignType *composite_ft, const char *key,
        V8ForeignType **out_ft, size_t *out_off)
{
    V8CompositeType *ct = (V8CompositeType *)composite_ft;
    size_t extra_off = 0;
    while (ct)
    {
        for (unsigned i = 0; i < ct->nfields; ++i)
            if (ct->fields[i].name && strcmp(ct->fields[i].name, key) == 0)
            {
                *out_ft = ct->fields[i].type;
                *out_off = extra_off + ct->fields[i].offset;
                return true;
            }
        /* first-member "inheritance" */
        if (ct->nfields > 0 && ct->fields[0].offset == 0 && ct->fields[0].type
                && UNIQTYPE_IS_COMPOSITE_TYPE(ct->fields[0].type->ft_type))
            ct = (V8CompositeType *)ct->fields[0].type;
        else
            ct = nullptr;
    }
    return false;
}

static Intercepted composite_get(Local<v8::Name> name,
        const v8::PropertyCallbackInfo<Value> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    v8::String::Utf8Value key(g_isolate, name);
    if (!*key) return Intercepted::kNo;

    V8ForeignType *fft;
    size_t off;
    if (!CompositeProxy_FindField(p->pr_type, *key, &fft, &off))
        return Intercepted::kNo; /* prototype resolves methods; unknown -> undefined */
    if (!p->p_ptr)
    {
        Throwf("access to a released foreign proxy");
        return Intercepted::kYes;
    }
    if (!fft)
    {
        Throwf("field '%s': foreign datatype is not handled", *key);
        return Intercepted::kYes;
    }
    void *field = (char *)ss_translate(p->p_ptr) + off;
    Local<Value> v;
    if (fft->ft_getfrom(field, fft).ToLocal(&v))
        info.GetReturnValue().Set(v);
    return Intercepted::kYes;
}

static Intercepted composite_set(Local<v8::Name> name, Local<Value> value,
        const v8::PropertyCallbackInfo<void> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    v8::String::Utf8Value key(g_isolate, name);
    if (!*key) return Intercepted::kNo;

    V8ForeignType *fft;
    size_t off;
    if (!CompositeProxy_FindField(p->pr_type, *key, &fft, &off))
    {
        Throwf("no field '%s' in foreign type '%s'", *key,
                UNIQTYPE_NAME(p->pr_type->ft_type));
        return Intercepted::kYes;
    }
    if (!p->p_ptr)
    {
        Throwf("access to a released foreign proxy");
        return Intercepted::kYes;
    }
    if (!fft)
    {
        Throwf("field '%s': foreign datatype is not handled", *key);
        return Intercepted::kYes;
    }
    if (!ForeignType_IsTriviallyCopiable(fft))
    {
        Throwf("cannot assign to field '%s' (not trivially copiable)", *key);
        return Intercepted::kYes;
    }
    void *field = (char *)ss_translate(p->p_ptr) + off;
    fft->ft_storeinto(value, field, fft);
    return Intercepted::kYes;
}

static Intercepted composite_query(Local<v8::Name> name,
        const v8::PropertyCallbackInfo<v8::Integer> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return Intercepted::kNo;
    v8::String::Utf8Value key(g_isolate, name);
    if (!*key) return Intercepted::kNo;
    V8ForeignType *fft;
    size_t off;
    if (!CompositeProxy_FindField(p->pr_type, *key, &fft, &off))
        return Intercepted::kNo;
    info.GetReturnValue().Set(v8::Integer::New(g_isolate, v8::PropertyAttribute::None));
    return Intercepted::kYes;
}

static void composite_enumerate(const v8::PropertyCallbackInfo<v8::Array> &info)
{
    V8Proxy *p = Proxy_Untyped(info.This());
    if (!p) return;
    V8CompositeType *ct = (V8CompositeType *)p->pr_type;
    std::vector<Local<Value>> names;
    for (unsigned i = 0; i < ct->nfields; ++i)
        if (ct->fields[i].name && ct->fields[i].type)
            names.push_back(v8::String::NewFromUtf8(g_isolate,
                    ct->fields[i].name).ToLocalChecked());
    info.GetReturnValue().Set(
            v8::Array::New(g_isolate, names.data(), names.size()));
}

/* toString: "(name){f: v, ...}" -- same shape as the Lua port */
static void composite_tostring(const v8::FunctionCallbackInfo<Value> &args)
{
    V8Proxy *p = Proxy_Untyped(args.This());
    if (!p) return;
    if (!p->p_ptr)
    {
        args.GetReturnValue().Set(
                v8::String::NewFromUtf8Literal(g_isolate, "<released foreign proxy>"));
        return;
    }
    V8CompositeType *ct = (V8CompositeType *)p->pr_type;
    std::string out = "(";
    out += UNIQTYPE_NAME(ct->ft.ft_type);
    out += "){";
    bool first = true;
    for (unsigned i = 0; i < ct->nfields; ++i)
    {
        V8FieldInfo *f = &ct->fields[i];
        if (!f->type) continue;
        if (!first) out += ", ";
        first = false;
        out += f->name;
        out += ": ";
        void *field = (char *)ss_translate(p->p_ptr) + f->offset;
        Local<Value> v;
        if (f->type->ft_getfrom(field, f->type).ToLocal(&v))
        {
            Local<v8::String> s;
            /* our proxies stringify through their prototype toString */
            if (v->ToDetailString(ctx()).ToLocal(&s))
            {
                v8::String::Utf8Value u(g_isolate, s);
                out += *u ? *u : "?";
            }
        }
        else return; /* pending exception propagates */
    }
    out += "}";
    args.GetReturnValue().Set(
            v8::String::NewFromUtf8(g_isolate, out.c_str()).ToLocalChecked());
}

/* Constructor: T() zero-init, T(proxy) copy, T({f: v}) object literal,
 * T(v1, v2, ...) positional. */
static MaybeLocal<Value> composite_ctor(int argc, const Local<Value> *argv,
        V8ForeignType *ft)
{
    V8CompositeType *ct = (V8CompositeType *)ft;
    size_t size = UNIQTYPE_SIZE_IN_BYTES(ft->ft_type);

    void *chunk = totally_malloc(size);
    if (!chunk) return Throwf("out of memory constructing foreign struct");
    __liballocs_set_alloc_type(ss_translate(chunk), ft->ft_type);
    Local<v8::Object> obj;
    if (!Proxy_NewObject(chunk, ft).ToLocal(&obj))
    {
        free(chunk);
        return MaybeLocal<Value>();
    }
    Proxy_Register(obj);
    totally_free(chunk); /* proxy's policy reference keeps the chunk alive */

    void *raw = ss_translate(Proxy_Untyped(obj)->p_ptr);

    if (argc == 0)
    {
        memset(raw, 0, size);
        return obj;
    }

    if (argc == 1)
    {
        /* copy construction from a same-type (or derived) proxy */
        V8Proxy *other = Proxy_Untyped(argv[0]);
        if (other && other->pr_type == ft)
        {
            if (other->p_ptr == Proxy_Untyped(obj)->p_ptr)
                return Throwf("trying to copy inside yourself");
            memcpy(raw, ss_translate(other->p_ptr), size);
            return obj;
        }

        /* object literal: initialise fields by name (absent -> zero) */
        if (!other && argv[0]->IsObject())
        {
            Local<v8::Object> init = argv[0].As<v8::Object>();
            for (unsigned i = 0; i < ct->nfields; ++i)
            {
                V8FieldInfo *f = &ct->fields[i];
                if (!f->type) continue;
                Local<Value> v;
                if (!init->Get(ctx(), v8::String::NewFromUtf8(g_isolate,
                        f->name).ToLocalChecked()).ToLocal(&v))
                    return MaybeLocal<Value>();
                if (!v->IsUndefined())
                {
                    if (!f->type->ft_storeinto(v, (char *)raw + f->offset, f->type))
                        return MaybeLocal<Value>();
                }
                else
                    memset((char *)raw + f->offset, 0,
                            UNIQTYPE_SIZE_IN_BYTES(f->type->ft_type));
            }
            return obj;
        }
    }

    /* positional: one argument per field in declaration order */
    if ((unsigned)argc > ct->nfields)
        return Throwf("too many arguments (foreign type '%s' has %u fields)",
                UNIQTYPE_NAME(ft->ft_type), ct->nfields);
    for (unsigned i = 0; i < ct->nfields; ++i)
    {
        V8FieldInfo *f = &ct->fields[i];
        if (!f->type)
        {
            if ((int)i < argc)
                return Throwf("field '%s' has an unhandled foreign datatype", f->name);
            continue;
        }
        if ((int)i < argc)
        {
            if (!f->type->ft_storeinto(argv[i], (char *)raw + f->offset, f->type))
                return MaybeLocal<Value>();
        }
        else
            memset((char *)raw + f->offset, 0, UNIQTYPE_SIZE_IN_BYTES(f->type->ft_type));
    }
    return obj;
}

static void composite_configure(v8::Isolate *isolate,
        Local<v8::FunctionTemplate> tmpl, V8ForeignType *ft)
{
    v8::NamedPropertyHandlerConfiguration named(
            composite_get, composite_set, composite_query,
            nullptr /* deleter */, composite_enumerate,
            Local<Value>(), v8::PropertyHandlerFlags::kOnlyInterceptStrings);
    tmpl->InstanceTemplate()->SetHandler(named);
    Proxy_AddInspect(isolate, tmpl, composite_tostring);
    (void)ft;
}

V8ForeignType *CompositeProxy_NewType(const struct uniqtype *type)
{
    unsigned nfields = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
    const char **field_names = UNIQTYPE_COMPOSITE_SUBOBJ_NAMES(type);
    if (!field_names) nfields = 0; /* incomplete struct */

    V8CompositeType *ct = new V8CompositeType();
    ct->ft.ft_type = type;
    Proxy_SetupType(&ct->ft);
    ct->ft.ft_constructor = composite_ctor;
    ct->nfields = nfields;
    ct->fields = (V8FieldInfo *)calloc(nfields ? nfields : 1, sizeof(V8FieldInfo));
    for (unsigned i = 0; i < nfields; ++i)
        ct->fields[i].name = field_names[i];
    return &ct->ft;
}

void CompositeProxy_InitType(V8ForeignType *ft)
{
    V8CompositeType *ct = (V8CompositeType *)ft;
    const struct uniqtype *type = ft->ft_type;

    for (unsigned i = 0; i < ct->nfields; ++i)
    {
        const struct uniqtype_rel_info *rel = &type->related[i];
        ct->fields[i].offset = rel->un.memb.off;
        v8::TryCatch tc(g_isolate); /* swallow: field stays invalid on failure */
        ct->fields[i].type = ForeignType_GetOrCreate(rel->un.memb.ptr);
    }
    Proxy_CreateTemplate(ft, composite_configure);
}
