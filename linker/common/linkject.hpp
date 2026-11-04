// DRAFT (P3294 "Code Injection with Token Sequences") -- alternative to the
// std::format-based trampoline synthesis in src/auditor.cpp.
//
// =========================================================================
//  Why this exists
// =========================================================================
// The default auditor (src/auditor.cpp) builds the runtime stub library by
// *printing C++ source text*: gen_code() std::format()s one `extern "C"`
// trampoline per Python-bound function, and process_lmap() prints the by-value
// `struct` definitions recovered from DWARF, all into the link-stub source
// (linkstub.cpp, in the auditor's work dir), which compile_stubs() then hands to
// the compiler.
//
// This header is the heart of the LINKPY_INJECT_STUBS *alternative*. The auditor
// (via src/codegen_inject.cpp) instead emits only a compile-time *descriptor* of
// the structs and functions plus one `consteval` block. At linkstub.cpp's own
// compile time, inject_stubs() parses that descriptor and *injects* the struct
// definitions and trampolines as token sequences (P3294), splicing the C type
// spellings, function names and arities in hygienically.
//
// The runtime compiler invocation (compile_stubs) is unchanged -- the link map
// is only discovered at audit time -- but the trampoline *shape* now lives here
// as token sequences rather than as std::format strings inside the auditor.
//
// The consteval string helpers, the type-spelling -> token-sequence mapping and
// the struct-definition injection are shared with the vmextensions injection
// back end (pyc_inject.hpp); they live in the project-wide common/tokenject.hpp.
//
// NOTE: no P3294 compiler exists yet; this is an unverified design sketch and
// the exact spellings are expected to change before the proposal lands.

#pragma once
#include <meta>
#include <string_view>
#include <vector>

#include "tokenject.hpp"

// pycpputils.hpp (run_function / run_function_void / pack_args / arg_to_PyObject)
// is included by the linkstub template ahead of this header.

namespace linkpy {

using tokenject::Field;
using tokenject::split;
using tokenject::type_tokens;

// -------------------------------------------------------------------------
// Descriptor model
// -------------------------------------------------------------------------
// codegen_inject.cpp emits a line-oriented descriptor, '|'-delimited so that C
// type spellings (which contain spaces, e.g. "const char *") survive intact.
// Member/param lists are ';'-separated, each entry "<type>:<name>". Grammar:
//
//   S|<name>|<type>:<field>;<type>:<field>;...      a struct definition
//   F|<module>|<name>|<return-type>|<type>:<p>;...   a function trampoline
//
// Structs are emitted dependencies-first (matching load_dwarf_structs order), so
// injecting them in descriptor order satisfies C++'s define-before-use.

struct Struct { std::string_view name; std::vector<Field> fields; };
struct Func {
    std::string_view module, name, ret;
    std::vector<Field> params;
};

// "int:x;double:y" -> [{int,x},{double,y}]; "" -> []. Tolerates a trailing ';'.
consteval std::vector<Field> parse_fields(std::string_view list)
{
    std::vector<Field> fs;
    if (list.empty()) return fs;
    for (std::string_view e : split(list, ';'))
    {
        if (e.empty()) continue;
        std::size_t c = e.find(':');
        fs.push_back(Field{ e.substr(0, c), e.substr(c + 1) });
    }
    return fs;
}

// -------------------------------------------------------------------------
// The injection
// -------------------------------------------------------------------------
consteval void inject_stubs(std::string_view desc)
{
    for (std::string_view line : split(desc, '\n'))
    {
        if (line.empty()) continue;
        std::vector<std::string_view> f = split(line, '|');

        // ---- struct definition: S|name|type:field;... --------------------
        if (f[0] == "S")
        {
            Struct s{ f[1], parse_fields(f.size() > 2 ? f[2] : std::string_view{}) };
            tokenject::inject_struct(s.name, s.fields);
            continue;
        }

        // ---- function trampoline: F|module|name|ret|type:p;... -----------
        if (f[0] == "F")
        {
            Func fn{ f[1], f[2], f[3],
                     parse_fields(f.size() > 4 ? f[4] : std::string_view{}) };

            // Parameter declaration list: "<type> <pname>, ...".
            std::meta::info decl = ^^{ };
            bool first = true;
            for (const Field &p : fn.params)
            {
                decl = first
                    ? ^^{ \tokens(type_tokens(p.type)) \id(p.name) }
                    : ^^{ \tokens(decl), \tokens(type_tokens(p.type)) \id(p.name) };
                first = false;
            }

            // Forwarding argument list: ", p0, p1, ..." (the parameter names),
            // appended after module/name/argc to run_function[_void].
            std::meta::info fwd = ^^{ };
            for (const Field &p : fn.params)
                fwd = ^^{ \tokens(fwd), \id(p.name) };

            const std::size_t argc = fn.params.size();

            // static CallSite site{"mod","name"}; run_function_void(site, argc, p0, ...)
            // for void returns, else return run_function<RET>(site, argc, p0, ...),
            // the same shape gen_code prints. \(fn.module) / \(fn.name)
            // interpolate the names as string literals; \(argc) the arity as an
            // integer literal. Qualified linkpy::py:: to match the namespaced
            // marshaller; a Lua variant of this draft back end would need the
            // qualifier parameterized like gen_code's CALLEE_NS (this path is
            // Python-only today).
            std::meta::info call = (fn.ret == "void")
                ? ^^{ static linkpy::py::CallSite site{\(fn.module), \(fn.name)};
                      linkpy::py::run_function_void(site, \(argc) \tokens(fwd)); }
                : ^^{ static linkpy::py::CallSite site{\(fn.module), \(fn.name)};
                      return linkpy::py::run_function<\tokens(type_tokens(fn.ret))>(
                          site, \(argc) \tokens(fwd)); };

            std::meta::queue_injection(^^{
                extern "C" \tokens(type_tokens(fn.ret)) \id(fn.name)(\tokens(decl))
                {
                    \tokens(call)
                }
            });
        }
    }
}

} // namespace linkpy
