// DRAFT (P3294 "Code Injection with Token Sequences") -- alternative to the
// string-templated translator synthesis in src/specialise.c.
//
// =========================================================================
//  Why this exists
// =========================================================================
// The default SPECIALISE_CONVERSION path (src/specialise.c) synthesises a
// per-type translator by *printing C++ source text*: it walks the runtime
// uniqtype and fprintf()s a `struct __pyc_T0 { ... };` for each composite plus
// two `extern "C"` wrappers around PyObject_to_T<T> / T_to_PyObject<T>. That
// string is then handed to g++-16 and dlopen()ed.
//
// This header is the heart of the INJECT_CONVERSION *alternative*. Instead of
// the C driver emitting fragile, manually-formatted C++ text, it emits only a
// compile-time *descriptor* of the type (one identifier + field list per
// composite) and a single `consteval` block. At compile time of the generated
// TU, inject_translators() parses that descriptor and *injects* the structs and
// wrappers as token sequences (P3294), splicing in the type names hygienically.
//
// The runtime g++ invocation + dlopen still happen (the type layout is only
// known at runtime, from the uniqtype), but the *shape* of the generated code
// now lives here as token sequences rather than as printf format strings spread
// through the C driver. The driver's job collapses to "serialise the uniqtype".
//
// =========================================================================
//  P3294 primitives used (syntax per P3294R2 / P3289 consteval blocks)
// =========================================================================
//   ^^{ tokens }                a token-sequence literal -> std::meta::info
//   \tokens(e)                  splice another token sequence e into ^^{...}
//   \id(sv)                     form an identifier token from a string constant
//   \(e)                        interpolate a scalar/string constant as a token
//   consteval { ... }           a consteval block: an injection context
//   std::meta::queue_injection  queue a token sequence for injection into the
//                               enclosing context (here: namespace scope)
//
// NOTE: there is no P3294 compiler yet; this is an unverified design sketch.
// Token-sequence/interpolation spellings are expected to shift before the
// proposal lands -- treat the exact syntax as illustrative.

#pragma once
#include <Python.h>
#include <meta>
#include <string_view>
#include <vector>
#include <utility>
#include <stdexcept>

// pycpputils.hpp (which defines PyObject_to_T<T> / T_to_PyObject<T>) is expected
// to have been included/embedded ahead of this header in the generated TU.

namespace pyc {

// -------------------------------------------------------------------------
// Descriptor model
// -------------------------------------------------------------------------
// The C driver (src/specialise_inject.c) emits a line-oriented descriptor of
// the composite, dependencies first. Grammar (one record per line):
//
//     C <name> <nfields>      begin composite <name> with <nfields> members
//     M <type> <field>        a member: single-identifier type + field name
//     T <name>                the top-level type to translate
//
// Every <type> is one identifier -- a scalar keyword (int/float/double/bool) or
// a previously-defined composite's synthesised name (__pyc_TN). The driver only
// emits types PyObject_to_T<T> actually specialises for; anything else makes it
// fall back to the generic converter and this path is never reached.

struct Member { std::string_view type; std::string_view name; };
struct Composite { std::string_view name; std::vector<Member> members; };

// Split `desc` into whitespace-separated words, in order. constexpr-friendly.
consteval std::vector<std::string_view> words_of(std::string_view desc)
{
    std::vector<std::string_view> words;
    std::size_t i = 0, n = desc.size();
    while (i < n)
    {
        while (i < n && (desc[i] == ' ' || desc[i] == '\n' ||
                         desc[i] == '\t' || desc[i] == '\r')) ++i;
        std::size_t start = i;
        while (i < n && desc[i] != ' ' && desc[i] != '\n' &&
                        desc[i] != '\t' && desc[i] != '\r') ++i;
        if (i > start) words.push_back(desc.substr(start, i - start));
    }
    return words;
}

consteval std::size_t to_size(std::string_view s)
{
    std::size_t v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    return v;
}

// Parse the descriptor into its composites (in emission/definition order) and
// set `top` to the requested top-level type name.
consteval std::vector<Composite> parse(std::string_view desc, std::string_view &top)
{
    std::vector<Composite> comps;
    auto w = words_of(desc);
    for (std::size_t i = 0; i < w.size();)
    {
        if (w[i] == "C")
        {
            Composite c;
            c.name = w[i + 1];
            std::size_t nf = to_size(w[i + 2]);
            i += 3;
            for (std::size_t f = 0; f < nf; ++f)
            {
                // each member arrives as: M <type> <field>
                c.members.push_back(Member{ w[i + 1], w[i + 2] });
                i += 3;
            }
            comps.push_back(std::move(c));
        }
        else if (w[i] == "T")
        {
            top = w[i + 1];
            i += 2;
        }
        else ++i; // unreachable for well-formed input
    }
    return comps;
}

// -------------------------------------------------------------------------
// Token-sequence helpers
// -------------------------------------------------------------------------
// A single-identifier type spelling -> a one-token token sequence. The scalar
// keywords are NOT identifiers, so \id() cannot spell them; they are emitted as
// literal keyword tokens. Everything else is a composite's synthesised name and
// is a genuine identifier, so \id() is correct.
consteval std::meta::info type_token(std::string_view spelling)
{
    if (spelling == "int")    return ^^{ int };
    if (spelling == "float")  return ^^{ float };
    if (spelling == "double") return ^^{ double };
    if (spelling == "bool")   return ^^{ bool };
    return ^^{ \id(spelling) };           // a composite name (__pyc_TN)
}

// -------------------------------------------------------------------------
// The injection
// -------------------------------------------------------------------------
// Called from the generated TU's namespace-scope `consteval { ... }` block.
// Injects, in order:
//   * one `struct <name> { ... };` per composite (deps precede users), and
//   * the two extern "C" wrappers naming the top type by splice.
consteval void inject_translators(std::string_view desc)
{
    std::string_view top;
    std::vector<Composite> comps = parse(desc, top);

    // 1. Inject each composite definition. Members accumulate into a token
    //    sequence that is then spliced into the struct body.
    for (const Composite &c : comps)
    {
        std::meta::info body = ^^{ };
        for (const Member &m : c.members)
            body = ^^{ \tokens(body) \tokens(type_token(m.type)) \id(m.name); };
        std::meta::queue_injection(^^{ struct \id(c.name) { \tokens(body) }; });
    }

    // 2. Inject the two extern "C" entry points dlsym() will look up. These are
    //    byte-for-byte the wrappers src/specialise.c prints, except the type is
    //    spliced in as a reflection rather than substituted into a format string.
    //    PyObject_to_T / T_to_PyObject may throw; the catch keeps any C++
    //    exception from crossing the extern "C" boundary (turning it into a
    //    Python error instead), exactly as in the string-templated path.
    std::meta::queue_injection(^^{
        extern "C" int __pyc_translate(PyObject *src, void *dest)
        {
            try
            {
                *static_cast<\id(top) *>(dest) = PyObject_to_T<\id(top)>(src);
                return 0;
            }
            catch (const std::exception &e)
            {
                if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, e.what());
                return -1;
            }
        }
        extern "C" PyObject *__pyc_fromvalue(void *src)
        {
            try { return T_to_PyObject<\id(top)>(*static_cast<\id(top) *>(src)); }
            catch (const std::exception &e)
            {
                if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, e.what());
                return nullptr;
            }
        }
    });
}

} // namespace pyc
