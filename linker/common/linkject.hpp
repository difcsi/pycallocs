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
// =========================================================================
//  P3294 primitives used (syntax per P3294R2 / P3289 consteval blocks)
// =========================================================================
//   ^^{ tokens }                a token-sequence literal -> std::meta::info
//   \tokens(e)                  splice another token sequence e
//   \id(sv)                     form an identifier token from a string constant
//   \(e)                        interpolate a string/scalar constant as a token
//                               (a string constant -> a string-literal token)
//   consteval { ... }           a consteval block: an injection context
//   std::meta::queue_injection  queue tokens for injection into the enclosing
//                               (namespace) context
//
// NOTE: no P3294 compiler exists yet; this is an unverified design sketch and
// the exact spellings are expected to change before the proposal lands.

#pragma once
#include <meta>
#include <string_view>
#include <vector>

// pycpputils.hpp (run_function / run_function_void / pack_args / arg_to_PyObject)
// is included by the linkstub template ahead of this header.

namespace linkpy {

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

struct Field { std::string_view type; std::string_view name; };
struct Struct { std::string_view name; std::vector<Field> fields; };
struct Func {
    std::string_view module, name, ret;
    std::vector<Field> params;
};

consteval std::vector<std::string_view> split(std::string_view s, char d)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == d)
        {
            if (i > start) out.push_back(s.substr(start, i - start));
            else if (i == start) out.push_back({}); // preserve empties for arity
            start = i + 1;
        }
    return out;
}

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

consteval std::size_t to_size(std::string_view s)
{
    std::size_t v = 0;
    for (char ch : s) v = v * 10 + (ch - '0');
    return v;
}

// -------------------------------------------------------------------------
// Type spelling -> token sequence
// -------------------------------------------------------------------------
// A C type spelling is a sequence of whitespace-separated words plus '*'s, e.g.
// "const char *", "struct Point", "int". Keywords are NOT identifiers, so \id
// cannot spell them -- they are emitted as literal keyword tokens; '*' as a
// literal punctuator; everything else (a struct tag or typedef name) via \id.
consteval std::meta::info word_token(std::string_view w)
{
    // The keywords that can appear in the C type spellings DWARF hands back here.
    if (w == "void")     return ^^{ void };
    if (w == "bool")     return ^^{ bool };
    if (w == "char")     return ^^{ char };
    if (w == "short")    return ^^{ short };
    if (w == "int")      return ^^{ int };
    if (w == "long")     return ^^{ long };
    if (w == "unsigned") return ^^{ unsigned };
    if (w == "signed")   return ^^{ signed };
    if (w == "float")    return ^^{ float };
    if (w == "double")   return ^^{ double };
    if (w == "const")    return ^^{ const };
    if (w == "volatile") return ^^{ volatile };
    if (w == "struct")   return ^^{ struct };
    if (w == "union")    return ^^{ union };
    if (w == "enum")     return ^^{ enum };
    if (w == "*")        return ^^{ * };
    return ^^{ \id(w) };                   // a tag/typedef name
}

// Split a spelling into words, treating each '*' as its own word, and fold them
// into one token sequence.
consteval std::meta::info type_tokens(std::string_view spelling)
{
    std::meta::info ts = ^^{ };
    std::size_t i = 0, n = spelling.size();
    while (i < n)
    {
        while (i < n && (spelling[i] == ' ' || spelling[i] == '\t')) ++i;
        if (i >= n) break;
        if (spelling[i] == '*')
        {
            ts = ^^{ \tokens(ts) * };
            ++i;
            continue;
        }
        std::size_t start = i;
        while (i < n && spelling[i] != ' ' && spelling[i] != '\t' &&
                        spelling[i] != '*') ++i;
        ts = ^^{ \tokens(ts) \tokens(word_token(spelling.substr(start, i - start))) };
    }
    return ts;
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
            std::meta::info body = ^^{ };
            for (const Field &fld : s.fields)
                body = ^^{ \tokens(body) \tokens(type_tokens(fld.type)) \id(fld.name); };
            std::meta::queue_injection(^^{ struct \id(s.name) { \tokens(body) }; });
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

            // run_function_void("mod","name",argc, p0, ...)  for void returns,
            // else  return run_function<RET>("mod","name",argc, p0, ...).
            // \(fn.module) / \(fn.name) interpolate the names as string literals;
            // \(argc) the arity as an integer literal.
            std::meta::info call = (fn.ret == "void")
                ? ^^{ run_function_void(\(fn.module), \(fn.name), \(argc) \tokens(fwd)); }
                : ^^{ return run_function<\tokens(type_tokens(fn.ret))>(
                          \(fn.module), \(fn.name), \(argc) \tokens(fwd)); };

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
