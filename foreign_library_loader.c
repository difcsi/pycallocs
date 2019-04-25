#include "foreign_library.h"
#include "structmember.h"
#include <dlfcn.h>
#include <link.h>

typedef struct {
    PyObject_HEAD
    struct link_map *dl_handle;
} ForeignLibraryLoaderObject;

static void foreignlibloader_dealloc(ForeignLibraryLoaderObject *self)
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
    ForeignLibraryLoaderObject *loader;
};

static void add_type_to_module(const struct uniqtype *type, struct add_sym_ctxt* ctxt)
{
    switch (type->un.info.kind)
    {
        case VOID:
        case BASE:
        case COMPOSITE:
        {
            const char *type_name = UNIQTYPE_NAME(type);
            // TODO: Manage name clashes
            if (PyObject_HasAttrString(ctxt->module, type_name)) return;

            ForeignTypeObject *ptype = ForeignType_GetOrCreate(type);
            if (!ptype) return;

            PyModule_AddObject(ctxt->module, type_name, (PyObject *) ptype);

            if (type->un.info.kind == COMPOSITE)
            {
                unsigned nb_memb = type->un.composite.nmemb;
                for (int i = 0; i < nb_memb; ++i)
                {
                    add_type_to_module(type->related[i].un.t.ptr, ctxt);
                }
            }
            return;
        }
        case ENUMERATION: // TODO: handle enums properly
            return;
        case ARRAY:
        case SUBRANGE:
            add_type_to_module(type->related[0].un.t.ptr, ctxt);
            return;
        case ADDRESS:
            // TODO: check related[1] is always defined
            add_type_to_module(type->related[1].un.t.ptr, ctxt);
            return;
        case SUBPROGRAM:
        {
            unsigned nb_subtypes = type->un.subprogram.narg + type->un.subprogram.nret;
            for (int i = 0; i < nb_subtypes; ++i)
            {
                add_type_to_module(type->related[i].un.t.ptr, ctxt);
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
        /*|| ELF64_ST_TYPE(sym->st_info) == STT_OBJECT*/)
        && ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
        && sym->st_shndx != SHN_UNDEF
        && sym->st_shndx != SHN_ABS)
    {
        char *symname = strtab + sym->st_name;
        // Ignore unamed symbols and reserved names
        if (symname[0] == '\0' || symname[0] == '_') return 0;

        void *obj = (void *)(loadAddress + sym->st_value);

        PyObject *func = ForeignFunction_New(symname, obj, (PyObject *) ctxt->loader);
        if (!func)
        {
            // Ignore functions that fail to be loaded
            PyErr_Clear();
            return 0;
        }

        PyModule_AddObject(ctxt->module, symname, func);
        add_type_to_module(ForeignFunction_GetType(func), ctxt);
    }

    return 0;
}

static PyObject *foreignlibloader_create(PyObject *self, PyObject *spec)
{
    Py_RETURN_NONE;
}

static PyObject *foreignlibloader_exec(ForeignLibraryLoaderObject *self, PyObject *module)
{
    struct add_sym_ctxt ctxt;
    ctxt.module = module;
    ctxt.loader = self;

    dl_iterate_syms(self->dl_handle, add_sym_to_module, &ctxt);
    Py_RETURN_NONE;
}

static PyMethodDef foreignlibloader_methods[] = {
    {"create_module", (PyCFunction) foreignlibloader_create, METH_O, NULL},
    {"exec_module", (PyCFunction) foreignlibloader_exec, METH_O, NULL},
    {NULL}
};

static int foreignlibloader_init(ForeignLibraryLoaderObject* self, PyObject *args, PyObject *kwds)
{
    static char *kw_names[] = {"filename", NULL};
    const char *dlname;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s:ForeignLibraryLoader",
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

PyTypeObject ForeignLibraryLoader_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignLibraryLoader",
    .tp_doc = "Loader to import foreign library with liballocs",
    .tp_basicsize = sizeof(ForeignLibraryLoaderObject),
    .tp_itemsize = 0,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) foreignlibloader_init,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = foreignlibloader_methods,
    .tp_dealloc = (destructor) foreignlibloader_dealloc,
};

