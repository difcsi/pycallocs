/* Struct/union proxies: port of src/composite_proxy.c. Field access goes
 * through __index/__newindex metamethods dispatching over a per-type field
 * table (the CPython version uses a PyGetSetDef array; the logic is the same:
 * field address = ss_translate(p_ptr) + offset, then the field type's
 * ft_getfrom / ft_storeinto). The offset-0 first-member chain is walked on
 * lookup miss, giving the same visible behaviour as the Python port's
 * tp_base "struct inheritance". */

#include "luaproxy.h"

#include <assert.h>
#include <string.h>

struct lp_field_info {
    LuaForeignType *type; /* NULL when the field's type is not handled */
    size_t offset;
    const char *name;
};

typedef struct LuaCompositeType {
    LuaForeignType ft;
    unsigned nfields;
    struct lp_field_info *fields;
} LuaCompositeType;

/* Find `key` in ct's fields, following the offset-0 first-member chain on
 * miss. On success returns the field and sets *extra_off to the accumulated
 * offset of the composite level the field was found in (always 0 in practice
 * since the chain only follows offset-0 members, but kept for clarity). */
static struct lp_field_info *find_field(LuaCompositeType *ct, const char *key,
        size_t *extra_off)
{
    *extra_off = 0;
    while (ct)
    {
        for (unsigned i = 0; i < ct->nfields; ++i)
            if (ct->fields[i].name && strcmp(ct->fields[i].name, key) == 0)
                return &ct->fields[i];

        /* First-member "inheritance": retry in the offset-0 leading composite */
        if (ct->nfields > 0 && ct->fields[0].offset == 0 && ct->fields[0].type
                && UNIQTYPE_IS_COMPOSITE_TYPE(ct->fields[0].type->ft_type))
            ct = (LuaCompositeType *)ct->fields[0].type;
        else
            ct = NULL;
    }
    return NULL;
}

bool LuaCompositeProxy_FindField(LuaForeignType *composite_ft, const char *key,
        LuaForeignType **out_ft, size_t *out_off)
{
    size_t extra_off;
    struct lp_field_info *f =
        find_field((LuaCompositeType *)composite_ft, key, &extra_off);
    if (!f) return false;
    *out_ft = f->type;
    *out_off = extra_off + f->offset;
    return true;
}

static int compositeproxy_index(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    LuaCompositeType *ct = (LuaCompositeType *)p->pr_type;
    const char *key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : NULL;
    if (!key)
        return luaL_error(L, "foreign struct fields are accessed by name");
    if (strcmp(key, "release") == 0)
    {
        lua_pushcfunction(L, LuaProxy_Release);
        return 1;
    }
    if (!p->p_ptr)
        return luaL_error(L, "access to a released foreign proxy");

    size_t extra_off;
    struct lp_field_info *f = find_field(ct, key, &extra_off);
    if (!f)
        return luaL_error(L, "no field '%s' in foreign type '%s'",
                key, UNIQTYPE_NAME(ct->ft.ft_type));
    if (!f->type)
        return luaL_error(L, "field '%s': foreign datatype is not handled", key);

    void *field = (char *)ss_translate(p->p_ptr) + extra_off + f->offset;
    if (f->type->ft_getfrom(L, field, f->type)) return lua_error(L);
    return 1;
}

static int compositeproxy_newindex(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    LuaCompositeType *ct = (LuaCompositeType *)p->pr_type;
    const char *key = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : NULL;
    if (!key)
        return luaL_error(L, "foreign struct fields are accessed by name");
    if (!p->p_ptr)
        return luaL_error(L, "access to a released foreign proxy");

    size_t extra_off;
    struct lp_field_info *f = find_field(ct, key, &extra_off);
    if (!f)
        return luaL_error(L, "no field '%s' in foreign type '%s'",
                key, UNIQTYPE_NAME(ct->ft.ft_type));
    if (!f->type)
        return luaL_error(L, "field '%s': foreign datatype is not handled", key);
    if (!LuaForeignType_IsTriviallyCopiable(f->type))
        return luaL_error(L, "cannot assign to field '%s' (not trivially copiable)", key);

    void *field = (char *)ss_translate(p->p_ptr) + extra_off + f->offset;
    if (f->type->ft_storeinto(L, 3, field, f->type)) return lua_error(L);
    return 0;
}

static int compositeproxy_tostring(lua_State *L)
{
    LuaProxy *p = (LuaProxy *)lua_touserdata(L, 1);
    LuaCompositeType *ct = (LuaCompositeType *)p->pr_type;
    if (!p->p_ptr)
    {
        lua_pushstring(L, "<released foreign proxy>");
        return 1;
    }
    luaL_checkstack(L, 8, "nested struct tostring");

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    luaL_addchar(&b, '(');
    luaL_addstring(&b, UNIQTYPE_NAME(ct->ft.ft_type));
    luaL_addstring(&b, "){");
    bool first = true;
    for (unsigned i = 0; i < ct->nfields; ++i)
    {
        struct lp_field_info *f = &ct->fields[i];
        if (!f->type) continue; /* skip fields with unhandled types */
        if (!first) luaL_addstring(&b, ", ");
        first = false;
        luaL_addstring(&b, f->name);
        luaL_addstring(&b, ": ");
        void *field = (char *)ss_translate(p->p_ptr) + f->offset;
        if (f->type->ft_getfrom(L, field, f->type)) return lua_error(L);
        luaL_tolstring(L, -1, NULL); /* value -> its display string (on top) */
        lua_remove(L, -2);           /* drop the raw value: luaL_addvalue allows
                                        exactly one extra stack slot */
        luaL_addvalue(&b);
    }
    luaL_addchar(&b, '}');
    luaL_pushresult(&b);
    return 1;
}

/* Constructor: T() zero-initialises, T(proxy) copies, T{f=...} initialises
 * fields by name, T(v1, v2, ...) initialises fields in declaration order.
 * Pushes the new registered proxy. */
static int compositeproxy_ctor(lua_State *L, int first_arg, LuaForeignType *ft)
{
    LuaCompositeType *ct = (LuaCompositeType *)ft;
    size_t size = UNIQTYPE_SIZE_IN_BYTES(ft->ft_type);
    int nargs = lua_gettop(L) - first_arg + 1;
    if (nargs < 0) nargs = 0;

    void *chunk = totally_malloc(size);
    if (!chunk)
    {
        lua_pushstring(L, "out of memory constructing foreign struct");
        return -1;
    }
    __liballocs_set_alloc_type(ss_translate(chunk), ft->ft_type);
    LuaProxy *p = LuaProxy_PushNew(L, chunk, ft);
    int proxy_idx = lua_gettop(L);
    LuaProxy_Register(L, proxy_idx);
    totally_free(chunk); /* the proxy's policy reference keeps the chunk alive */

    void *raw = ss_translate(p->p_ptr);

    if (nargs == 0)
    {
        memset(raw, 0, size);
        return 0;
    }

    if (nargs == 1)
    {
        /* Copy construction from a same-type (or derived) proxy */
        LuaProxy *other = LuaProxy_Untyped(L, first_arg);
        if (other && other->pr_type == ft)
        {
            if (other->p_ptr == p->p_ptr)
            {
                lua_remove(L, proxy_idx);
                lua_pushstring(L, "trying to copy inside yourself");
                return -1;
            }
            memcpy(raw, ss_translate(other->p_ptr), size);
            return 0;
        }

        /* Table argument: initialise fields by name (absent fields -> zero) */
        if (lua_type(L, first_arg) == LUA_TTABLE)
        {
            for (unsigned i = 0; i < ct->nfields; ++i)
            {
                struct lp_field_info *f = &ct->fields[i];
                if (!f->type) continue;
                lua_getfield(L, first_arg, f->name);
                if (!lua_isnil(L, -1))
                {
                    if (f->type->ft_storeinto(L, -1, (char *)raw + f->offset, f->type))
                    {
                        lua_remove(L, -2);        /* the fetched value */
                        lua_remove(L, proxy_idx); /* the half-built proxy */
                        return -1;
                    }
                }
                else
                    memset((char *)raw + f->offset, 0,
                            UNIQTYPE_SIZE_IN_BYTES(f->type->ft_type));
                lua_pop(L, 1);
            }
            return 0;
        }
    }

    /* Positional arguments: one per field in declaration order */
    if ((unsigned)nargs > ct->nfields)
    {
        lua_remove(L, proxy_idx);
        lua_pushfstring(L, "too many arguments (foreign type '%s' has %d fields)",
                UNIQTYPE_NAME(ft->ft_type), ct->nfields);
        return -1;
    }
    for (unsigned i = 0; i < ct->nfields; ++i)
    {
        struct lp_field_info *f = &ct->fields[i];
        if (!f->type)
        {
            if ((int)i < nargs)
            {
                lua_remove(L, proxy_idx);
                lua_pushfstring(L, "field '%s' has an unhandled foreign datatype",
                        ct->fields[i].name);
                return -1;
            }
            continue;
        }
        if ((int)i < nargs)
        {
            if (f->type->ft_storeinto(L, first_arg + i, (char *)raw + f->offset, f->type))
            {
                lua_remove(L, proxy_idx);
                return -1;
            }
        }
        else
            memset((char *)raw + f->offset, 0, UNIQTYPE_SIZE_IN_BYTES(f->type->ft_type));
    }
    return 0;
}

static const luaL_Reg composite_metamethods[] = {
    {"__index", compositeproxy_index},
    {"__newindex", compositeproxy_newindex},
    {"__tostring", compositeproxy_tostring},
    {NULL, NULL}
};

/* Two-phase creation (mirrors CompositeProxy_NewType/InitType): NewType only
 * allocates the type and its field slots so the memo entry can exist before
 * we recurse into member types; InitType resolves members and builds the
 * metatable. Field types stay NULL when unhandled (access raises). */
LuaForeignType *LuaCompositeProxy_NewType(lua_State *L, const struct uniqtype *type)
{
    (void)L;
    unsigned nfields = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
    const char **field_names = UNIQTYPE_COMPOSITE_SUBOBJ_NAMES(type);
    if (!field_names) nfields = 0; /* incomplete struct */

    LuaCompositeType *ct = calloc(1, sizeof *ct);
    ct->ft.ft_type = type;
    LuaProxy_SetupType(&ct->ft);
    ct->ft.ft_constructor = compositeproxy_ctor;
    ct->nfields = nfields;
    ct->fields = calloc(nfields ? nfields : 1, sizeof *ct->fields);
    for (unsigned i = 0; i < nfields; ++i)
        ct->fields[i].name = field_names[i];
    return &ct->ft;
}

void LuaCompositeProxy_InitType(lua_State *L, LuaForeignType *ft)
{
    LuaCompositeType *ct = (LuaCompositeType *)ft;
    const struct uniqtype *type = ft->ft_type;

    for (unsigned i = 0; i < ct->nfields; ++i)
    {
        const struct uniqtype_rel_info *rel = &type->related[i];
        ct->fields[i].offset = rel->un.memb.off;
        ct->fields[i].type = LuaForeignType_GetOrCreate(L, rel->un.memb.ptr);
        if (!ct->fields[i].type) lua_pop(L, 1); /* drop error msg; field stays invalid */
    }

    LuaProxy_CreateMetatable(L, ft, composite_metamethods);
}
