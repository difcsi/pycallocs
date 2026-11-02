#ifndef VMEXT_MINICRUNCH_H
#define VMEXT_MINICRUNCH_H

#include <liballocs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long base;
    unsigned size;
} Bounds;
Bounds __fetch_bounds_internal(const void *obj, const struct uniqtype *t);

#ifdef __cplusplus
}
#endif

#endif /* VMEXT_MINICRUNCH_H */
