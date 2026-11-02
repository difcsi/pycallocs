/* Foreign library loading: port of src/library_loader.c (minus the Python
 * import-hook half, replaced by an explicit allocs.load("libfoo.so")).
 * Walks the freshly dlopen'ed library's own .dynsym; every defined global or
 * weak FUNC/OBJECT symbol whose liballocs type is known becomes an entry in
 * the returned module table, named types it transitively mentions become
 * callable type-wrapper entries, and every export that could not materialise
 * is recorded (symbol -> reason) in module.__unsupported.
 *
 * The dlopen handle is deliberately never dlclose'd: proxies into the library
 * may outlive the module table, and unloading code whose finalizers are still
 * reachable is how interop layers crash. */

#include "luaproxy.h"

#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <string.h>

#include <dlsym_iter.h> /* shared .dynsym walk (vmextensions/common) */

struct add_sym_ctxt {
    lua_State *L;
    int module_idx; /* absolute stack index of the module table */
    int unsup_idx;  /* absolute stack index of the __unsupported table */
};

/* module[type_name] = wrapper for `type` ($ -> _ in names, first writer wins).
 * Returns 0 added, 1 already present, -1 unhandled type. */
static int add_type_to_module(const struct uniqtype *type, struct add_sym_ctxt *ctxt)
{
    lua_State *L = ctxt->L;
    char type_name[256];
    strncpy(type_name, UNIQTYPE_NAME(type), sizeof type_name - 1);
    type_name[sizeof type_name - 1] = '\0';
    for (char *c = type_name; *c; ++c)
        if (*c == '$') *c = '_';

    if (lua_getfield(L, ctxt->module_idx, type_name) != LUA_TNIL)
    {
        lua_pop(L, 1);
        return 1;
    }
    lua_pop(L, 1);

    LuaForeignType *ft = LuaForeignType_GetOrCreate(L, type);
    if (!ft)
    {
        lua_pop(L, 1); /* the error message */
        return -1;
    }
    LuaForeignType_PushWrapper(L, ft);
    lua_setfield(L, ctxt->module_idx, type_name);
    return 0;
}

static void recursively_add_useful_types(const struct uniqtype *type,
        struct add_sym_ctxt *ctxt)
{
    if (!type) return;
    switch (UNIQTYPE_KIND(type))
    {
        case BASE:
            /* Ignore bit field types */
            if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type) != 8 * UNIQTYPE_SIZE_IN_BYTES(type))
                return;
            /* fallthrough */
        case VOID:
            add_type_to_module(type, ctxt);
            return;
        case COMPOSITE:
            if (add_type_to_module(type, ctxt) == 0)
            {
                unsigned nmemb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
                for (unsigned i = 0; i < nmemb; ++i)
                    recursively_add_useful_types(type->related[i].un.t.ptr, ctxt);
            }
            return;
        case ENUMERATION:
            if (add_type_to_module(type, ctxt) == 0)
                recursively_add_useful_types(UNIQTYPE_ENUM_BASE_TYPE(type), ctxt);
            return;
        case ARRAY:
        case SUBRANGE:
            recursively_add_useful_types(UNIQTYPE_ARRAY_ELEMENT_TYPE(type), ctxt);
            return;
        case ADDRESS:
            recursively_add_useful_types(UNIQTYPE_ULTIMATE_POINTEE_TYPE(type), ctxt);
            return;
        case SUBPROGRAM:
        {
            unsigned nb = type->un.subprogram.narg + type->un.subprogram.nret;
            for (unsigned i = 0; i < nb; ++i)
                recursively_add_useful_types(type->related[i].un.t.ptr, ctxt);
            return;
        }
        default:
            return;
    }
}

/* __unsupported[symname] = "stage" or "stage: <message on stack top>".
 * When msg_on_stack, consumes the pushed error message. */
static void record_unsupported(struct add_sym_ctxt *ctxt, const char *symname,
        const char *stage, bool msg_on_stack)
{
    lua_State *L = ctxt->L;
    if (msg_on_stack)
    {
        lua_pushfstring(L, "%s: %s", stage,
                lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_remove(L, -2);
    }
    else
        lua_pushstring(L, stage);
    lua_setfield(L, ctxt->unsup_idx, symname);
}

static int add_sym_to_module(const ElfW(Sym) *sym, ElfW(Addr) loadAddress,
        char *strtab, void *arg)
{
    struct add_sym_ctxt *ctxt = arg;
    lua_State *L = ctxt->L;

    if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC
                || ELF64_ST_TYPE(sym->st_info) == STT_OBJECT)
            && (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
                || ELF64_ST_BIND(sym->st_info) == STB_WEAK)
            && sym->st_shndx != SHN_UNDEF
            && sym->st_shndx != SHN_ABS)
    {
        const char *symname = strtab + sym->st_name;
        /* Ignore unnamed symbols and reserved names */
        if (symname[0] == '\0' || symname[0] == '_') return 0;

        void *data = (void *)(loadAddress + sym->st_value);

        const struct uniqtype *type = ss_alloc_get_type(data);
        if (!type)
        {
            record_unsupported(ctxt, symname, "no liballocs type at symbol", false);
            return 0;
        }
        recursively_add_useful_types(type, ctxt);

        LuaForeignType *ftype = LuaForeignType_GetOrCreate(L, type);
        if (!ftype)
        {
            record_unsupported(ctxt, symname, "no foreign type for symbol's uniqtype", true);
            return 0;
        }

        if (ftype->ft_getfrom(L, data, ftype))
        {
            record_unsupported(ctxt, symname, "proxy creation failed", true);
            return 0;
        }
        lua_setfield(L, ctxt->module_idx, symname);
    }
    return 0;
}

/* The C scalar base types are supplied by the preloaded base-types provider
 * meta-DSO; declare them weak so a missing provider degrades to fewer entries
 * rather than a load failure. */
#define WEAK_UNIQTYPE(t) extern struct uniqtype __uniqtype__##t __attribute__((weak))
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

static void add_base_types_to_module(struct add_sym_ctxt *ctxt)
{
#define ADD_BASE_TYPE(t) \
    do { if (&__uniqtype__##t) add_type_to_module(&__uniqtype__##t, ctxt); } while (0)
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

/* allocs.load(path) -> module table */
int LuaLibraryLoader_Load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    struct link_map *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
        return luaL_error(L, "loading of native shared library '%s' failed: %s",
                path, dlerror());

    lua_newtable(L); /* the module */
    int module_idx = lua_gettop(L);
    lua_newtable(L); /* __unsupported */
    int unsup_idx = lua_gettop(L);

    struct add_sym_ctxt ctxt = { L, module_idx, unsup_idx };
    dl_iterate_syms(handle, add_sym_to_module, &ctxt);
    add_base_types_to_module(&ctxt);

    lua_setfield(L, module_idx, "__unsupported");
    return 1;
}
