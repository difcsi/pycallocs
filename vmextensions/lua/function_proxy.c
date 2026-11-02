/* Foreign function proxies: port of src/function_proxy.c (call path; Lua
 * closures as C function pointers arrive in a later phase). A function proxy
 * is a userdata whose __call marshals arguments through the LuaForeignType
 * vtable into libffi. The cif is prepared lazily on first call.
 *
 * Marshalling per call:
 *   pass 1: ft_getdataptr zero-copy for args that already have C storage
 *   pass 2: ft_storeinto the rest into a stack scratch buffer
 *   Begin/EndDeferFrees bracket ffi_call so a chunk whose last C reference
 *   the callee overwrites can still be returned safely (struct_global_swap).
 *   The return value is wrapped by-value via ft_copyfrom. */

#define _GNU_SOURCE /* dladdr */
#include "luaproxy.h"

#include <dlfcn.h>
#include <dwarf.h>
#include <ffi.h>
#include <string.h>

#include <ffitypes.h> /* shared uniqtype -> ffi_type mapping (vmextensions/common) */

typedef struct LuaFunctionType {
    LuaForeignType ft;
    ffi_cif *ff_cif;
    LuaForeignType **ff_argtypes;
    LuaForeignType *ff_rettype;
    const char *ff_closure_mtname; /* metatable for closure proxies */
} LuaFunctionType;

/* A closure proxy is a function proxy whose p_ptr is an ffi trampoline
 * calling back into a Lua function. */
typedef struct lp_closure_ctx {
    struct LuaFunctionType *type;
    ffi_closure *closure;
    int fn_ref; /* registry ref anchoring the Lua callable */
} lp_closure_ctx;

typedef struct LuaClosureProxy {
    LuaProxy base;
    lp_closure_ctx *ctx;
} LuaClosureProxy;

/* Lazy cif preparation; 0 ok, -1 with error message pushed. */
static int funproxytype_setup(lua_State *L, LuaFunctionType *self)
{
    if (self->ff_cif) return 0;
    const struct uniqtype *type = self->ft.ft_type;

    if (type->un.subprogram.nret != 1)
    {
        lua_pushstring(L, "foreign functions not having exactly one return value are not supported");
        return -1;
    }
    const struct uniqtype *ret_type = type->related[0].un.t.ptr;
    self->ff_rettype = LuaForeignType_GetOrCreate(L, ret_type);
    if (!self->ff_rettype) return -1; /* error message already pushed */
    ffi_type *ffi_ret_type = ffi_type_for_uniqtype(ret_type);
    if (!ffi_ret_type)
    {
        lua_pushstring(L, "cannot get ABI encoding for return value");
        return -1;
    }

    unsigned narg = type->un.subprogram.narg;
    ffi_type **ffi_arg_types = NULL;
    if (narg > 0)
    {
        ffi_arg_types = malloc((1 + narg) * sizeof(ffi_type *));
        self->ff_argtypes = malloc((1 + narg) * sizeof(LuaForeignType *));
        ffi_arg_types[narg] = NULL;
        self->ff_argtypes[narg] = NULL;
    }
    for (unsigned i = 0; i < narg; ++i)
    {
        const struct uniqtype *arg_type = type->related[i + 1].un.t.ptr;
        ffi_arg_types[i] = ffi_type_for_uniqtype(arg_type);
        self->ff_argtypes[i] = arg_type
            ? LuaForeignType_GetOrCreate(L, arg_type) : NULL;
        if (!self->ff_argtypes[i] && arg_type) lua_pop(L, 1); /* its error message */
        if (!ffi_arg_types[i] || !self->ff_argtypes[i])
        {
            lua_pushfstring(L, "cannot get ABI encoding for argument %d", (int)i + 1);
            ffi_arg_types[i + 1] = NULL;
            self->ff_argtypes[i + 1] = NULL;
            free_ffi_type_arr(ffi_arg_types);
            free(self->ff_argtypes);
            self->ff_argtypes = NULL;
            free_ffi_type(ffi_ret_type);
            return -1;
        }
    }

    ffi_cif *cif = malloc(sizeof(ffi_cif));
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, narg, ffi_ret_type, ffi_arg_types) == FFI_OK)
    {
        self->ff_cif = cif;
        return 0;
    }

    lua_pushstring(L, "failure in call information initialization");
    free(cif);
    free_ffi_type_arr(ffi_arg_types);
    free(self->ff_argtypes);
    self->ff_argtypes = NULL;
    free_ffi_type(ffi_ret_type);
    return -1;
}

/* __call: proxy at 1, Lua arguments at 2..top. */
static int funproxy_call(lua_State *L)
{
    LuaProxy *self = (LuaProxy *)lua_touserdata(L, 1);
    LuaFunctionType *type = (LuaFunctionType *)self->pr_type;
    if (funproxytype_setup(L, type)) return lua_error(L);

    unsigned narg = type->ft.ft_type->un.subprogram.narg;
    unsigned given = (unsigned)(lua_gettop(L) - 1);
    if (given != narg)
        return luaL_error(L, "this function takes exactly %d argument%s (%d given)",
                narg, narg == 1 ? "" : "s", given);

    void *ff_args[narg > 0 ? narg : 1];
    unsigned argsize = 0;
    for (unsigned i = 0; i < narg; ++i)
    {
        LuaForeignType *aft = type->ff_argtypes[i];
        void *data_ptr = NULL;
        if (aft->ft_getdataptr)
        {
            data_ptr = aft->ft_getdataptr(L, 2 + i, aft);
            ff_args[i] = data_ptr;
        }
        if (!data_ptr) argsize += UNIQTYPE_SIZE_IN_BYTES(aft->ft_type);
    }

    char argvals[argsize > 0 ? argsize : 1];
    char *cur_arg = argvals;
    for (unsigned i = 0; i < narg; ++i)
    {
        LuaForeignType *aft = type->ff_argtypes[i];
        if (aft->ft_getdataptr && ff_args[i]) continue;

        /* (MaterializePointee for plain Lua values passed as T* arrives with
         * the closure/materialisation phase; ft_storeinto raises the
         * canonical type error meanwhile.) */
        if (aft->ft_storeinto(L, 2 + i, cur_arg, aft)) return lua_error(L);
        ff_args[i] = cur_arg;
        cur_arg += UNIQTYPE_SIZE_IN_BYTES(aft->ft_type);
    }

    const struct uniqtype *ret_type = type->ft.ft_type->related[0].un.t.ptr;
    LuaForeignType *ret_ftype = type->ff_rettype;

    int r;
    /* Defer GC frees across the call AND the return-value wrapping: if the
     * callee overwrites the C slot that held the only reference to a chunk it
     * then returns, the delref must not free it before ft_copyfrom re-adopts
     * it. */
    LuaProxy_BeginDeferFrees();
    {
        unsigned natret = UNIQTYPE_SIZE_IN_BYTES(ret_type);
        unsigned retsize = natret;
        if (sizeof(ffi_arg) > retsize) retsize = sizeof(ffi_arg);
        char retval[retsize];

        ffi_call(type->ff_cif, (void (*)(void))self->p_ptr, retval, ff_args);

        char *retp = retval;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        if (natret < sizeof(ffi_arg) &&
            (UNIQTYPE_IS_ENUM_TYPE(ret_type) || UNIQTYPE_IS_POINTER_TYPE(ret_type) ||
             (UNIQTYPE_IS_BASE_TYPE(ret_type) && ret_type->un.base.enc != DW_ATE_float)))
            retp += sizeof(ffi_arg) - natret;
#endif
        r = ret_ftype->ft_copyfrom
            ? ret_ftype->ft_copyfrom(L, retp, ret_ftype)
            : ret_ftype->ft_getfrom(L, retp, ret_ftype);
    }
    LuaProxy_EndDeferFrees(L);
    if (r) return lua_error(L);
    return 1;
}

static int funproxy_tostring(lua_State *L)
{
    LuaProxy *self = (LuaProxy *)lua_touserdata(L, 1);
    const struct uniqtype *type = self->pr_type->ft_type;

    const char *symname = "<unknown>";
    Dl_info dlinfo;
    if (dladdr(self->p_ptr, &dlinfo) && dlinfo.dli_sname) symname = dlinfo.dli_sname;

    unsigned nret = type->un.subprogram.nret;
    unsigned narg = type->un.subprogram.narg;

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "<foreign function '");
    for (unsigned a = 0; a < nret; ++a)
    {
        if (a > 0) luaL_addstring(&b, ", ");
        struct uniqtype *t = type->related[a].un.t.ptr;
        luaL_addstring(&b, t ? UNIQTYPE_NAME(t) : "<unknown>");
    }
    luaL_addchar(&b, ' ');
    luaL_addstring(&b, symname);
    luaL_addchar(&b, '(');
    for (unsigned a = 0; a < narg; ++a)
    {
        if (a > 0) luaL_addstring(&b, ", ");
        struct uniqtype *t = type->related[nret + a].un.t.ptr;
        luaL_addstring(&b, t ? UNIQTYPE_NAME(t) : "<unknown>");
    }
    luaL_addstring(&b, ")'>");
    luaL_pushresult(&b);
    return 1;
}

static const luaL_Reg function_metamethods[] = {
    {"__call", funproxy_call},
    {"__tostring", funproxy_tostring},
    {NULL, NULL}
};

/* ---- closures: Lua function -> C function pointer ------------------------ */

/* The ffi trampoline target. Runs with no Lua frame beneath it, so it must
 * never longjmp: the Lua callable is invoked under lua_pcall, and any failure
 * (stack, conversion, error in the callable) leaves a zeroed return value and
 * a warning. */
static void closure_trampoline(ffi_cif *cif, void *ret, void **args, void *ud)
{
    lp_closure_ctx *ctx = ud;
    LuaFunctionType *ft = ctx->type;
    lua_State *L = lp_globalL;
    unsigned nargs = cif->nargs;

    /* Zero the whole return slot first (integral returns are widened to a
     * full ffi_arg by libffi's contract; error paths return 0). */
    if (cif->rtype != &ffi_type_void)
    {
        size_t retsz = cif->rtype->size;
        if (cif->rtype->type != FFI_TYPE_STRUCT && retsz < sizeof(ffi_arg))
            retsz = sizeof(ffi_arg);
        memset(ret, 0, retsz);
    }

    if (!L || !lua_checkstack(L, (int)nargs + 4)) return;
    int base = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->fn_ref);
    for (unsigned i = 0; i < nargs; ++i)
    {
        LuaForeignType *aft = ft->ff_argtypes[i];
        /* Copy by value so the argument outlives the C caller's frame. */
        int r = aft->ft_copyfrom ? aft->ft_copyfrom(L, args[i], aft)
                                 : aft->ft_getfrom(L, args[i], aft);
        if (r)
        {
            lua_warning(L, "luaproxy: closure argument conversion failed", 0);
            lua_settop(L, base);
            return;
        }
    }
    if (lua_pcall(L, (int)nargs, 1, 0) != LUA_OK)
    {
        lua_warning(L, "luaproxy: error in foreign closure: ", 1);
        lua_warning(L, lua_isstring(L, -1) ? lua_tostring(L, -1) : "?", 0);
        lua_settop(L, base);
        return;
    }
    if (ft->ff_rettype->ft_storeinto(L, -1, ret, ft->ff_rettype))
        lua_warning(L, "luaproxy: closure return conversion failed", 0);
    lua_settop(L, base);
}

static int closureproxy_gc(lua_State *L)
{
    LuaClosureProxy *cp = (LuaClosureProxy *)lua_touserdata(L, 1);
    if (cp->ctx)
    {
        luaL_unref(L, LUA_REGISTRYINDEX, cp->ctx->fn_ref);
        ffi_closure_free(cp->ctx->closure);
        free(cp->ctx);
        cp->ctx = NULL;
    }
    return LuaProxy_GC(L);
}

/* ft_constructor of SUBPROGRAM types: allocs.int.fun(allocs.int)(add_one)
 * builds a C function pointer calling the Lua function. */
static int closureproxy_ctor(lua_State *L, int first_arg, LuaForeignType *f)
{
    LuaFunctionType *ft = (LuaFunctionType *)f;
    int nargs = lua_gettop(L) - first_arg + 1;
    if (nargs != 1)
    {
        lua_pushstring(L, "closure constructor takes exactly one callable argument");
        return -1;
    }
    if (lua_type(L, first_arg) != LUA_TFUNCTION
            && !luaL_getmetafield(L, first_arg, "__call"))
    {
        lua_pushstring(L, "cannot create a foreign closure over a non-callable value");
        return -1;
    }
    if (lua_type(L, first_arg) != LUA_TFUNCTION) lua_pop(L, 1); /* the __call field */
    if (funproxytype_setup(L, ft)) return -1;

    lp_closure_ctx *ctx = calloc(1, sizeof *ctx);
    void *code = NULL;
    ctx->closure = ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (!ctx->closure)
    {
        free(ctx);
        lua_pushstring(L, "failed to allocate ffi closure");
        return -1;
    }
    ctx->type = ft;
    lua_pushvalue(L, first_arg);
    ctx->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    LuaClosureProxy *cp =
        (LuaClosureProxy *)lua_newuserdatauv(L, sizeof(LuaClosureProxy), 1);
    cp->base.p_ptr = code;
    cp->base.pr_type = &ft->ft;
    cp->base.ap_length = 0;
    cp->base.pr_registered = false;
    cp->ctx = ctx;
    luaL_setmetatable(L, ft->ff_closure_mtname);

    if (ffi_prep_closure_loc(ctx->closure, ft->ff_cif, closure_trampoline,
                ctx, code) != FFI_OK)
    {
        lua_pop(L, 1); /* the userdata; its __gc will clean ctx up */
        lua_pushstring(L, "failed to create closure for callable");
        return -1;
    }

    /* Register like any base proxy (mirrors the Python port); what actually
     * keeps a stored closure pointer alive is LuaProxy_RetainStoredPtr, which
     * requires the cache entry made here. */
    LuaProxy_Register(L, -1);
    return 0;
}

static void create_closure_metatable(lua_State *L, LuaFunctionType *ft)
{
    char name[48];
    snprintf(name, sizeof name, "luaproxy:%p!closure", (void *)ft);
    ft->ff_closure_mtname = strdup(name);
    luaL_newmetatable(L, ft->ff_closure_mtname);
    lua_pushlightuserdata(L, ft);
    lua_setfield(L, -2, "__luaproxy_ft");
    lua_pushcfunction(L, closureproxy_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, funproxy_call);
    lua_setfield(L, -2, "__call");
    lua_pushcfunction(L, funproxy_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

LuaForeignType *LuaFunctionProxy_NewType(lua_State *L, const struct uniqtype *type)
{
    (void)L;
    LuaFunctionType *ft = calloc(1, sizeof *ft);
    ft->ft.ft_type = type;
    LuaProxy_SetupType(&ft->ft);
    ft->ft.ft_copyfrom = NULL; /* a function has no by-value copy; getfrom wraps it */
    ft->ft.ft_constructor = closureproxy_ctor;
    return &ft->ft;
}

void LuaFunctionProxy_InitType(lua_State *L, LuaForeignType *ft)
{
    LuaProxy_CreateMetatable(L, ft, function_metamethods);
    create_closure_metatable(L, (LuaFunctionType *)ft);
}
