#include "foreign_library.h"
#include <stdbool.h>

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

static PyObject *foreignhandler_getfrom(void *data, ForeignTypeObject *type, PyObject *allocator)
{
    PyTypeObject *handler_type = (PyTypeObject *) type->ft_constructor;
    ForeignHandlerObject *obj = PyObject_New(ForeignHandlerObject, handler_type);
    if (obj)
    {
        if (allocator)
        {
            obj->fho_ptr = data;
            Py_INCREF(allocator);
            obj->fho_allocator = allocator;
        }
        else
        {
            obj->fho_ptr = PyMem_Malloc(handler_type->tp_itemsize);
            memcpy(obj->fho_ptr, data, handler_type->tp_itemsize);
            obj->fho_allocator = (PyObject *) obj;
        }
    }
    return (PyObject *) obj;
}

static int foreignhandler_storeinto(PyObject *obj, void *dest, ForeignTypeObject *type)
{
    PyTypeObject *handler_type = (PyTypeObject *) type->ft_constructor;
    if (!PyObject_TypeCheck(obj, handler_type))
    {
        PyErr_Format(PyExc_TypeError, "expected value of type %s, got %s",
        handler_type->tp_name, Py_TYPE(obj)->tp_name);
        return -1;
    }
    ForeignHandlerObject *fho = (ForeignHandlerObject *) obj;
    memcpy(dest, fho->fho_ptr, handler_type->tp_itemsize);
    return 0;
}

typedef struct {
    PyTypeObject tp_base;
    PyGetSetDef memb[];
} ForeignCompositeHandlerTypeObject;

// TODO: Check that subclassing a foreign composite handler works
PyTypeObject ForeignComposite_HandlerMetatype = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "allocs.ForeignCompositeHandlerMetatype",
    .tp_doc = "Metatype for foreign composite handlers",
    .tp_base = &PyType_Type,
    .tp_basicsize = sizeof(ForeignCompositeHandlerTypeObject),
    .tp_itemsize = sizeof(PyGetSetDef),
};

static PyObject *foreigncomposite_getfield(ForeignHandlerObject *self, struct uniqtype_rel_info *field_info)
{
    void *field = self->fho_ptr + field_info->un.memb.off;
    // TODO: Avoid this dict lookup v
    ForeignTypeObject *ftype = ForeignType_GetOrCreate(field_info->un.memb.ptr);
    PyObject *ret = ftype->ft_getfrom(field, ftype, self->fho_allocator);
    Py_DECREF(ftype);
    return ret;
}

static int foreigncomposite_setfield(ForeignHandlerObject *self, PyObject *value, struct uniqtype_rel_info *field_info)
{
    void *field = self->fho_ptr + field_info->un.memb.off;
    // TODO: Avoid this dict lookup v
    ForeignTypeObject *ftype = ForeignType_GetOrCreate(field_info->un.memb.ptr);
    int ret = ftype->ft_storeinto(value, field, ftype);
    Py_DECREF(ftype);
    return ret;
}

static int foreigncomposite_init(ForeignHandlerObject *self, PyObject *args, PyObject *kwargs)
{
    PyTypeObject *type = Py_TYPE(self);

    unsigned nargs = PyTuple_GET_SIZE(args);
    unsigned nkwargs = kwargs ? PyDict_Size(kwargs) : 0;

    // Default initialization => zero initialize the whole structure
    if (nargs == 0 && nkwargs == 0)
    {
        memset(self->fho_ptr, 0, type->tp_itemsize);
        return 0;
    }

    // Copy if 1 positional same type struct argument and no keyword argument
    if (nargs == 1 && nkwargs == 0)
    {
        PyObject *arg = PyTuple_GET_ITEM(args, 0);
        if (PyObject_TypeCheck(arg, type))
        {
            ForeignHandlerObject *other = (ForeignHandlerObject *) arg;

            // Checking pointer equality is enough for checking aliasing
            if (self->fho_ptr == other->fho_ptr)
            {
                PyErr_SetString(PyExc_ValueError, "Trying to copy inside yourself");
                return -1;
            }

            memcpy(self->fho_ptr, other->fho_ptr, type->tp_itemsize);
            return 0;
        }
    }

    // Else we initialize fields in order or by taking a keyword argument
    unsigned initialized_fields = 0;
    for (unsigned i = 0 ; type->tp_getset[i].name ; ++i)
    {
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
                struct uniqtype_rel_info *field_info = type->tp_getset[i].closure;
                void *field = self->fho_ptr + field_info->un.memb.off;
                memset(field, 0, UNIQTYPE_SIZE_IN_BYTES(field_info->un.memb.ptr));
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

static PyObject *foreigncomposite_repr(ForeignHandlerObject *self)
{
    PyTypeObject *type = Py_TYPE(self);
    PyObject *field_repr_list = PyList_New(0);
    for (int i = 0 ; type->tp_getset[i].name ; ++i)
    {
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
    if (!field_names) return NULL;

    ForeignCompositeHandlerTypeObject *htype =
        PyObject_GC_NewVar(ForeignCompositeHandlerTypeObject, &ForeignComposite_HandlerMetatype, nb_fields+1);

    for (int i_field = 0 ; i_field < nb_fields ; ++i_field)
    {
        const struct uniqtype_rel_info *field_info = &type->related[i_field];
        bool nested_field = field_info->un.memb.ptr->un.info.kind == COMPOSITE;
        htype->memb[i_field] = (PyGetSetDef){
            .name = (char *) field_names[i_field],
            .get = (getter) foreigncomposite_getfield,
            .set = nested_field ? NULL : (setter) foreigncomposite_setfield,
            .doc = NULL,
            .closure = (void *) field_info,
        };
    }
    htype->memb[nb_fields] = (PyGetSetDef){NULL};

    htype->tp_base = (PyTypeObject){
        .ob_base = htype->tp_base.ob_base, // <- Keep the object base
        .tp_name = UNIQTYPE_NAME(type),
        .tp_basicsize = sizeof(ForeignHandlerObject),
        .tp_itemsize = UNIQTYPE_SIZE_IN_BYTES(type),
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
        .tp_new = foreignhandler_new,
        .tp_dealloc = (destructor) foreignhandler_dealloc,
        .tp_getset = htype->memb,
        .tp_init = (initproc) foreigncomposite_init,
        .tp_repr = (reprfunc) foreigncomposite_repr,
    };

    if (PyType_Ready((PyTypeObject *) htype) < 0)
    {
        Py_DECREF(htype);
        return NULL;
    }

    ForeignTypeObject *ftype = PyObject_New(ForeignTypeObject, &ForeignType_Type);
    ftype->ft_type = type;
    ftype->ft_constructor = (PyObject *) htype;
    ftype->ft_getfrom = foreignhandler_getfrom;
    ftype->ft_storeinto = foreignhandler_storeinto;
    return ftype;
}

