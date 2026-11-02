/* Pointer and array proxies: port of src/address_proxy.c. One shared proxy
 * shape serves both kinds (LuaAddressType; arrays alias the pointer-to-element
 * metatable), giving:
 *   p[i], p[i] = v   1-based element access (stores only for trivially
 *                    copiable element types)
 *   #p               inferred or static element count
 *   p:sub(i, j)      interior slice proxy (1-based, inclusive), anchored to p
 *   p:str()          the pointee bytes as a Lua string (char elements)
 *   p.field          direct field access when the pointee is a composite
 *                    (mirror of the Python port's tp_base subclassing)
 * Length inference for bare T*: liballocs bounds via minicrunch, then a
 * NUL-scan for char pointees (capped by whatever bound we do have), then 1
 * (interior pointer to a single element). */

#include "luaproxy.h"

#include <assert.h>
#include <string.h>

/* liballocs lifetime hook, added by the trapptrwrites CIL pass in fixture
 * code; here declared by hand exactly like the Python port. */
void __notify_ptr_write(const void **dest, const void *val);

static LuaAddressType *addr_type(LuaProxy *p)
{
    return (LuaAddressType *)p->pr_type;
}

static size_t elem_size(LuaAddressType *at)
{
    return at->pointee_type ? UNIQTYPE_SIZE_IN_BYTES(at->pointee_type->ft_type) : 0;
}

/* ---- length inference ---------------------------------------------------- */

static void addrproxy_initlength(LuaProxy *p, LuaAddressType *at)
{
    /* Degraded proxy class (pointee kind unhandled): length 0, no indexing. */
    if (!at->pointee_type)
    {
        p->ap_length = 0;
        return;
    }
    const struct uniqtype *ptyp = at->pointee_type->ft_type;
    if (UNIQTYPE_SIZE_IN_BYTES(ptyp) == 0 || !UNIQTYPE_HAS_KNOWN_LENGTH(ptyp))
    {
        p->ap_length = 0;
        return;
    }

    void *ptr = ss_translate(p->p_ptr);
    Bounds bounds = __fetch_bounds_internal(ptr, ptyp);
    unsigned long esz = UNIQTYPE_SIZE_IN_BYTES(ptyp);

    /* (Alaska's chunk-bounds fallback from the Python port is omitted: the
     * v1 Lua build never runs with SS_HAVE_ALASKA.) */
    bool liballocs_exact = ((void *)bounds.base == ptr && bounds.size != 1);

    if ((void *)bounds.base == ptr && bounds.size != 1)
        p->ap_length = bounds.size / esz;
    else if (LuaForeignBaseType_IsChar(ptyp) && esz == 1)
    {
        /* A char pointer whose exact extent liballocs could not determine:
         * its C length is its NUL terminator. Cap the scan by whatever upper
         * bound we do have so it never reads past the containing allocation;
         * with no bound at all, do not guess. */
        unsigned long cap = 0;
        if (bounds.base && bounds.size > 1
                && (char *)ptr >= (char *)bounds.base
                && (char *)ptr < (char *)bounds.base + bounds.size)
            cap = bounds.size - (unsigned long)((char *)ptr - (char *)bounds.base);
        if (cap)
        {
            unsigned long n = 0;
            while (n < cap && ((const char *)ptr)[n] != '\0') ++n;
            p->ap_length = n;
        }
        else p->ap_length = 1;
    }
    else
        p->ap_length = 1; /* interior pointer to a single element */

    (void)liballocs_exact;
}

/* ---- element access ------------------------------------------------------ */

static int addrproxy_getitem(lua_State *L, LuaProxy *p, lua_Integer i)
{
    LuaAddressType *at = addr_type(p);
    if (!p->p_ptr)
        return luaL_error(L, "access to a released foreign proxy");
    if (!at->pointee_type)
        return luaL_error(L, "pointee foreign datatype is not handled");
    if (i < 1 || (size_t)i > p->ap_length)
    {
        /* Out-of-range reads yield nil (not an error) so ipairs/generic-for
         * terminate naturally -- the Lua counterpart of Python's IndexError
         * driving the sequence protocol. Writes out of range still raise. */
        lua_pushnil(L);
        return 1;
    }
    void *item = (char *)ss_translate(p->p_ptr) + (i - 1) * elem_size(at);
    if (at->pointee_type->ft_getfrom(L, item, at->pointee_type)) return lua_error(L);
    return 1;
}

static int addrproxy_setitem(lua_State *L, LuaProxy *p, lua_Integer i, int validx)
{
    LuaAddressType *at = addr_type(p);
    if (!p->p_ptr)
        return luaL_error(L, "access to a released foreign proxy");
    if (!at->pointee_type)
        return luaL_error(L, "pointee foreign datatype is not handled");
    if (!LuaForeignType_IsTriviallyCopiable(at->pointee_type))
        return luaL_error(L, "cannot assign elements of this type (not trivially copiable)");
    if (i < 1 || (size_t)i > p->ap_length)
        return luaL_error(L, "index %I out of range (length %I)",
                (LUAI_UACINT)i, (LUAI_UACINT)p->ap_length);
    void *item = (char *)ss_translate(p->p_ptr) + (i - 1) * elem_size(at);
    if (at->pointee_type->ft_storeinto(L, validx, item, at->pointee_type))
        return lua_error(L);
    return 0;
}

/* p:sub(i, j): interior slice [i..j] (1-based, inclusive) as a new derived
 * proxy anchored to p through uservalue 1. */
static int addrproxy_sub(lua_State *L)
{
    LuaProxy *p = LuaProxy_Untyped(L, 1);
    if (!p) return luaL_error(L, "sub: not an address proxy");
    LuaAddressType *at = addr_type(p);
    lua_Integer i = luaL_checkinteger(L, 2);
    lua_Integer j = luaL_optinteger(L, 3, (lua_Integer)p->ap_length);
    if (i < 1) i = 1;
    if (j > (lua_Integer)p->ap_length) j = (lua_Integer)p->ap_length;
    if (j < i) j = i - 1; /* empty slice */

    LuaProxy *slice = LuaProxy_PushNew(L,
            (char *)p->p_ptr + (i - 1) * elem_size(at), p->pr_type);
    slice->ap_length = (size_t)(j - i + 1);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1); /* keep the source proxy (and its chain) alive */
    return 1;
}

/* p:str(): the pointee bytes as a Lua string (char elements only). Trailing
 * NULs inside the counted range are trimmed for statically-sized char arrays,
 * matching what a C reader would consider "the string". */
static int addrproxy_str(lua_State *L)
{
    LuaProxy *p = LuaProxy_Untyped(L, 1);
    if (!p) return luaL_error(L, "str: not an address proxy");
    LuaAddressType *at = addr_type(p);
    if (!at->pointee_type || !LuaForeignBaseType_IsChar(at->pointee_type->ft_type))
        return luaL_error(L, "str: only available for char pointees");
    const char *s = (const char *)ss_translate(p->p_ptr);
    size_t n = p->ap_length;
    while (n > 0 && s[n - 1] == '\0') --n;
    lua_pushlstring(L, s, n);
    return 1;
}

static int addrproxy_index(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        int isnum;
        lua_Integer i = lua_tointegerx(L, 2, &isnum);
        if (!isnum) return luaL_error(L, "invalid index");
        return addrproxy_getitem(L, p, i);
    }
    if (lua_type(L, 2) == LUA_TSTRING)
    {
        const char *key = lua_tostring(L, 2);
        if (strcmp(key, "sub") == 0) { lua_pushcfunction(L, addrproxy_sub); return 1; }
        if (strcmp(key, "str") == 0) { lua_pushcfunction(L, addrproxy_str); return 1; }
        if (strcmp(key, "release") == 0) { lua_pushcfunction(L, LuaProxy_Release); return 1; }
        if (!p->p_ptr)
            return luaL_error(L, "access to a released foreign proxy");

        /* Direct field access on a composite pointee (Python: the address
         * proxy class subclasses the pointee's composite class). */
        LuaAddressType *at = addr_type(p);
        if (at->pointee_type && UNIQTYPE_IS_COMPOSITE_TYPE(at->pointee_type->ft_type))
        {
            LuaForeignType *fft;
            size_t off;
            if (LuaCompositeProxy_FindField(at->pointee_type, key, &fft, &off))
            {
                if (!fft)
                    return luaL_error(L, "field '%s': foreign datatype is not handled", key);
                void *field = (char *)ss_translate(p->p_ptr) + off;
                if (fft->ft_getfrom(L, field, fft)) return lua_error(L);
                return 1;
            }
            return luaL_error(L, "no field '%s' in foreign type '%s'",
                    key, UNIQTYPE_NAME(at->pointee_type->ft_type));
        }
    }
    return luaL_error(L, "invalid index into address proxy");
}

static int addrproxy_newindex(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        int isnum;
        lua_Integer i = lua_tointegerx(L, 2, &isnum);
        if (!isnum) return luaL_error(L, "invalid index");
        return addrproxy_setitem(L, p, i, 3);
    }
    if (lua_type(L, 2) == LUA_TSTRING)
    {
        const char *key = lua_tostring(L, 2);
        LuaAddressType *at = addr_type(p);
        if (at->pointee_type && UNIQTYPE_IS_COMPOSITE_TYPE(at->pointee_type->ft_type))
        {
            LuaForeignType *fft;
            size_t off;
            if (LuaCompositeProxy_FindField(at->pointee_type, key, &fft, &off))
            {
                if (!fft)
                    return luaL_error(L, "field '%s': foreign datatype is not handled", key);
                if (!LuaForeignType_IsTriviallyCopiable(fft))
                    return luaL_error(L, "cannot assign to field '%s'", key);
                void *field = (char *)ss_translate(p->p_ptr) + off;
                if (fft->ft_storeinto(L, 3, field, fft)) return lua_error(L);
                return 0;
            }
            return luaL_error(L, "no field '%s' in foreign type '%s'",
                    key, UNIQTYPE_NAME(at->pointee_type->ft_type));
        }
    }
    return luaL_error(L, "invalid index into address proxy");
}

static int addrproxy_len(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    lua_pushinteger(L, (lua_Integer)p->ap_length);
    return 1;
}

static int addrproxy_tostring(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    LuaAddressType *at = addr_type(p);
    if (!p->p_ptr)
    {
        lua_pushstring(L, "<released foreign proxy>");
        return 1;
    }

    if (at->pointee_type && LuaForeignBaseType_IsChar(at->pointee_type->ft_type)
            && elem_size(at) == 1)
    {
        const char *s = (const char *)ss_translate(p->p_ptr);
        size_t n = p->ap_length;
        while (n > 0 && s[n - 1] == '\0') --n;
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        luaL_addstring(&b, "<'");
        luaL_addlstring(&b, s, n);
        luaL_addstring(&b, "'>");
        luaL_pushresult(&b);
        return 1;
    }

    luaL_checkstack(L, 8, "address proxy tostring");
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addstring(&b, "<[");
    for (size_t i = 0; i < p->ap_length; ++i)
    {
        if (i > 0) luaL_addstring(&b, ", ");
        void *item = (char *)ss_translate(p->p_ptr) + i * elem_size(at);
        if (at->pointee_type->ft_getfrom(L, item, at->pointee_type))
            return lua_error(L);
        luaL_tolstring(L, -1, NULL);
        lua_remove(L, -2);
        luaL_addvalue(&b);
    }
    luaL_addstring(&b, "]>");
    luaL_pushresult(&b);
    return 1;
}

static const luaL_Reg addr_metamethods[] = {
    {"__index", addrproxy_index},
    {"__newindex", addrproxy_newindex},
    {"__len", addrproxy_len},
    {"__tostring", addrproxy_tostring},
    {NULL, NULL}
};

/* ---- vtable: ADDRESS ----------------------------------------------------- */

static int addrproxy_getfrom(lua_State *L, void *data, LuaForeignType *ft)
{
    LuaAddressType *at = (LuaAddressType *)ft;
    void *ptr = *(void **)data; /* data is the address of a pointer */

    if (!ptr)
    {
        lua_pushnil(L);
        return 0;
    }

    /* Prefer returning the existing base proxy when that does not break
     * semantics (void pointee: always; else when type-compatible). */
    if (LuaProxy_GetOrCreateBase(L, ptr))
    {
        LuaProxy *base = (LuaProxy *)lua_touserdata(L, -1);
        if (at->pointee_type
                && UNIQTYPE_KIND(at->pointee_type->ft_type) == VOID)
            return 0;
        if (base->p_ptr == ptr && at->pointee_type
                && (base->pr_type->ft_mtname == ft->ft_mtname /* same proxy class */
                    || LuaProxy_TypeAcceptable(base->pr_type, at->pointee_type)))
            return 0;

        LuaProxy *p = LuaProxy_PushNew(L, ptr, ft);
        lua_insert(L, -2);
        lua_setiuservalue(L, -2, 1); /* anchor the base proxy */
        addrproxy_initlength(p, at);
        return 0;
    }

    LuaProxy *p = LuaProxy_PushNew(L, ptr, ft);
    addrproxy_initlength(p, at);
    return 0;
}

static bool addrproxy_typecheck(lua_State *L, int idx, LuaAddressType *at)
{
    LuaProxy *p = LuaProxy_Untyped(L, idx);
    if (!p) return false;

    /* Same proxy class: pointer/array/slice over the same element type. */
    if (p->pr_type->ft_mtname && p->pr_type->ft_mtname == at->ft.ft_mtname)
        return true;

    if (!at->pointee_type) return false;

    /* void pointee accepts any proxy */
    if (UNIQTYPE_KIND(at->pointee_type->ft_type) == VOID) return true;

    /* A proxy of the pointee type itself (or a struct derived from it). */
    if (LuaProxy_TypeAcceptable(p->pr_type, at->pointee_type)) return true;

    return false;
}

static int addrproxy_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)
{
    LuaAddressType *at = (LuaAddressType *)ft;

    /* nil -> NULL. Barriers are disjoint safe no-ops outside their domain. */
    if (lua_isnil(L, idx))
    {
        ss_dec_refcount(*(void **)dest);
        __notify_ptr_write((const void **)dest, NULL);
        *(void **)dest = NULL;
        return 0;
    }

    if (!addrproxy_typecheck(L, idx, at))
    {
        lua_pushfstring(L, "expected reference to object of type %s, got %s",
                at->pointee_type ? UNIQTYPE_NAME(at->pointee_type->ft_type) : "?",
                luaL_typename(L, idx));
        return -1;
    }

    LuaProxy *p = LuaProxy_Untyped(L, idx);
    void *val = p->p_ptr;
    /* The exact barrier ordering of the Python port (src/address_proxy.c):
     * drop the old pointee's refs, take the new one's, tell liballocs, then
     * store. */
    ss_dec_refcount(*(void **)dest);
    ss_inc_refcount(val);
    __notify_ptr_write((const void **)dest, val);
    *(void **)dest = val;
    if (!ss_is_handle(val))
        LuaProxy_RetainStoredPtr(L, idx, (const void **)dest, val);
    return 0;
}

static void *addrproxy_getdataptr(lua_State *L, int idx, LuaForeignType *ft)
{
    LuaAddressType *at = (LuaAddressType *)ft;

    if (lua_isnil(L, idx))
    {
        static void *np = 0;
        return &np;
    }

    if (addrproxy_typecheck(L, idx, at))
        return &((LuaProxy *)lua_touserdata(L, idx))->p_ptr;

    /* One-element pointer cell passed where T* is expected: hand over the
     * pointer stored IN the cell (p_ptr itself), so a cell can be a T**
     * out-param first and the resulting T* handle afterwards. */
    LuaProxy *p = LuaProxy_Untyped(L, idx);
    if (p && p->ap_length == 1)
    {
        const struct uniqtype *k = p->pr_type->ft_type;
        if (UNIQTYPE_KIND(k) == ADDRESS || UNIQTYPE_KIND(k) == ARRAY)
        {
            LuaAddressType *pat = (LuaAddressType *)p->pr_type;
            if (pat->pointee_type == (LuaForeignType *)at
                    || (pat->pointee_type && pat->pointee_type->ft_type == ft->ft_type))
                return p->p_ptr;
        }
    }
    return NULL;
}

LuaForeignType *LuaAddressProxy_NewType(lua_State *L, const struct uniqtype *type)
{
    (void)L;
    const struct uniqtype *pointee = type->related[0].un.t.ptr;
    if (!pointee)
    {
        lua_pushstring(L, "pointer type with no pointee");
        return NULL;
    }

    LuaAddressType *at = calloc(1, sizeof *at);
    at->ft.ft_type = type;
    at->ft.ft_getfrom = addrproxy_getfrom;
    at->ft.ft_copyfrom = addrproxy_getfrom;
    at->ft.ft_storeinto = addrproxy_storeinto;
    at->ft.ft_getdataptr = addrproxy_getdataptr;
    /* T_ptr(): a fresh NULL-initialised one-element pointer cell (a T* slot),
     * passable wherever C expects T** -- the out-parameter idiom. */
    at->ft.ft_constructor = LuaForeignType_CellCtor;
    return &at->ft;
}

void LuaAddressProxy_InitType(lua_State *L, LuaForeignType *ft)
{
    LuaAddressType *at = (LuaAddressType *)ft;
    const struct uniqtype *pointee = ft->ft_type->related[0].un.t.ptr;
    at->pointee_type = LuaForeignType_GetOrCreate(L, pointee);
    if (!at->pointee_type) lua_pop(L, 1); /* degraded (opaque, length-0) class */

    LuaProxy_CreateMetatable(L, ft, addr_metamethods);
}

/* ---- vtable: ARRAY ------------------------------------------------------- */

/* Fill [1..len] from a Lua sequence (or zero-init when tbl_idx == 0), zeroing
 * the tail. Mirrors addrproxy_init. */
static int array_fill(lua_State *L, LuaProxy *p, int tbl_idx)
{
    LuaAddressType *at = addr_type(p);
    size_t esz = elem_size(at);
    void *raw = ss_translate(p->p_ptr);

    if (!tbl_idx)
    {
        memset(raw, 0, p->ap_length * esz);
        return 0;
    }
    lua_Integer arglen = luaL_len(L, tbl_idx);
    if (arglen > (lua_Integer)p->ap_length)
    {
        lua_pushfstring(L, "sequence too long to be stored at this address (only %d items)",
                (int)p->ap_length);
        return -1;
    }
    for (lua_Integer i = 1; i <= arglen; ++i)
    {
        lua_geti(L, tbl_idx, i);
        int r = at->pointee_type->ft_storeinto(L, -1, (char *)raw + (i - 1) * esz,
                at->pointee_type);
        if (r) { lua_remove(L, -2); return -1; }
        lua_pop(L, 1);
    }
    if ((size_t)arglen < p->ap_length)
        memset((char *)raw + arglen * esz, 0, (p->ap_length - arglen) * esz);
    return 0;
}

/* Array construction from Lua: T_array(n) or T_array{v1, v2, ...}. */
static int arrayproxy_ctor(lua_State *L, int first_arg, LuaForeignType *ft)
{
    LuaAddressType *at = (LuaAddressType *)ft;
    if (!at->pointee_type)
    {
        lua_pushstring(L, "array element foreign datatype is not handled");
        return -1;
    }
    int nargs = lua_gettop(L) - first_arg + 1;
    if (nargs != 1)
    {
        lua_pushstring(L, "array constructor takes exactly one argument (length or sequence)");
        return -1;
    }

    lua_Integer len;
    int tbl_idx = 0;
    if (lua_type(L, first_arg) == LUA_TTABLE)
    {
        len = luaL_len(L, first_arg);
        tbl_idx = first_arg;
    }
    else if (lua_isinteger(L, first_arg))
        len = lua_tointeger(L, first_arg);
    else
    {
        lua_pushstring(L, "array constructor argument must be a table or an integer length");
        return -1;
    }
    if (len < 0)
    {
        lua_pushstring(L, "array length cannot be negative");
        return -1;
    }

    size_t esz = elem_size(at);
    bool is_char = LuaForeignBaseType_IsChar(at->pointee_type->ft_type);
    /* char arrays get one extra NUL element so C string readers stay in
     * bounds (mirror of the Python port's hack) */
    void *chunk = totally_malloc((len + (is_char ? 1 : 0)) * esz);
    if (!chunk)
    {
        lua_pushstring(L, "out of memory constructing foreign array");
        return -1;
    }
    if (is_char) memset((char *)chunk + len * esz, 0, esz);
    __liballocs_set_alloc_type(ss_translate(chunk),
            __liballocs_get_or_create_array_type(
                (struct uniqtype *)at->pointee_type->ft_type, (unsigned)len));

    LuaProxy *p = LuaProxy_PushNew(L, chunk, ft);
    p->ap_length = (size_t)len;
    LuaProxy_Register(L, -1);
    totally_free(chunk); /* kept alive by the proxy's policy reference */

    if (array_fill(L, p, tbl_idx))
    {
        lua_remove(L, -2); /* the half-built proxy below the error message */
        return -1;
    }
    return 0;
}

static int arrayproxy_getfrom(lua_State *L, void *data, LuaForeignType *ft)
{
    if (LuaProxy_GetFrom(L, data, ft)) return -1;
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, -1);
    if (UNIQTYPE_HAS_KNOWN_LENGTH(ft->ft_type))
        p->ap_length = UNIQTYPE_ARRAY_LENGTH(ft->ft_type);
    else
        addrproxy_initlength(p, (LuaAddressType *)ft);
    return 0;
}

static int arrayproxy_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)
{
    lua_pushstring(L, "cannot copy full arrays (for now)");
    return -1;
}

LuaForeignType *LuaArrayProxy_NewType(lua_State *L, const struct uniqtype *type)
{
    (void)L;
    LuaAddressType *at = calloc(1, sizeof *at);
    at->ft.ft_type = type;
    at->ft.ft_constructor = arrayproxy_ctor;
    at->ft.ft_getfrom = arrayproxy_getfrom;
    at->ft.ft_copyfrom = NULL;
    at->ft.ft_storeinto = arrayproxy_storeinto;
    at->ft.ft_getdataptr = LuaProxy_GetDataPtr;
    return &at->ft;
}

void LuaArrayProxy_InitType(lua_State *L, LuaForeignType *ft)
{
    LuaAddressType *at = (LuaAddressType *)ft;
    const struct uniqtype *elem = ft->ft_type->related[0].un.t.ptr;
    if (!elem) return;

    /* Arrays share the pointer-to-element proxy class: same metatable, same
     * pointee_type. This is what lets a length-1 array cell pass where C
     * expects a pointer to the element. */
    const struct uniqtype *addr_uniqtype = __liballocs_get_or_create_address_type(elem);
    LuaForeignType *addr_ft = addr_uniqtype
        ? LuaForeignType_GetOrCreate(L, addr_uniqtype) : NULL;
    if (!addr_ft)
    {
        if (addr_uniqtype) lua_pop(L, 1); /* error message from GetOrCreate */
        at->pointee_type = NULL;
        return;
    }
    at->pointee_type = ((LuaAddressType *)addr_ft)->pointee_type;
    at->ft.ft_mtname = addr_ft->ft_mtname; /* alias, do not free separately */
}
