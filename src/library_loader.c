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
    PyObject *unsupported; // dict: export name -> reason it didn't materialize
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
    if (!ptype)
    {
        PyErr_Clear();
        return -1;
    }

    PyModule_AddObject(ctxt->module, type_name, (PyObject *) ptype);

    return 0;
}

static void recursively_add_useful_types(const struct uniqtype *type, struct add_sym_ctxt* ctxt)
{
    if (!type) return;
    switch (UNIQTYPE_KIND(type))
    {
        case BASE:
            // Ignore bit field types
            if (UNIQTYPE_BASE_TYPE_BIT_SIZE(type) != 8 * UNIQTYPE_SIZE_IN_BYTES(type))
                return;
            // falltrough
        case VOID:
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
        case ENUMERATION:
            // Expose the enum itself (handled as its underlying integer). If the
            // metadata records an underlying base type, surface that too.
            if (add_type_to_module(type, ctxt) == 0)
                recursively_add_useful_types(UNIQTYPE_ENUM_BASE_TYPE(type), ctxt);
            return;
        case ARRAY:
        case SUBRANGE:
            recursively_add_useful_types(UNIQTYPE_ARRAY_ELEMENT_TYPE(type), ctxt);
            return;
        case ADDRESS:
            recursively_add_useful_types(UNIQTYPE_ULTIMATE_POINTEE_TYPE(type), ctxt);
            return;
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

// Record an export we could not materialize into
// module.__pycallocs_unsupported__ (a dict symbol -> reason string), so a
// whole-library import stays introspectable: `dir(mod)` is what worked,
// the dict is what didn't and why. Consumes/clears any pending exception
// (its text becomes part of the reason).
static void record_unsupported(struct add_sym_ctxt *ctxt, const char *symname,
        const char *stage)
{
    PyObject *reason;
    PyObject *exc = PyErr_GetRaisedException();
    if (exc)
    {
        PyObject *exc_str = PyObject_Str(exc);
        reason = PyUnicode_FromFormat("%s: %S", stage, exc_str);
        Py_XDECREF(exc_str);
        Py_DECREF(exc);
    }
    else reason = PyUnicode_FromString(stage);
    if (!reason) { PyErr_Clear(); return; }
    if (ctxt->unsupported)
        PyDict_SetItemString(ctxt->unsupported, symname, reason);
    Py_DECREF(reason);
    PyErr_Clear();
}

static int add_sym_to_module(const ElfW(Sym) *sym, ElfW(Addr) loadAddress,
        char *strtab, void *arg)
{
    struct add_sym_ctxt *ctxt = arg;

    // Weak defined symbols are part of a library's API surface too (many
    // libraries export API under STB_WEAK), so accept both bindings.
    if ((ELF64_ST_TYPE(sym->st_info) == STT_FUNC
        || ELF64_ST_TYPE(sym->st_info) == STT_OBJECT)
        && (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL
            || ELF64_ST_BIND(sym->st_info) == STB_WEAK)
        && sym->st_shndx != SHN_UNDEF
        && sym->st_shndx != SHN_ABS)
    {
        char *symname = strtab + sym->st_name;
        // Ignore unamed symbols and reserved names
        if (symname[0] == '\0' || symname[0] == '_') return 0;

        void *data = (void *)(loadAddress + sym->st_value);

        const struct uniqtype *type = ss_alloc_get_type(data);
        if (!type)
        {
            record_unsupported(ctxt, symname, "no liballocs type at symbol");
            return 0;
        }
        recursively_add_useful_types(type, ctxt);

        ForeignTypeObject *ftype = ForeignType_GetOrCreate(type);
        if (!ftype)
        {
            record_unsupported(ctxt, symname, "no ForeignType for symbol's uniqtype");
            return 0;
        }

        PyObject *obj = ftype->ft_getfrom(data, ftype);
        Py_DECREF(ftype);
        if (!obj)
        {
            record_unsupported(ctxt, symname, "proxy creation failed");
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

// Iterating a library's own symbols only surfaces the base types it actually
// uses. The C scalar base types are supplied by the base-types provider .so
// (preloaded), so expose the full set on every foreign module too -- e.g. so
// elflib.basic.uint__16 resolves even though basic.c never mentions a 16-bit
// unsigned. add_type_to_module() uses the canonical UNIQTYPE_NAME (with $$ ->
// __, matching the symbol-derived types) and skips any name already present.
static void add_base_types_to_module(struct add_sym_ctxt *ctxt)
{
#define ADD_BASE_TYPE(typ) add_type_to_module(&__uniqtype__##typ, ctxt)
    ADD_BASE_TYPE(void);
    ADD_BASE_TYPE(int);
    ADD_BASE_TYPE(unsigned_int);
    ADD_BASE_TYPE(signed_char);
    ADD_BASE_TYPE(unsigned_char);
    ADD_BASE_TYPE(long_int);
    ADD_BASE_TYPE(unsigned_long_int);
    ADD_BASE_TYPE(short_int);
    ADD_BASE_TYPE(short_unsigned_int);
    ADD_BASE_TYPE(float);
    ADD_BASE_TYPE(double);
#undef ADD_BASE_TYPE
}

static PyObject *libloader_exec(LibraryLoaderObject *self, PyObject *module)
{
    struct add_sym_ctxt ctxt;
    ctxt.module = module;
    ctxt.loader = self;
    ctxt.unsupported = PyDict_New();

    dl_iterate_syms(self->dl_handle, add_sym_to_module, &ctxt);
    add_base_types_to_module(&ctxt);
    // Full-library introspection: which exports did NOT materialize, and why.
    // Empty dict == every accepted export imported cleanly.
    if (ctxt.unsupported)
        PyModule_AddObject(module, "__pycallocs_unsupported__", ctxt.unsupported);
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

