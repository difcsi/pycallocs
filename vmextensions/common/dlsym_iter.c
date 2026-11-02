/* Shared .dynsym iteration for the VM extensions' library loaders (moved out
 * of the per-VM library_loader.c files, where it existed twice verbatim). */

#include "dlsym_iter.h"

#include <assert.h>
#include <elf.h>

int dl_iterate_syms(struct link_map *handle,
        int (*callback)(const ElfW(Sym) *, ElfW(Addr), char *, void *), void *arg)
{
    const ElfW(Dyn) *p_dyn = handle->l_ld;
    const ElfW(Sym) *p_dynsym = 0;
    char *p_dynstr = 0;
    while (p_dyn->d_tag != DT_NULL)
    {
        switch (p_dyn->d_tag)
        {
            case DT_SYMTAB:
                p_dynsym = (const ElfW(Sym) *)p_dyn->d_un.d_ptr;
                break;
            case DT_STRTAB:
                p_dynstr = (char *)p_dyn->d_un.d_ptr;
                break;
            default: break;
        }
        ++p_dyn;
    }
    if (!p_dynsym || !p_dynstr) return -1;
    assert((char *)p_dynstr > (char *)p_dynsym);
    assert(((char *)p_dynstr - (char *)p_dynsym) % sizeof(ElfW(Sym)) == 0);

    int ret = 0;
    for (const ElfW(Sym) *sym = p_dynsym; (char *)sym != p_dynstr; ++sym)
    {
        ret = callback(sym, handle->l_addr, p_dynstr, arg);
        if (ret != 0) break;
    }
    return ret;
}
