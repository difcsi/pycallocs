#ifndef VMEXT_FFITYPES_H
#define VMEXT_FFITYPES_H

/* uniqtype -> ffi_type mapping, shared by every VM extension's function
 * proxy. COMPOSITE builds a real elements[] array (required for AMD64 ABI
 * classification) and patches size/alignment for unions.
 *
 * Ownership: results referring to COMPOSITE types are heap-allocated with
 * malloc and must be released with free_ffi_type / free_ffi_type_arr; all
 * other results are the static libffi singletons (the free functions know
 * the difference). Returns NULL for unrepresentable types. */

#include <ffi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct uniqtype;

ffi_type *ffi_type_for_uniqtype(const struct uniqtype *type);
void free_ffi_type(ffi_type *typ);
/* Frees a NULL-terminated malloc'd array of ffi_type* and the array itself. */
void free_ffi_type_arr(ffi_type **arr);

#ifdef __cplusplus
}
#endif

#endif /* VMEXT_FFITYPES_H */
