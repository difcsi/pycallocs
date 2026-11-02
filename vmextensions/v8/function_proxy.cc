/* Foreign function proxies: V8 port of the Lua function_proxy.c. A function
 * proxy is a JS object whose call handler marshals arguments through the
 * V8ForeignType vtable into libffi (cif prepared lazily; uniqtype->ffi_type
 * from vmextensions/common). Closures wrap a JS function as a C function
 * pointer via ffi_closure; the trampoline enters the module-init context
 * under TryCatch and never leaves a pending exception. */

#define _GNU_SOURCE 1 /* dladdr */
#include "v8proxy.h"

#include <dlfcn.h>
#include <dwarf.h>
#include <ffi.h>
#include <cstring>
#include <string>
#include <vector>

using v8::Local;
using v8::MaybeLocal;
using v8::Value;

static Local<v8::Context> ctx() { return g_context.Get(g_isolate); }

/* Lazy cif preparation; false with exception pending on failure. */
static bool funproxytype_setup(V8FunctionType *self)
{
    if (self->ff_cif) return true;
    const struct uniqtype *type = self->ft.ft_type;

    if (type->un.subprogram.nret != 1)
    {
        Throwf("foreign functions not having exactly one return value are not supported");
        return false;
    }
    const struct uniqtype *ret_type = type->related[0].un.t.ptr;
    self->ff_rettype = ForeignType_GetOrCreate(ret_type);
    if (!self->ff_rettype) return false;
    ffi_type *ffi_ret_type = ffi_type_for_uniqtype(ret_type);
    if (!ffi_ret_type)
    {
        Throwf("cannot get ABI encoding for return value");
        return false;
    }

    unsigned narg = type->un.subprogram.narg;
    ffi_type **ffi_arg_types = nullptr;
    if (narg > 0)
    {
        ffi_arg_types = (ffi_type **)malloc((1 + narg) * sizeof(ffi_type *));
        self->ff_argtypes = (V8ForeignType **)malloc((1 + narg) * sizeof(V8ForeignType *));
        ffi_arg_types[narg] = nullptr;
        self->ff_argtypes[narg] = nullptr;
    }
    for (unsigned i = 0; i < narg; ++i)
    {
        const struct uniqtype *arg_type = type->related[i + 1].un.t.ptr;
        ffi_arg_types[i] = arg_type ? ffi_type_for_uniqtype(arg_type) : nullptr;
        {
            v8::TryCatch tc(g_isolate); /* swallow; we throw our own below */
            self->ff_argtypes[i] = arg_type ? ForeignType_GetOrCreate(arg_type) : nullptr;
        }
        if (!ffi_arg_types[i] || !self->ff_argtypes[i])
        {
            ffi_arg_types[i + 1] = nullptr;
            free_ffi_type_arr(ffi_arg_types);
            free(self->ff_argtypes);
            self->ff_argtypes = nullptr;
            free_ffi_type(ffi_ret_type);
            Throwf("cannot get ABI encoding for argument %u", i + 1);
            return false;
        }
    }

    ffi_cif *cif = (ffi_cif *)malloc(sizeof(ffi_cif));
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, narg, ffi_ret_type, ffi_arg_types) == FFI_OK)
    {
        self->ff_cif = cif;
        return true;
    }
    free(cif);
    free_ffi_type_arr(ffi_arg_types);
    free(self->ff_argtypes);
    self->ff_argtypes = nullptr;
    free_ffi_type(ffi_ret_type);
    Throwf("failure in call information initialization");
    return false;
}

static void funproxy_call(const v8::FunctionCallbackInfo<Value> &args)
{
    V8Proxy *self = Proxy_Untyped(args.This());
    if (!self || !self->p_ptr) { Throwf("call on a released function proxy"); return; }
    V8FunctionType *type = (V8FunctionType *)self->pr_type;
    if (!funproxytype_setup(type)) return;

    unsigned narg = type->ft.ft_type->un.subprogram.narg;
    if ((unsigned)args.Length() != narg)
    {
        Throwf("this function takes exactly %u argument%s (%d given)",
                narg, narg == 1 ? "" : "s", args.Length());
        return;
    }

    std::vector<void *> ff_args(narg ? narg : 1);
    unsigned argsize = 0;
    for (unsigned i = 0; i < narg; ++i)
    {
        V8ForeignType *aft = type->ff_argtypes[i];
        void *data_ptr = nullptr;
        if (aft->ft_getdataptr)
        {
            data_ptr = aft->ft_getdataptr(args[i], aft);
            ff_args[i] = data_ptr;
        }
        if (!data_ptr) argsize += UNIQTYPE_SIZE_IN_BYTES(aft->ft_type);
    }

    std::vector<char> argvals(argsize ? argsize : 1);
    char *cur_arg = argvals.data();
    for (unsigned i = 0; i < narg; ++i)
    {
        V8ForeignType *aft = type->ff_argtypes[i];
        if (aft->ft_getdataptr && ff_args[i]) continue;
        if (!aft->ft_storeinto(args[i], cur_arg, aft)) return;
        ff_args[i] = cur_arg;
        cur_arg += UNIQTYPE_SIZE_IN_BYTES(aft->ft_type);
    }

    const struct uniqtype *ret_type = type->ft.ft_type->related[0].un.t.ptr;
    V8ForeignType *ret_ftype = type->ff_rettype;

    MaybeLocal<Value> result;
    /* Defer GC frees across the call AND the return-value wrapping
     * (struct_global_swap). */
    Proxy_BeginDeferFrees();
    {
        unsigned natret = UNIQTYPE_SIZE_IN_BYTES(ret_type);
        unsigned retsize = natret;
        if (sizeof(ffi_arg) > retsize) retsize = sizeof(ffi_arg);
        std::vector<char> retval(retsize);

        ffi_call(type->ff_cif, (void (*)())self->p_ptr, retval.data(), ff_args.data());

        char *retp = retval.data();
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        if (natret < sizeof(ffi_arg) &&
            (UNIQTYPE_IS_ENUM_TYPE(ret_type) || UNIQTYPE_IS_POINTER_TYPE(ret_type) ||
             (UNIQTYPE_IS_BASE_TYPE(ret_type) && ret_type->un.base.enc != DW_ATE_float)))
            retp += sizeof(ffi_arg) - natret;
#endif
        result = ret_ftype->ft_copyfrom
            ? ret_ftype->ft_copyfrom(retp, ret_ftype)
            : ret_ftype->ft_getfrom(retp, ret_ftype);
    }
    Proxy_EndDeferFrees();
    Local<Value> v;
    if (result.ToLocal(&v)) args.GetReturnValue().Set(v);
}

static void funproxy_tostring(const v8::FunctionCallbackInfo<Value> &args)
{
    V8Proxy *self = Proxy_Untyped(args.This());
    if (!self) return;
    const struct uniqtype *type = self->pr_type->ft_type;

    const char *symname = "<unknown>";
    Dl_info dlinfo;
    if (self->p_ptr && dladdr(self->p_ptr, &dlinfo) && dlinfo.dli_sname)
        symname = dlinfo.dli_sname;

    unsigned nret = type->un.subprogram.nret;
    unsigned narg = type->un.subprogram.narg;
    std::string out = "<foreign function '";
    for (unsigned a = 0; a < nret; ++a)
    {
        if (a > 0) out += ", ";
        struct uniqtype *t = type->related[a].un.t.ptr;
        out += t ? UNIQTYPE_NAME(t) : "<unknown>";
    }
    out += " ";
    out += symname;
    out += "(";
    for (unsigned a = 0; a < narg; ++a)
    {
        if (a > 0) out += ", ";
        struct uniqtype *t = type->related[nret + a].un.t.ptr;
        out += t ? UNIQTYPE_NAME(t) : "<unknown>";
    }
    out += ")'>";
    args.GetReturnValue().Set(
            v8::String::NewFromUtf8(g_isolate, out.c_str()).ToLocalChecked());
}

static void function_configure(v8::Isolate *isolate,
        Local<v8::FunctionTemplate> tmpl, V8ForeignType *ft)
{
    tmpl->InstanceTemplate()->SetCallAsFunctionHandler(funproxy_call);
    Proxy_AddInspect(isolate, tmpl, funproxy_tostring);
    (void)ft;
}

/* ---- closures: JS function -> C function pointer ------------------------- */

struct closure_ctx {
    V8FunctionType *type;
    ffi_closure *closure;
    v8::Global<v8::Function> fn;
};

/* Attached to closure proxies via a second internal-field-adjacent record:
 * the V8Proxy payload stays the standard one; the ctx pointer rides in the
 * proxy's private property as an External. */
static v8::Local<v8::Private> closure_ctx_key()
{
    return v8::Private::ForApi(g_isolate,
            v8::String::NewFromUtf8Literal(g_isolate, "v8proxy.closure"));
}

static void closure_trampoline(ffi_cif *cif, void *ret, void **argv, void *ud)
{
    closure_ctx *cc = static_cast<closure_ctx *>(ud);
    V8FunctionType *ft = cc->type;
    unsigned nargs = cif->nargs;

    /* zero the whole (possibly ffi_arg-widened) return slot first */
    if (cif->rtype != &ffi_type_void)
    {
        size_t retsz = cif->rtype->size;
        if (cif->rtype->type != FFI_TYPE_STRUCT && retsz < sizeof(ffi_arg))
            retsz = sizeof(ffi_arg);
        memset(ret, 0, retsz);
    }
    if (!g_isolate) return;

    v8::Isolate::Scope iso_scope(g_isolate);
    v8::HandleScope hs(g_isolate);
    Local<v8::Context> context = g_context.Get(g_isolate);
    v8::Context::Scope ctx_scope(context);
    v8::TryCatch tc(g_isolate);

    std::vector<Local<Value>> jsargs(nargs ? nargs : 1);
    for (unsigned i = 0; i < nargs; ++i)
    {
        V8ForeignType *aft = ft->ff_argtypes[i];
        MaybeLocal<Value> m = aft->ft_copyfrom
            ? aft->ft_copyfrom(argv[i], aft)
            : aft->ft_getfrom(argv[i], aft);
        if (!m.ToLocal(&jsargs[i])) return; /* zeroed ret; tc swallows */
    }
    Local<Value> result;
    if (!cc->fn.Get(g_isolate)->Call(context, v8::Undefined(g_isolate),
            (int)nargs, jsargs.data()).ToLocal(&result))
    {
        fprintf(stderr, "v8proxy: error in foreign closure\n");
        return; /* tc swallows; zeroed ret stands */
    }
    ft->ff_rettype->ft_storeinto(result, ret, ft->ff_rettype);
    /* any store failure: tc swallows the pending exception; zeroed ret */
}

static MaybeLocal<Value> closure_ctor(int argc, const Local<Value> *argv,
        V8ForeignType *f)
{
    V8FunctionType *ft = (V8FunctionType *)f;
    if (argc != 1 || !argv[0]->IsFunction())
        return Throwf("closure constructor takes exactly one function argument");
    if (!funproxytype_setup(ft)) return MaybeLocal<Value>();

    closure_ctx *cc = new closure_ctx();
    void *code = nullptr;
    cc->closure = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (!cc->closure)
    {
        delete cc;
        return Throwf("failed to allocate ffi closure");
    }
    cc->type = ft;
    cc->fn.Reset(g_isolate, argv[0].As<v8::Function>());

    Local<v8::Object> obj;
    if (!Proxy_NewObject(code, f).ToLocal(&obj))
    {
        ffi_closure_free(cc->closure);
        delete cc; /* Global dtor resets fn */
        return MaybeLocal<Value>();
    }
    if (ffi_prep_closure_loc(cc->closure, ft->ff_cif, closure_trampoline,
            cc, code) != FFI_OK)
    {
        ffi_closure_free(cc->closure);
        delete cc;
        return Throwf("failed to create closure for callable");
    }
    /* Anchor the ctx on the proxy; freed when the proxy dies. NOTE v1 leak:
     * the ffi_closure itself and the ctx are reclaimed only via the External
     * finalizer path below; the trampoline must not outlive C references,
     * which Proxy_RetainStoredPtr guarantees for stored pointers. */
    obj->SetPrivate(ctx(), closure_ctx_key(),
            v8::External::New(g_isolate, cc)).Check();

    Proxy_Register(obj); /* cache entry enables RetainStoredPtr */
    return obj;
}

V8ForeignType *FunctionProxy_NewType(const struct uniqtype *type)
{
    V8FunctionType *ft = new V8FunctionType();
    ft->ft.ft_type = type;
    ft->ff_cif = nullptr;
    ft->ff_argtypes = nullptr;
    ft->ff_rettype = nullptr;
    Proxy_SetupType(&ft->ft);
    ft->ft.ft_copyfrom = nullptr; /* functions have no by-value copy */
    ft->ft.ft_constructor = closure_ctor;
    return &ft->ft;
}

void FunctionProxy_InitType(V8ForeignType *ft)
{
    Proxy_CreateTemplate(ft, function_configure);
}
