/* uniqtype -> LuaForeignType dispatch, memoisation and the Lua-visible type
 * wrapper objects. Port of src/foreign_type.c.
 *
 * Memoisation policy (copied from the Python side): structural kinds (BASE,
 * ADDRESS, ARRAY) can exist as several identical uniqtype instances in one
 * process (one per meta-DSO plus __liballocs_get_or_create_* synthesised
 * ones), so they are interned by UNIQTYPE_NAME string; nominal kinds
 * (COMPOSITE, SUBPROGRAM) keep pointer identity. Types are immortal (the memo
 * never evicts), exactly like the Python typdict.
 *
 * Two-phase creation: New allocates the type and the memo entry is stored
 * BEFORE Init recurses into member/pointee types, so recursive types
 * (struct node { struct node *next; }) terminate. */

#include "luaproxy.h"

#include <string.h>

static int lp_type_memo_ref = LUA_NOREF;     /* memo key -> lightuserdata(ft) */
static int lp_type_wrappers_ref = LUA_NOREF; /* lightuserdata(ft) -> wrapper ud */

#define FTTYPE_MT "luaproxy.ForeignType"

/* ---- VOID ---------------------------------------------------------------- */

static int void_getfrom(lua_State *L, void *data, LuaForeignType *ft)
{
    (void)data; (void)ft;
    lua_pushnil(L);
    return 0;
}

static int void_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)
{
    (void)dest; (void)ft;
    if (!lua_isnil(L, idx))
    {
        lua_pushstring(L, "the only accepted value for the void foreign type is nil");
        return -1;
    }
    return 0;
}

/* ---- dispatch ------------------------------------------------------------ */

/* Allocate + fill the vtable for `type`. NULL with error message pushed on
 * unsupported kinds. Does NOT touch the memo. */
static LuaForeignType *type_new(lua_State *L, const struct uniqtype *type)
{
    switch (UNIQTYPE_KIND(type))
    {
        case VOID:
        {
            LuaForeignType *ft = calloc(1, sizeof *ft);
            ft->ft_type = type;
            ft->ft_getfrom = void_getfrom;
            ft->ft_copyfrom = void_getfrom;
            ft->ft_storeinto = void_storeinto;
            return ft;
        }
        case BASE:
        {
            LuaForeignType *ft = calloc(1, sizeof *ft);
            ft->ft_type = type;
            if (LuaForeignBaseType_Init(L, ft) < 0) { free(ft); return NULL; }
            /* allocs.int() / allocs.int(5): a one-element out-parameter cell */
            ft->ft_constructor = LuaForeignType_CellCtor;
            return ft;
        }
        case ENUMERATION:
        {
            LuaForeignType *ft = calloc(1, sizeof *ft);
            ft->ft_type = type;
            if (LuaForeignEnumType_Init(L, ft) < 0) { free(ft); return NULL; }
            return ft;
        }
        case COMPOSITE:
            return LuaCompositeProxy_NewType(L, type);
        case ADDRESS:
            return LuaAddressProxy_NewType(L, type);
        case ARRAY:
            return LuaArrayProxy_NewType(L, type);
        case SUBPROGRAM:
            return LuaFunctionProxy_NewType(L, type);
        default:
            lua_pushfstring(L, "cannot create foreign type for '%s'",
                    UNIQTYPE_NAME(type));
            return NULL;
    }
}

static void type_init(lua_State *L, LuaForeignType *ft)
{
    switch (UNIQTYPE_KIND(ft->ft_type))
    {
        case COMPOSITE:
            LuaCompositeProxy_InitType(L, ft);
            break;
        case ADDRESS:
            LuaAddressProxy_InitType(L, ft);
            break;
        case ARRAY:
            LuaArrayProxy_InitType(L, ft);
            break;
        case SUBPROGRAM:
            LuaFunctionProxy_InitType(L, ft);
            break;
        default:
            break;
    }
}

/* Push the memo key for `type` (string for structural kinds, integer for
 * nominal ones). */
static void push_memo_key(lua_State *L, const struct uniqtype *type)
{
    switch (UNIQTYPE_KIND(type))
    {
        case BASE: case ADDRESS: case ARRAY:
        {
            const char *name = UNIQTYPE_NAME(type);
            if (name) { lua_pushstring(L, name); return; }
            break;
        }
        default: break;
    }
    lua_pushinteger(L, (lua_Integer)(uintptr_t)type);
}

LuaForeignType *LuaForeignType_GetOrCreate(lua_State *L, const struct uniqtype *type)
{
    if (!type)
    {
        lua_pushstring(L, "LuaForeignType_GetOrCreate called with NULL uniqtype");
        return NULL;
    }
    if (lp_type_memo_ref == LUA_NOREF)
    {
        lua_newtable(L);
        lp_type_memo_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, lp_type_memo_ref);
    push_memo_key(L, type);
    lua_pushvalue(L, -1); /* keep a copy of the key for the store below */
    lua_rawget(L, -3);
    if (lua_islightuserdata(L, -1))
    {
        LuaForeignType *ft = (LuaForeignType *)lua_touserdata(L, -1);
        lua_pop(L, 3);
        return ft;
    }
    lua_pop(L, 1); /* the nil */

    LuaForeignType *ft = type_new(L, type);
    if (!ft)
    {
        /* error message on top; drop memo table + key beneath it */
        lua_remove(L, -2);
        lua_remove(L, -2);
        return NULL;
    }
    /* memo[key] = ft BEFORE Init recurses (recursion termination) */
    lua_pushlightuserdata(L, ft);
    lua_rawset(L, -3);
    lua_pop(L, 1); /* memo table */
    type_init(L, ft);
    return ft;
}

bool LuaForeignType_IsTriviallyCopiable(const LuaForeignType *ft)
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

int LuaForeignType_NewCell(lua_State *L, LuaForeignType *elem_ft, int init_idx)
{
    if (init_idx) init_idx = lua_absindex(L, init_idx);
    struct uniqtype *arrtype = __liballocs_get_or_create_flexible_array_type(
            (struct uniqtype *)elem_ft->ft_type);
    if (!arrtype)
    {
        lua_pushfstring(L, "cannot create a cell of type '%s'",
                UNIQTYPE_NAME(elem_ft->ft_type));
        return -1;
    }
    LuaForeignType *arr_ft = LuaForeignType_GetOrCreate(L, arrtype);
    if (!arr_ft) return -1; /* error message already pushed */
    if (!arr_ft->ft_constructor)
    {
        lua_pushfstring(L, "cell type '%s' is not constructible", UNIQTYPE_NAME(arrtype));
        return -1;
    }

    /* A zero-initialised one-element array... */
    lua_pushinteger(L, 1);
    if (arr_ft->ft_constructor(L, lua_gettop(L), arr_ft))
    {
        lua_remove(L, -2); /* the pushed length below the error message */
        return -1;
    }
    lua_remove(L, -2); /* the pushed length */

    /* ...optionally initialised from the given value. */
    if (init_idx)
    {
        LuaProxy *p = (LuaProxy *)lua_touserdata(L, -1);
        if (elem_ft->ft_storeinto(L, init_idx, ss_translate(p->p_ptr), elem_ft))
        {
            lua_remove(L, -2); /* the half-built cell */
            return -1;
        }
    }
    return 0;
}

int LuaForeignType_CellCtor(lua_State *L, int first_arg, LuaForeignType *ft)
{
    int nargs = lua_gettop(L) - first_arg + 1;
    if (nargs < 0) nargs = 0;
    if (nargs > 1)
    {
        lua_pushstring(L, "cell constructor takes at most one argument");
        return -1;
    }
    return LuaForeignType_NewCell(L, ft, nargs == 1 ? first_arg : 0);
}

/* ---- Lua-visible type wrapper objects ------------------------------------ */

static LuaForeignType *check_fttype(lua_State *L, int idx)
{
    return *(LuaForeignType **)luaL_checkudata(L, idx, FTTYPE_MT);
}

static int fttype_call(lua_State *L)
{
    LuaForeignType *ft = check_fttype(L, 1);
    if (!ft->ft_constructor)
        return luaL_error(L, "cannot construct values of foreign type '%s' from Lua",
                UNIQTYPE_NAME(ft->ft_type));
    if (ft->ft_constructor(L, 2, ft)) return lua_error(L);
    return 1;
}

static int fttype_tostring(lua_State *L)
{
    LuaForeignType *ft = check_fttype(L, 1);
    lua_pushfstring(L, "<foreign type '%s'>", UNIQTYPE_NAME(ft->ft_type));
    return 1;
}

/* T.fun(arg1_t, arg2_t, ...): the type of functions taking the given argument
 * types and returning T. The return type rides in upvalue 1 so the Python-like
 * dot spelling works (no method-call colon needed). */
static int fttype_fun(lua_State *L)
{
    LuaForeignType *ret_ft = check_fttype(L, lua_upvalueindex(1));
    int nargs = lua_gettop(L);
    const struct uniqtype *argtypes[nargs > 0 ? nargs : 1];
    for (int i = 0; i < nargs; ++i)
        argtypes[i] = check_fttype(L, 1 + i)->ft_type;

    const struct uniqtype *funtype = __liballocs_get_or_create_subprogram_type(
            (struct uniqtype *)ret_ft->ft_type, nargs, (struct uniqtype **)argtypes);
    if (!funtype)
        return luaL_error(L, "failed to create requested function type");
    LuaForeignType *ft = LuaForeignType_GetOrCreate(L, funtype);
    if (!ft) return lua_error(L);
    LuaForeignType_PushWrapper(L, ft);
    return 1;
}

/* Type algebra on wrappers: T.ptr, T.array, T.fun, T.name. */
static int fttype_index(lua_State *L)
{
    LuaForeignType *ft = check_fttype(L, 1);
    const char *key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : NULL;
    if (!key) { lua_pushnil(L); return 1; }

    if (strcmp(key, "ptr") == 0)
    {
        const struct uniqtype *ptrtype =
            __liballocs_get_or_create_address_type(ft->ft_type);
        if (!ptrtype)
            return luaL_error(L, "cannot create pointer type to '%s'",
                    UNIQTYPE_NAME(ft->ft_type));
        LuaForeignType *pft = LuaForeignType_GetOrCreate(L, ptrtype);
        if (!pft) return lua_error(L);
        LuaForeignType_PushWrapper(L, pft);
        return 1;
    }
    if (strcmp(key, "array") == 0)
    {
        const struct uniqtype *arrtype =
            __liballocs_get_or_create_flexible_array_type((struct uniqtype *)ft->ft_type);
        if (!arrtype)
            return luaL_error(L, "cannot create array of type '%s'",
                    UNIQTYPE_NAME(ft->ft_type));
        LuaForeignType *aft = LuaForeignType_GetOrCreate(L, arrtype);
        if (!aft) return lua_error(L);
        LuaForeignType_PushWrapper(L, aft);
        return 1;
    }
    if (strcmp(key, "fun") == 0)
    {
        lua_pushvalue(L, 1); /* the wrapper becomes upvalue 1 */
        lua_pushcclosure(L, fttype_fun, 1);
        return 1;
    }
    if (strcmp(key, "name") == 0)
    {
        lua_pushstring(L, UNIQTYPE_NAME(ft->ft_type));
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

void LuaForeignType_InitMeta(lua_State *L)
{
    if (lp_type_wrappers_ref != LUA_NOREF) return;
    lua_newtable(L); /* wrapper cache: types are immortal, so strong refs */
    lp_type_wrappers_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (luaL_newmetatable(L, FTTYPE_MT))
    {
        lua_pushcfunction(L, fttype_call);
        lua_setfield(L, -2, "__call");
        lua_pushcfunction(L, fttype_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, fttype_index);
        lua_setfield(L, -2, "__index");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "__luaproxy_fttype");
    }
    lua_pop(L, 1);
}

void LuaForeignType_PushWrapper(lua_State *L, LuaForeignType *ft)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, lp_type_wrappers_ref);
    lua_pushlightuserdata(L, ft);
    if (lua_rawget(L, -2) == LUA_TUSERDATA)
    {
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);

    LuaForeignType **slot =
        (LuaForeignType **)lua_newuserdatauv(L, sizeof(LuaForeignType *), 0);
    *slot = ft;
    luaL_setmetatable(L, FTTYPE_MT);

    lua_pushlightuserdata(L, ft);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);   /* wrappers[ft] = wrapper */
    lua_remove(L, -2);   /* the wrapper cache table */
}
