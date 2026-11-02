/* Shared uniqtype -> ffi_type mapping for the VM extensions' function proxies
 * (moved out of the per-VM function_proxy.c files, where it existed twice
 * near-verbatim; allocation is plain malloc/free so no VM runtime is
 * involved). */

#include "ffitypes.h"

#include <stdlib.h>

#include <liballocs.h>
#include <dwarf.h>

void free_ffi_type(ffi_type *typ)
{
    if (typ->type == FFI_TYPE_STRUCT)
    {
        free_ffi_type_arr(typ->elements);
        free(typ);
    }
}

void free_ffi_type_arr(ffi_type **arr)
{
    if (!arr) return;
    for (int i = 0; arr[i]; ++i) free_ffi_type(arr[i]);
    free(arr);
}

ffi_type *ffi_type_for_uniqtype(const struct uniqtype *type)
{
    switch (UNIQTYPE_KIND(type))
    {
        case VOID:
            return &ffi_type_void;
        case ARRAY:
        case ADDRESS:
        case SUBPROGRAM:
            return &ffi_type_pointer;
        case ENUMERATION:
            /* Prefer the recorded underlying base type; when liballocs did
             * not record one (the common case) fall back to a signed integer
             * sized to the enum -- C enum constants have type int. */
            if (type->related[0].un.t.ptr)
                type = type->related[0].un.t.ptr; /* fall through as BASE */
            else
            {
                unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
                if (size == 1) return &ffi_type_sint8;
                if (size == 2) return &ffi_type_sint16;
                if (size == 4) return &ffi_type_sint32;
                if (size == 8) return &ffi_type_sint64;
                return NULL;
            }
            /* fallthrough */
        case BASE:
        {
            unsigned size = UNIQTYPE_SIZE_IN_BYTES(type);
            switch (type->un.base.enc)
            {
                case DW_ATE_boolean:
                    return &ffi_type_uint8;
                case DW_ATE_address:
                case DW_ATE_unsigned:
                case DW_ATE_unsigned_char:
                    if (size == 1) return &ffi_type_uint8;
                    if (size == 2) return &ffi_type_uint16;
                    if (size == 4) return &ffi_type_uint32;
                    if (size == 8) return &ffi_type_uint64;
                    return NULL;
                case DW_ATE_signed:
                case DW_ATE_signed_char:
                    if (size == 1) return &ffi_type_sint8;
                    if (size == 2) return &ffi_type_sint16;
                    if (size == 4) return &ffi_type_sint32;
                    if (size == 8) return &ffi_type_sint64;
                    return NULL;
                case DW_ATE_float:
                    if (size == 4) return &ffi_type_float;
                    if (size == 8) return &ffi_type_double;
                    if (size == 16) return &ffi_type_longdouble;
                    return NULL;
#ifdef FFI_TARGET_HAS_COMPLEX_TYPE
                case DW_ATE_complex_float:
                    if (size == 8) return &ffi_type_complex_float;
                    if (size == 16) return &ffi_type_complex_double;
                    if (size == 32) return &ffi_type_complex_longdouble;
                    return NULL;
#endif
                default:
                    return NULL;
            }
        }
        case COMPOSITE:
        {
            /* Field ffi_types are required to comply with ABI rules like the
             * AMD64 ELF classification. */
            int nb_fields = type->un.composite.nmemb;
            ffi_type **field_ffi_types = malloc((1 + nb_fields) * sizeof(ffi_type *));
            for (int i = 0; i < nb_fields; ++i)
            {
                field_ffi_types[i] = ffi_type_for_uniqtype(type->related[i].un.t.ptr);
                if (!field_ffi_types[i])
                {
                    free_ffi_type_arr(field_ffi_types);
                    return NULL;
                }
            }
            field_ffi_types[nb_fields] = NULL;

            ffi_type *ffityp = malloc(sizeof(ffi_type));
            *ffityp = (ffi_type){
                .type = FFI_TYPE_STRUCT,
                .elements = field_ffi_types,
            };

            /* Fills size/alignment (ffi_get_struct_offsets needs a newer
             * libffi, so a dummy cif prep is used instead). */
            ffi_cif dummy;
            if (ffi_prep_cif(&dummy, FFI_DEFAULT_ABI, 0, ffityp, NULL) != FFI_OK)
            {
                free_ffi_type(ffityp);
                return NULL;
            }

            /* Patch size and alignment to handle unions. */
            if (ffityp->size != UNIQTYPE_SIZE_IN_BYTES(type))
            {
                ffityp->size = UNIQTYPE_SIZE_IN_BYTES(type);
                for (int i = 0; i < nb_fields; ++i)
                    if (field_ffi_types[i]->alignment > ffityp->alignment)
                        ffityp->alignment = field_ffi_types[i]->alignment;
            }
            return ffityp;
        }
        case SUBRANGE:
        default:
            return NULL;
    }
}
