/* uniqtype -> V8ForeignType dispatch, memoisation, JS-visible type wrapper
 * objects and out-parameter cells: V8 port of the Lua foreign_type.c.
 *
 * Memoisation: BASE/ADDRESS/ARRAY interned by UNIQTYPE_NAME string (several
 * structurally identical instances can coexist per process), COMPOSITE/
 * SUBPROGRAM by uniqtype pointer. Types are immortal. Two-phase creation
 * (memo store between New and Init) terminates recursive struct types. */

#include "v8proxy.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using v8::Local;
using v8::MaybeLocal;
using v8::Value;

static std::unordered_map<std::string, V8ForeignType *> g_memo_byname;
static std::unordered_map<const struct uniqtype *, V8ForeignType *> g_memo_byptr;
static std::unordered_map<V8ForeignType *, v8::Global<v8::Object>> g_wrappers;
static v8::Global<v8::FunctionTemplate> g_fttype_template;
static char g_fttype_tag; /* brand for type wrapper objects */

static Local<v8::Context> ctx() { return g_context.Get(g_isolate); }

/* ---- VOID ---------------------------------------------------------------- */

static MaybeLocal<Value> void_get(void *, V8ForeignType *)
{
    return v8::Undefined(g_isolate).As<Value>();
}
static bool void_store(Local<Value> v, void *, V8ForeignType *)
{
    if (!v->IsNullOrUndefined())
    {
        Throwf("the only accepted values for the void foreign type are null/undefined");
        return false;
    }
    return true;
}

/* ---- dispatch ------------------------------------------------------------ */

static V8ForeignType *type_new(const struct uniqtype *type)
{
    switch (UNIQTYPE_KIND(type))
    {
        case VOID:
        {
            V8ForeignType *ft = new V8ForeignType();
            ft->ft_type = type;
            ft->ft_getfrom = void_get;
            ft->ft_copyfrom = void_get;
            ft->ft_storeinto = void_store;
            return ft;
        }
        case BASE:
        {
            V8ForeignType *ft = new V8ForeignType();
            ft->ft_type = type;
            if (!ForeignBaseType_Init(ft))
            {
                delete ft;
                Throwf("unsupported foreign base type '%s'", UNIQTYPE_NAME(type));
                return nullptr;
            }
            ft->ft_constructor = ForeignType_CellCtor;
            return ft;
        }
        case ENUMERATION:
        {
            V8ForeignType *ft = new V8ForeignType();
            ft->ft_type = type;
            if (!ForeignEnumType_Init(ft))
            {
                delete ft;
                Throwf("unsupported foreign enumeration '%s'", UNIQTYPE_NAME(type));
                return nullptr;
            }
            return ft;
        }
        case COMPOSITE:
            return CompositeProxy_NewType(type);
        case ADDRESS:
            return AddressProxy_NewType(type);
        case ARRAY:
            return ArrayProxy_NewType(type);
        case SUBPROGRAM:
            return FunctionProxy_NewType(type);
        default:
            Throwf("cannot create foreign type for '%s'", UNIQTYPE_NAME(type));
            return nullptr;
    }
}

static void type_init(V8ForeignType *ft)
{
    switch (UNIQTYPE_KIND(ft->ft_type))
    {
        case COMPOSITE:  CompositeProxy_InitType(ft); break;
        case ADDRESS:    AddressProxy_InitType(ft); break;
        case ARRAY:      ArrayProxy_InitType(ft); break;
        case SUBPROGRAM: FunctionProxy_InitType(ft); break;
        default: break;
    }
}

V8ForeignType *ForeignType_GetOrCreate(const struct uniqtype *type)
{
    if (!type)
    {
        Throwf("ForeignType_GetOrCreate called with NULL uniqtype");
        return nullptr;
    }

    bool byname = false;
    std::string key;
    switch (UNIQTYPE_KIND(type))
    {
        case BASE: case ADDRESS: case ARRAY:
        {
            const char *name = UNIQTYPE_NAME(type);
            if (name) { byname = true; key = name; }
            break;
        }
        default: break;
    }

    if (byname)
    {
        auto it = g_memo_byname.find(key);
        if (it != g_memo_byname.end()) return it->second;
    }
    else
    {
        auto it = g_memo_byptr.find(type);
        if (it != g_memo_byptr.end()) return it->second;
    }

    V8ForeignType *ft = type_new(type);
    if (!ft) return nullptr; /* exception pending */
    /* memo BEFORE Init recurses (recursion termination) */
    if (byname) g_memo_byname[key] = ft;
    else g_memo_byptr[type] = ft;
    type_init(ft);
    return ft;
}

bool ForeignType_IsTriviallyCopiable(const V8ForeignType *ft)
{
    switch (UNIQTYPE_KIND(ft->ft_type))
    {
        case COMPOSITE:
        case ARRAY:
        case SUBPROGRAM:
            return false;
        default:
            return true;
    }
}

/* ---- cells --------------------------------------------------------------- */

MaybeLocal<Value> ForeignType_NewCell(V8ForeignType *elem_ft, Local<Value> init)
{
    struct uniqtype *arrtype = __liballocs_get_or_create_flexible_array_type(
            (struct uniqtype *)elem_ft->ft_type);
    if (!arrtype)
        return Throwf("cannot create a cell of type '%s'",
                UNIQTYPE_NAME(elem_ft->ft_type));
    V8ForeignType *arr_ft = ForeignType_GetOrCreate(arrtype);
    if (!arr_ft) return MaybeLocal<Value>();
    if (!arr_ft->ft_constructor)
        return Throwf("cell type '%s' is not constructible", UNIQTYPE_NAME(arrtype));

    Local<Value> one = v8::Number::New(g_isolate, 1);
    Local<Value> cell;
    if (!arr_ft->ft_constructor(1, &one, arr_ft).ToLocal(&cell))
        return MaybeLocal<Value>();

    if (!init.IsEmpty() && !init->IsUndefined())
    {
        V8Proxy *p = Proxy_Untyped(cell);
        if (p && !elem_ft->ft_storeinto(init, ss_translate(p->p_ptr), elem_ft))
            return MaybeLocal<Value>();
    }
    return cell;
}

MaybeLocal<Value> ForeignType_CellCtor(int argc, const Local<Value> *argv,
        V8ForeignType *ft)
{
    if (argc > 1)
        return Throwf("cell constructor takes at most one argument");
    return ForeignType_NewCell(ft, argc == 1 ? argv[0] : Local<Value>());
}

/* ---- JS type wrapper objects --------------------------------------------- */

static V8ForeignType *wrapper_fttype(Local<Value> v)
{
    if (v.IsEmpty() || !v->IsObject()) return nullptr;
    Local<v8::Object> obj = v.As<v8::Object>();
    if (obj->InternalFieldCount() < 2) return nullptr;
    if (obj->GetAlignedPointerFromInternalField(0) != &g_fttype_tag) return nullptr;
    return static_cast<V8ForeignType *>(obj->GetAlignedPointerFromInternalField(1));
}

static void fttype_call(const v8::FunctionCallbackInfo<Value> &args)
{
    V8ForeignType *ft = wrapper_fttype(args.This());
    if (!ft) { Throwf("not a foreign type"); return; }
    if (!ft->ft_constructor)
    {
        Throwf("cannot construct values of foreign type '%s' from JS",
                UNIQTYPE_NAME(ft->ft_type));
        return;
    }
    int argc = args.Length();
    std::vector<Local<Value>> argv(argc);
    for (int i = 0; i < argc; ++i) argv[i] = args[i];
    Local<Value> result;
    if (ft->ft_constructor(argc, argv.data(), ft).ToLocal(&result))
        args.GetReturnValue().Set(result);
}

static void fttype_tostring(const v8::FunctionCallbackInfo<Value> &args)
{
    V8ForeignType *ft = wrapper_fttype(args.This());
    if (!ft) return;
    char buf[256];
    snprintf(buf, sizeof buf, "<foreign type '%s'>", UNIQTYPE_NAME(ft->ft_type));
    args.GetReturnValue().Set(
            v8::String::NewFromUtf8(g_isolate, buf).ToLocalChecked());
}

/* T.ptr / T.array lazy getters running the liballocs type algebra */
static void fttype_getter(Local<v8::Name> name,
        const v8::PropertyCallbackInfo<Value> &info)
{
    V8ForeignType *ft = wrapper_fttype(info.This());
    if (!ft) return;
    v8::String::Utf8Value key(g_isolate, name);
    const struct uniqtype *result_type = nullptr;
    if (strcmp(*key, "ptr") == 0)
        result_type = __liballocs_get_or_create_address_type(ft->ft_type);
    else if (strcmp(*key, "array") == 0)
        result_type = __liballocs_get_or_create_flexible_array_type(
                (struct uniqtype *)ft->ft_type);
    else if (strcmp(*key, "name") == 0)
    {
        info.GetReturnValue().Set(v8::String::NewFromUtf8(g_isolate,
                UNIQTYPE_NAME(ft->ft_type)).ToLocalChecked());
        return;
    }
    if (!result_type)
    {
        Throwf("cannot create derived type of '%s'", UNIQTYPE_NAME(ft->ft_type));
        return;
    }
    V8ForeignType *derived = ForeignType_GetOrCreate(result_type);
    if (!derived) return;
    Local<v8::Object> wrapper;
    if (ForeignType_Wrapper(derived).ToLocal(&wrapper))
        info.GetReturnValue().Set(wrapper);
}

/* T.fun(argT...): the type of functions returning T */
static void fttype_fun(const v8::FunctionCallbackInfo<Value> &args)
{
    V8ForeignType *ret_ft = wrapper_fttype(args.This());
    if (!ret_ft) { Throwf("fun: not a foreign type"); return; }
    int nargs = args.Length();
    std::vector<const struct uniqtype *> argtypes(nargs);
    for (int i = 0; i < nargs; ++i)
    {
        V8ForeignType *aft = wrapper_fttype(args[i]);
        if (!aft) { Throwf("fun: argument %d is not a foreign type", i + 1); return; }
        argtypes[i] = aft->ft_type;
    }
    const struct uniqtype *funtype = __liballocs_get_or_create_subprogram_type(
            (struct uniqtype *)ret_ft->ft_type, nargs,
            (struct uniqtype **)argtypes.data());
    if (!funtype) { Throwf("failed to create requested function type"); return; }
    V8ForeignType *ft = ForeignType_GetOrCreate(funtype);
    if (!ft) return;
    Local<v8::Object> wrapper;
    if (ForeignType_Wrapper(ft).ToLocal(&wrapper))
        args.GetReturnValue().Set(wrapper);
}

static Local<v8::FunctionTemplate> fttype_template()
{
    if (!g_fttype_template.IsEmpty()) return g_fttype_template.Get(g_isolate);
    v8::Isolate *isolate = g_isolate;
    v8::EscapableHandleScope hs(isolate);
    Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);
    tmpl->SetClassName(v8::String::NewFromUtf8Literal(isolate, "ForeignType"));
    Local<v8::ObjectTemplate> inst = tmpl->InstanceTemplate();
    inst->SetInternalFieldCount(2);
    inst->SetCallAsFunctionHandler(fttype_call);
    inst->SetNativeDataProperty(
            v8::String::NewFromUtf8Literal(isolate, "ptr"), fttype_getter);
    inst->SetNativeDataProperty(
            v8::String::NewFromUtf8Literal(isolate, "array"), fttype_getter);
    inst->SetNativeDataProperty(
            v8::String::NewFromUtf8Literal(isolate, "name"), fttype_getter);
    tmpl->PrototypeTemplate()->Set(isolate, "fun",
            v8::FunctionTemplate::New(isolate, fttype_fun));
    tmpl->PrototypeTemplate()->Set(isolate, "toString",
            v8::FunctionTemplate::New(isolate, fttype_tostring));
    g_fttype_template.Reset(isolate, hs.Escape(tmpl));
    return g_fttype_template.Get(isolate);
}

/* Called from the environment cleanup hook, while the isolate is still
 * alive: every static-storage Global must be Reset here, because their C++
 * destructors run at exit() AFTER the isolate is destroyed (resetting a
 * non-empty Global then segfaults in GlobalHandles::Destroy). */
void ForeignType_Teardown()
{
    g_wrappers.clear();
    g_fttype_template.Reset();
}

MaybeLocal<v8::Object> ForeignType_Wrapper(V8ForeignType *ft)
{
    auto it = g_wrappers.find(ft);
    if (it != g_wrappers.end()) return it->second.Get(g_isolate);

    Local<v8::Object> obj;
    if (!fttype_template()->InstanceTemplate()->NewInstance(ctx()).ToLocal(&obj))
        return MaybeLocal<v8::Object>();
    obj->SetAlignedPointerInInternalField(0, &g_fttype_tag);
    obj->SetAlignedPointerInInternalField(1, ft);
    g_wrappers[ft] = v8::Global<v8::Object>(g_isolate, obj); /* types immortal */
    return obj;
}
