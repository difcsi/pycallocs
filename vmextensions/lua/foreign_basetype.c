/* Scalar (BASE/ENUMERATION) marshalling: port of src/foreign_basetype.c.
 * Selection is by DWARF encoding x size. Scalars have no proxy type: they are
 * always converted by value (ft_mtname stays NULL).
 *
 * Numeric fidelity differences vs Python (which has bignums):
 *  - uint64 values above INT64_MAX are pushed as lua_Number (lossy above 2^53)
 *    and cannot be stored back exactly; stores accept non-negative lua_Integer
 *    only.
 *  - long double degrades through lua_Number (double).
 *  - _Complex values marshal as {re=..., im=...} tables. */

#include "luaproxy.h"

#include <dwarf.h>
#include <string.h>

#ifndef __STDC_NO_COMPLEX__
# include <complex.h>
#endif

static int bool_getfrom(lua_State *L, void *data, LuaForeignType *ft)
{
    (void)ft;
    lua_pushboolean(L, *(uint8_t *)data != 0);
    return 0;
}
static int bool_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)
{
    (void)ft;
    *(uint8_t *)dest = lua_toboolean(L, idx) ? 1 : 0;
    return 0;
}

/* Fetch an integer off the stack without raising: integers pass through,
 * floats with integral value convert, single-char strings give their byte
 * (mirroring Python's char_to_long acceptance). */
static bool tointeger_arg(lua_State *L, int idx, bool accept_char, lua_Integer *out)
{
    int isnum;
    lua_Integer i = lua_tointegerx(L, idx, &isnum);
    if (isnum) { *out = i; return true; }
    if (accept_char && lua_type(L, idx) == LUA_TSTRING)
    {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        if (len == 1) { *out = (unsigned char)s[0]; return true; }
    }
    return false;
}

#define DEFINE_UINT_GETSTORE(size) \
static int uint##size##_getfrom(lua_State *L, void *data, LuaForeignType *ft)\
{\
    (void)ft;\
    uint##size##_t u = *(uint##size##_t *)data;\
    if ((uint64_t)u > (uint64_t)LUA_MAXINTEGER)\
        lua_pushnumber(L, (lua_Number)u);\
    else\
        lua_pushinteger(L, (lua_Integer)u);\
    return 0;\
}\
static int uint##size##_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)\
{\
    (void)ft;\
    lua_Integer i;\
    if (!tointeger_arg(L, idx, false, &i))\
    {\
        lua_pushfstring(L, "expected an integer, got %s", luaL_typename(L, idx));\
        return -1;\
    }\
    if (i < 0 || (lua_Integer)(uint##size##_t)i != i)\
    {\
        lua_pushstring(L, "argument does not fit into a " #size " bit unsigned integer");\
        return -1;\
    }\
    *(uint##size##_t *)dest = (uint##size##_t)i;\
    return 0;\
}
DEFINE_UINT_GETSTORE(8)
DEFINE_UINT_GETSTORE(16)
DEFINE_UINT_GETSTORE(32)
DEFINE_UINT_GETSTORE(64)

#define DEFINE_INT_GETSTORE(size) \
static int int##size##_getfrom(lua_State *L, void *data, LuaForeignType *ft)\
{\
    (void)ft;\
    lua_pushinteger(L, (lua_Integer)*(int##size##_t *)data);\
    return 0;\
}\
static int int##size##_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)\
{\
    (void)ft;\
    lua_Integer i;\
    if (!tointeger_arg(L, idx, false, &i))\
    {\
        lua_pushfstring(L, "expected an integer, got %s", luaL_typename(L, idx));\
        return -1;\
    }\
    if ((lua_Integer)(int##size##_t)i != i)\
    {\
        lua_pushstring(L, "argument does not fit into a " #size " bit signed integer");\
        return -1;\
    }\
    *(int##size##_t *)dest = (int##size##_t)i;\
    return 0;\
}
DEFINE_INT_GETSTORE(8)
DEFINE_INT_GETSTORE(16)
DEFINE_INT_GETSTORE(32)
DEFINE_INT_GETSTORE(64)

/* Chars read as Lua strings of the char's byte width (like Python bytes);
 * stores accept a string of that exact length, or an integer code point. */
#define DEFINE_CHAR_GETSTORE(size) \
static int char##size##_getfrom(lua_State *L, void *data, LuaForeignType *ft)\
{\
    (void)ft;\
    lua_pushlstring(L, (const char *)data, size / 8);\
    return 0;\
}\
static int uchar##size##_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)\
{\
    if (lua_type(L, idx) == LUA_TSTRING)\
    {\
        size_t len;\
        const char *s = lua_tolstring(L, idx, &len);\
        if (len == size / 8) { memcpy(dest, s, size / 8); return 0; }\
        if (size != 8 || len != 1)\
        {\
            lua_pushfstring(L, "expected a string of length %d, got length %d",\
                    (int)(size / 8), (int)len);\
            return -1;\
        }\
    }\
    return uint##size##_storeinto(L, idx, dest, ft);\
}\
static int schar##size##_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)\
{\
    if (lua_type(L, idx) == LUA_TSTRING)\
        return uchar##size##_storeinto(L, idx, dest, ft);\
    return int##size##_storeinto(L, idx, dest, ft);\
}
DEFINE_CHAR_GETSTORE(8)
DEFINE_CHAR_GETSTORE(16)
DEFINE_CHAR_GETSTORE(32)
DEFINE_CHAR_GETSTORE(64)

#define DEFINE_FLOAT_GETSTORE(t) \
static int t##_getfrom(lua_State *L, void *data, LuaForeignType *ft)\
{\
    (void)ft;\
    lua_pushnumber(L, (lua_Number)*(t *)data);\
    return 0;\
}\
static int t##_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)\
{\
    (void)ft;\
    int isnum;\
    lua_Number f = lua_tonumberx(L, idx, &isnum);\
    if (!isnum)\
    {\
        lua_pushfstring(L, "expected a number, got %s", luaL_typename(L, idx));\
        return -1;\
    }\
    *(t *)dest = (t)f;\
    return 0;\
}
DEFINE_FLOAT_GETSTORE(float)
DEFINE_FLOAT_GETSTORE(double)
typedef long double long_double;
DEFINE_FLOAT_GETSTORE(long_double)

#ifndef __STDC_NO_COMPLEX__
/* Complex values marshal as {re=<num>, im=<num>} tables; stores also accept a
 * plain number (imaginary part 0). */
static bool tocomplex_arg(lua_State *L, int idx, double *re, double *im)
{
    int isnum;
    lua_Number f = lua_tonumberx(L, idx, &isnum);
    if (isnum) { *re = f; *im = 0; return true; }
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    lua_getfield(L, idx, "re");
    lua_getfield(L, idx, "im");
    int okre, okim;
    *re = lua_tonumberx(L, -2, &okre);
    *im = lua_tonumberx(L, -1, &okim);
    lua_pop(L, 2);
    return okre && okim;
}
#define DEFINE_COMPLEX_GETSTORE(t) \
static int t##_getfrom(lua_State *L, void *data, LuaForeignType *ft)\
{\
    (void)ft;\
    complex double z = (complex double)*(t *)data;\
    lua_createtable(L, 0, 2);\
    lua_pushnumber(L, creal(z));\
    lua_setfield(L, -2, "re");\
    lua_pushnumber(L, cimag(z));\
    lua_setfield(L, -2, "im");\
    return 0;\
}\
static int t##_storeinto(lua_State *L, int idx, void *dest, LuaForeignType *ft)\
{\
    (void)ft;\
    double re, im;\
    if (!tocomplex_arg(L, idx, &re, &im))\
    {\
        lua_pushfstring(L, "expected a number or {re=,im=} table, got %s",\
                luaL_typename(L, idx));\
        return -1;\
    }\
    *(t *)dest = (t)CMPLX(re, im);\
    return 0;\
}
typedef complex float cmplx_float;
DEFINE_COMPLEX_GETSTORE(cmplx_float)
typedef complex double cmplx_double;
DEFINE_COMPLEX_GETSTORE(cmplx_double)
typedef complex long double cmplx_long_double;
DEFINE_COMPLEX_GETSTORE(cmplx_long_double)
#endif

#define CHECK_SIZE(sz, getfrom, storeinto) \
if (size == sz)\
{\
    ft->ft_getfrom = getfrom;\
    ft->ft_copyfrom = getfrom;\
    ft->ft_storeinto = storeinto;\
    return 0;\
}

/* ft->ft_type must be of kind BASE. 0 on success, -1 with message pushed. */
int LuaForeignBaseType_Init(lua_State *L, LuaForeignType *ft)
{
    const struct uniqtype *type = ft->ft_type;
    unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);

    /* Bit fields are not handled (mirrors the Python port). */
    if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type) != 8 * size)
    {
        lua_pushstring(L, "unsupported foreign bit field type");
        return -1;
    }

    switch (type->un.base.enc)
    {
        case 0:
            /* liballocs' synthetic __uninterpreted_byte: treat a 1-byte
             * uninterpreted base like unsigned char so untyped heap buffers
             * still get working indexing. */
            CHECK_SIZE(1, char8_getfrom, uchar8_storeinto)
            break;

        case DW_ATE_boolean:
            ft->ft_getfrom = bool_getfrom;
            ft->ft_copyfrom = bool_getfrom;
            ft->ft_storeinto = bool_storeinto;
            return 0;

        case DW_ATE_address:
        case DW_ATE_unsigned:
            CHECK_SIZE(1, uint8_getfrom, uint8_storeinto)
            CHECK_SIZE(2, uint16_getfrom, uint16_storeinto)
            CHECK_SIZE(4, uint32_getfrom, uint32_storeinto)
            CHECK_SIZE(8, uint64_getfrom, uint64_storeinto)
            break;

        case DW_ATE_signed:
            CHECK_SIZE(1, int8_getfrom, int8_storeinto)
            CHECK_SIZE(2, int16_getfrom, int16_storeinto)
            CHECK_SIZE(4, int32_getfrom, int32_storeinto)
            CHECK_SIZE(8, int64_getfrom, int64_storeinto)
            break;

        case DW_ATE_unsigned_char:
            CHECK_SIZE(1, char8_getfrom, uchar8_storeinto)
            CHECK_SIZE(2, char16_getfrom, uchar16_storeinto)
            CHECK_SIZE(4, char32_getfrom, uchar32_storeinto)
            CHECK_SIZE(8, char64_getfrom, uchar64_storeinto)
            break;

        case DW_ATE_signed_char:
            CHECK_SIZE(1, char8_getfrom, schar8_storeinto)
            CHECK_SIZE(2, char16_getfrom, schar16_storeinto)
            CHECK_SIZE(4, char32_getfrom, schar32_storeinto)
            CHECK_SIZE(8, char64_getfrom, schar64_storeinto)
            break;

        case DW_ATE_float:
            CHECK_SIZE(4, float_getfrom, float_storeinto)
            CHECK_SIZE(8, double_getfrom, double_storeinto)
            CHECK_SIZE(16, long_double_getfrom, long_double_storeinto)
            break;

#ifndef __STDC_NO_COMPLEX__
        case DW_ATE_complex_float:
            CHECK_SIZE(8, cmplx_float_getfrom, cmplx_float_storeinto)
            CHECK_SIZE(16, cmplx_double_getfrom, cmplx_double_storeinto)
            CHECK_SIZE(32, cmplx_long_double_getfrom, cmplx_long_double_storeinto)
            break;
#endif

        default:
            break;
    }

    lua_pushfstring(L, "unsupported foreign base type '%s'", UNIQTYPE_NAME(type));
    return -1;
}

/* Enumerations are exposed as their underlying integer: reuse the recorded
 * base type's handlers when present, else fall back to a signed integer of
 * the enum's size (C enum constants have type int; signed is the safe default
 * so negative enumerators round-trip). */
int LuaForeignEnumType_Init(lua_State *L, LuaForeignType *ft)
{
    const struct uniqtype *type = ft->ft_type;
    const struct uniqtype *base = UNIQTYPE_ENUM_BASE_TYPE(type);
    if (base && UNIQTYPE_IS_BASE_TYPE(base))
    {
        /* The scalar handlers are size-specialised and never read ft_type, so
         * borrowing them while keeping ft_type = the enum is safe. */
        LuaForeignType tmp = *ft;
        tmp.ft_type = base;
        if (LuaForeignBaseType_Init(L, &tmp) < 0) return -1;
        ft->ft_getfrom = tmp.ft_getfrom;
        ft->ft_copyfrom = tmp.ft_copyfrom;
        ft->ft_storeinto = tmp.ft_storeinto;
        return 0;
    }

    unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
    CHECK_SIZE(1, int8_getfrom, int8_storeinto)
    CHECK_SIZE(2, int16_getfrom, int16_storeinto)
    CHECK_SIZE(4, int32_getfrom, int32_storeinto)
    CHECK_SIZE(8, int64_getfrom, int64_storeinto)

    lua_pushstring(L, "unsupported foreign enumeration size");
    return -1;
}

bool LuaForeignBaseType_IsChar(const struct uniqtype *type)
{
    if (!UNIQTYPE_IS_BASE_TYPE(type)) return false;
    switch (type->un.base.enc)
    {
        case DW_ATE_unsigned_char:
        case DW_ATE_signed_char:
            return true;
        default:
            return false;
    }
}
