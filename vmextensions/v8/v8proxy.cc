/* v8proxy module entry point: Node.js native addon (raw V8 API) exposing C
 * allocations to JavaScript via liballocs, sibling of vmextensions/cpython's
 * allocs module and vmextensions/lua's luaproxy.
 *
 *   const allocs = require('.../v8proxy.node');
 *   const lib = allocs.load('libfoo.so');
 */

#include "v8proxy.h"

#include <node.h>
#include <dlfcn.h>

/* File scope, NOT inside the anonymous namespace: an anonymous-namespace
 * declaration gets internal C++ linkage (observed as a mangled, never-defined
 * weak symbol -> null address) even under extern "C" with gcc. */
#define WEAK_UNIQTYPE(t) extern "C" struct uniqtype __uniqtype__##t __attribute__((weak))
WEAK_UNIQTYPE(void);
WEAK_UNIQTYPE(int);
WEAK_UNIQTYPE(unsigned_int);
WEAK_UNIQTYPE(signed_char);
WEAK_UNIQTYPE(unsigned_char);
WEAK_UNIQTYPE(long_int);
WEAK_UNIQTYPE(unsigned_long_int);
WEAK_UNIQTYPE(short_int);
WEAK_UNIQTYPE(short_unsigned_int);
WEAK_UNIQTYPE(float);
WEAK_UNIQTYPE(double);

namespace {

/* Preload-gate probe kept from Phase 0: _debug_symbol_type(libpath, symname)
 * -> uniqtype name. */
void DebugSymbolType(const v8::FunctionCallbackInfo<v8::Value> &args)
{
    v8::Isolate *isolate = args.GetIsolate();
    v8::String::Utf8Value libpath(isolate, args[0]);
    v8::String::Utf8Value symname(isolate, args[1]);

    void *handle = dlopen(*libpath, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) { Throwf("%s", dlerror()); return; }
    void *sym = dlsym(handle, *symname);
    if (!sym) { Throwf("symbol not found"); return; }
    const struct uniqtype *t = ss_alloc_get_type(sym);
    const char *name = t ? UNIQTYPE_NAME(t) : "<untyped>";
    args.GetReturnValue().Set(
            v8::String::NewFromUtf8(isolate, name).ToLocalChecked());
}

/* allocs.proxy_from_address(BigInt|Number) -> base proxy */
void ProxyFromAddress(const v8::FunctionCallbackInfo<v8::Value> &args)
{
    uint64_t addr = 0;
    if (args[0]->IsBigInt())
    {
        bool lossless;
        addr = args[0].As<v8::BigInt>()->Uint64Value(&lossless);
        if (!lossless) { Throwf("proxy_from_address: address out of range"); return; }
    }
    else if (args[0]->IsNumber())
        addr = (uint64_t)args[0].As<v8::Number>()->Value();
    else { Throwf("proxy_from_address: expected an address"); return; }

    v8::Local<v8::Object> obj;
    if (!Proxy_GetOrCreateBase((void *)(uintptr_t)addr).ToLocal(&obj))
    {
        Throwf("proxy_from_address: no liballocs proxy type for address %p",
                (void *)(uintptr_t)addr);
        return;
    }
    args.GetReturnValue().Set(obj);
}

void add_type_field(v8::Local<v8::Context> ctx, v8::Local<v8::Object> exports,
        const char *name, struct uniqtype *u)
{
    if (!u) return; /* provider not preloaded: degrade */
    v8::TryCatch tc(g_isolate); /* swallow: base-type publishing is best-effort */
    V8ForeignType *ft = ForeignType_GetOrCreate(u);
    if (!ft) return;
    v8::Local<v8::Object> wrapper;
    if (!ForeignType_Wrapper(ft).ToLocal(&wrapper)) return;
    exports->Set(ctx,
            v8::String::NewFromUtf8(g_isolate, name).ToLocalChecked(),
            wrapper).Check();
}

void Cleanup(void *)
{
    ForeignType_Teardown();
    Proxy_Teardown();
}

void Init(v8::Local<v8::Object> exports, v8::Local<v8::Value>,
          v8::Local<v8::Context> context)
{
    v8::Isolate *isolate = context->GetIsolate();
    if (g_isolate && g_isolate != isolate)
    {
        isolate->ThrowException(v8::Exception::Error(
                v8::String::NewFromUtf8Literal(isolate,
                    "v8proxy already serves another isolate (one per process)")));
        return;
    }
    Proxy_Init(isolate, context);
    node::AddEnvironmentCleanupHook(isolate, Cleanup, nullptr);

    auto set = [&](const char *name, v8::FunctionCallback cb) {
        exports->Set(context,
                v8::String::NewFromUtf8(isolate, name).ToLocalChecked(),
                v8::FunctionTemplate::New(isolate, cb)
                    ->GetFunction(context).ToLocalChecked()).Check();
    };
    set("load", LibraryLoader_Load);
    set("proxy_from_address", ProxyFromAddress);
    set("_debug_symbol_type", DebugSymbolType);

#define ADD_TYPE_WITH_NAME(name, t) add_type_field(context, exports, name, &__uniqtype__##t)
#define ADD_TYPE(t) ADD_TYPE_WITH_NAME(#t, t)
    ADD_TYPE(void);
    ADD_TYPE(int);
    ADD_TYPE(unsigned_int);
    ADD_TYPE(signed_char);
    ADD_TYPE(unsigned_char);
    if ((char)-1 < 0) ADD_TYPE_WITH_NAME("char", signed_char);
    else ADD_TYPE_WITH_NAME("char", unsigned_char);
    ADD_TYPE(long_int);
    ADD_TYPE(unsigned_long_int);
    ADD_TYPE(short_int);
    ADD_TYPE_WITH_NAME("unsigned_short_int", short_unsigned_int);
    ADD_TYPE(float);
    ADD_TYPE(double);
#undef ADD_TYPE
#undef ADD_TYPE_WITH_NAME

    exports->Set(context,
            v8::String::NewFromUtf8Literal(isolate, "_VERSION"),
            v8::String::NewFromUtf8Literal(isolate, "0.1")).Check();
}

} // namespace

NODE_MODULE_INIT(/* exports, module, context */)
{
    Init(exports, module, context);
}
