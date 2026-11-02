#ifndef VMEXT_DLSYM_ITER_H
#define VMEXT_DLSYM_ITER_H

/* Iterate a freshly dlopen'ed library's own .dynsym (DT_SYMTAB/DT_STRTAB out
 * of its link_map), calling `callback` for every ElfW(Sym). Shared by every
 * VM extension's library loader. The callback returns non-zero to stop the
 * iteration (that value is returned); 0 continues. Returns -1 if the library
 * has no dynamic symbol table. */

#include <link.h>

#ifdef __cplusplus
extern "C" {
#endif

int dl_iterate_syms(struct link_map *handle,
        int (*callback)(const ElfW(Sym) *, ElfW(Addr), char *, void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* VMEXT_DLSYM_ITER_H */
