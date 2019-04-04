#include "foreign_library.h"
#include "structmember.h"
#include <dlfcn.h>
#include <link.h>

typedef struct {
    PyObject_HEAD
    struct link_map *dl_handle;
    PyObject *dl_uniqtype_dict; // Map uniqtype's to PyTypeObject's
} ForeignLibraryLoaderObject;

static int foreignlibloader_traverse(ForeignLibraryLoaderObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->dl_uniqtype_dict);
    return 0;
}

static int foreignlibloader_clear(ForeignLibraryLoaderObject *self)
{
    Py_CLEAR(self->dl_uniqtype_dict);
    return 0;
}

static void foreignlibloader_dealloc(ForeignLibraryLoaderObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->dl_uniqtype_dict);
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
    PyObject *loader;
};

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
        void *obj = (void *)(loadAddress + sym->st_value);
        PyModule_AddObject(ctxt->module, symname, ForeignFunction_New(symname, obj, ctxt->loader));
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
    ctxt.loader = (PyObject *) self;

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

    self->dl_uniqtype_dict = PyDict_New();
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
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_methods = foreignlibloader_methods,
    .tp_dealloc = (destructor) foreignlibloader_dealloc,
    .tp_traverse = (traverseproc) foreignlibloader_traverse,
    .tp_clear = (inquiry) foreignlibloader_clear,
};
