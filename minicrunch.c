
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include "liballocs.h"
#include "liballocs_private.h"
#include <limits.h>
#include <uniqtype-defs.h>
#include <minicrunch.h>



struct uniqtype_containment_ctxt
{
	struct uniqtype *u_container;
	unsigned u_offset_within_container;
	struct uniqtype_rel_info *u_containpos;
	struct uniqtype_containment_ctxt *next;
};


static _Bool bounds_cb(struct uniqtype *u, struct uniqtype_containment_ctxt *ucc,
	unsigned u_offset_from_search_start, void *arg_void)
{
	struct bounds_cb_arg *arg = (struct bounds_cb_arg *) arg_void;

	/* If we've just descended through an object of array type, 
	 * remember this fact. This is so that we can calculate the
	 * whole-array bounds, if we're doing arithmetic on a 
	 * pointer to some element of an array of this type.
	 * 
	 * Also, for arrays of arrays, say int[][], 
	 * we actually want to range over the outermost bounds.
	 * This is not the case of arrays of structs of arrays.
	 * So we want to clear the state once we descend through a non-array. */
	if (ucc && ucc->u_container && UNIQTYPE_IS_ARRAY_TYPE(ucc->u_container))
	{
		arg->innermost_containing_array_type_span_start_offset
		 = u_offset_from_search_start - ucc->u_offset_within_container;
		arg->innermost_containing_array_t = ucc->u_container;
		// FIXME: get rid of innermost and outermost and just walk the contexts
		if (!arg->outermost_containing_array_t)
		{
			arg->outermost_containing_array_type_span_start_offset
			 = u_offset_from_search_start - ucc->u_offset_within_container;
			arg->outermost_containing_array_t = ucc->u_container;
			arg->accum_array_bounds = UNIQTYPE_ARRAY_LENGTH(ucc->u_container);
			if (arg->accum_array_bounds < 1) arg->accum_array_bounds = 0;
		}
		else arg->accum_array_bounds *= UNIQTYPE_ARRAY_LENGTH(ucc->u_container);
	}
	else
	{
		arg->outermost_containing_array_type_span_start_offset = 0;
		arg->outermost_containing_array_t = NULL;
		arg->innermost_containing_array_type_span_start_offset = 0;
		arg->innermost_containing_array_t = NULL;
		arg->accum_array_bounds = 0;
	}
	
	if (u_offset_from_search_start < arg->target_offset)
	{
		return 0; // keep going
	}
	
	// now we have span_start_offset >= target_offset
	if (u_offset_from_search_start > arg->target_offset)
	{
		/* We've overshot. If this happens, it means the target offset
		 * is not a subobject start offset. This shouldn't happen,
		 * unless the caller makes a wild pointer. */
		return 1;
	}
	
	if (u_offset_from_search_start == arg->target_offset)
	{
		/* We've hit a subobject that starts at the right place.
		 * It might still be an enclosing object, not the object we're
		 * looking for. We differentiate using the size of the passed-in
		 * type -- this is the size of object that the pointer
		 * arithmetic is being done on. Keep going til we hit something
		 * of that size. */
		if (u->pos_maxoff < arg->passed_in_t->pos_maxoff)
		{
			// usually shouldn't happen, but might with __like_a prefixing
			// arg->success = 1;
			// return 1;
			// EXCEPT it also happens with stack frames that are de-facto unions.
			// In that case, we should continue with the sibling members,
			// looking for one whose size does match.
			return 0; // keep going here too
		}
		if (u->pos_maxoff > arg->passed_in_t->pos_maxoff)
		{
			// keep going
			return 0;
		}
		
		assert(u->pos_maxoff == arg->passed_in_t->pos_maxoff);
		arg->success = 1;
		arg->matched_t = u;
		cache_containment_facts(u, ucc);

		return 1;
	}
	
	assert(0);
	__builtin_unreachable();
}

Bounds __make_bounds(unsigned long base, unsigned long limit)
{
    Bounds b = {
        .base = base,
        .size = limit - base};
    return b;
}

Bounds __fetch_bounds_internal(const void *obj, const void *derived, const struct uniqtype *t)
{
    if (!obj)
        goto return_min_bounds;

    // DO QUERY

    struct allocator *a = NULL;
    const void *alloc_start;
    unsigned long alloc_size_bytes;
    struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
    const void *alloc_site;
    struct liballocs_err *err = __liballocs_get_alloc_info(obj,
                                                           &a,
                                                           &alloc_start,
                                                           &alloc_size_bytes,
                                                           &alloc_uniqtype,
                                                           &alloc_site);
    if (__builtin_expect(err != NULL, 0))
        goto out; /* liballocs has already counted this abort */

    // SUBOBJ SEARCH
    struct uniqtype *cur_obj_uniqtype = alloc_uniqtype;
    struct uniqtype *cur_containing_uniqtype = NULL;
    struct uniqtype_rel_info *cur_contained_pos = NULL;
    unsigned cumulative_offset_searched = 0;
    unsigned target_offset_within_uniqtype = (char *)obj - (char *)alloc_start;

    // Expects

    if (__builtin_expect(err == &__liballocs_err_unrecognised_alloc_site, 0))
    {
        if (!(alloc_start && alloc_size_bytes))
            goto abort_returning_max_bounds;
    }
    else if (__builtin_expect(err != NULL, 0))
    {
        goto abort_returning_max_bounds; // liballocs has already counted this abort
    }
    /* If we didn't get alloc site information, we might still have
     * start and size info. This can be enough for bounds checks. */
    if (alloc_start && alloc_size_bytes && !alloc_uniqtype)
    {
        /* Pretend it's a char allocation */
        alloc_uniqtype = pointer_to___uniqtype__signed_char;
    }
    if (t == pointer_to___uniqtype__signed_char || t == pointer_to___uniqtype__unsigned_char)
    {
        goto return_alloc_bounds; // FIXME: this is C-specific -- belongs in front-end instrumentation (__fetch_alloc_bounds?)
    }
    if (__builtin_expect(t->pos_maxoff == UNIQTYPE_POS_MAXOFF_UNBOUNDED, 0))
    {
        goto return_min_bounds; // FIXME: also belongs in instrumentation -- can test for an incomplete type
    } // -- bounds are no use if the caller thinks it's incomplete, even if does have bounded size

    /* For bounds checking,
     * what we're really asking about is the regularity of the memory around obj,
     * when considered in strides of t->pos_maxoff.
     * It doesn't actually matter what t is.
     * So:
     *
     * - find the outermost uniqtype at offset obj - alloc_start
     * - descend (offset zero) until we find something of *the same size or smaller than*
              t->pos_maxoff
     * - if we find smaller, that means we used __like_a prefixing; bounds are only the pointee;
     * - if we find equi-sized, use the bounds of the containing array if there is one.
     */

    struct bounds_cb_arg arg = {
        .passed_in_t = t,
        .target_offset = target_offset_within_uniqtype};
    _Bool ret = __liballocs_search_subobjects_spanning(
        alloc_uniqtype,
        target_offset_within_uniqtype,
        bounds_cb,
        &arg,
        NULL, NULL);
    if (arg.success)
    {
        if (arg.innermost_containing_array_t)
        {
            // bounds are the whole array
            const char *lower = (char *)alloc_start + arg.innermost_containing_array_type_span_start_offset;
            const char *upper = !UNIQTYPE_HAS_KNOWN_LENGTH(arg.innermost_containing_array_t) ? /* use the allocation's limit */
                                    alloc_start + alloc_size_bytes
                                                                                             : (char *)alloc_start + arg.innermost_containing_array_type_span_start_offset + (UNIQTYPE_ARRAY_LENGTH(arg.innermost_containing_array_t) * t->pos_maxoff);
            unsigned period = UNIQTYPE_ARRAY_ELEMENT_TYPE(arg.innermost_containing_array_t)->pos_maxoff;
            if (a && a->is_cacheable)
                cache_bounds(lower, upper, period, 0, t,
                             /* depth HACK FIXME */ arg.innermost_containing_array_type_span_start_offset == 0 ? 0 : 1);
            return __make_bounds(
                (unsigned long)lower,
                (unsigned long)upper);
        }
        // bounds are just this object
        char *limit = (char *)obj + (t->pos_maxoff > 0 ? t->pos_maxoff : 1);
        if (a && a->is_cacheable)
        {
            cache_bounds(obj, limit, (t->pos_maxoff > 0 ? t->pos_maxoff : 1), 0, t, /* HACK FIXME depth */ 1);
        }
        return __make_bounds((unsigned long)obj, (unsigned long)limit);
    }
    else
    {
        debug_println(1, "minicrunch: no bounds for %p, target type %s, offset %d in allocation of %s at %p",
                      obj, NAME_FOR_UNIQTYPE(t), target_offset_within_uniqtype, NAME_FOR_UNIQTYPE(alloc_uniqtype),
                      alloc_start);
        goto return_min_bounds;
    }

return_min_bounds:
    if (a && a->is_cacheable)
        cache_bounds(obj, (char *)obj + 1, 0, 0, NULL, 1);
    return __make_bounds((unsigned long)obj, (unsigned long)obj + 1);

return_alloc_bounds:
{
    char *base = (char *)alloc_start;
    char *limit = (char *)alloc_start + alloc_size_bytes;
    unsigned long size = limit - base;

    /* CHECK: do the bounds include the derived-from pointer? If not, we abort. */
    if ((unsigned long)obj - (unsigned long)base > size)
        goto abort_returning_max_bounds;

    // FIXME: the period here seems not right
    // -- if we are chars, should ignore alloc_uniqtype and go for 1?
    if (a && a->is_cacheable)
        cache_bounds(base,
                     limit,
                     /* period */ alloc_uniqtype ? alloc_uniqtype->pos_maxoff : 1,
                     /* offset to a t, i.e. to a char */ 0,
                     t,
                     /* HACK FIXME depth */ 0);
    return __make_bounds(
        (unsigned long)base,
        (unsigned long)limit);
}

out:
abort_returning_max_bounds:
    /* HACK: to avoid repeated slow-path queries for uninstrumented/unindexed
     * allocations, we cache a range of bytes here.
     * PROBLEM: if we currently need *bigger* bounds, we need some way to extend
     * them, since we won't hit this path again. Otherwise we'll get bounds errors.
     * Need to record that the bound are synthetic... use NULL alloc_base. */
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) < (y)) ? (y) : (x))
    cache_fake_bounds(
        MIN((char *)obj, (char *)derived),
        MAX((char *)obj, (char *)derived) + (t ? t->pos_maxoff : 0),
        /* period */ (t ? t->pos_maxoff : 1),
        0,
        t,
        0 /* depth */
    );

    debug_println(1, "minicrunch: failed to fetch bounds for pointer %p (deriving %p); liballocs said %s (alloc site %p)",
                  obj, derived, err ? __liballocs_errstring(err) : "no allocation found spanning queried pointer", alloc_site);
    return __make_bounds((unsigned long) 0, (unsigned long) -1); // MAX_BOUNDS
}
