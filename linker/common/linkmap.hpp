#pragma once
#include <string>
#include <vector>
#include <set>
#include <iosfwd>
struct FunLinkEntry
{
    std::string module_name;
    std::string return_type;
    std::string name;
    std::vector<std::pair<std::string, std::string>> params;

    bool is_loaded;
    FunLinkEntry(const std::string &module, const std::string &name, const std::string &return_type,
                 const std::vector<std::pair<std::string, std::string>> &params)
        : module_name(module), return_type(return_type), name(name), params(params), is_loaded(false) {}
    // Parse an entry from the colon-delimited form (module:name:ret:type:name:...).
    // Nothing in-tree reads an .lmap file any more (DWARF is the sole source of
    // types), but this round-trips with to_string() so an entry can still be
    // passed around as a string if ever needed.
    FunLinkEntry(const std::string &line);
    // Serialise an entry to that same colon-delimited form. Used as the in-memory
    // interchange the linkpychk runner pipes into tools/pytypecheck.py (--lmap -).
    std::string to_string() const;
    std::vector<std::string> get_funcvec();
    // "int a, int b" — the parameter list as a C declaration.
    std::string params_decl() const;
    // "a, b" — just the parameter names, for forwarding to a call.
    std::string params_args() const;
    // True if any parameter is a pointer that ESCAPES into Python, i.e. one the
    // trampoline wraps in a pycallocs proxy (passed by reference). This mirrors
    // arg_to_PyObject in pycpputils.hpp: char*/const char* become a by-value
    // Python str (the C pointer does not escape) and are excluded; any other
    // pointer is included. Used to emit Yukon's heap-promotion hints.
    bool has_escaping_pointer() const;
    friend std::istream &operator>>(std::istream &is, FunLinkEntry &e);
    friend std::ostream &operator<<(std::ostream &os, const FunLinkEntry &e);
};

const FunLinkEntry* find_function(const std::vector<FunLinkEntry> &linkmap, const std::string &name);

// Yukon heap-promotion hints. escaping_functions() returns the names of the
// entries whose pointer arguments escape into Python (sorted, de-duplicated);
// write_heap_hints() writes the hints file (header comment + one name per line).
// Shared by pystubgen (header-derived) and the standalone heaphints tool
// (DWARF-derived) so both emit byte-identical files.
std::set<std::string> escaping_functions(const std::vector<FunLinkEntry> &entries);
void write_heap_hints(std::ostream &os, const std::set<std::string> &names);