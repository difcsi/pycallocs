#include "foreign_library.h"
#include "structmember.h"
#include <dlfcn.h>
#include <link.h>

/* We should replace all this with a solution using PEP 302 */

typedef struct {
    PyObject_HEAD
    PyObject *dl_name;
    PyObject *dl_dict;
    struct link_map *dl_handle;
} ForeignLibraryObject;

static PyMemberDef foreignlib_members[] = {
    {"__dict__", T_OBJECT, offsetof(ForeignLibraryObject, dl_dict), READONLY},
    {"__name__", T_OBJECT, offsetof(ForeignLibraryObject, dl_name), READONLY},
    {0}
};

static int foreignlib_traverse(ForeignLibraryObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->dl_dict);
    return 0;
}

static int foreignlib_clear(ForeignLibraryObject *self)
{
    Py_CLEAR(self->dl_dict);
    return 0;
}

static void foreignlib_dealloc(ForeignLibraryObject *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->dl_name);
    Py_XDECREF(self->dl_dict);
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

static int add_sym_to_dict(const ElfW(Sym) *sym, ElfW(Addr) loadAddress,
        char *strtab, void *dict)
{
    if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC
        /*|| ELF64_ST_TYPE(sym->st_info) == STT_OBJECT*/)
        && ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
        && sym->st_shndx != SHN_UNDEF
        && sym->st_shndx != SHN_ABS)
    {
        char *symname = strtab + sym->st_name;
        void *obj = (void *)(loadAddress + sym->st_value);
        PyDict_SetItemString(dict, symname, ForeignFunction_New(symname, obj));
    }

    return 0;
}

static int foreignlib_init(ForeignLibraryObject* self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"name", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "U:ForeignLibrary", kwlist,
                                     &self->dl_name))
        return -1;
    
    self->dl_dict = PyDict_New();
    if (!self->dl_dict)
        return -1;

    const char *dlname = PyUnicode_AsUTF8(self->dl_name);

    self->dl_handle = dlopen(dlname, RTLD_NOW | RTLD_GLOBAL);
    if (!self->dl_handle)
    {
        PyErr_Format(PyExc_ImportError, "Loading of native shared library '%s' failed", dlname);
        return -1;
    }
    dl_iterate_syms(self->dl_handle, add_sym_to_dict, self->dl_dict);

    return 0;
}

static PyObject *foreignlib_repr(ForeignLibraryObject *self)
{
    return PyUnicode_FromFormat("<foreign library '%U'>", self->dl_name);
}

PyTypeObject ForeignLibrary_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignLibrary",
    .tp_doc = "Foreign library imported through the allocs module",
    .tp_basicsize = sizeof(ForeignLibraryObject),
    .tp_itemsize = 0,    
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC, 
    .tp_members = foreignlib_members,
    .tp_dictoffset = offsetof(ForeignLibraryObject, dl_dict),    
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc) foreignlib_init,
    .tp_dealloc = (destructor) foreignlib_dealloc,
    .tp_traverse = (traverseproc) foreignlib_traverse,
    .tp_clear = (inquiry) foreignlib_clear,
    .tp_repr = (reprfunc) foreignlib_repr,
};
