#include "foreign_library.h"
#include "structmember.h"
#include <dlfcn.h>
#include <link.h>

typedef struct {
    PyObject_HEAD
    struct link_map *dl_handle;
} LibraryLoaderObject;

static void libloader_dealloc(LibraryLoaderObject *self)
{
    if (self->dl_handle) dlclose(self->dl_handle);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static int dl_iterate_syms(struct link_map *handle,
        int (*callback)(const ElfW(Sym)*, ElfW(Addr), char*, void*), void *arg)
{
    const ElfW(Dyn) *p_dyn = handle->l_ld;
    const ElfW(Sym) *p_dynsym = 0;
    char *p_dynstr = 0;
    while (p_dyn->d_tag != DT_NULL)
    {
        switch (p_dyn->d_tag)
        {
            case DT_SYMTAB:
                p_dynsym = (const ElfW(Sym)*) p_dyn->d_un.d_ptr;
                break;
            case DT_STRTAB:
                p_dynstr = (char*) p_dyn->d_un.d_ptr;
                break;
            default: break;
        }
        ++p_dyn;
    }
    if (!p_dynsym || !p_dynstr) return -1;
    assert((char*) p_dynstr > (char*) p_dynsym);
    assert(((char*) p_dynstr - (char*) p_dynsym) % sizeof (ElfW(Sym)) == 0);

    int ret = 0;
    for (const ElfW(Sym) *sym = p_dynsym; (char*) sym != p_dynstr; ++sym)
    {
        ret = callback(sym, handle->l_addr, p_dynstr, arg);
        if (ret != 0) break;
    }

    return ret;
}

struct add_sym_ctxt
{
    PyObject *module;
    LibraryLoaderObject *loader;
};

static int add_type_to_module(const struct uniqtype *type, struct add_sym_ctxt* ctxt)
{
    // FIXME: For many types these names will feel very weird
    char type_name[256];
    strncpy(type_name, UNIQTYPE_NAME(type), sizeof(type_name));
    type_name[255] = '\0'; // We want NULL-terminated string in all cases
    for (unsigned i = 0; i < 256; ++i)
    {
        // Replace $ by _
        if (type_name[i] == '$') type_name[i] = '_';
    }

    // TODO: Manage name clashes
    if (PyObject_HasAttrString(ctxt->module, type_name)) return 1;

    ForeignTypeObject *ptype = ForeignType_GetOrCreate(type);
    if (!ptype) return -1;

    PyModule_AddObject(ctxt->module, type_name, (PyObject *) ptype);

    return 0;
}

static void recursively_add_useful_types(const struct uniqtype *type, struct add_sym_ctxt* ctxt)
{
    if (!type) return;
    switch (UNIQTYPE_KIND(type))
    {
        case VOID:
        case BASE:
            add_type_to_module(type, ctxt);
            return;
        case COMPOSITE:
            if (add_type_to_module(type, ctxt) == 0)
            {
                unsigned nb_memb = UNIQTYPE_COMPOSITE_MEMBER_COUNT(type);
                for (int i = 0; i < nb_memb; ++i)
                {
                    recursively_add_useful_types(type->related[i].un.t.ptr, ctxt);
                }
            }
            return;
        case ENUMERATION: // TODO: handle enums properly
            return;
        case ARRAY:
        case SUBRANGE:
            recursively_add_useful_types(UNIQTYPE_ARRAY_ELEMENT_TYPE(type), ctxt);
            return;
        case ADDRESS:
        {
            const struct uniqtype *pointee_type = UNIQTYPE_ULTIMATE_POINTEE_TYPE(type);
            recursively_add_useful_types(pointee_type, ctxt);

            // We need to be able to create closures if it's a function type
            // TODO: Find more user friendly function name
            if (pointee_type && UNIQTYPE_KIND(pointee_type) == SUBPROGRAM)
                add_type_to_module(pointee_type, ctxt);

            return;
        }
        case SUBPROGRAM:
        {
            unsigned nb_subtypes = type->un.subprogram.narg + type->un.subprogram.nret;
            for (int i = 0; i < nb_subtypes; ++i)
            {
                recursively_add_useful_types(type->related[i].un.t.ptr, ctxt);
            }
            return;
        }
        default:
            return; // not handled
    }
}

static int add_sym_to_module(const ElfW(Sym) *sym, ElfW(Addr) loadAddress,
        char *strtab, void *arg)
{
    struct add_sym_ctxt *ctxt = arg;

    if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC
        || ELF64_ST_TYPE(sym->st_info) == STT_OBJECT)
        && ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
        && sym->st_shndx != SHN_UNDEF
        && sym->st_shndx != SHN_ABS)
    {
        char *symname = strtab + sym->st_name;
        // Ignore unamed symbols and reserved names
        if (symname[0] == '\0' || symname[0] == '_') return 0;

        void *data = (void *)(loadAddress + sym->st_value);

        const struct uniqtype *type = __liballocs_get_alloc_type(data);
        if (!type) return 0;
        recursively_add_useful_types(type, ctxt);

        ForeignTypeObject *ftype = ForeignType_GetOrCreate(type);
        if (!ftype)
        {
            PyErr_Clear();
            return 0;
        }

        PyObject *obj = ftype->ft_getfrom(data, ftype);
        Py_DECREF(ftype);
        if (!obj)
        {
            PyErr_Clear();
            return 0;
        }

        PyModule_AddObject(ctxt->module, symname, obj);
    }

    return 0;
}

static PyObject *libloader_create(PyObject *self, PyObject *spec)
{
    Py_RETURN_NONE;
}

static PyObject *libloader_exec(LibraryLoaderObject *self, PyObject *module)
{
    struct add_sym_ctxt ctxt;
    ctxt.module = module;
    ctxt.loader = self;

    dl_iterate_syms(self->dl_handle, add_sym_to_module, &ctxt);
    Py_RETURN_NONE;
}

static PyMethodDef libloader_methods[] = {
    {"create_module", (PyCFunction) libloader_create, METH_O, NULL},
    {"exec_module", (PyCFunction) libloader_exec, METH_O, NULL},
    {NULL}
};

static int libloader_init(LibraryLoaderObject* self, PyObject *args, PyObject *kwds)
{
    static char *kw_names[] = {"filename", NULL};
    const char *dlname;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s:LibraryLoader",
                kw_names, &dlname))
    {
        return -1;
    }

    self->dl_handle = dlopen(dlname, RTLD_NOW | RTLD_GLOBAL);
    if (!self->dl_handle)
    {
        PyErr_Format(PyExc_ImportError, "Loading of native shared library '%s' failed", dlname);
        return -1;
    }

    return 0;
}

PyTypeObject LibraryLoader_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.LibraryLoader",
    .tp_doc = "Loader to import foreign library with liballocs",
    .tp_basicsize = sizeof(LibraryLoaderObject),
    .tp_itemsize = 0,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) libloader_init,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = libloader_methods,
    .tp_dealloc = (destructor) libloader_dealloc,
};

