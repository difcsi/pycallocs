/* Scalar (BASE/ENUMERATION) marshalling: V8 port of the Lua/CPython
 * foreign_basetype. Selection is by DWARF encoding x size. Scalars have no
 * proxy template (always converted by value).
 *
 * JS numeric mapping: integers <= 2^53 travel as Number; 64-bit values
 * outside Number's exact range travel as BigInt (better fidelity than the
 * Lua port); stores accept Number (integral) and BigInt (range-checked).
 * chars are 1-character strings; _Complex is an {re, im} object. */

#include "v8proxy.h"

#include <dwarf.h>
#include <cstring>

#ifndef __STDC_NO_COMPLEX__
# include <complex.h>
#endif

using v8::Local;
using v8::MaybeLocal;
using v8::Value;

static Local<v8::Context> ctx() { return g_context.Get(g_isolate); }

/* Fetch an integer without raising: Number with integral value, or BigInt.
 * Returns false (no throw) when not integer-like. */
static bool to_int64(Local<Value> v, int64_t *out, bool *lossless)
{
    *lossless = true;
    if (v->IsBigInt())
    {
        *out = v.As<v8::BigInt>()->Int64Value(lossless);
        return true;
    }
    if (v->IsNumber())
    {
        double d = v.As<v8::Number>()->Value();
        int64_t i = (int64_t)d;
        if ((double)i != d) return false; /* non-integral */
        *out = i;
        return true;
    }
    return false;
}

static bool to_uint64(Local<Value> v, uint64_t *out, bool *lossless)
{
    *lossless = true;
    if (v->IsBigInt())
    {
        *out = v.As<v8::BigInt>()->Uint64Value(lossless);
        return true;
    }
    if (v->IsNumber())
    {
        double d = v.As<v8::Number>()->Value();
        if (d < 0) { *lossless = false; *out = 0; return true; }
        uint64_t u = (uint64_t)d;
        if ((double)u != d) return false;
        *out = u;
        return true;
    }
    return false;
}

static bool bool_store(Local<Value> v, void *dest, V8ForeignType *)
{
    *(uint8_t *)dest = v->BooleanValue(g_isolate) ? 1 : 0;
    return true;
}
static MaybeLocal<Value> bool_get(void *data, V8ForeignType *)
{
    return v8::Boolean::New(g_isolate, *(uint8_t *)data != 0);
}

#define DEFINE_UINT_GETSTORE(size) \
static MaybeLocal<Value> uint##size##_get(void *data, V8ForeignType *) \
{ \
    uint##size##_t u = *(uint##size##_t *)data; \
    if ((uint64_t)u > (uint64_t)1 << 53) \
        return v8::BigInt::NewFromUnsigned(g_isolate, u); \
    return v8::Number::New(g_isolate, (double)u); \
} \
static bool uint##size##_store(Local<Value> v, void *dest, V8ForeignType *) \
{ \
    uint64_t u; bool lossless; \
    if (!to_uint64(v, &u, &lossless)) \
        { Throwf("expected an integer"); return false; } \
    if (!lossless || (uint64_t)(uint##size##_t)u != u) \
        { Throwf("argument does not fit into a " #size " bit unsigned integer"); return false; } \
    *(uint##size##_t *)dest = (uint##size##_t)u; \
    return true; \
}
DEFINE_UINT_GETSTORE(8)
DEFINE_UINT_GETSTORE(16)
DEFINE_UINT_GETSTORE(32)
DEFINE_UINT_GETSTORE(64)

#define DEFINE_INT_GETSTORE(size) \
static MaybeLocal<Value> int##size##_get(void *data, V8ForeignType *) \
{ \
    int##size##_t i = *(int##size##_t *)data; \
    if ((int64_t)i > ((int64_t)1 << 53) || (int64_t)i < -((int64_t)1 << 53)) \
        return v8::BigInt::New(g_isolate, i); \
    return v8::Number::New(g_isolate, (double)i); \
} \
static bool int##size##_store(Local<Value> v, void *dest, V8ForeignType *) \
{ \
    int64_t i; bool lossless; \
    if (!to_int64(v, &i, &lossless)) \
        { Throwf("expected an integer"); return false; } \
    if (!lossless || (int64_t)(int##size##_t)i != i) \
        { Throwf("argument does not fit into a " #size " bit signed integer"); return false; } \
    *(int##size##_t *)dest = (int##size##_t)i; \
    return true; \
}
DEFINE_INT_GETSTORE(8)
DEFINE_INT_GETSTORE(16)
DEFINE_INT_GETSTORE(32)
DEFINE_INT_GETSTORE(64)

/* chars read as strings of the char's byte width; stores accept a string of
 * that exact length or an integer code point */
#define DEFINE_CHAR_GETSTORE(size) \
static MaybeLocal<Value> char##size##_get(void *data, V8ForeignType *) \
{ \
    return v8::String::NewFromOneByte(g_isolate, (const uint8_t *)data, \
            v8::NewStringType::kNormal, size / 8).ToLocalChecked(); \
} \
static bool uchar##size##_store(Local<Value> v, void *dest, V8ForeignType *ft) \
{ \
    if (v->IsString()) \
    { \
        v8::Local<v8::String> s = v.As<v8::String>(); \
        if (s->Length() == size / 8) \
        { \
            uint8_t buf[size / 8]; \
            s->WriteOneByteV2(g_isolate, 0, size / 8, buf); \
            memcpy(dest, buf, size / 8); \
            return true; \
        } \
        Throwf("expected a string of length %d", size / 8); \
        return false; \
    } \
    return uint##size##_store(v, dest, ft); \
} \
static bool schar##size##_store(Local<Value> v, void *dest, V8ForeignType *ft) \
{ \
    if (v->IsString()) return uchar##size##_store(v, dest, ft); \
    return int##size##_store(v, dest, ft); \
}
DEFINE_CHAR_GETSTORE(8)
DEFINE_CHAR_GETSTORE(16)
DEFINE_CHAR_GETSTORE(32)
DEFINE_CHAR_GETSTORE(64)

#define DEFINE_FLOAT_GETSTORE(t) \
static MaybeLocal<Value> t##_get(void *data, V8ForeignType *) \
{ \
    return v8::Number::New(g_isolate, (double)*(t *)data); \
} \
static bool t##_store(Local<Value> v, void *dest, V8ForeignType *) \
{ \
    double d; \
    if (!v->NumberValue(ctx()).To(&d)) return false; \
    *(t *)dest = (t)d; \
    return true; \
}
DEFINE_FLOAT_GETSTORE(float)
DEFINE_FLOAT_GETSTORE(double)
typedef long double long_double;
DEFINE_FLOAT_GETSTORE(long_double)

#ifndef __STDC_NO_COMPLEX__
/* Complex values marshal as {re, im} objects; stores also accept a plain
 * number (imaginary part 0). */
static bool to_complex(Local<Value> v, double *re, double *im)
{
    if (v->IsNumber()) { *re = v.As<v8::Number>()->Value(); *im = 0; return true; }
    if (!v->IsObject()) return false;
    v8::Local<v8::Object> o = v.As<v8::Object>();
    v8::Local<Value> vre, vim;
    if (!o->Get(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "re")).ToLocal(&vre)
            || !o->Get(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "im")).ToLocal(&vim))
        return false;
    if (!vre->IsNumber() || !vim->IsNumber()) return false;
    *re = vre.As<v8::Number>()->Value();
    *im = vim.As<v8::Number>()->Value();
    return true;
}
#define DEFINE_COMPLEX_GETSTORE(t) \
static MaybeLocal<Value> t##_get(void *data, V8ForeignType *) \
{ \
    _Complex double z = (_Complex double)*(t *)data; \
    v8::Local<v8::Object> o = v8::Object::New(g_isolate); \
    o->Set(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "re"), \
            v8::Number::New(g_isolate, creal(z))).Check(); \
    o->Set(ctx(), v8::String::NewFromUtf8Literal(g_isolate, "im"), \
            v8::Number::New(g_isolate, cimag(z))).Check(); \
    return o; \
} \
static bool t##_store(Local<Value> v, void *dest, V8ForeignType *) \
{ \
    double re, im; \
    if (!to_complex(v, &re, &im)) \
        { Throwf("expected a number or {re, im} object"); return false; } \
    _Complex double z; \
    __real__ z = re; \
    __imag__ z = im; \
    *(t *)dest = (t)z; \
    return true; \
}
typedef _Complex float cmplx_float;
DEFINE_COMPLEX_GETSTORE(cmplx_float)
typedef _Complex double cmplx_double;
DEFINE_COMPLEX_GETSTORE(cmplx_double)
typedef _Complex long double cmplx_long_double;
DEFINE_COMPLEX_GETSTORE(cmplx_long_double)
#endif

#define CHECK_SIZE(sz, get, store) \
if (size == sz) \
{ \
    ft->ft_getfrom = get; \
    ft->ft_copyfrom = get; \
    ft->ft_storeinto = store; \
    return true; \
}

bool ForeignBaseType_Init(V8ForeignType *ft)
{
    const struct uniqtype *type = ft->ft_type;
    unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);

    if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type) != 8 * size)
        return false; /* bit fields unsupported */

    switch (type->un.base.enc)
    {
        case 0:
            /* liballocs' synthetic __uninterpreted_byte as unsigned char */
            CHECK_SIZE(1, char8_get, uchar8_store)
            break;
        case DW_ATE_boolean:
            ft->ft_getfrom = bool_get;
            ft->ft_copyfrom = bool_get;
            ft->ft_storeinto = bool_store;
            return true;
        case DW_ATE_address:
        case DW_ATE_unsigned:
            CHECK_SIZE(1, uint8_get, uint8_store)
            CHECK_SIZE(2, uint16_get, uint16_store)
            CHECK_SIZE(4, uint32_get, uint32_store)
            CHECK_SIZE(8, uint64_get, uint64_store)
            break;
        case DW_ATE_signed:
            CHECK_SIZE(1, int8_get, int8_store)
            CHECK_SIZE(2, int16_get, int16_store)
            CHECK_SIZE(4, int32_get, int32_store)
            CHECK_SIZE(8, int64_get, int64_store)
            break;
        case DW_ATE_unsigned_char:
            CHECK_SIZE(1, char8_get, uchar8_store)
            CHECK_SIZE(2, char16_get, uchar16_store)
            CHECK_SIZE(4, char32_get, uchar32_store)
            CHECK_SIZE(8, char64_get, uchar64_store)
            break;
        case DW_ATE_signed_char:
            CHECK_SIZE(1, char8_get, schar8_store)
            CHECK_SIZE(2, char16_get, schar16_store)
            CHECK_SIZE(4, char32_get, schar32_store)
            CHECK_SIZE(8, char64_get, schar64_store)
            break;
        case DW_ATE_float:
            CHECK_SIZE(4, float_get, float_store)
            CHECK_SIZE(8, double_get, double_store)
            CHECK_SIZE(16, long_double_get, long_double_store)
            break;
#ifndef __STDC_NO_COMPLEX__
        case DW_ATE_complex_float:
            CHECK_SIZE(8, cmplx_float_get, cmplx_float_store)
            CHECK_SIZE(16, cmplx_double_get, cmplx_double_store)
            CHECK_SIZE(32, cmplx_long_double_get, cmplx_long_double_store)
            break;
#endif
        default:
            break;
    }
    return false;
}

bool ForeignEnumType_Init(V8ForeignType *ft)
{
    const struct uniqtype *type = ft->ft_type;
    const struct uniqtype *base = UNIQTYPE_ENUM_BASE_TYPE(type);
    if (base && UNIQTYPE_IS_BASE_TYPE(base))
    {
        V8ForeignType tmp;
        tmp.ft_type = base;
        if (!ForeignBaseType_Init(&tmp)) return false;
        ft->ft_getfrom = tmp.ft_getfrom;
        ft->ft_copyfrom = tmp.ft_copyfrom;
        ft->ft_storeinto = tmp.ft_storeinto;
        return true;
    }
    unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
    CHECK_SIZE(1, int8_get, int8_store)
    CHECK_SIZE(2, int16_get, int16_store)
    CHECK_SIZE(4, int32_get, int32_store)
    CHECK_SIZE(8, int64_get, int64_store)
    return false;
}

bool ForeignBaseType_IsChar(const struct uniqtype *type)
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
