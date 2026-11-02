/* Proxy identity cache, lifetime bookkeeping and the generic proxy vtable.
 * Port of src/proxy.c with the CPython-specific machinery (refcounts, cyclic
 * GC traverse/clear) replaced by registry-resident Lua tables:
 *   PROXY_CACHE - weak-valued: integer(address) -> base proxy userdata
 *   SLOT_REFS   - strong: integer(C slot address) -> proxy userdata, so a C
 *                 pointer field keeps the proxy (and thus its chunk) alive;
 *                 maintained by the liballocs GC-policy callbacks
 *   DEFER_LIST  - strong array parking proxies doomed inside a foreign call
 * Host-C reference cycles are uncollectable in this model (Lua has no
 * traverse/clear protocol for foreign memory); see the plan for the accepted
 * v1 policy and the release() escape hatch. */

#include "luaproxy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

lua_State *lp_globalL = NULL;

static int lp_cache_ref = LUA_NOREF;      /* PROXY_CACHE */
static int lp_slot_refs_ref = LUA_NOREF;  /* SLOT_REFS */
static int lp_defer_list_ref = LUA_NOREF; /* DEFER_LIST */
static int lp_policy_id = -1;
static int lp_defer_depth = 0;

static lua_Integer addr_key(const void *p) { return (lua_Integer)(uintptr_t)p; }

static void push_cache(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, lp_cache_ref); }
static void push_slot_refs(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, lp_slot_refs_ref); }
static void push_defer_list(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, lp_defer_list_ref); }

/* Push PROXY_CACHE[addr]; true iff a live proxy was found (else nothing
 * remains pushed). Weak values may linger as dead-but-unswept entries only
 * between clearing and sweeping inside the GC; from the API view a fetched
 * value is always a valid userdata. */
static bool cache_get(lua_State *L, const void *addr)
{
    push_cache(L);
    if (lua_rawgeti(L, -1, addr_key(addr)) == LUA_TUSERDATA)
    {
        lua_remove(L, -2);
        return true;
    }
    lua_pop(L, 2);
    return false;
}

/* ---- liballocs GC-policy callbacks -------------------------------------- */
/* Fire on every tracked pointer write in policy-attached memory. They receive
 * no context, hence lp_globalL. They must not raise and must tolerate being
 * re-entered through Lua GC steps triggered by their own table stores. */

static void lp_addref(const void *target, const void **from)
{
    lua_State *L = lp_globalL;
    if (!L || !lua_checkstack(L, 4)) return;
    /* SLOT_REFS[from] = PROXY_CACHE[target].
     * Unlike the Python port we tolerate an unknown target: only registered
     * base proxies are kept alive by C slots. */
    if (!cache_get(L, target)) return;
    push_slot_refs(L);
    lua_insert(L, -2);
    lua_rawseti(L, -2, addr_key(from));
    lua_pop(L, 1);
}

static void lp_delref(const void *target, const void **from)
{
    (void)target; /* spurious calls from inexistent locations are expected */
    lua_State *L = lp_globalL;
    if (!L || !lua_checkstack(L, 4)) return;
    push_slot_refs(L);
    /* Inside a foreign-call defer window, park the proxy this slot points at
     * so dropping the slot ref cannot free it mid-call: the callee may be
     * returning that very chunk (struct_global_swap). */
    if (lp_defer_depth > 0)
    {
        if (lua_rawgeti(L, -1, addr_key(from)) == LUA_TUSERDATA)
        {
            push_defer_list(L);
            lua_insert(L, -2);
            lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
        }
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    lua_rawseti(L, -2, addr_key(from));
    lua_pop(L, 1);
}

void LuaProxy_BeginDeferFrees(void)
{
    ++lp_defer_depth;
}

void LuaProxy_EndDeferFrees(lua_State *L)
{
    if (--lp_defer_depth == 0)
    {
        /* Replace the defer list wholesale: any parked proxy not re-adopted
         * by the return-value wrapper loses its last strong ref here and dies
         * at the next collection. */
        lua_newtable(L);
        lua_rawseti(L, LUA_REGISTRYINDEX, lp_defer_list_ref);
    }
}

void LuaProxy_InitGCPolicy(lua_State *L)
{
    if (lp_cache_ref != LUA_NOREF) return; /* already initialised */

    lua_newtable(L);                   /* PROXY_CACHE */
    lua_createtable(L, 0, 1);          /* its metatable */
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");     /* weak values: cache never keeps a proxy alive */
    lua_setmetatable(L, -2);
    lp_cache_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    lp_slot_refs_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_newtable(L);
    lp_defer_list_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lp_policy_id = __liballocs_register_gc_policy(lp_addref, lp_delref);
}

/* ---- proxy userdata ----------------------------------------------------- */

LuaProxy *LuaProxy_Untyped(lua_State *L, int idx)
{
    if (lua_type(L, idx) != LUA_TUSERDATA) return NULL;
    if (!lua_getmetatable(L, idx)) return NULL;
    lua_pushstring(L, "__luaproxy_ft");
    lua_rawget(L, -2);
    bool is_proxy = lua_islightuserdata(L, -1);
    lua_pop(L, 2);
    return is_proxy ? (LuaProxy *)lua_touserdata(L, idx) : NULL;
}

/* __gc: mirror of registration. A registered base proxy detaches the lifetime
 * policy / handle refcount it owns (freeing the chunk when ours was the last
 * policy); derived proxies have nothing to do (their base is kept alive via
 * uservalue 1). The pr_registered flag decides -- the weak cache cannot, since
 * its value for a finalizable object is cleared before __gc runs. Idempotent;
 * must never raise. */
int LuaProxy_GC(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    if (!p || !p->p_ptr) return 0;

    if (p->pr_registered)
    {
        if (ss_is_handle(p->p_ptr))
            ss_dec_refcount(p->p_ptr);
        else
            __liballocs_detach_lifetime_policy(lp_policy_id, ss_translate(p->p_ptr));
        /* The weak cache entry is usually cleared already; remove it only when
         * it still maps to this very proxy (mirrors Proxy_Unregister's guard). */
        push_cache(L);
        lua_rawgeti(L, -1, addr_key(p->p_ptr));
        if (lua_rawequal(L, -1, 1))
        {
            lua_pushnil(L);
            lua_rawseti(L, -3, addr_key(p->p_ptr));
        }
        lua_pop(L, 2);
        p->pr_registered = false;
    }
    p->p_ptr = NULL;
    return 0;
}

static int luaproxy_eq(lua_State *L)
{
    LuaProxy *a = LuaProxy_Untyped(L, 1);
    LuaProxy *b = LuaProxy_Untyped(L, 2);
    lua_pushboolean(L, a && b && a->p_ptr == b->p_ptr && a->pr_type == b->pr_type);
    return 1;
}

void LuaProxy_CreateMetatable(lua_State *L, LuaForeignType *ft, const luaL_Reg *extra)
{
    char name[32];
    snprintf(name, sizeof name, "luaproxy:%p", (void *)ft);
    ft->ft_mtname = strdup(name);

    luaL_newmetatable(L, ft->ft_mtname);
    lua_pushlightuserdata(L, ft);
    lua_setfield(L, -2, "__luaproxy_ft");
    const char *tyname = UNIQTYPE_NAME(ft->ft_type);
    if (tyname)
    {
        lua_pushstring(L, tyname);
        lua_setfield(L, -2, "__name");
    }
    lua_pushcfunction(L, LuaProxy_GC);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, luaproxy_eq);
    lua_setfield(L, -2, "__eq");
    for (; extra && extra->name; ++extra)
    {
        lua_pushcfunction(L, extra->func);
        lua_setfield(L, -2, extra->name);
    }
    lua_pop(L, 1);
}

LuaProxy *LuaProxy_PushNew(lua_State *L, void *addr, LuaForeignType *ft)
{
    LuaProxy *p = (LuaProxy *)lua_newuserdatauv(L, sizeof(LuaProxy), 1);
    p->p_ptr = addr;
    p->pr_type = ft;
    p->ap_length = 0;
    p->pr_registered = false;
    luaL_setmetatable(L, ft->ft_mtname);
    return p;
}

void LuaProxy_Register(lua_State *L, int proxy_idx)
{
    proxy_idx = lua_absindex(L, proxy_idx);
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, proxy_idx);
    assert(p && p->p_ptr);

    push_cache(L);
    lua_pushvalue(L, proxy_idx);
    lua_rawseti(L, -2, addr_key(p->p_ptr));
    lua_pop(L, 1);
    p->pr_registered = true;

    /* Lua's interest in the pointee: a handle refcount for Alaska-backed
     * objects, else the liballocs lifetime policy (whose callbacks maintain
     * SLOT_REFS on tracked pointer writes). */
    if (ss_is_handle(p->p_ptr))
        ss_inc_refcount(p->p_ptr);
    else
        __liballocs_attach_lifetime_policy(lp_policy_id, ss_translate(p->p_ptr));
}

bool LuaProxy_GetOrCreateBase(lua_State *L, void *addr)
{
    /* ft_getfrom of the base type re-enters us; break the recursion so it
     * builds a plain unregistered proxy that we then register. */
    static bool creating_base = false;
    if (creating_base) return false;

    struct allocator *allocator = NULL;
    const void *alloc_start = NULL;
    struct uniqtype *alloc_type = NULL;
    void *raw = ss_translate(addr);
    struct liballocs_err *err = __liballocs_get_alloc_info(raw, &allocator,
            &alloc_start, NULL, &alloc_type, NULL);
    if (err || !allocator || !ALLOCATOR_HANDLE_LIFETIME_INSERT(allocator) || !alloc_type)
        return false;

    /* liballocs answers in raw address space; recover the base addresslike. */
    void *base = (char *)addr - ((char *)raw - (char *)alloc_start);

    if (cache_get(L, base)) return true;

    LuaForeignType *ft = LuaForeignType_GetOrCreate(L, alloc_type);
    if (!ft) { lua_pop(L, 1); return false; } /* pop error message */
    if (!ft->ft_mtname) return false;         /* no proxy kind for this type */

    creating_base = true;
    int r = ft->ft_getfrom(L, base, ft);
    creating_base = false;
    if (r) { lua_pop(L, 1); return false; }

    LuaProxy_Register(L, -1);
    return true;
}

/* ---- generic vtable for proxy-kinded types ------------------------------ */

int LuaProxy_GetFrom(lua_State *L, void *data, LuaForeignType *ft)
{
    if (LuaProxy_GetOrCreateBase(L, data))
    {
        LuaProxy *base = (LuaProxy *)lua_touserdata(L, -1);
        if (base->pr_type == ft && base->p_ptr == data)
            return 0; /* the base proxy is the result */

        /* Derived/interior proxy: anchor the base in uservalue 1. */
        LuaProxy_PushNew(L, data, ft);
        lua_insert(L, -2);
        lua_setiuservalue(L, -2, 1);
        return 0;
    }
    LuaProxy_PushNew(L, data, ft);
    return 0;
}

int LuaProxy_CopyFrom(lua_State *L, void *data, LuaForeignType *ft)
{
    size_t size = UNIQTYPE_SIZE_IN_BYTES(ft->ft_type);
    void *chunk = totally_malloc(size);
    if (!chunk)
    {
        lua_pushstring(L, "out of memory copying foreign value");
        return -1;
    }
    __liballocs_set_alloc_type(ss_translate(chunk), ft->ft_type);
    memcpy(ss_translate(chunk), ss_translate(data), size);
    LuaProxy_PushNew(L, chunk, ft);
    LuaProxy_Register(L, -1);
    totally_free(chunk); /* the proxy's policy reference keeps the chunk alive */
    return 0;
}

/* Accept a proxy whose type is ft, or whose type leads to ft through the
 * offset-0 first-member chain (C struct inheritance: a derived proxy is
 * acceptable where the base is expected). */
bool LuaProxy_TypeAcceptable(LuaForeignType *have, LuaForeignType *want)
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

int LuaProxy_StoreInto(lua_State *L, int idx, void *dest, LuaForeignType *ft)
{
    idx = lua_absindex(L, idx);
    LuaProxy *p = LuaProxy_Untyped(L, idx);
    if (p && LuaProxy_TypeAcceptable(p->pr_type, ft))
    {
        memcpy(ss_translate(dest), ss_translate(p->p_ptr),
                UNIQTYPE_SIZE_IN_BYTES(ft->ft_type));
        return 0;
    }

    /* Try to convert the value through the type's constructor. */
    if (ft->ft_constructor)
    {
        int top = lua_gettop(L);
        lua_pushvalue(L, idx);
        if (ft->ft_constructor(L, top + 1, ft) == 0)
        {
            LuaProxy *tmp = (LuaProxy *)lua_touserdata(L, -1);
            memcpy(ss_translate(dest), ss_translate(tmp->p_ptr),
                    UNIQTYPE_SIZE_IN_BYTES(ft->ft_type));
            lua_settop(L, top);
            return 0;
        }
        /* constructor pushed an error message; drop the copied value below it */
        lua_remove(L, top + 1);
        return -1;
    }

    lua_pushfstring(L, "expected value of foreign type '%s', got %s",
            UNIQTYPE_NAME(ft->ft_type), luaL_typename(L, idx));
    return -1;
}

void *LuaProxy_GetDataPtr(lua_State *L, int idx, LuaForeignType *ft)
{
    LuaProxy *p = LuaProxy_Untyped(L, idx);
    if (!p || !LuaProxy_TypeAcceptable(p->pr_type, ft)) return NULL;
    return p->p_ptr;
}

/* Recursive walk clearing every tracked pointer slot in a region (the manual
 * counterpart of the Python port's Proxy_ClearRef traversal): drop the strong
 * SLOT_REFS entry and NULL the C slot so a later __notify_free cannot follow
 * a dangling field. */
static void clear_tracked_slots(void *region, struct uniqtype *t)
{
    if (!t) return;
    if (UNIQTYPE_IS_POINTER_TYPE(t))
    {
        const void **slot = (const void **)region;
        if (*slot)
        {
            lp_delref(NULL, slot);
            *slot = NULL;
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

int LuaProxy_Release(lua_State *L)
{
    LuaProxy *p = LuaProxy_Untyped(L, 1);
    if (!p) return luaL_error(L, "release: not a foreign proxy");
    if (!p->p_ptr) return 0; /* already released/collected */

    void *raw = ss_translate(p->p_ptr);
    struct uniqtype *t = ss_alloc_get_type(p->p_ptr);
    if (!t) t = (struct uniqtype *)p->pr_type->ft_type;
    clear_tracked_slots(raw, t);

    if (p->pr_registered)
    {
        if (ss_is_handle(p->p_ptr))
            ss_dec_refcount(p->p_ptr);
        else
            __liballocs_detach_lifetime_policy(lp_policy_id, ss_translate(p->p_ptr));
        push_cache(L);
        lua_rawgeti(L, -1, addr_key(p->p_ptr));
        if (lua_rawequal(L, -1, 1))
        {
            lua_pushnil(L);
            lua_rawseti(L, -3, addr_key(p->p_ptr));
        }
        lua_pop(L, 2);
        p->pr_registered = false;
    }
    /* The backing chunk may be gone now; poison the proxy so later access
     * raises instead of dereferencing freed memory. */
    p->p_ptr = NULL;
    return 0;
}

/* Port of Proxy_RetainStoredPtr: after a pointer store, keep the stored-from
 * proxy alive while the slot references it, covering values liballocs cannot
 * track (ffi closure trampolines in mmap'd memory). For liballocs-tracked
 * values the GC-policy addref already recorded a SLOT_REFS entry keyed by
 * `dest`; detect that and skip to avoid double-tracking. Only registered base
 * proxies are retained. */
void LuaProxy_RetainStoredPtr(lua_State *L, int val_idx, const void **dest, const void *val)
{
    if (!val) return;
    val_idx = lua_absindex(L, val_idx);
    LuaProxy *p = LuaProxy_Untyped(L, val_idx);
    if (!p || p->p_ptr != (void *)val) return;

    push_slot_refs(L);
    if (lua_rawgeti(L, -1, addr_key(dest)) != LUA_TNIL)
    {
        lua_pop(L, 2); /* the GC policy already manages this slot */
        return;
    }
    lua_pop(L, 1);

    /* Retain only when this proxy is the registered base for val. */
    if (cache_get(L, val))
    {
        if (lua_touserdata(L, -1) == (void *)p)
        {
            lua_rawseti(L, -2, addr_key(dest)); /* SLOT_REFS[dest] = proxy */
            lua_pop(L, 1);                      /* the SLOT_REFS table */
        }
        else
            lua_pop(L, 2); /* the fetched proxy + SLOT_REFS */
    }
    else
        lua_pop(L, 1); /* just SLOT_REFS (cache_get pushed nothing) */
}

void LuaProxy_SetupType(LuaForeignType *ft)
{
    ft->ft_constructor = NULL;
    ft->ft_getfrom = LuaProxy_GetFrom;
    ft->ft_copyfrom = LuaProxy_CopyFrom;
    ft->ft_storeinto = LuaProxy_StoreInto;
    ft->ft_getdataptr = LuaProxy_GetDataPtr;
}
