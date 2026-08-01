#include <liballocs.h>

typedef struct {
    unsigned long base;
    unsigned size;
} Bounds;
Bounds __fetch_bounds_internal(const void *obj, const struct uniqtype *t);
