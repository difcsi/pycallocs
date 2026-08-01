
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdio.h>   /* fprintf for diagnostics */
#include "liballocs.h"
#include <limits.h>
#include <uniqtype-defs.h>
#include <minicrunch.h>

/* NB: we deliberately do NOT include liballocs_private.h. Its debug_printf macro
 * relies on liballocs-internal, hidden-visibility helpers (get_exe_command_basename
 * etc.) that aren't usable from this separate DSO. We use fprintf(stderr, ...)
 * for the rare diagnostic paths instead, and rely only on liballocs' public API. */


/* State threaded through the subobject walk below. Besides the search result
 * proper (success/matched_t) we accumulate the *array run* enclosing the
 * target offset -- see bounds_cb for what that means. */
struct bounds_cb_arg
{
	const struct uniqtype *passed_in_t;
	unsigned target_offset;
	_Bool success;
	const struct uniqtype *matched_t;

	/* The array run: the outermost array we descended through in an
	 * uninterrupted chain of array descents, and how many elements of the
	 * innermost element type it spans. For int[3] that is (int[3], offset
	 * 0, 3); for int[2][3] it is (int[2][3], offset 0, 6). Descending
	 * through a non-array resets the run, so an array nested in a struct
	 * starts a fresh run at that member's offset. A flexible or otherwise
	 * unbounded array sets array_run_unbounded: "as many as fit". */
	const struct uniqtype *array_run_t;
	unsigned array_run_start_offset;
	size_t array_run_nelems;
	_Bool array_run_unbounded;
};

/* Callback for __liballocs_walk_subobjects_spanning. The current liballocs API
 * passes the containment context explicitly (containing uniqtype + the offset
 * at which it starts) rather than via a uniqtype_containment_ctxt list.
 *
 * We are called for each subobject spanning the target offset, outermost
 * first, so the run state below is maintained incrementally as we descend. */
static int bounds_cb(struct uniqtype *u, unsigned span_start_offset, unsigned depth,
	struct uniqtype *containing, struct uniqtype_rel_info *contained_pos,
	unsigned containing_span_start_offset, void *arg_void)
{
	struct bounds_cb_arg *arg = (struct bounds_cb_arg *) arg_void;

	/* If we've just descended through an object of array type, fold it into
	 * the current run. For arrays of arrays, say int[2][3], we want to range
	 * over the whole flattened extent, so the element counts multiply while
	 * the run keeps the *outermost* array's start offset.
	 *
	 * This is not the case for arrays of structs of arrays: descending
	 * through a non-array clears the run, so the inner array starts a new
	 * one at its own offset rather than being merged with the outer. That
	 * is what the else branch below is for. */
	if (containing && UNIQTYPE_IS_ARRAY_TYPE(containing))
	{
		if (!arg->array_run_t)
		{
			/* Start a new run. Its start offset is where the outermost
			 * array of the run begins, which is what bounds are relative to. */
			arg->array_run_t = containing;
			arg->array_run_start_offset = containing_span_start_offset;
			arg->array_run_nelems = UNIQTYPE_HAS_KNOWN_LENGTH(containing)
			 ? (size_t) UNIQTYPE_ARRAY_LENGTH(containing) : 0;
			arg->array_run_unbounded = !UNIQTYPE_HAS_KNOWN_LENGTH(containing);
		}
		else
		{
			/* Continuing a run: an inner array multiplies the element
			 * count. Once any level is unbounded the whole run is. */
			if (UNIQTYPE_HAS_KNOWN_LENGTH(containing))
				arg->array_run_nelems *= (size_t) UNIQTYPE_ARRAY_LENGTH(containing);
			else arg->array_run_unbounded = 1;
		}
	}
	else
	{
		arg->array_run_t = NULL;
		arg->array_run_start_offset = 0;
		arg->array_run_nelems = 0;
		arg->array_run_unbounded = 0;
	}

	if (span_start_offset < arg->target_offset)
	{
		return 0; // keep going
	}

	// now we have span_start_offset >= target_offset
	if (span_start_offset > arg->target_offset)
	{
		/* We've overshot. If this happens, it means the target offset
		 * is not a subobject start offset. This shouldn't happen,
		 * unless the caller makes a wild pointer. */
		return 1;
	}

	/* We've hit a subobject that starts at the right place. It might still
	 * be an enclosing object, not the object we're looking for. We
	 * differentiate using the size of the passed-in type -- this is the size
	 * of object that the pointer arithmetic is being done on. Keep going
	 * until we hit something of exactly that size: smaller happens with
	 * __like_a prefixing and with stack frames that are de-facto unions
	 * (continue with the siblings), larger is the ordinary enclosing-object
	 * case (descend). Either way the move is the same. */
	if (u->pos_maxoff != arg->passed_in_t->pos_maxoff)
	{
		return 0; // keep going
	}

	/* Matched. Note what the run state means at this point: bounds_cb is
	 * called outermost-first, so arg->array_run_* describes the chain of
	 * array descents that ended at *this* subobject. If the last thing we
	 * descended through was not an array -- a struct member, say -- the run
	 * was cleared on that step, and the caller falls back to the object's
	 * own extent.
	 *
	 * That is the point of tracking a run rather than an "innermost
	 * containing array": the innermost array of int[2][3] is int[3], whose
	 * length alone would give bounds three elements wide instead of six.
	 * Multiplying along the run gives the flattened extent, which is what
	 * pointer arithmetic on the element type may legally range over. */
	arg->success = 1;
	arg->matched_t = u;

	/* Stop the walk here. Descending further would take us into the
	 * subobjects of the matched object, and -- more to the point -- would
	 * clobber the run state we just accumulated on the way down to it. */

	return 1;
}

Bounds __make_bounds(unsigned long base, unsigned long limit)
{
    Bounds b = {
        .base = base,
        .size = limit - base};
    return b;
}


Bounds __fetch_bounds_internal(const void *obj, const struct uniqtype *t)
{
    if (!obj)
        goto return_min_bounds;

    // DO QUERY -- which allocation is obj in, and what type does it have?
    // Nothing here is minicrunch-specific; it is the standard liballocs
    // query, and the interesting part is what we do with the answer.
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
    unsigned target_offset_within_uniqtype = (char *)obj - (char *)alloc_start;

    struct uniqtype *cur_obj_uniqtype = alloc_uniqtype;
    struct uniqtype *cur_containing_uniqtype = NULL;
    struct uniqtype_rel_info *cur_contained_pos = NULL;
    unsigned cumulative_offset_searched = 0;

    /* NB: an unrecognised allocation *site* is reported as an error by
     * __liballocs_get_alloc_info, so the bail above catches it and it never
     * reaches here. There used to be a second, unreachable copy of that
     * test at this point, reading as though site-less allocations were
     * handled when they were not; it is gone.
     *
     * If you do want site-less allocations usable for bounds -- they do
     * carry a start and a size -- the place to relax is the bail above,
     * not here. The walk then needs *some* type to walk, which is what the
     * "pretend it's a char allocation" fallback below supplies; but a
     * char-typed walk will not match a non-char passed-in type, so such
     * allocations come out with the object's own extent, not an array run.
     *
     * Without alloc site info we may still have start and size. */
    if (alloc_start && alloc_size_bytes && !alloc_uniqtype)
    {
        /* Pretend it's a char allocation */
        alloc_uniqtype = &__uniqtype__signed_char;
    }
    if (t == &__uniqtype__signed_char || t == &__uniqtype__unsigned_char)
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
     * - descend (offset zero) until we find something of *the same size as*
              t->pos_maxoff
     * - if we descended through arrays to get there, the bounds are the whole
     *   array run; otherwise they are just the object itself.
     */

    struct bounds_cb_arg arg = {
        .passed_in_t = t,
        .target_offset = target_offset_within_uniqtype};
    int ret = __liballocs_walk_subobjects_spanning(
        target_offset_within_uniqtype,
        alloc_uniqtype,
        bounds_cb,
        &arg);
    (void) ret;
    if (arg.success)
    {
        if (arg.array_run_t)
        {
            // bounds are the whole array run
            const char *lower = (const char *)alloc_start + arg.array_run_start_offset;
            const char *upper = arg.array_run_unbounded ? /* use the allocation's limit */
                                    (const char *)alloc_start + alloc_size_bytes
                                                        : lower + arg.array_run_nelems * t->pos_maxoff;
            return __make_bounds(
                (unsigned long)lower,
                (unsigned long)upper);
        }
        // bounds are just this object
        char *limit = (char *)obj + (t->pos_maxoff > 0 ? t->pos_maxoff : 1);
        return __make_bounds((unsigned long)obj, (unsigned long)limit);
    }
    else
    {
        fprintf(stderr, "minicrunch: no bounds for %p, target type %s, offset %d in allocation of %s at %p\n",
                      obj, NAME_FOR_UNIQTYPE(t), target_offset_within_uniqtype,
                      NAME_FOR_UNIQTYPE(alloc_uniqtype), alloc_start);
        goto return_min_bounds;
    }

return_min_bounds:
    return __make_bounds((unsigned long)obj, (unsigned long)obj + 1);

return_alloc_bounds:
{
    char *base = (char *)alloc_start;
    char *limit = (char *)alloc_start + alloc_size_bytes;
    unsigned long size = limit - base;

    /* CHECK: do the bounds include the derived-from pointer? If not, we abort. */
    if ((unsigned long)obj - (unsigned long)base > size)
        goto abort_returning_max_bounds;

    return __make_bounds(
        (unsigned long)base,
        (unsigned long)limit);
}

out:
abort_returning_max_bounds:
    fprintf(stderr, "minicrunch: failed to fetch bounds for pointer %p; liballocs said %s (alloc site %p)\n", obj,
                  err ? __liballocs_errstring(err) : "no allocation found spanning queried pointer", alloc_site);

    return __make_bounds((unsigned long) 0, (unsigned long) -1); // MAX_BOUNDS
}
