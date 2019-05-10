#include "foreign_library.h"

struct field_info {
    ForeignTypeObject *type;
    size_t offset;
};

typedef struct {
    PyTypeObject tp_base;
    struct field_info *fct_fieldinfos;
} ForeignCompositeProxyTypeObject;

static PyObject *foreigncompositetype_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign composite proxy types");
    return NULL;
}

static void foreigncompositetype_dealloc(ForeignCompositeProxyTypeObject *self)
{
    for (int i = 0; self->tp_base.tp_getset[i].name ; ++i)
    {
        Py_XDECREF(self->fct_fieldinfos[i].type);
    }
    PyMem_Free(self->tp_base.tp_getset);
    PyMem_Free(self->fct_fieldinfos);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyTypeObject ForeignComposite_ProxyMetatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignCompositeProxyType",
    .tp_doc = "Type for foreign composite proxies",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(ForeignCompositeProxyTypeObject),
    .tp_new = foreigncompositetype_new,
    .tp_dealloc = (destructor) foreigncompositetype_dealloc,
};

static PyObject *foreigncomposite_getinvalidfield(PyObject *self, void *cb)
{
    PyErr_SetString(PyExc_TypeError, "Field's foreign datatype is not handled");
    return NULL;
}

static PyObject *foreigncomposite_getfield(ForeignProxyObject *self, struct field_info *field_info)
{
    void *field = self->fpo_ptr + field_info->offset;
    ForeignTypeObject *ftype = field_info->type;
    return ftype->ft_getfrom(field, ftype, self->fpo_allocator);
}

static int foreigncomposite_setfield(ForeignProxyObject *self, PyObject *value, struct field_info *field_info)
{
    void *field = self->fpo_ptr + field_info->offset;
    ForeignTypeObject *ftype = field_info->type;
    return ftype->ft_storeinto(value, field, ftype);
}

static int foreigncomposite_init(ForeignProxyObject *self, PyObject *args, PyObject *kwargs)
{
    PyTypeObject *type = Py_TYPE(self);

    unsigned nargs = PyTuple_GET_SIZE(args);
    unsigned nkwargs = kwargs ? PyDict_Size(kwargs) : 0;

    // Default initialization => zero initialize the whole structure
    if (nargs == 0 && nkwargs == 0)
    {
        memset(self->fpo_ptr, 0, type->tp_itemsize);
        return 0;
    }

    // Copy if 1 positional same type struct argument and no keyword argument
    if (nargs == 1 && nkwargs == 0)
    {
        PyObject *arg = PyTuple_GET_ITEM(args, 0);
        if (PyObject_TypeCheck(arg, type))
        {
            ForeignProxyObject *other = (ForeignProxyObject *) arg;

            // Checking pointer equality is enough for checking aliasing
            if (self->fpo_ptr == other->fpo_ptr)
            {
                PyErr_SetString(PyExc_ValueError, "Trying to copy inside yourself");
                return -1;
            }

            memcpy(self->fpo_ptr, other->fpo_ptr, type->tp_itemsize);
            return 0;
        }
    }

    // Else we initialize fields in order or by taking a keyword argument
    unsigned initialized_fields = 0;
    for (unsigned i = 0 ; type->tp_getset[i].name ; ++i)
    {
        // No initialization for unrecognized types
        if (type->tp_getset[i].get != (getter) foreigncomposite_getfield) continue;

        // Direct call to setfield to also copy nested structs
        if (i < nargs) // Use positional arguments first
        {
            PyObject *value = PyTuple_GET_ITEM(args, i);
            if (foreigncomposite_setfield(self, value, type->tp_getset[i].closure) < 0)
                return -1;
            ++initialized_fields;
        }
        else
        {
            PyObject *arg = kwargs ? PyDict_GetItemString(kwargs, type->tp_getset[i].name) : NULL;
            if (arg)
            {
                if (foreigncomposite_setfield(self, arg, type->tp_getset[i].closure) < 0)
                    return -1;
                ++initialized_fields;
            }
            else
            {
                // No initialization data for the field => zero initialization
                struct field_info *field_info = type->tp_getset[i].closure;
                void *field = self->fpo_ptr + field_info->offset;
                memset(field, 0, UNIQTYPE_SIZE_IN_BYTES(field_info->type->ft_type));
            }
        }
    }

    if (initialized_fields <= nargs + nkwargs) return 0;
    else if (initialized_fields <= nargs)
    {
        PyErr_SetString(PyExc_TypeError, "Invalid keyword arguments");
        return -1;
    }
    else
    {
        PyErr_Format(PyExc_ValueError,
                "Too many positional arguments (there are only %d fields)",
                initialized_fields);
        return -1;
    }
}

static PyObject *foreigncomposite_repr(ForeignProxyObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject *field_repr_list = PyList_New(0);
    for (int i = 0 ; type->tp_getset[i].name ; ++i)
    {
        // Skip unrecognized types. TODO: Maybe show them...
        if (type->tp_getset[i].get != (getter) foreigncomposite_getfield) continue;

        PyObject *field_obj = foreigncomposite_getfield(self, type->tp_getset[i].closure);
        PyObject *field_val_repr = PyObject_Repr(field_obj);
        Py_DECREF(field_obj);
        if (!field_val_repr)
        {
            Py_DECREF(field_repr_list);
            return NULL;
        }
        PyObject *field_repr = PyUnicode_FromFormat("%s: %U", type->tp_getset[i].name, field_val_repr);
        Py_DECREF(field_val_repr);
        PyList_Append(field_repr_list, field_repr);
        Py_DECREF(field_repr);
    }
    PyObject *sep = PyUnicode_FromString(", ");
    PyObject *fields_str = PyUnicode_Join(sep, field_repr_list);
    Py_DECREF(sep);
    Py_DECREF(field_repr_list);
    PyObject *repr = PyUnicode_FromFormat("(%s){%U}", type->tp_name, fields_str);
    Py_DECREF(fields_str);
    return repr;
}

ForeignTypeObject *ForeignComposite_NewType(const struct uniqtype *type)
{
    int nb_fields = type->un.composite.nmemb;
    const char **field_names = __liballocs_uniqtype_subobject_names(type);
    if (!field_names) nb_fields = 0; // Consider it as an incomplete struct

    ForeignCompositeProxyTypeObject *htype =
        PyObject_GC_NewVar(ForeignCompositeProxyTypeObject, &ForeignComposite_ProxyMetatype, 0);
    PyGetSetDef *getsetdefs = PyMem_Malloc((nb_fields + 1) * sizeof(PyGetSetDef));
    struct field_info *field_infos = PyMem_Malloc(nb_fields * sizeof(struct field_info));

    for (int i_field = 0 ; i_field < nb_fields ; ++i_field)
    {
        // We cannot initialize field_infos types here because it could generate
        // infinite recursion for recursive structs.
        // Instead wait for a future call to ForeignComposite_InitType.

        getsetdefs[i_field] = (PyGetSetDef){
            .name = (char *) field_names[i_field],
            .get = foreigncomposite_getinvalidfield,
            .set = NULL,
            .doc = NULL,
            .closure = &field_infos[i_field],
        };
    }
    getsetdefs[nb_fields] = (PyGetSetDef){NULL};

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base, // <- Keep the object base
        .tp_name = UNIQTYPE_NAME(type),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(type),
        .tp_base = &ForeignProxy_Type,
        .tp_getset = getsetdefs,
        .tp_init = (initproc) foreigncomposite_init,
        .tp_repr = (reprfunc) foreigncomposite_repr,
    };
    htype->fct_fieldinfos = field_infos;

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        Py_DECREF(htype);
        return NULL;
    }

    ForeignTypeObject *ftype = ForeignProxy_NewType(type, (PyTypeObject *) htype);
    Py_INCREF(htype);
    ftype->ft_constructor = (PyObject *) htype;
    return ftype;
}

void ForeignComposite_InitType(ForeignTypeObject *self, const struct uniqtype *type)
{
    PyTypeObject *proxytype = self->ft_proxy_type;

    for (int i_field = 0 ; proxytype->tp_getset[i_field].name ; ++i_field)
    {
        const struct uniqtype_rel_info *field_rel_info = &type->related[i_field];

        struct field_info *finfo = proxytype->tp_getset[i_field].closure;
        finfo->type = ForeignType_GetOrCreate(field_rel_info->un.memb.ptr);
        finfo->offset = field_rel_info->un.memb.off;

        if (finfo->type)
        {
            proxytype->tp_getset[i_field].get = (getter) foreigncomposite_getfield;
            if (ForeignType_IsTriviallyCopiable(finfo->type))
            {
                proxytype->tp_getset[i_field].set = (setter) foreigncomposite_setfield;
            }
        }
    }
}
