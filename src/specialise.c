// Optional fast path for lazily crossing a plain PyObject into C by reference.
//
// When built with -DPYCALLOCS_SPECIALISE_CONVERSION (the SPECIALISE_CONVERSION
// CMake option), this module generates, compiles and dlopens a specialised
// instantiation of linkpy's PyObject_to_T<T> template for each foreign composite
// type encountered, and uses it to copy a plain Python object into a backing C
// struct. Compiled straight-line conversion code amortises better than walking
// the uniqtype on every call; the compile cost is paid once per type and cached.
//
// The whole body is guarded so the file is empty (a no-op) in the default build.
// On ANY unsupported type / compile / load failure the entry point reports
// "unsupported" so the caller transparently falls back to the generic,
// uniqtype-driven converter (see AddressProxy_MaterializePointee).

#include "foreign_library.h"

#ifdef PYCALLOCS_SPECIALISE_CONVERSION

#include <dlfcn.h>
#include <dwarf.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PYCALLOCS_TRANSLATE_CXX
#define PYCALLOCS_TRANSLATE_CXX "g++-16"
#endif

// Embed linkpy's pycpputils.hpp (which defines PyObject_to_T) into the extension
// at build time, so each generated translator is self-contained: the client
// needs no copy of the header on disk at runtime. This requires a C23
// #embed-capable C compiler (gcc >= 15 / clang >= 19) for the
// SPECIALISE_CONVERSION build -- consistent with already needing g++-16's
// -std=c++26 -freflection to compile the translators themselves.
#if defined(__has_embed)
#  if __has_embed("pycpputils.hpp")
#    define PYC_HAVE_EMBED 1
#  endif
#endif
#ifndef PYC_HAVE_EMBED
#  error "SPECIALISE_CONVERSION needs a #embed-capable C compiler (C23: gcc>=15 / clang>=19) with pycpputils.hpp on the include path. Build the extension with e.g. -DUSE_ALASKA_CLANG=OFF -DUSE_ALASKA=OFF (system cc) or set CC accordingly."
#endif

// The full text of pycpputils.hpp as a NUL-terminated C string, baked in at
// compile time; written verbatim into every generated translator source.
static const char pycpputils_hpp[] = {
#embed "pycpputils.hpp"
    , '\0'
};

// A generated translator .so carries both conversion directions for one type
// (a single compile serves both): PyObject -> T (to_c) for by-pointer arguments,
// and T -> PyObject (to_py, a types.SimpleNamespace) for by-value returns.
typedef int       (*translate_fn)(PyObject *src, void *dest); // 0 ok / -1 (exc)
typedef PyObject *(*fromvalue_fn)(void *src);                 // new ref / NULL(exc)

struct translators {
    translate_fn to_c;   // PyObject_to_T<T>
    fromvalue_fn to_py;  // T_to_PyObject<T>
};

// ---------------------------------------------------------------------------
// Toolchain helpers
// ---------------------------------------------------------------------------

// "-I<py include> [-I<py platinclude>]" for the running interpreter, queried
// once via sysconfig (we hold the GIL, this runs inside a Python call).
static const char *py_include_flags(void)
{
    static char *cached = NULL;
    if (cached) return cached;
    cached = strdup("");

    PyObject *sc = PyImport_ImportModule("sysconfig");
    if (!sc) { PyErr_Clear(); return cached; }
    PyObject *inc  = PyObject_CallMethod(sc, "get_path", "s", "include");
    PyObject *plat = PyObject_CallMethod(sc, "get_path", "s", "platinclude");
    Py_DECREF(sc);

    if (inc && plat)
    {
        const char *i = PyUnicode_AsUTF8(inc);
        const char *p = PyUnicode_AsUTF8(plat);
        char buf[2 * PATH_MAX + 16];
        if (i && p && strcmp(i, p) != 0) snprintf(buf, sizeof buf, "-I%s -I%s", i, p);
        else if (i)                      snprintf(buf, sizeof buf, "-I%s", i);
        else                             buf[0] = '\0';
        free(cached);
        cached = strdup(buf);
    }
    else PyErr_Clear();
    Py_XDECREF(inc);
    Py_XDECREF(plat);
    return cached;
}

// A process-lifetime scratch dir for generated sources and .so files.
static const char *gen_dir(void)
{
    static char dir[] = "/tmp/pycallocs-spec-XXXXXX";
    static int made = 0;
    if (!made)
    {
        if (!mkdtemp(dir)) return NULL;
        made = 1;
    }
    return dir;
}

// Compile `src` into the shared object `out`, capturing diagnostics in `log`.
// Returns 0 on success. The build command is PYCALLOCS_TRANSLATE_BUILD_CMD (a
// compiler-invocation PREFIX to which "-o <out> <src>" is appended) when set,
// else assembled from the baked toolchain macros plus the interpreter's include
// dirs. LD_AUDIT/LD_DEBUG are dropped for the child and LD_PRELOAD is stripped
// around the call: the liballocs preload crashes cc1plus (mirrors linkpy's
// auditor compile_stubs).
static int compile_translator(const char *src, const char *out, const char *log)
{
    char cmd[8 * PATH_MAX];
    const char *prefix = getenv("PYCALLOCS_TRANSLATE_BUILD_CMD");
    if (prefix)
    {
        snprintf(cmd, sizeof cmd,
                 "env -u LD_AUDIT -u LD_DEBUG %s -o %s %s > %s 2>&1",
                 prefix, out, src, log);
    }
    else
    {
        // pycpputils.hpp is inlined into `src`, so only the interpreter's own
        // headers (Python.h, pulled in by the embedded header) need an -I here.
        snprintf(cmd, sizeof cmd,
                 "env -u LD_AUDIT -u LD_DEBUG %s -std=c++26 -freflection -shared "
                 "-fPIC -o %s %s %s > %s 2>&1",
                 PYCALLOCS_TRANSLATE_CXX, out, src, py_include_flags(), log);
    }

    char *saved = getenv("LD_PRELOAD");
    char *preload = saved ? strdup(saved) : NULL;
    if (preload) unsetenv("LD_PRELOAD");
    int rc = system(cmd);
    if (preload) { setenv("LD_PRELOAD", preload, 1); free(preload); }

    return (rc == 0) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// C++ source synthesis from the uniqtype
// ---------------------------------------------------------------------------

// The C++ name to use for a base-type member, restricted to exactly the set
// PyObject_to_T specialises for; anything else => unsupported (NULL), so the
// whole conversion falls back to the generic converter.
static const char *cpp_base_type_name(const struct uniqtype *t)
{
    unsigned size = UNIQTYPE_SIZE_IN_BYTES(t);
    switch (t->un.base.enc)
    {
        case DW_ATE_boolean: return "bool";
        case DW_ATE_signed:  return (size == sizeof(int))    ? "int"    : NULL;
        case DW_ATE_float:
            if (size == sizeof(float))  return "float";
            if (size == sizeof(double)) return "double";
            return NULL;
        default: return NULL;
    }
}

struct gen_ctx {
    FILE *out;          // destination source file
    PyObject *names;    // uniqtype* (PyLong) -> synthesized struct name (PyUnicode)
    int counter;
};

// Emit (if needed) the definition of `t` and return a C++ type name usable in a
// declaration, or NULL if `t` is not expressible for PyObject_to_T. Nested
// composites are emitted first (dependencies precede users).
static const char *emit_type(struct gen_ctx *ctx, const struct uniqtype *t)
{
    if (!t) return NULL;
    switch (UNIQTYPE_KIND(t))
    {
        case BASE:      return cpp_base_type_name(t);
        case COMPOSITE: break;
        default:        return NULL; // pointers/arrays/unions: fall back
    }

    PyObject *key = PyLong_FromVoidPtr((void *) t);
    PyObject *have = PyDict_GetItem(ctx->names, key); // borrowed
    if (have) { Py_DECREF(key); return PyUnicode_AsUTF8(have); }

    int nmemb = t->un.composite.nmemb;
    const char **fnames = UNIQTYPE_COMPOSITE_SUBOBJ_NAMES(t);
    if (!fnames) { Py_DECREF(key); return NULL; } // incomplete type

    // We synthesise the composite as a C++ struct (one field per member), which
    // PyObject_to_T / T_to_PyObject then walk by field. That is only valid for
    // non-overlapping members. Unions (and other overlapping-member composites)
    // would get the wrong size/layout -- reads/writes land on the wrong bytes --
    // so bail and let the generic uniqtype converter (a real proxy) handle them.
    for (int a = 0; a < nmemb; ++a)
    {
        unsigned long off_a = t->related[a].un.memb.off;
        unsigned long end_a = off_a + UNIQTYPE_SIZE_IN_BYTES(t->related[a].un.memb.ptr);
        for (int b = a + 1; b < nmemb; ++b)
        {
            unsigned long off_b = t->related[b].un.memb.off;
            unsigned long end_b = off_b + UNIQTYPE_SIZE_IN_BYTES(t->related[b].un.memb.ptr);
            if (off_a < end_b && off_b < end_a) { Py_DECREF(key); return NULL; }
        }
    }

    // Resolve every member first so their definitions are emitted ahead of ours.
    const char *mtypes[nmemb >= 1 ? nmemb : 1];
    for (int i = 0; i < nmemb; ++i)
    {
        mtypes[i] = emit_type(ctx, t->related[i].un.memb.ptr);
        if (!mtypes[i]) { Py_DECREF(key); return NULL; }
    }

    char name[32];
    snprintf(name, sizeof name, "__pyc_T%d", ctx->counter++);
    PyObject *nameobj = PyUnicode_FromString(name);
    PyDict_SetItem(ctx->names, key, nameobj); // dict keeps it alive for the run
    Py_DECREF(key);

    fprintf(ctx->out, "struct %s {\n", name);
    for (int i = 0; i < nmemb; ++i)
        fprintf(ctx->out, "    %s %s;\n", mtypes[i], fnames[i]);
    fprintf(ctx->out, "};\n");

    const char *ret = PyUnicode_AsUTF8(nameobj);
    Py_DECREF(nameobj); // still referenced by ctx->names
    return ret;
}

// ---------------------------------------------------------------------------
// Build + cache
// ---------------------------------------------------------------------------

// Generate, compile and load both translators for composite `t`. On success
// fills *out and returns 0; returns -1 on unsupported type / build failure
// (never sets a Python exception -- failures degrade to the generic path).
static int build_translators(const struct uniqtype *t, struct translators *out)
{
    const char *dir = gen_dir();
    if (!dir) return -1;

    static int id = 0;
    int n = id++;
    char srcpath[PATH_MAX], sopath[PATH_MAX], logpath[PATH_MAX];
    snprintf(srcpath, sizeof srcpath, "%s/t%d.cpp", dir, n);
    snprintf(sopath,  sizeof sopath,  "%s/t%d.so",  dir, n);
    snprintf(logpath, sizeof logpath, "%s/t%d.log", dir, n);

    FILE *sf = fopen(srcpath, "w");
    if (!sf) return -1;

    // Inline the embedded header instead of #include-ing it, so the generated
    // translator builds without pycpputils.hpp being present on the client.
    fputs(pycpputils_hpp, sf);
    fputc('\n', sf);
    struct gen_ctx ctx = { sf, PyDict_New(), 0 };
    const char *top = emit_type(&ctx, t);
    if (top)
    {
        // PyObject_to_T / T_to_PyObject may throw (e.g. a missing attribute); an
        // exception must not cross the extern "C" boundary, so each wrapper
        // converts it into a Python error.
        fprintf(sf,
            "extern \"C\" int __pyc_translate(PyObject *src, void *dest) {\n"
            "    try { *static_cast<%s*>(dest) = PyObject_to_T<%s>(src); return 0; }\n"
            "    catch (const std::exception &e) {\n"
            "        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, e.what());\n"
            "        return -1;\n"
            "    }\n"
            "}\n"
            "extern \"C\" PyObject *__pyc_fromvalue(void *src) {\n"
            "    try { return T_to_PyObject<%s>(*static_cast<%s*>(src)); }\n"
            "    catch (const std::exception &e) {\n"
            "        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, e.what());\n"
            "        return nullptr;\n"
            "    }\n"
            "}\n", top, top, top, top);
    }
    Py_XDECREF(ctx.names);
    fclose(sf);
    if (!top) return -1; // a member type was unsupported

    if (compile_translator(srcpath, sopath, logpath) != 0)
    {
        // Either the compiler reported errors (see the log) or the compiler
        // could not be spawned at all. The latter happens under the liballocs
        // preload, whose syscall trapping makes fork/exec from the running
        // process fail (no log is produced in that case) -- the same limitation
        // that makes linkpy compile its stubs at audit time. Either way we fall
        // back to the generic, uniqtype-driven converter so conversion still
        // succeeds, just without the specialised fast path.
        fprintf(stderr, "[pycallocs] could not build specialised translator for "
                "'%s' (see %s if present); using generic conversion\n",
                UNIQTYPE_NAME(t), logpath);
        return -1;
    }

    void *h = dlopen(sopath, RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        fprintf(stderr, "[pycallocs] dlopen('%s') failed: %s\n", sopath, dlerror());
        return -1;
    }
    out->to_c  = (translate_fn) dlsym(h, "__pyc_translate");
    out->to_py = (fromvalue_fn) dlsym(h, "__pyc_fromvalue");
    if (!out->to_c || !out->to_py)
    {
        fprintf(stderr, "[pycallocs] dlsym failed: %s\n", dlerror());
        dlclose(h);
        return -1;
    }
    return 0;
}

// uniqtype* -> struct translators* on success (cached, process-lifetime), or
// NULL when known-unsupported (cached as Py_None). Built once per type.
static const struct translators *get_or_build_translators(const struct uniqtype *t)
{
    static PyObject *cache = NULL;
    if (!cache) cache = PyDict_New();

    PyObject *key = PyLong_FromVoidPtr((void *) t);
    PyObject *hit = PyDict_GetItem(cache, key); // borrowed
    if (hit)
    {
        Py_DECREF(key);
        return (hit == Py_None) ? NULL : PyLong_AsVoidPtr(hit);
    }

    struct translators *tr = malloc(sizeof *tr);
    PyObject *val;
    if (tr && build_translators(t, tr) == 0) val = PyLong_FromVoidPtr(tr);
    else    { free(tr); val = Py_None; Py_INCREF(val); }
    PyDict_SetItem(cache, key, val);
    Py_DECREF(val);
    Py_DECREF(key);
    return (val == Py_None) ? NULL : tr;
}

// ---------------------------------------------------------------------------
// Public entry points (declared in foreign_library.h)
// ---------------------------------------------------------------------------

int Specialise_Convert(PyObject *obj, ForeignTypeObject *pointee, PyObject **out)
{
    const struct translators *tr = get_or_build_translators(pointee->ft_type);
    if (!tr) return 1; // unsupported -> caller falls back to the generic path

    unsigned size = UNIQTYPE_SIZE_IN_BYTES(pointee->ft_type);
    char buf[size];
    memset(buf, 0, size);
    if (tr->to_c(obj, buf) < 0) return -1; // translator set a Python exception

    // Wrap the filled buffer as a registered proxy (malloc + set_alloc_type +
    // memcpy + Proxy_Register). Supported types are scalar-only, so the plain
    // memcpy in Proxy_CopyFrom is a correct deep copy.
    *out = Proxy_CopyFrom(buf, pointee);
    return *out ? 0 : -1;
}

int Specialise_FromValue(void *src, ForeignTypeObject *type, PyObject **out)
{
    const struct translators *tr = get_or_build_translators(type->ft_type);
    if (!tr) return 1; // unsupported -> caller falls back to ft_copyfrom

    // Return the by-value struct as a plain Python object (types.SimpleNamespace
    // mirroring its fields), decoupled from C memory -- the inverse of the
    // by-pointer argument path. Round-trips: T -> SimpleNamespace -> PyObject_to_T<T>.
    *out = tr->to_py(src);
    return *out ? 0 : -1; // NULL => translator set a Python exception
}

#else
typedef int pycallocs_specialise_disabled_translation_unit;
#endif
