// DRAFT (P3294 "Code Injection with Token Sequences") -- consteval machinery
// shared by the two token-injection back ends:
//
//   linker/common/linkject.hpp        (LINKPY_INJECT_STUBS: linkstub trampolines)
//   vmextensions/cpython/pyc_inject.hpp (INJECT_CONVERSION: PyObject translators)
//
// Both back ends receive a line-oriented descriptor of C types at the generated
// TU's compile time and expand it into struct definitions (plus back-end-specific
// entry points) via token-sequence injection. The descriptor grammars differ,
// but the string parsing helpers, the C-type-spelling -> token-sequence mapping
// and the struct-definition injection are identical; they live here.
//
// NOTE: no P3294 compiler exists yet; like its two consumers this is an
// unverified design sketch and the exact spellings (^^{...}, \tokens, \id, \(),
// consteval blocks, std::meta::queue_injection) are expected to change before
// the proposal lands.

#pragma once
#include <meta>
#include <string_view>
#include <vector>

namespace tokenject {

// A (type spelling, identifier) pair: a struct field or a function parameter.
struct Field { std::string_view type; std::string_view name; };

// -------------------------------------------------------------------------
// consteval string helpers
// -------------------------------------------------------------------------

// Split on a single delimiter, preserving empty entries (so record arity is
// visible to the caller).
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

// Split into whitespace-separated words, in order, dropping the whitespace.
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
    // The keywords that can appear in the C type spellings DWARF (or a
    // liballocs uniqtype walk) hands back here.
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
// into one token sequence. A single-word spelling degenerates to word_token.
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
// Struct-definition injection
// -------------------------------------------------------------------------
// Queue `struct <name> { <type> <field>; ... };` for injection into the
// enclosing (namespace) context. Callers emit dependencies first, so injecting
// in call order satisfies C++'s define-before-use.
consteval void inject_struct(std::string_view name, const std::vector<Field> &fields)
{
    std::meta::info body = ^^{ };
    for (const Field &fld : fields)
        body = ^^{ \tokens(body) \tokens(type_tokens(fld.type)) \id(fld.name); };
    std::meta::queue_injection(^^{ struct \id(name) { \tokens(body) }; });
}

} // namespace tokenject
