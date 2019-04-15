#include "foreign_library.h"

typedef struct {
    PyObject_HEAD
    void *fho_ptr;
    PyObject *fho_allocator;
} ForeignHandlerObject;

static PyObject *foreignhandler_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    ForeignHandlerObject *obj = (ForeignHandlerObject *) type->tp_alloc(type, 0);
    if (obj)
    {
        obj->fho_ptr = PyMem_Malloc(type->tp_itemsize);
        obj->fho_allocator = (PyObject *) obj;
    }
    return (PyObject *) obj;
}

static void foreignhandler_dealloc(ForeignHandlerObject *self)
{
    if (self->fho_allocator == (PyObject *) self) PyMem_Free(self->fho_ptr);
    else Py_DECREF(self->fho_allocator);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

typedef struct {
    PyTypeObject t;
    PyObject *dlloader;
    PyGetSetDef memb[];
} ForeignCompositeTypeObject;

static PyObject *foreigntype_disabled_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyErr_SetString(PyExc_TypeError, "Cannot create foreign type from Python");
    return NULL;
}

static void foreigntype_dealloc(ForeignCompositeTypeObject *self)
{
    Py_DECREF(self->dlloader);
    PyType_Type.tp_dealloc((PyObject *) self);
}

PyTypeObject ForeignCompositeType_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignCompositeType",
    .tp_doc = "Metatype for foreign composite types",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(ForeignCompositeTypeObject),
    .tp_itemsize = sizeof(PyGetSetDef),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = foreigntype_disabled_new,
    .tp_dealloc = (destructor) foreigntype_dealloc,
};

static PyObject *foreigncomposite_getfield(ForeignHandlerObject *self, struct uniqtype_rel_info *field_info)
{
    void *field = self->fho_ptr + field_info->un.memb.off;
    PyObject *dlloader = ((ForeignCompositeTypeObject *) Py_TYPE(self))->dlloader;
    return pyobject_from_type(field, field_info->un.memb.ptr, dlloader, self->fho_allocator);
}

static int foreigncomposite_setfield(ForeignHandlerObject *self, PyObject *value, struct uniqtype_rel_info *field_info)
{
    void *field = self->fho_ptr + field_info->un.memb.off;
    PyObject *dlloader = ((ForeignCompositeTypeObject *) Py_TYPE(self))->dlloader;
    return store_pyobject_as_type(value, field, field_info->un.memb.ptr, dlloader);
}

static int foreigncomposite_init(ForeignHandlerObject *self, PyObject *args, PyObject *kwds)
{
    // Maybe we could have a convenient initializer

    // Python programmer do not expect uninitialized values
    // => zero-initialize the data
    memset(self->fho_ptr, 0, Py_TYPE(self)->tp_itemsize);

    return 0;
}

static PyObject *foreigncomposite_repr(ForeignHandlerObject *self)
{
    ForeignCompositeTypeObject *type = (ForeignCompositeTypeObject *) Py_TYPE(self);
    PyObject *field_repr_list = PyList_New(0);
    for (int i = 0 ; type->memb[i].name ; ++i)
    {
        PyObject *field_obj = foreigncomposite_getfield(self, type->memb[i].closure);
        PyObject *field_val_repr = PyObject_Repr(field_obj);
        Py_DECREF(field_obj);
        if (!field_val_repr)
        {
            Py_DECREF(field_repr_list);
            return NULL;
        }
        PyObject *field_repr = PyUnicode_FromFormat("%s: %U", type->memb[i].name, field_val_repr);
        Py_DECREF(field_val_repr);
        PyList_Append(field_repr_list, field_repr);
        Py_DECREF(field_repr);
    }
    PyObject *sep = PyUnicode_FromString(", ");
    PyObject *fields_str = PyUnicode_Join(sep, field_repr_list);
    Py_DECREF(sep);
    Py_DECREF(field_repr_list);
    PyObject *repr = PyUnicode_FromFormat("(%s){%U}", type->t.tp_name, fields_str);
    Py_DECREF(fields_str);
    return repr;
}

PyObject *ForeignHandler_NewCompositeType(const struct uniqtype *type, PyObject *dlloader)
{
    if (!UNIQTYPE_IS_COMPOSITE_TYPE(type)) return NULL;

    int nb_fields = type->un.composite.nmemb;
    const char **field_names = __liballocs_uniqtype_subobject_names(type);
    if (!field_names) return NULL;

    ForeignCompositeTypeObject *ftype = PyObject_GC_NewVar(ForeignCompositeTypeObject, &ForeignCompositeType_Type, nb_fields+1);

    for (int i_field = 0 ; i_field < nb_fields ; ++i_field)
    {
        ftype->memb[i_field] = (PyGetSetDef){
            .name = (char *) field_names[i_field],
            .get = (getter) foreigncomposite_getfield,
            .set = (setter) foreigncomposite_setfield,
            .doc = NULL,
            .closure = (void *) &type->related[i_field]
        };
    }
    ftype->memb[nb_fields] = (PyGetSetDef){NULL};

    ftype->t = (PyTypeObject){
        .ob_base = ftype->t.ob_base, // <- Keep the object base
        .tp_name = UNIQTYPE_NAME(type),
        .tp_basicsize = sizeof(ForeignHandlerObject),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(type),
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
        .tp_new = foreignhandler_new,
        .tp_dealloc = (destructor) foreignhandler_dealloc,
        .tp_getset = ftype->memb,
        .tp_init = (initproc) foreigncomposite_init,
        .tp_repr = (reprfunc) foreigncomposite_repr,
    };

    if (PyType_Ready((PyTypeObject *) ftype) < 0)
    {
        Py_DECREF(ftype);
        return NULL;
    }

    Py_INCREF(dlloader);
    ftype->dlloader = dlloader;

    return (PyObject *) ftype;
}

// Borrow everything from the caller, type must be compatible with
// ForeignHandlerObject layout
PyObject *ForeignHandler_FromData(void *data, PyTypeObject *type)
{
    ForeignHandlerObject *obj = PyObject_New(ForeignHandlerObject, type);
    if (obj)
    {
        obj->fho_ptr = PyMem_Malloc(type->tp_itemsize);
        memcpy(obj->fho_ptr, data, type->tp_itemsize);
        obj->fho_allocator = (PyObject *) obj;
    }
    return (PyObject *) obj;
}

PyObject *ForeignHandler_FromPtr(void *ptr, PyTypeObject *type, PyObject *allocator)
{
    ForeignHandlerObject *obj = PyObject_New(ForeignHandlerObject, type);
    if (obj)
    {
        obj->fho_ptr = ptr;
        Py_INCREF(allocator);
        obj->fho_allocator = allocator;
    }
    return (PyObject *) obj;
}

void *ForeignHandler_GetDataAddr(PyObject *self)
{
    ForeignHandlerObject *obj = (ForeignHandlerObject *) self;
    return obj->fho_ptr;
}
