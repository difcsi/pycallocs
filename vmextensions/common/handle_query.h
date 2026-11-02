#ifndef STACKSCAN_HANDLE_QUERY_H
#define STACKSCAN_HANDLE_QUERY_H

/*
 * Handle-aware liballocs glue ("translate-then-index") -- HEADER-ONLY, always-inline.
 *
 * liballocs indexes every allocation by its real address in the pageindex /
 * bigalloc tree, which only spans canonical (lower-half) addresses. An Alaska
 * handle is a non-canonical pointer with bit 63 set; handing one straight to a
 * liballocs query indexes a top-bit-set address and faults (a hard SIGSEGV -- see
 * docs/liballocs-alaska-integration.md). Each wrapper therefore translates a
 * handle to its backing pointer with alaska_translate() *before* the normal
 * address-indexed query. alaska_translate() is a no-op on non-handles, so every
 * wrapper is safe to call on an arbitrary pointer (handle or raw).
 *
 * Everything here is `static inline __attribute__((always_inline))` so consumers
 * only need to #include this header -- there is no library to link.
 *
 * liballocs is assumed to be ALWAYS present: its query and lifetime-policy symbols
 * are declared as ordinary externs and called directly. (The pycallocs extension
 * links -lallocs and runs under the liballocs preload, so these resolve at load.)
 *
 * Alaska is a *configure-time* option, selected by the build (the full Alaska
 * handle-transforming compiler) and signalled to this header by defining the
 * SS_HAVE_ALASKA macro. When SS_HAVE_ALASKA is defined the wrappers bind to the
 * Alaska runtime (translate, pin/unpin); otherwise pointers are never handles, so
 * translate becomes a compile-time passthrough and the pin operations no-op.
 */


#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uniqtype;
struct allocator;

#define SS_ALWAYS_INLINE static inline __attribute__((always_inline))

#ifdef SS_HAVE_ALASKA
/* Alaska: handle -> backing+offset, passthrough on non-handles
 * (libalaska / libalaska_translate_native.a). */
extern void *alaska_translate(void *ptr);

/* Alaska: pin/unpin a handle so the relocating GC will not move its backing
 * object while pinned. No-ops on non-handles. (libalaska, core/translate.cpp) */
extern void alaska_pin(void *ptr);
extern void alaska_unpin(void *ptr);
extern int  alaska_is_pinned(void *ptr);

/* Alaska: bump/drop a handle's reference count -- the same operation the Alaska
 * compiler inserts at instrumented pointer stores (runtime/rt/refcount.cpp). When
 * the count reaches 0 the handle's backing object becomes eligible for reclamation
 * by Anchorage (off-stack) and any refcount-nonzero cycles are collected by its
 * trial-deletion cycle collector. No-ops on non-handles (the runtime checks the
 * tag bit). pycallocs calls these for stores its own (non-Alaska-compiled) C code
 * makes, which the compiler passes never see. */
extern void alaska_inc_refcount(void *ptr);
extern void alaska_dec_refcount(void *ptr);
#endif /* SS_HAVE_ALASKA */

/* liballocs query aliases (query.c): __liballocs_get_base /
 * __liballocs_get_alloc_size / __liballocs_get_outermost_type /
 * __liballocs_get_leaf_allocator. */
extern void           *alloc_get_base(void *);
extern unsigned long   alloc_get_size(void *);
extern struct uniqtype *alloc_get_type(void *);
extern struct allocator *alloc_get_allocator(void *);
/* liballocs lifetime-policy API (Bertholon & Kell). Policy 0 is the manual
 * deallocation policy by convention. */
extern void __liballocs_attach_lifetime_policy(int policy_id, const void *obj);
extern void __liballocs_detach_lifetime_policy(int policy_id, const void *obj);
#define SS_MANUAL_DEALLOCATION_POLICY 0

/* Whether the build was configured with the Alaska handle transformation.
 * A compile-time constant in this configure-time model. */
SS_ALWAYS_INLINE _Bool ss_libalaska_is_present(void)
{
#ifdef SS_HAVE_ALASKA
	return 1;
#else
	return 0;
#endif
}

/* Non-zero iff `ptr` is an Alaska handle (non-canonical, bit 63 set -- the same
 * test the Alaska runtime uses, alaska.hpp). Always 0 without Alaska, where no
 * pointer is ever a handle. Lets callers tell apart handle-backed objects (whose
 * lifetime Alaska refcounts) from raw pointers like libffi closure trampolines. */
SS_ALWAYS_INLINE int ss_is_handle(const void *ptr)
{
#ifdef SS_HAVE_ALASKA
	return (intptr_t) ptr < 0;
#else
	(void) ptr;
	return 0;
#endif
}

/* Translate a handle to its backing pointer; pass non-handles through. */
SS_ALWAYS_INLINE void *ss_translate(void *ptr)
{
#ifdef SS_HAVE_ALASKA
	return alaska_translate(ptr);
#else
	return ptr;
#endif
}

/* Pin a handle so its backing object cannot be relocated by the GC until the
 * matching ss_unpin(). Translate after pinning to obtain a backing pointer that
 * stays valid across a GC safepoint. No-op without Alaska / on non-handles. */
SS_ALWAYS_INLINE void ss_pin(void *handle)
{
#ifdef SS_HAVE_ALASKA
	alaska_pin(handle);
#else
	(void) handle;
#endif
}

/* Release a pin taken with ss_pin(). No-op without Alaska / on non-handles. */
SS_ALWAYS_INLINE void ss_unpin(void *handle)
{
#ifdef SS_HAVE_ALASKA
	alaska_unpin(handle);
#else
	(void) handle;
#endif
}

/* Non-zero iff `handle` is a handle currently pinned against GC relocation.
 * Returns 0 without Alaska / on non-handles. */
SS_ALWAYS_INLINE int ss_ispinned(void *handle)
{
#ifdef SS_HAVE_ALASKA
	return alaska_is_pinned(handle);
#else
	(void) handle;
	return 0;
#endif
}

/* Increment a handle's reference count: records that one more live slot (a C
 * struct field, or a proxy's owned base handle) holds this handle. No-op without
 * Alaska / on non-handles. */
SS_ALWAYS_INLINE void ss_inc_refcount(void *handle)
{
#ifdef SS_HAVE_ALASKA
	alaska_inc_refcount(handle);
#else
	(void) handle;
#endif
}

/* Decrement a handle's reference count: a slot that held this handle is being
 * overwritten or destroyed. Reaching 0 makes the backing object reclaimable by
 * Anchorage. No-op without Alaska / on non-handles. */
SS_ALWAYS_INLINE void ss_dec_refcount(void *handle)
{
#ifdef SS_HAVE_ALASKA
	alaska_dec_refcount(handle);
#else
	(void) handle;
#endif
}

/* Base address of the allocation containing `ptr`, or NULL if unknown. */
SS_ALWAYS_INLINE void *ss_alloc_get_base(void *ptr)
{
	return alloc_get_base(ss_translate(ptr));
}

/* Size in bytes of that allocation, or 0 if unknown. */
SS_ALWAYS_INLINE unsigned long ss_alloc_get_size(void *ptr)
{
	return alloc_get_size(ss_translate(ptr));
}

/* Outermost uniqtype of that allocation, or NULL if unknown/untyped. */
SS_ALWAYS_INLINE struct uniqtype *ss_alloc_get_type(void *ptr)
{
	return alloc_get_type(ss_translate(ptr));
}

/* The liballocs allocator managing that allocation ("alaska" for handle-backed
 * objects), or NULL if unknown. */
SS_ALWAYS_INLINE struct allocator *ss_alloc_get_allocator(void *ptr)
{
	return alloc_get_allocator(ss_translate(ptr));
}

/* Attach the liballocs MANUAL lifetime policy to a handle's backing object: while
 * attached, liballocs (and any allocator that honours its lifetime policies, such
 * as Alaska's GC) will not reclaim the object until the policy is detached.
 * liballocs keys objects by backing base, so translate first. */
SS_ALWAYS_INLINE void ss_attach_manual_policy(void *handle)
{
	__liballocs_attach_lifetime_policy(SS_MANUAL_DEALLOCATION_POLICY, ss_translate(handle));
}

/* Detach the manual lifetime policy (the explicit "I'm done" / manual free of the
 * policy). After this the object is governed solely by its remaining policies
 * (e.g. Alaska's GC). */
SS_ALWAYS_INLINE void ss_detach_manual_policy(void *handle)
{
	__liballocs_detach_lifetime_policy(SS_MANUAL_DEALLOCATION_POLICY, ss_translate(handle));
}

// takes multiple arguments, is an assert
#ifdef SS_HAVE_ALASKA
#define SS_ASSERT_PINNED(handles,...) do { \
	assert(ss_libalaska_is_present()); \
	void *handles_array[] = {__VA_ARGS__}; \
	for (size_t i = 0; i < sizeof(handles_array)/sizeof(handles_array[0]); ++i) \
		assert(ss_ispinned(handles_array[i])); \
} while(0)
#else
/* Without Alaska there are no handles to pin: pinning is vacuously satisfied. */
#define SS_ASSERT_PINNED(handles,...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* STACKSCAN_HANDLE_QUERY_H */
