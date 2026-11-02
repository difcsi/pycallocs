/* Foreign library loading: V8 port of the Lua library_loader.c.
 * allocs.load(path) dlopens the library, walks its own .dynsym (shared
 * dl_iterate_syms from vmextensions/common) and returns a plain object with:
 *   - a property per defined global/weak FUNC/OBJECT symbol whose liballocs
 *     type is known (proxy or by-value depending on the type)
 *   - callable type-wrapper properties for named types the symbols
 *     transitively mention ($ -> _ renaming, first writer wins)
 *   - the base scalar types
 *   - __unsupported: { symbol: reason } for exports that did not materialise
 * The dlopen handle is deliberately never closed. */

#include "v8proxy.h"

#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <cstring>

using v8::Local;
using v8::Value;

struct add_sym_ctxt {
    Local<v8::Context> ctx;
    Local<v8::Object> module;
    Local<v8::Object> unsupported;
};

static Local<v8::String> jstr(const char *s)
{
    return v8::String::NewFromUtf8(g_isolate, s).ToLocalChecked();
}

/* module[type_name] = wrapper ($ -> _). 0 added, 1 already present, -1 unhandled */
static int add_type_to_module(const struct uniqtype *type, add_sym_ctxt *c)
{
    char type_name[256];
    strncpy(type_name, UNIQTYPE_NAME(type), sizeof type_name - 1);
    type_name[sizeof type_name - 1] = '\0';
    for (char *p = type_name; *p; ++p)
        if (*p == '$') *p = '_';

    v8::TryCatch tc(g_isolate); /* swallow: type publishing is best-effort */
    bool has = false;
    if (!c->module->HasOwnProperty(c->ctx, jstr(type_name)).To(&has)) return -1;
    if (has) return 1;

    V8ForeignType *ft = ForeignType_GetOrCreate(type);
    if (!ft) return -1;
    Local<v8::Object> wrapper;
    if (!ForeignType_Wrapper(ft).ToLocal(&wrapper)) return -1;
    c->module->Set(c->ctx, jstr(type_name), wrapper).Check();
    return 0;
}

static void recursively_add_useful_types(const struct uniqtype *type, add_sym_ctxt *c)
{
    if (!type) return;
    switch (UNIQTYPE_KIND(type))
    {
        case BASE:
            if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type) != 8 * UNIQTYPE_SIZE_IN_BYTES(type))
                return; /* bit field types */
            /* fallthrough */
        case VOID:
            add_type_to_module(type, c);
            return;
        case COMPOSITE:
            if (add_type_to_module(type, c) == 0)
            {
                unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
                for (unsigned i = 0; i < nmemb; ++i)
                    recursively_add_useful_types(type->related[i].un.t.ptr, c);
            }
            return;
        case ENUMERATION:
            if (add_type_to_module(type, c) == 0)
                recursively_add_useful_types(UNIQTYPE_ENUM_BASE_TYPE(type), c);
            return;
        case ARRAY:
        case SUBRANGE:
            recursively_add_useful_types(UNIQTYPE_ARRAY_ELEMENT_TYPE(type), c);
            return;
        case ADDRESS:
            recursively_add_useful_types(UNIQTYPE_ULTIMATE_POINTEE_TYPE(type), c);
            return;
        case SUBPROGRAM:
        {
            unsigned nb = type->un.subprogram.narg + type->un.subprogram.nret;
            for (unsigned i = 0; i < nb; ++i)
                recursively_add_useful_types(type->related[i].un.t.ptr, c);
            return;
        }
        default:
            return;
    }
}

static int add_sym_to_module(const ElfW(Sym) *sym, ElfW(Addr) loadAddress,
        char *strtab, void *arg)
{
    add_sym_ctxt *c = static_cast<add_sym_ctxt *>(arg);

    if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC
                || ELF64_ST_TYPE(sym->st_info) == STT_OBJECT)
            && (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
                || ELF64_ST_BIND(sym->st_info) == STB_WEAK)
            && sym->st_shndx != SHN_UNDEF
            && sym->st_shndx != SHN_ABS)
    {
        const char *symname = strtab + sym->st_name;
        if (symname[0] == '\0' || symname[0] == '_') return 0;

        void *data = (void *)(loadAddress + sym->st_value);

        /* Non-fatal per symbol: failures land in __unsupported. tc.Reset()
         * drops any pending exception before the recording Set. */
        v8::TryCatch tc(g_isolate);
        auto unsupported = [&](const char *stage) {
            tc.Reset();
            c->unsupported->Set(c->ctx, jstr(symname), jstr(stage)).Check();
        };

        const struct uniqtype *type = ss_alloc_get_type(data);
        if (!type)
        {
            unsupported("no liballocs type at symbol");
            return 0;
        }
        recursively_add_useful_types(type, c);

        V8ForeignType *ftype = ForeignType_GetOrCreate(type);
        if (!ftype)
        {
            unsupported("no foreign type for symbol's uniqtype");
            return 0;
        }

        Local<Value> obj;
        if (!ftype->ft_getfrom(data, ftype).ToLocal(&obj))
        {
            unsupported("proxy creation failed");
            return 0;
        }
        c->module->Set(c->ctx, jstr(symname), obj).Check();
    }
    return 0;
}

/* The C scalar base types come from the preloaded provider; weak so a missing
 * provider degrades rather than failing the load. */
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

static void add_base_types_to_module(add_sym_ctxt *c)
{
#define ADD_BASE_TYPE(t) \
    do { if (&__uniqtype__##t) add_type_to_module(&__uniqtype__##t, c); } while (0)
    ADD_BASE_TYPE(void);
    ADD_BASE_TYPE(int);
    ADD_BASE_TYPE(unsigned_int);
    ADD_BASE_TYPE(signed_char);
    ADD_BASE_TYPE(unsigned_char);
    ADD_BASE_TYPE(long_int);
    ADD_BASE_TYPE(unsigned_long_int);
    ADD_BASE_TYPE(short_int);
    ADD_BASE_TYPE(short_unsigned_int);
    ADD_BASE_TYPE(float);
    ADD_BASE_TYPE(double);
#undef ADD_BASE_TYPE
}

void LibraryLoader_Load(const v8::FunctionCallbackInfo<Value> &args)
{
    v8::Isolate *isolate = args.GetIsolate();
    v8::HandleScope hs(isolate);
    Local<v8::Context> ctx = isolate->GetCurrentContext();
    v8::String::Utf8Value path(isolate, args[0]);
    if (!*path) { Throwf("load: expected a path string"); return; }

    struct link_map *handle =
        (struct link_map *)dlopen(*path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        Throwf("loading of native shared library '%s' failed: %s", *path, dlerror());
        return;
    }

    add_sym_ctxt c = { ctx, v8::Object::New(isolate), v8::Object::New(isolate) };
    dl_iterate_syms(handle, add_sym_to_module, &c);
    add_base_types_to_module(&c);
    c.module->Set(ctx, jstr("__unsupported"), c.unsupported).Check();
    args.GetReturnValue().Set(c.module);
}
