#include "foreign_library.h"

struct field_info {
    ForeignTypeObject *type;
    size_t offset;
};

typedef struct {
    PyTypeObject tp_base;
    struct field_info *fct_fieldinfos;
} CompositeProxyTypeObject;

static PyObject *compositeproxytype_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    PyErr_SetString(PyExc_TypeError, "Cannot directly create foreign composite proxy types");
    return NULL;
}

static void compositeproxytype_dealloc(CompositeProxyTypeObject *self)
{
    for (int i = 0; self->tp_base.tp_getset[i].name ; ++i)
    {
        Py_XDECREF(self->fct_fieldinfos[i].type);
    }
    PyMem_Free(self->tp_base.tp_getset);
    PyMem_Free(self->fct_fieldinfos);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyTypeObject CompositeProxy_Metatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.CompositeProxyType",
    .tp_doc = "Type for foreign composite proxies",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(CompositeProxyTypeObject),
    .tp_new = compositeproxytype_new,
    .tp_dealloc = (destructor) compositeproxytype_dealloc,
};

static PyObject *compositeproxy_getinvalidfield(PyObject *self, void *cb)
{
    PyErr_SetString(PyExc_TypeError, "Field's foreign datatype is not handled");
    return NULL;
}

static PyObject *compositeproxy_getfield(ProxyObject *self, struct field_info *field_info)
{
    void *field = self->p_ptr + field_info->offset;
    ForeignTypeObject *ftype = field_info->type;
    return ftype->ft_getfrom(field, ftype);
}

static int compositeproxy_setfield(ProxyObject *self, PyObject *value, struct field_info *field_info)
{
    void *field = self->p_ptr + field_info->offset;
    ForeignTypeObject *ftype = field_info->type;
    return ftype->ft_storeinto(value, field, ftype);
}

static int compositeproxy_init(ProxyObject *self, PyObject *args, PyObject *kwargs)
{
    PyTypeObject *type = Py_TYPE(self);

    bool convert_mode = !PyTuple_Check(args) && kwargs == NULL;
    unsigned nargs = convert_mode ? 1 : PyTuple_GET_SIZE(args);
    unsigned nkwargs = kwargs ? PyDict_Size(kwargs) : 0;

    // Default initialization => zero initialize the whole structure
    if (nargs == 0 && nkwargs == 0)
    {
        memset(self->p_ptr, 0, type->tp_itemsize);
        return 0;
    }

    // Copy if 1 positional same type struct argument and no keyword argument
    if (nargs == 1 && nkwargs == 0)
    {
        PyObject *arg = convert_mode ? args : PyTuple_GET_ITEM(args, 0);
        if (PyObject_TypeCheck(arg, type))
        {
            ProxyObject *other = (ProxyObject *) arg;

            // Checking pointer equality is enough for checking aliasing
            if (self->p_ptr == other->p_ptr)
            {
                PyErr_SetString(PyExc_ValueError, "Trying to copy inside yourself");
                return -1;
            }

            memcpy(self->p_ptr, other->p_ptr, type->tp_itemsize);
            return 0;
        }

        if (PyDict_Check(arg))
        {
            // Convert the single dictionary argument into kwargs
            nargs = 0;
            kwargs = arg;
            nkwargs = PyDict_Size(arg);
        }
        else
        {
            // Try to initialize from a plain object using attributes for all
            // fields. All attributes must match, or we fail.
            bool objinit_success = true;
            for (unsigned i = 0 ; type->tp_getset[i].name ; ++i)
            {
                PyObject *field = PyObject_GetAttrString(arg, type->tp_getset[i].name);
                if (!field || compositeproxy_setfield(self, field,
                            type->tp_getset[i].closure) < 0)
                {
                    PyErr_Clear();
                    objinit_success = false;
                    break;
                }
            }
            if (objinit_success) return 0;
        }
    }

    // Reject further initialization if we are in convert mode
    if (convert_mode && !nkwargs)
    {
        PyErr_Format(PyExc_TypeError,
            "Cannot convert object of type '%s' into foreign composite '%s'",
            Py_TYPE(args)->tp_name, type->tp_name);
        return -1;
    }

    // Else we initialize fields in order or by taking a keyword argument
    unsigned initialized_fields = 0;
    for (unsigned i = 0 ; type->tp_getset[i].name ; ++i)
    {
        // No initialization for unrecognized types
        if (type->tp_getset[i].get != (getter) compositeproxy_getfield) continue;

        // Direct call to setfield to also copy nested structs
        if (i < nargs) // Use positional arguments first
        {
            PyObject *value = PyTuple_GET_ITEM(args, i);
            if (compositeproxy_setfield(self, value, type->tp_getset[i].closure) < 0)
                return -1;
            ++initialized_fields;
        }
        else
        {
            PyObject *arg = kwargs ? PyDict_GetItemString(kwargs, type->tp_getset[i].name) : NULL;
            if (arg)
            {
                if (compositeproxy_setfield(self, arg, type->tp_getset[i].closure) < 0)
                    return -1;
                ++initialized_fields;
            }
            else
            {
                // No initialization data for the field => zero initialization
                struct field_info *field_info = type->tp_getset[i].closure;
                void *field = self->p_ptr + field_info->offset;
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

static PyObject *compositeproxy_ctor(PyObject *args, PyObject *kwds, ForeignTypeObject *type)
{
    ProxyObject *obj = PyObject_GC_New(ProxyObject, type->ft_proxy_type);
    if (obj)
    {
        obj->p_ptr = malloc(type->ft_proxy_type->tp_itemsize);
        __liballocs_set_alloc_type(obj->p_ptr, type->ft_type);
        Proxy_Register(obj);
        __liballocs_detach_manual_dealloc_policy(obj->p_ptr);
        // Note that because the Python GC policy has been attached obj->p_ptr
        // is never freed at this point
        if (compositeproxy_init(obj, args, kwds) < 0)
        {
            Py_DECREF(obj);
            return NULL;
        }
    }
    return (PyObject *) obj;
}

static PyObject *compositeproxy_repr(ProxyObject *self)
{
    PyTypeObject *type = Py_TYPE(self);

    // Break repr cycles: do not print a nested struct with an already seen type
    int rec = Py_ReprEnter((PyObject *) type);
    if (rec < 0) return NULL;
    if (rec > 0) return PyUnicode_FromFormat("(%s){...}", type->tp_name);

    PyObject *field_repr_list = PyList_New(0);
    for (int i = 0 ; type->tp_getset[i].name ; ++i)
    {
        // Skip unrecognized types. TODO: Maybe show them...
        if (type->tp_getset[i].get != (getter) compositeproxy_getfield) continue;

        PyObject *field_obj = compositeproxy_getfield(self, type->tp_getset[i].closure);
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
    Py_ReprLeave((PyObject *) type);
    return repr;
}

static int compositeproxy_traverse(void *data, PyTypeObject *type, visitproc visit, void *arg)
{
    for (int i_field = 0 ; type->tp_getset[i_field].name ; ++i_field)
    {
        struct field_info *field_info = type->tp_getset[i_field].closure;
        if (field_info->type && field_info->type->ft_traverse)
        {
            int vret = field_info->type->ft_traverse(data + field_info->offset,
                    visit, arg, field_info->type);
            if (vret) return vret;
        }
    }
    return 0;
}

static int compositeproxy_traverse_ft(void *data, visitproc visit, void *arg, struct ForeignTypeObject *ftype)
{
    return compositeproxy_traverse(data, ftype->ft_proxy_type, visit, arg);
}

static int compositeproxy_traverse_py(ProxyObject *self, visitproc visit, void *arg)
{
    return compositeproxy_traverse(self->p_ptr, Py_TYPE(self), visit, arg);
}

static int compositeproxy_clear_py(ProxyObject *self)
{
    return compositeproxy_traverse(self->p_ptr, Py_TYPE(self), Proxy_ClearRef, NULL);
}

ForeignTypeObject *CompositeProxy_NewType(const struct uniqtype *type)
{
    int nb_fields = type->un.composite.nmemb;
    const char **field_names = UNIQTYPE_COMPOSITE_SUBOBJ_NAMES(type);
    if (!field_names) nb_fields = 0; // Consider it as an incomplete struct

    CompositeProxyTypeObject *htype =
        PyObject_GC_NewVar(CompositeProxyTypeObject, &CompositeProxy_Metatype, 0);
    PyGetSetDef *getsetdefs = PyMem_Malloc((nb_fields + 1) * sizeof(PyGetSetDef));
    struct field_info *field_infos = PyMem_Malloc(nb_fields * sizeof(struct field_info));

    for (int i_field = 0 ; i_field < nb_fields ; ++i_field)
    {
        // We cannot initialize field_infos types here because it could generate
        // infinite recursion for recursive structs.
        // Instead wait for a future call to CompositeProxy_InitType.

        getsetdefs[i_field] = (PyGetSetDef){
            .name = (char *) field_names[i_field],
            .get = compositeproxy_getinvalidfield,
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
        .tp_base = &Proxy_Type,
        .tp_getset = getsetdefs,
        .tp_init = (initproc) compositeproxy_init,
        .tp_repr = (reprfunc) compositeproxy_repr,
        .tp_traverse = (traverseproc) compositeproxy_traverse_py,
        .tp_clear = (inquiry) compositeproxy_clear_py,
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    };
    htype->fct_fieldinfos = field_infos;

    ForeignTypeObject *ftype = Proxy_NewType(type, (PyTypeObject *) htype);
    ftype->ft_constructor = compositeproxy_ctor;
    ftype->ft_traverse = compositeproxy_traverse_ft;
    return ftype;
}

void CompositeProxy_InitType(ForeignTypeObject *self, const struct uniqtype *type)
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
            proxytype->tp_getset[i_field].get = (getter) compositeproxy_getfield;
            if (ForeignType_IsTriviallyCopiable(finfo->type))
            {
                proxytype->tp_getset[i_field].set = (setter) compositeproxy_setfield;
            }
            if (finfo->offset == 0 && UNIQTYPE_IS_COMPOSITE_TYPE(finfo->type->ft_type))
            {
                // First element idiom => we are a subtype of the first member
                // if it is also a composite type.
                // TODO: Manage name clashes with first element
                proxytype->tp_base = finfo->type->ft_proxy_type;
            }
        }
        else PyErr_Clear();
    }

    int typready __attribute__((unused)) = PyType_Ready(proxytype);
    assert(typready == 0);
}
