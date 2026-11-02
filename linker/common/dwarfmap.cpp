#include "dwarfmap.hpp"

#include <elfutils/libdw.h>
#include <dwarf.h>

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>
#include <utility>
#include <set>

// #define DEBUG
#ifdef DEBUG
#include <iostream>
#endif

namespace {

// Reconstruct the C spelling of a type from its DWARF DIE, so the generated
// trampoline declarations match what the .lmap path used to emit (e.g. "int",
// "float", "struct HelloWorld", "const char *"). Qualifiers and pointers are
// peeled recursively via the DIE's own DW_AT_type.
std::string type_name(Dwarf_Die *die);

// The C type referenced by `owner`'s DW_AT_type, or "void" when absent (a
// subprogram with no DW_AT_type returns void; a pointer with none is void*).
std::string referenced_type(Dwarf_Die *owner)
{
    Dwarf_Attribute attr;
    if (dwarf_attr(owner, DW_AT_type, &attr) == nullptr)
        return "void";
    Dwarf_Die target;
    if (dwarf_formref_die(&attr, &target) == nullptr)
        return "void";
    return type_name(&target);
}

std::string type_name(Dwarf_Die *die)
{
    switch (dwarf_tag(die))
    {
    case DW_TAG_base_type:
    case DW_TAG_typedef:
    {
        const char *n = dwarf_diename(die);
        return n ? n : "void";
    }
    case DW_TAG_structure_type:
    {
        const char *n = dwarf_diename(die);
        return n ? std::string("struct ") + n : "void";
    }
    case DW_TAG_union_type:
    {
        const char *n = dwarf_diename(die);
        return n ? std::string("union ") + n : "void";
    }
    case DW_TAG_enumeration_type:
    {
        const char *n = dwarf_diename(die);
        return n ? std::string("enum ") + n : "void";
    }
    case DW_TAG_pointer_type:
        return referenced_type(die) + " *";
    case DW_TAG_const_type:
        return "const " + referenced_type(die);
    case DW_TAG_volatile_type:
        return "volatile " + referenced_type(die);
    default:
    {
        const char *n = dwarf_diename(die);
        return n ? n : "void";
    }
    }
}

// Derive the scripting module name from a DWARF decl-file path the way the
// .lmap path derived it from the stub-header filename: take the basename up to
// the first '.', so "mytest.py.c"/"mytest.py.h" and "mytest.lua.c"/"mytest.lua.h"
// all map to "mytest".
std::string module_from_decl_file(const char *decl_file)
{
    std::string base = std::filesystem::path(decl_file).filename().string();
    std::size_t dot = base.find('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

// True when `decl_file` belongs to a linkpy stub header/source (contains ".py."
// for Python or ".lua." for Lua), which is how we tell our generated functions
// apart from anything else the compiler may have emitted DWARF for. A stub
// library is single-language, so either marker identifies one of ours.
bool is_stub_decl_file(const char *decl_file)
{
    if (decl_file == nullptr)
        return false;
    std::string s(decl_file);
    return s.find(".py.") != std::string::npos ||
           s.find(".lua.") != std::string::npos;
}

// The language marker is_stub_decl_file matched: "py", "lua", or "" when the
// decl-file is not one of ours. Kept on FunLinkEntry::lang so the auditor can
// verify the stub library is single-language (and pick the callee marshaller).
std::string lang_from_decl_file(const char *decl_file)
{
    if (decl_file == nullptr)
        return "";
    std::string s(decl_file);
    if (s.find(".py.") != std::string::npos)
        return "py";
    if (s.find(".lua.") != std::string::npos)
        return "lua";
    return "";
}

// If `owner`'s DW_AT_type denotes a struct passed/returned by value (peeling
// any const/volatile/typedef wrappers, but NOT pointers — those only need a
// forward declaration), copy that structure DIE into `out` and return true.
bool struct_type_of(Dwarf_Die *owner, Dwarf_Die *out)
{
    Dwarf_Attribute attr;
    if (dwarf_attr(owner, DW_AT_type, &attr) == nullptr)
        return false;
    Dwarf_Die t;
    if (dwarf_formref_die(&attr, &t) == nullptr)
        return false;
    for (;;)
    {
        switch (dwarf_tag(&t))
        {
        case DW_TAG_structure_type:
            *out = t;
            return true;
        case DW_TAG_const_type:
        case DW_TAG_volatile_type:
        case DW_TAG_typedef:
        {
            Dwarf_Attribute inner;
            if (dwarf_attr(&t, DW_AT_type, &inner) == nullptr)
                return false;
            if (dwarf_formref_die(&inner, &t) == nullptr)
                return false;
            continue;
        }
        default:
            return false;
        }
    }
}

// The spelling of a struct member. A fixed-size array member is `elem name[N]`
// in C, bounds after the name, so the dimensions come back separately for the
// emitter to append to the field name; any other member gets its ordinary
// type spelling and an empty suffix.
static void member_type_and_dims(Dwarf_Die *member, std::string &type, std::string &dims)
{
    dims.clear();
    Dwarf_Attribute attr;
    Dwarf_Die t;
    if (dwarf_attr(member, DW_AT_type, &attr) == nullptr
        || dwarf_formref_die(&attr, &t) == nullptr
        || dwarf_tag(&t) != DW_TAG_array_type)
    {
        type = referenced_type(member);
        return;
    }
    type = referenced_type(&t); // the element type
    Dwarf_Die sub;
    if (dwarf_child(&t, &sub) != 0)
        return;
    do
    {
        if (dwarf_tag(&sub) != DW_TAG_subrange_type)
            continue;
        Dwarf_Attribute a;
        Dwarf_Word n = 0;
        Dwarf_Sword sn = 0;
        if (dwarf_attr_integrate(&sub, DW_AT_count, &a) != nullptr
            && dwarf_formudata(&a, &n) == 0)
            dims += "[" + std::to_string(n) + "]";
        else if (dwarf_attr_integrate(&sub, DW_AT_upper_bound, &a) != nullptr
                 && dwarf_formudata(&a, &n) == 0)
            dims += "[" + std::to_string(n + 1) + "]";
        else if (dwarf_attr_integrate(&sub, DW_AT_upper_bound, &a) != nullptr
                 && dwarf_formsdata(&a, &sn) == 0)
            dims += "[" + std::to_string(sn + 1) + "]";
        else
            dims += "[]";
    } while (dwarf_siblingof(&sub, &sub) == 0);
}

// Append the definition of `struct_die` (and any by-value struct it contains,
// emitted first so it is defined before use) to `out`, skipping names already
// in `seen`. `seen` is marked before recursing so a malformed cycle terminates.
void collect_struct(Dwarf_Die *struct_die, std::vector<StructDef> &out,
                    std::set<std::string> &seen)
{
    const char *n = dwarf_diename(struct_die);
    if (n == nullptr || seen.count(n))
        return;
    seen.insert(n);

    StructDef def;
    def.name = n;
    std::vector<Dwarf_Die> nested;

    Dwarf_Die member;
    if (dwarf_child(struct_die, &member) == 0)
    {
        do
        {
            if (dwarf_tag(&member) != DW_TAG_member)
                continue;
            const char *mname = dwarf_diename(&member);
            std::string mtype, mdims;
            member_type_and_dims(&member, mtype, mdims);
            def.fields.emplace_back(mtype, std::string(mname ? mname : "") + mdims);
            Dwarf_Die ms;
            if (struct_type_of(&member, &ms))
                nested.push_back(ms);
        } while (dwarf_siblingof(&member, &member) == 0);
    }

    for (auto &ms : nested)
        collect_struct(&ms, out, seen); // dependencies before the dependent
    out.push_back(std::move(def));
}

FunLinkEntry entry_from_subprogram(Dwarf_Die *die, const char *decl_file)
{
    const char *fname = dwarf_diename(die);
    std::string module = module_from_decl_file(decl_file);
    std::string return_type = referenced_type(die);

    std::vector<std::pair<std::string, std::string>> params;
    Dwarf_Die child;
    if (dwarf_child(die, &child) == 0)
    {
        do
        {
            if (dwarf_tag(&child) != DW_TAG_formal_parameter)
                continue;
            std::string type = referenced_type(&child);
            // GCC keeps parameter names with -g, but they're only used to name
            // locals inside the generated trampoline, so synthesise a unique
            // identifier when one is missing rather than failing.
            const char *pname = dwarf_diename(&child);
            std::string name = pname ? pname : ("arg" + std::to_string(params.size()));
            params.emplace_back(std::move(type), std::move(name));
        } while (dwarf_siblingof(&child, &child) == 0);
    }

    FunLinkEntry entry(module, fname ? fname : "", return_type, params);
    entry.lang = lang_from_decl_file(decl_file);
    return entry;
}

} // namespace

std::vector<FunLinkEntry> load_dwarf_map(const std::string &path)
{
    std::vector<FunLinkEntry> linkmap;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
#ifdef DEBUG
        std::cerr << "load_dwarf_map: cannot open " << path << std::endl;
#endif
        return linkmap;
    }

    Dwarf *dbg = dwarf_begin(fd, DWARF_C_READ);
    if (dbg == nullptr)
    {
#ifdef DEBUG
        std::cerr << "load_dwarf_map: no DWARF in " << path << std::endl;
#endif
        close(fd);
        return linkmap;
    }

    Dwarf_Off off = 0;
    Dwarf_Off next_off = 0;
    size_t header_size = 0;
    while (dwarf_nextcu(dbg, off, &next_off, &header_size,
                        nullptr, nullptr, nullptr) == 0)
    {
        Dwarf_Die cu;
        if (dwarf_offdie(dbg, off + header_size, &cu) != nullptr)
        {
            Dwarf_Die child;
            if (dwarf_child(&cu, &child) == 0)
            {
                do
                {
                    if (dwarf_tag(&child) != DW_TAG_subprogram)
                        continue;
                    // Only defined, externally-visible stub functions: a
                    // definition carries DW_AT_low_pc (a bare declaration does
                    // not), and the decl-file marks it as one of ours.
                    if (dwarf_diename(&child) == nullptr)
                        continue;
                    if (!dwarf_hasattr(&child, DW_AT_low_pc))
                        continue;
                    const char *decl_file = dwarf_decl_file(&child);
                    if (!is_stub_decl_file(decl_file))
                        continue;
                    linkmap.push_back(entry_from_subprogram(&child, decl_file));
                } while (dwarf_siblingof(&child, &child) == 0);
            }
        }
        off = next_off;
    }

    dwarf_end(dbg);
    close(fd);
    return linkmap;
}

std::vector<StructDef> load_dwarf_structs(const std::string &path)
{
    std::vector<StructDef> structs;
    std::set<std::string> seen;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return structs;

    Dwarf *dbg = dwarf_begin(fd, DWARF_C_READ);
    if (dbg == nullptr)
    {
        close(fd);
        return structs;
    }

    Dwarf_Off off = 0;
    Dwarf_Off next_off = 0;
    size_t header_size = 0;
    while (dwarf_nextcu(dbg, off, &next_off, &header_size,
                        nullptr, nullptr, nullptr) == 0)
    {
        Dwarf_Die cu;
        if (dwarf_offdie(dbg, off + header_size, &cu) != nullptr)
        {
            Dwarf_Die child;
            if (dwarf_child(&cu, &child) == 0)
            {
                do
                {
                    // Mirror load_dwarf_map's filter: defined stub functions only.
                    if (dwarf_tag(&child) != DW_TAG_subprogram)
                        continue;
                    if (dwarf_diename(&child) == nullptr)
                        continue;
                    if (!dwarf_hasattr(&child, DW_AT_low_pc))
                        continue;
                    if (!is_stub_decl_file(dwarf_decl_file(&child)))
                        continue;

                    // Return type, then each parameter: collect any by-value struct.
                    Dwarf_Die s;
                    if (struct_type_of(&child, &s))
                        collect_struct(&s, structs, seen);

                    Dwarf_Die param;
                    if (dwarf_child(&child, &param) == 0)
                    {
                        do
                        {
                            if (dwarf_tag(&param) != DW_TAG_formal_parameter)
                                continue;
                            Dwarf_Die ps;
                            if (struct_type_of(&param, &ps))
                                collect_struct(&ps, structs, seen);
                        } while (dwarf_siblingof(&param, &param) == 0);
                    }
                } while (dwarf_siblingof(&child, &child) == 0);
            }
        }
        off = next_off;
    }

    dwarf_end(dbg);
    close(fd);
    return structs;
}
