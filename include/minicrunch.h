#include <liballocs.h>


typedef struct {
    unsigned long base;
    unsigned size;
} Bounds;

struct bounds_cb_arg
{
	const struct uniqtype *passed_in_t;
	unsigned target_offset;
	_Bool success;
	const struct uniqtype *matched_t;
	const struct uniqtype *innermost_containing_array_t;
	unsigned innermost_containing_array_type_span_start_offset;
	const struct uniqtype *outermost_containing_array_t;
	unsigned outermost_containing_array_type_span_start_offset;
	size_t accum_array_bounds;
};


static _Bool bounds_cb(struct uniqtype *u, struct uniqtype_containment_ctxt *ucc,
	unsigned u_offset_from_search_start, void *arg_void);


Bounds __fetch_bounds_internal(const void *obj, const void *derived, const struct uniqtype *t);