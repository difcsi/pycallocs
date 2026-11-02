// DRAFT alternative to src/specialise.c using P3294 token-sequence injection.
//
// Same public contract as specialise.c -- it defines Specialise_Convert /
// Specialise_FromValue and is selected, instead of specialise.c, when the
// extension is built with -DPYCALLOCS_INJECT_CONVERSION (the INJECT_CONVERSION
// CMake option). The two are mutually exclusive: each guards its whole body on
// its own macro, so whichever is defined supplies the entry points and the
// other compiles to an empty translation unit.
//
// The ONLY difference from specialise.c is what gets written into the generated
// .cpp. specialise.c prints the full C++ (struct defs + extern "C" wrappers)
// from C. Here we print a compact *descriptor* of the type plus one consteval
// block; the structs and wrappers are synthesised at the generated TU's own
// compile time by pyc::inject_translators() (include/pyc_inject.hpp), via P3294
// token-sequence injection. The g++-16 invocation and dlopen are unchanged --
// the layout is only known at runtime -- but the brittle, manually-formatted
// codegen moves out of C and into hygienic token sequences.
//
// There is no P3294 compiler yet; this file is an unverified design sketch.

#include "foreign_library.h"

#ifdef PYCALLOCS_INJECT_CONVERSION

#include <dlfcn.h>
#include <dwarf.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PYCALLOCS_TRANSLATE_CXX
#define PYCALLOCS_TRANSLATE_CXX "g++-16"
#endif

// Both pycpputils.hpp (PyObject_to_T / T_to_PyObject) and pyc_inject.hpp (the
// token-sequence injector) are baked into the extension at build time and
// written verbatim into every generated translator, so the client needs neither
// on disk at runtime. Mirrors specialise.c's single #embed of pycpputils.hpp.
#if defined(__has_embed)
#  if __has_embed("pycpputils.hpp") && __has_embed("pyc_inject.hpp")
#    define PYC_HAVE_EMBED 1
#  endif
#endif
#ifndef PYC_HAVE_EMBED
#  error "INJECT_CONVERSION needs a #embed-capable C compiler (C23: gcc>=15 / clang>=19) with pycpputils.hpp and pyc_inject.hpp on the embed path (--embed-dir). Build the extension with e.g. -DUSE_ALASKA_CLANG=OFF -DUSE_ALASKA=OFF (system cc) or set CC accordingly."
#endif

static const char pycpputils_hpp[] = {
#embed "pycpputils.hpp"
    , '\0'
};
static const char pyc_inject_hpp[] = {
#embed "pyc_inject.hpp"
    , '\0'
};

typedef int       (*translate_fn)(PyObject *src, void *dest); // 0 ok / -1 (exc)
typedef PyObject *(*fromvalue_fn)(void *src);                 // new ref / NULL(exc)

struct translators {
    translate_fn to_c;   // PyObject_to_T<T>
    fromvalue_fn to_py;  // T_to_PyObject<T>
};

// ---------------------------------------------------------------------------
// Toolchain helpers (identical in spirit to specialise.c)
// ---------------------------------------------------------------------------

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

static const char *gen_dir(void)
{
    static char dir[] = "/tmp/pycallocs-inject-XXXXXX";
    static int made = 0;
    if (!made)
    {
        if (!mkdtemp(dir)) return NULL;
        made = 1;
    }
    return dir;
}

// Identical to specialise.c's compiler driver: needs -std=c++26 -freflection
// (for both the embedded PyObject_to_T template and the consteval injection),
// and drops LD_AUDIT/LD_DEBUG/LD_PRELOAD around the call so the liballocs
// preload does not crash cc1plus.
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
// Descriptor synthesis from the uniqtype
// ---------------------------------------------------------------------------
// Where specialise.c::emit_type prints `struct ... { ... };`, this prints the
// descriptor records pyc::inject_translators() consumes (see pyc_inject.hpp).
// The supported-type checks (scalar base types only; non-overlapping composite
// members; complete types) are identical: any miss returns NULL and the whole
// conversion falls back to the generic uniqtype-driven converter.

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
    FILE *out;          // descriptor destination
    PyObject *names;    // uniqtype* (PyLong) -> synthesized name (PyUnicode)
    int counter;
};

// Emit (if needed) the descriptor records for `t` and return its descriptor
// type name, or NULL if `t` is not expressible for PyObject_to_T. Nested
// composites are emitted first so their `C` records precede any `M` line that
// names them -- pyc::inject_translators injects in that same order, satisfying
// C++'s define-before-use.
static const char *emit_descriptor(struct gen_ctx *ctx, const struct uniqtype *t)
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

    // Reject overlapping members (e.g. unions): a flat C++ struct would put the
    // fields at the wrong offsets. Same guard as specialise.c.
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

    // Resolve members first so their descriptor records precede ours.
    const char *mtypes[nmemb >= 1 ? nmemb : 1];
    for (int i = 0; i < nmemb; ++i)
    {
        mtypes[i] = emit_descriptor(ctx, t->related[i].un.memb.ptr);
        if (!mtypes[i]) { Py_DECREF(key); return NULL; }
    }

    char name[32];
    snprintf(name, sizeof name, "__pyc_T%d", ctx->counter++);
    PyObject *nameobj = PyUnicode_FromString(name);
    PyDict_SetItem(ctx->names, key, nameobj);
    Py_DECREF(key);

    // C <name> <nfields> / M <type> <field> ...   (consumed by pyc::parse)
    fprintf(ctx->out, "C %s %d\n", name, nmemb);
    for (int i = 0; i < nmemb; ++i)
        fprintf(ctx->out, "M %s %s\n", mtypes[i], fnames[i]);

    const char *ret = PyUnicode_AsUTF8(nameobj);
    Py_DECREF(nameobj);
    return ret;
}

// ---------------------------------------------------------------------------
// Build + cache (driver identical to specialise.c, only the generated TU body
// differs: embed + descriptor + one consteval injection block)
// ---------------------------------------------------------------------------

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

    // Inline the embedded headers so the translator is self-contained.
    fputs(pycpputils_hpp, sf);
    fputc('\n', sf);
    fputs(pyc_inject_hpp, sf);
    fputc('\n', sf);

    // Emit the descriptor as a raw string literal, then the consteval block that
    // turns it into structs + wrappers via token-sequence injection.
    fputs("constexpr char __pyc_desc[] = R\"PYCDESC(\n", sf);
    struct gen_ctx ctx = { sf, PyDict_New(), 0 };
    const char *top = emit_descriptor(&ctx, t);
    if (top) fprintf(sf, "T %s\n", top);
    Py_XDECREF(ctx.names);
    fputs(")PYCDESC\";\n", sf);
    fputs("consteval { pyc::inject_translators(__pyc_desc); }\n", sf);
    fclose(sf);
    if (!top) return -1; // a member type was unsupported

    if (compile_translator(srcpath, sopath, logpath) != 0)
    {
        fprintf(stderr, "[pycallocs] could not build injected translator for "
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

static const struct translators *get_or_build_translators(const struct uniqtype *t)
{
    static PyObject *cache = NULL;
    if (!cache) cache = PyDict_New();

    PyObject *key = PyLong_FromVoidPtr((void *) t);
    PyObject *hit = PyDict_GetItem(cache, key);
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
// Public entry points (declared in foreign_library.h) -- identical to
// specialise.c; only the translator construction above differs.
// ---------------------------------------------------------------------------

int Specialise_Convert(PyObject *obj, ForeignTypeObject *pointee, PyObject **out)
{
    const struct translators *tr = get_or_build_translators(pointee->ft_type);
    if (!tr) return 1; // unsupported -> caller falls back to the generic path

    unsigned size = UNIQTYPE_SIZE_IN_BYTES(pointee->ft_type);
    char buf[size];
    memset(buf, 0, size);
    if (tr->to_c(obj, buf) < 0) return -1;

    *out = Proxy_CopyFrom(buf, pointee);
    return *out ? 0 : -1;
}

int Specialise_FromValue(void *src, ForeignTypeObject *type, PyObject **out)
{
    const struct translators *tr = get_or_build_translators(type->ft_type);
    if (!tr) return 1; // unsupported -> caller falls back to ft_copyfrom

    *out = tr->to_py(src);
    return *out ? 0 : -1;
}

#else
typedef int pycallocs_inject_disabled_translation_unit;
#endif
