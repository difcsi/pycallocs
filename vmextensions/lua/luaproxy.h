#ifndef LUAPROXY_H
#define LUAPROXY_H

/* luaproxy: liballocs-driven C interop for Lua, mirroring the Python-side
 * pycallocs extension (include/foreign_library.h). Foreign data is described
 * by LuaForeignType, a per-uniqtype vtable that is the whole marshalling
 * protocol; proxies are full userdata holding only the addresslike pointer.
 *
 * Vtable conventions (stack protocol):
 *  - ft_getfrom / ft_copyfrom push exactly one Lua value on success.
 *  - ft_storeinto reads the value at a stack index, leaves the stack balanced.
 *  - ft_constructor consumes nothing, reads args [first_arg .. lua_gettop] and
 *    pushes the constructed value on success.
 *  - All return 0 on success, or -1 with an error message pushed. They must
 *    not raise (longjmp): they get called from libffi frames, __gc finalizers
 *    and liballocs GC-policy callbacks. Only outermost module closures may
 *    turn a -1 into lua_error().
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include <lua.h>
#include <lauxlib.h>

#include <liballocs.h>
#include "../common/minicrunch.h"
/* Translate-then-index glue: ss_translate() re-derives the raw backing from an
 * Alaska handle (identity without SS_HAVE_ALASKA); ss_inc/dec_refcount and
 * ss_is_handle no-op without Alaska. Never cache a translated pointer across
 * calls that may run the Alaska GC. */
#include <handle_query.h>

#ifdef SS_HAVE_ALASKA
#define totally_malloc(x) halloc(x)
#define totally_free(ptr) hfree(ptr)
#else
#define totally_malloc(x) malloc(x)
#define totally_free(ptr) __liballocs_detach_manual_dealloc_policy((ptr))
#endif

struct LuaForeignType;

/* Payload of every proxy full userdata. No copy of the C data: every access
 * re-derives the raw pointer via ss_translate(p_ptr). Interior/derived proxies
 * keep their base proxy alive through uservalue 1. */
typedef struct LuaProxy {
    void *p_ptr;                    /* the underlying addresslike (may be a handle) */
    struct LuaForeignType *pr_type; /* foreign type of the pointee */
    size_t ap_length;               /* element count for address/array proxies */
    /* True iff this proxy is the registered base proxy for p_ptr (it owns a
     * lifetime-policy attach / handle refcount that __gc must release). Kept
     * here, NOT derived from the identity cache: the cache is weak-valued and
     * Lua clears weak values for finalizable objects BEFORE running __gc, so
     * the cache can never answer this question inside the finalizer. */
    bool pr_registered;
} LuaProxy;

typedef struct LuaForeignType {
    const struct uniqtype *ft_type;
    /* Registry key of the proxy metatable for this type, or NULL when values
     * of this type never have a proxy (scalars: always converted by value). */
    const char *ft_mtname;

    int (*ft_constructor)(lua_State *L, int first_arg, struct LuaForeignType *ft);
    int (*ft_getfrom)(lua_State *L, void *data, struct LuaForeignType *ft);
    int (*ft_copyfrom)(lua_State *L, void *data, struct LuaForeignType *ft);
    int (*ft_storeinto)(lua_State *L, int idx, void *dest, struct LuaForeignType *ft);
    /* Raw pointer to existing data of this type held by the value at idx, or
     * NULL (never errors, never pushes). libffi zero-copy argument path. */
    void *(*ft_getdataptr)(lua_State *L, int idx, struct LuaForeignType *ft);
} LuaForeignType;

/* The single Lua state luaproxy serves. liballocs GC-policy callbacks carry no
 * context argument, so one state per process is a hard limitation. NULL until
 * luaopen_luaproxy; callbacks no-op while NULL. */
extern lua_State *lp_globalL;

/* ---- foreign_type.c ---- */
LuaForeignType *LuaForeignType_GetOrCreate(lua_State *L, const struct uniqtype *type);
bool LuaForeignType_IsTriviallyCopiable(const LuaForeignType *ft);
void LuaForeignType_InitMeta(lua_State *L);
/* Push the (cached) Lua-visible wrapper object for a foreign type. */
void LuaForeignType_PushWrapper(lua_State *L, LuaForeignType *ft);
/* One-element out-parameter "cells": a length-1 array proxy of elem_ft,
 * initialised from the value at init_idx (zero/NULL when init_idx == 0).
 * Because arrays share the pointer-to-element proxy class, the cell passes
 * anywhere C expects a pointer to the element:
 *   w = allocs.int(); f(w); print(w[1])    -- C: void f(int *)
 * Pushes the cell; 0/-1 with message pushed. */
int LuaForeignType_NewCell(lua_State *L, LuaForeignType *elem_ft, int init_idx);
/* ft_constructor implementation for scalar and pointer types: zero or one
 * argument, building a cell. */
int LuaForeignType_CellCtor(lua_State *L, int first_arg, LuaForeignType *ft);

/* ---- function_proxy.c ---- */
LuaForeignType *LuaFunctionProxy_NewType(lua_State *L, const struct uniqtype *type);
void LuaFunctionProxy_InitType(lua_State *L, LuaForeignType *ft);

/* ---- proxy.c ---- */
void LuaProxy_InitGCPolicy(lua_State *L);
/* NULL if the value at idx is not a luaproxy userdata. */
LuaProxy *LuaProxy_Untyped(lua_State *L, int idx);
/* Create the proxy metatable for ft (fills ft->ft_mtname). `extra` entries are
 * added on top of the common __gc/__eq/__luaproxy_ft fields. Must run before
 * the first proxy of this type is created (__gc must pre-exist: Lua only marks
 * a userdata for finalization if __gc is present when its metatable is set). */
void LuaProxy_CreateMetatable(lua_State *L, LuaForeignType *ft, const luaL_Reg *extra);
/* Push a fresh unregistered proxy userdata of ft at addr (sets metatable). */
LuaProxy *LuaProxy_PushNew(lua_State *L, void *addr, LuaForeignType *ft);
/* Register the proxy at proxy_idx as the base proxy for its address: identity
 * cache entry + liballocs lifetime-policy attach (or Alaska handle refcount). */
void LuaProxy_Register(lua_State *L, int proxy_idx);
/* Push the registered base proxy for the allocation containing addr, creating
 * (and registering) it if liballocs can type the allocation. Returns false
 * (nothing pushed) when no base proxy can exist (untyped/static memory). */
bool LuaProxy_GetOrCreateBase(lua_State *L, void *addr);
/* Generic vtable implementations for proxy-kinded types (composite, and later
 * address/array/function). LuaProxy_SetupType wires them into ft. */
int LuaProxy_GetFrom(lua_State *L, void *data, LuaForeignType *ft);
int LuaProxy_CopyFrom(lua_State *L, void *data, LuaForeignType *ft);
int LuaProxy_StoreInto(lua_State *L, int idx, void *dest, LuaForeignType *ft);
void *LuaProxy_GetDataPtr(lua_State *L, int idx, LuaForeignType *ft);
void LuaProxy_SetupType(LuaForeignType *ft);
/* Defer-free window bracketing foreign calls (struct_global_swap mechanism). */
void LuaProxy_BeginDeferFrees(void);
void LuaProxy_EndDeferFrees(lua_State *L);
/* proxy:release() -- the manual escape hatch for host-C reference cycles,
 * which Lua's GC cannot collect (no traverse protocol over C memory): clears
 * every tracked pointer slot inside the proxy's allocation (dropping the
 * strong refs those slots held), then unregisters the proxy and marks it
 * dead. Lua CFunction (self at index 1). */
int LuaProxy_Release(lua_State *L);
/* The generic __gc (public so specialised proxy metatables -- closures -- can
 * chain to it from their own finalizer). */
int LuaProxy_GC(lua_State *L);

/* ---- foreign_basetype.c ---- */
int LuaForeignBaseType_Init(lua_State *L, LuaForeignType *ft);
int LuaForeignEnumType_Init(lua_State *L, LuaForeignType *ft);
bool LuaForeignBaseType_IsChar(const struct uniqtype *type);

/* True iff a proxy of type `have` is acceptable where `want` is expected
 * (same type, or `want` reached through the offset-0 first-member chain). */
bool LuaProxy_TypeAcceptable(LuaForeignType *have, LuaForeignType *want);
/* After a pointer `val` (from the proxy at val_idx) has been stored into the C
 * slot `dest`, keep that proxy alive while the slot references it, covering
 * values liballocs cannot track (e.g. ffi closure trampolines). */
void LuaProxy_RetainStoredPtr(lua_State *L, int val_idx, const void **dest, const void *val);

/* ---- composite_proxy.c ---- */
LuaForeignType *LuaCompositeProxy_NewType(lua_State *L, const struct uniqtype *type);
void LuaCompositeProxy_InitType(lua_State *L, LuaForeignType *ft);
/* Resolve a field by name on a composite foreign type (walking the offset-0
 * first-member chain). False if absent; *out_ft may still be NULL for a field
 * whose type is unhandled. */
bool LuaCompositeProxy_FindField(LuaForeignType *composite_ft, const char *key,
        LuaForeignType **out_ft, size_t *out_off);

/* ---- address_proxy.c ---- */
/* Shared shape for ADDRESS and ARRAY foreign types: both dispatch element
 * access through pointee_type (the element type for arrays). Arrays reuse the
 * pointer-to-element proxy metatable (ft_mtname aliases the address type's),
 * which is also what makes one-element cells usable as out-parameters. */
typedef struct LuaAddressType {
    LuaForeignType ft;
    LuaForeignType *pointee_type; /* NULL when the pointee kind is unhandled */
} LuaAddressType;
LuaForeignType *LuaAddressProxy_NewType(lua_State *L, const struct uniqtype *type);
void LuaAddressProxy_InitType(lua_State *L, LuaForeignType *ft);
LuaForeignType *LuaArrayProxy_NewType(lua_State *L, const struct uniqtype *type);
void LuaArrayProxy_InitType(lua_State *L, LuaForeignType *ft);

/* ---- library_loader.c ---- */
int LuaLibraryLoader_Load(lua_State *L); /* lua_CFunction: load("libfoo.so") -> module table */

#endif /* LUAPROXY_H */
