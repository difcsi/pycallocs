#pragma once
#include <Python.h>
#include <meta>
#include <type_traits>
#include <string>
#include <stdexcept>
#include <cstring>
#include <iostream>

// Everything lives in linkpy::py so a cross-language linkstub TU can include
// this header together with luacpputils.hpp (linkpy::lua) without the
// run_function templates colliding.
namespace linkpy::py {

// Starts the interpreter on first use (defined by the VM template; the
// linkstub does not start it at load time).
void ensure_vm();

PyObject *import_module(const char *name);
PyObject *import_function(PyObject *module, const char *name);
PyObject *call_function(PyObject *pFunc, PyObject *args);
void init_py();

// One record per generated trampoline, defined `static` inside it with the
// module and function names it was generated with. The marshaller adds the
// interned Python strings for them on the first call (under the GIL, which
// every marshalling caller holds), so later calls resolve the callable with
// two dictionary lookups instead of a full import.
struct CallSite
{
    const char *module;
    const char *function;
    PyObject *module_name;    // interned, nullptr until the first call
    PyObject *function_name;  // interned, nullptr until the first call
};

// The callable of a call site as a new reference, with a new reference to
// its module in *module_out. It is the live sys.modules entry (imported on
// the first call, or again if something dropped it) and the live attribute,
// so a function rebound by importlib.reload() or a module object replaced in
// sys.modules is what the next call gets. Prints the Python error and throws
// std::runtime_error when the module or the function is missing.
PyObject *resolve_callable(CallSite &site, PyObject **module_out);

// The interned Python string for a struct member's name, one per member for
// the life of the process, made on first use (under the GIL, which every
// marshalling caller holds). An attribute get or set with an interned name
// skips building and hashing a string per field per call.
template <std::meta::info M>
PyObject *field_name()
{
    static PyObject *name = PyUnicode_InternFromString(
        std::define_static_string(std::meta::identifier_of(M)));
    return name;
}

// Wrap a raw C pointer in a pycallocs proxy via allocs.proxy_from_address.
// Returns a NEW reference, or NULL with a Python exception set.
PyObject *proxy_from_c_ptr(const void *ptr);

// The inverse, for by-reference arguments ARRIVING from a Python host: extract
// the raw C address from a pycallocs proxy (allocs.address_of, falling back to
// debug_handle_info for allocs builds predating it). Throws std::runtime_error
// when obj is not a proxy or allocs is unavailable.
void *ptr_from_proxy(PyObject *obj);

// Fixed-size char arrays inside a by-value struct cross as Python bytes. A C
// array cannot be returned or assigned, so the aggregate loop in PyObject_to_T
// fills the member in place: a bytes/bytearray no longer than the array is
// copied in and the remainder is zeroed, which leaves a char[N] NUL-terminated
// for the C side. Only the tail is written to zero: the whole array was not
// cleared beforehand (that cost a 64 KiB memset per call for a full chunk).
template <typename T>
void PyObject_into_array(PyObject *obj, T &dest, const char *field)
{
    using Elem = std::remove_cv_t<std::remove_extent_t<T>>;
    static_assert(std::is_same_v<Elem, char> || std::is_same_v<Elem, unsigned char>
                      || std::is_same_v<Elem, signed char>,
                  "only char arrays cross as bytes");
    const char *buf;
    Py_ssize_t n;
    if (PyBytes_Check(obj))
    {
        buf = PyBytes_AS_STRING(obj);
        n = PyBytes_GET_SIZE(obj);
    }
    else if (PyByteArray_Check(obj))
    {
        buf = PyByteArray_AS_STRING(obj);
        n = PyByteArray_GET_SIZE(obj);
    }
    else
    {
        throw std::runtime_error(std::string("field '") + field + "' must be bytes or bytearray");
    }
    if (n > static_cast<Py_ssize_t>(std::extent_v<T>))
    {
        throw std::runtime_error(std::string("field '") + field + "': " + std::to_string(n)
                                 + " bytes do not fit a char[" + std::to_string(std::extent_v<T>) + "]");
    }
    std::memcpy(dest, buf, static_cast<size_t>(n));
    std::memset(reinterpret_cast<char *>(dest) + n, 0, static_cast<size_t>(std::extent_v<T>) - static_cast<size_t>(n));
}

// Convert a Python object into a C/C++ value of type T. Defined here (rather
// than in a .cpp) so that every translation unit that instantiates it — in
// particular the runtime-generated linkstub.cpp — can emit the needed code.
// Aggregate/struct types are marshalled field-by-field using C++26 static
// reflection (P2996): each non-static data member is read from the Python
// object's same-named attribute, recursing through PyObject_to_T. This
// replaces the per-struct specializations that would otherwise be needed.
template <typename T>
T PyObject_to_T(PyObject *pValue)
{
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    {
        if (PyLong_Check(pValue))
        {
            if constexpr (std::is_unsigned_v<T>)
                return static_cast<T>(PyLong_AsUnsignedLongLong(pValue));
            else
                return static_cast<T>(PyLong_AsLongLong(pValue));
        }
        else if (PyFloat_Check(pValue))
        {
            return static_cast<T>(PyFloat_AsDouble(pValue));
        }
        else
        {
            throw std::runtime_error("Invalid return type");
        }
    }
    else if constexpr (std::is_same<T, float>::value)
    {
        if (PyLong_Check(pValue))
        {
            return static_cast<float>(PyLong_AsLong(pValue));
        }
        else if (PyFloat_Check(pValue))
        {
            return static_cast<float>(PyFloat_AsDouble(pValue));
        }
        else
        {
            throw std::runtime_error("Invalid return type");
        }
    }
    else if constexpr (std::is_same<T, double>::value)
    {
        if (PyLong_Check(pValue))
        {
            return static_cast<double>(PyLong_AsLong(pValue));
        }
        else if (PyFloat_Check(pValue))
        {
            return PyFloat_AsDouble(pValue);
        }
        else
        {
            throw std::runtime_error("Invalid return type");
        }
    }
    else if constexpr (std::is_same<T, bool>::value)
    {
        return PyObject_IsTrue(pValue);
    }
    else if constexpr (std::is_same<T, std::string>::value)
    {
        return std::string(PyUnicode_AsUTF8(pValue));
    }
    else if constexpr (std::is_aggregate_v<T>)
    {
        // Not value-initialised: the loop below assigns every member (an array
        // member is filled and its tail zeroed by PyObject_into_array), so a
        // zero-fill here would be a second pass over the whole struct.
        T result;
        constexpr auto ctx = std::meta::access_context::current();
        template for (constexpr auto member : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, ctx)))
        {
            constexpr const char *field =
                std::define_static_string(std::meta::identifier_of(member));
            PyObject *attr = PyObject_GetAttr(pValue, field_name<member>());
            if (attr == NULL)
            {
                PyErr_Print();
                throw std::runtime_error(std::string("Missing attribute '") + field + "'");
            }
            using FieldT = [:std::meta::type_of(member):];
            if constexpr (std::is_array_v<FieldT>)
                PyObject_into_array(attr, result.[:member:], field);
            else
                result.[:member:] = PyObject_to_T<FieldT>(attr);
            Py_DECREF(attr);
        }
        return result;
    }
    else
    {
        throw std::runtime_error("Unsupported return type for PyObject_to_T");
    }
}

// Inverse of PyObject_to_T: convert a C/C++ value into a freshly-owned Python
// object (a new reference the caller must Py_DECREF). Aggregates are turned into
// a types.SimpleNamespace whose attributes mirror the struct's fields, so a
// value round-trips: T -> SimpleNamespace(field=...) -> PyObject_to_T<T>.
template <typename T>
PyObject *T_to_PyObject(const T &value)
{
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    {
        if constexpr (std::is_unsigned_v<T>)
            return PyLong_FromUnsignedLongLong(static_cast<unsigned long long>(value));
        else
            return PyLong_FromLongLong(static_cast<long long>(value));
    }
    else if constexpr (std::is_same<T, float>::value)
    {
        return PyFloat_FromDouble(static_cast<double>(value));
    }
    else if constexpr (std::is_same<T, double>::value)
    {
        return PyFloat_FromDouble(value);
    }
    else if constexpr (std::is_same<T, bool>::value)
    {
        return PyBool_FromLong(value);
    }
    else if constexpr (std::is_same<T, std::string>::value)
    {
        return PyUnicode_FromString(value.c_str());
    }
    else if constexpr (std::is_same_v<T, const char *> || std::is_same_v<T, char *>)
    {
        // A char* FIELD of a by-value struct: the same string mapping a top-level
        // char* argument gets (arg_to_PyObject), NULL included.
        if (value == nullptr) Py_RETURN_NONE;
        PyObject *str = PyUnicode_FromString(value);
        if (str == nullptr)
        {
            PyErr_Print();
            throw std::runtime_error("T_to_PyObject: char* is not valid UTF-8");
        }
        return str;
    }
    else if constexpr (std::is_array_v<T>)
    {
        // A fixed-size char array crosses whole, as a bytes as long as the
        // array; the struct's own length field, if any, says how much of it
        // means anything. Checked before is_aggregate_v: arrays are aggregates.
        using Elem = std::remove_cv_t<std::remove_extent_t<T>>;
        static_assert(std::is_same_v<Elem, char> || std::is_same_v<Elem, unsigned char>
                          || std::is_same_v<Elem, signed char>,
                      "only char arrays cross as bytes");
        return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(value), std::extent_v<T>);
    }
    else if constexpr (std::is_aggregate_v<T>)
    {
        // types.SimpleNamespace, looked up once per struct type and kept for
        // the process (importing `types` on every crossing cost more than the
        // namespace object itself).
        static PyObject *ns_cls = nullptr;
        if (ns_cls == nullptr)
        {
            PyObject *types = PyImport_ImportModule("types");
            if (types == NULL)
            {
                PyErr_Print();
                throw std::runtime_error("T_to_PyObject: cannot import 'types'");
            }
            ns_cls = PyObject_GetAttrString(types, "SimpleNamespace");
            Py_DECREF(types);
            if (ns_cls == NULL)
            {
                PyErr_Print();
                throw std::runtime_error("T_to_PyObject: no types.SimpleNamespace");
            }
        }
        PyObject *obj = PyObject_CallNoArgs(ns_cls);
        if (obj == NULL)
        {
            PyErr_Print();
            throw std::runtime_error("T_to_PyObject: cannot create namespace");
        }

        constexpr auto ctx = std::meta::access_context::current();
        template for (constexpr auto member : std::define_static_array(
                          std::meta::nonstatic_data_members_of(^^T, ctx)))
        {
            constexpr const char *field =
                std::define_static_string(std::meta::identifier_of(member));
            PyObject *attr = T_to_PyObject(value.[:member:]);
            if (PyObject_SetAttr(obj, field_name<member>(), attr) < 0)
            {
                Py_XDECREF(attr);
                Py_DECREF(obj);
                PyErr_Print();
                throw std::runtime_error(std::string("T_to_PyObject: cannot set attribute '") + field + "'");
            }
            Py_DECREF(attr);
        }
        return obj;
    }
    else
    {
        throw std::runtime_error("Unsupported type for T_to_PyObject");
    }
}



// Decide, per top-level argument, how a C value becomes a Python object:
//  - char* / const char*  -> Python str (string semantics, NOT a proxy)
//  - any other pointer T*  -> a pycallocs proxy wrapping the pointee, so the
//    object is passed BY REFERENCE and Python writes propagate back into C
//  - everything else       -> the existing by-value T_to_PyObject path
// Returns a NEW reference (caller / PyTuple_SetItem takes ownership).
template <typename T>
PyObject *arg_to_PyObject(const T &value)
{
    if constexpr (std::is_same_v<T, const char *> || std::is_same_v<T, char *>)
    {
        if (value == nullptr) Py_RETURN_NONE;
        PyObject *str = PyUnicode_FromString(value);
        if (str == nullptr)
        {
            PyErr_Print();
            throw std::runtime_error("arg_to_PyObject: char* is not valid UTF-8");
        }
        return str;
    }
    else if constexpr (std::is_pointer_v<T>)
    {
        if (value == nullptr) Py_RETURN_NONE;
        PyObject *proxy = proxy_from_c_ptr(value);
        if (proxy == nullptr)
            throw std::runtime_error("arg_to_PyObject: cannot proxy pointer "
                                     "argument (no liballocs type / scalar pointee)");
        return proxy;
    }
    else
    {
        return T_to_PyObject(value);
    }
}

// The arguments of one call as the array the vectorcall protocol takes: slot
// 0 is the one PY_VECTORCALL_ARGUMENTS_OFFSET lets the callee use, the
// references in the rest are owned here and dropped on the way out, whether
// the call returned or an argument's conversion threw.
template <std::size_t N>
struct ArgArray
{
    PyObject *slots[N + 1] = {};
    ~ArgArray()
    {
        for (std::size_t i = 1; i <= N; i++)
            Py_XDECREF(slots[i]);
    }
    PyObject **args() { return slots + 1; }
    ArgArray() = default;
    ArgArray(const ArgArray &) = delete;
    ArgArray &operator=(const ArgArray &) = delete;
};

// Marshal each C argument into the argument array. arg_to_PyObject routes
// pointer arguments through a pycallocs proxy and everything else through the
// reflection-aware T_to_PyObject. The arguments travel by reference from the
// trampoline's own by-value parameters (the C ABI's copy) down to the
// conversion: a 64 KiB struct is not copied again on the way.
template <typename... Args>
static void pack_argv(PyObject **out, const Args &... args)
{
    [[maybe_unused]] std::size_t idx = 0;
    ((out[idx++] = arg_to_PyObject(args)), ...);
}

// Holds the GIL for one trampoline call. The linkstub parks the GIL once the
// interpreter is up (template.cpp), so between calls other Python threads run.
struct GilGuard
{
    PyGILState_STATE state;
    GilGuard() : state(PyGILState_Ensure()) {}
    ~GilGuard() { PyGILState_Release(state); }
    GilGuard(const GilGuard &) = delete;
    GilGuard &operator=(const GilGuard &) = delete;
};

template <typename... Args>
void run_function_void(CallSite &site, int argc, const Args &... args)
{
    (void) argc;
    ensure_vm();
    GilGuard gil;
#ifdef DEBUG
    std::cout << "Running function " << site.function << " from module " << site.module << " with " << argc << " arguments" << std::endl;
#endif
    PyObject *pModule = nullptr;
    PyObject *pFunc = resolve_callable(site, &pModule);

    ArgArray<sizeof...(Args)> argv;
    pack_argv(argv.args(), args...);
    PyObject *pValue = PyObject_Vectorcall(pFunc, argv.args(),
                                           sizeof...(Args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
    if (pValue == NULL)
        PyErr_Print();
    Py_XDECREF(pValue);

    Py_DECREF(pFunc);
    Py_DECREF(pModule);
}

template <typename T, typename... Args>
T run_function(CallSite &site, int argc, const Args &... args)
{
    (void) argc;
    ensure_vm();
    GilGuard gil;
#ifdef DEBUG
    std::cout << "Running function " << site.function << " from module " << site.module << " with " << argc << " arguments" << std::endl;
#endif
    PyObject *pModule = nullptr;
    PyObject *pFunc = resolve_callable(site, &pModule);

    ArgArray<sizeof...(Args)> argv;
    pack_argv(argv.args(), args...);
    PyObject *pValue = PyObject_Vectorcall(pFunc, argv.args(),
                                           sizeof...(Args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
    if (pValue == NULL)
    {
        PyErr_Print();
        throw std::runtime_error("Failed to call function");
    }

    T result = PyObject_to_T<T>(pValue);

    Py_DECREF(pValue);
    Py_DECREF(pFunc);
    Py_DECREF(pModule);

    return result;
}

} // namespace linkpy::py
