#include "linkmap.hpp"
#include <sstream>
#include <ostream>
#include <algorithm>
#include <format>
#include <iterator>

#define DEBUG

#ifdef DEBUG
#include <iostream>
#endif

#define DELIMITER ':'

FunLinkEntry::FunLinkEntry(const std::string &line)
{
    std::istringstream iss(line);
    std::getline(iss, module_name, DELIMITER);
    std::getline(iss, name, DELIMITER);
    std::getline(iss, return_type, DELIMITER);

    while (!iss.eof())
    {
        std::string type, param;
        std::getline(iss, type, DELIMITER);
        std::getline(iss, param, DELIMITER);
        if (type.empty() || param.empty())
            break;
        params.emplace_back(type, param);
    }
    is_loaded = false;
}

std::vector<std::string> FunLinkEntry::get_funcvec()
{
    std::vector<std::string> funcvec;
    funcvec.reserve(this->params.size() * 2 + 3);
    funcvec.push_back(return_type);
    funcvec.push_back(name);
    for (const auto &param : params)
    {
        funcvec.push_back(param.first);
        funcvec.push_back(param.second);
    }
    return funcvec;
}

std::string FunLinkEntry::params_decl() const
{
    std::string out;
    auto it = std::back_inserter(out);
    for (std::size_t i = 0; i < params.size(); ++i)
        std::format_to(it, "{}{} {}", i ? ", " : "", params[i].first, params[i].second);
    return out;
}

std::string FunLinkEntry::params_args() const
{
    std::string out;
    auto it = std::back_inserter(out);
    for (std::size_t i = 0; i < params.size(); ++i)
        std::format_to(it, "{}{}", i ? ", " : "", params[i].second);
    return out;
}

// Classify a single (type, name) parameter. The '*' marking a pointer may land
// in either field depending on how the declarator was spelled and parsed:
// gen_lmap splits at the last whitespace, so "struct Point *p" yields
// type="struct Point", name="*p", while "struct Point* p" keeps the star in the
// type; the DWARF path renders it as "struct Point *". So count stars across
// both fields, and derive the base type by stripping stars/qualifiers from the
// type field. A single-level char pointer is a Python str (by value), not an
// escaping proxy pointer.
static bool param_escapes(const std::pair<std::string, std::string> &p)
{
    int stars = static_cast<int>(std::count(p.first.begin(), p.first.end(), '*') +
                                 std::count(p.second.begin(), p.second.end(), '*'));
    if (stars == 0)
        return false; // not a pointer

    // base = type with '*' removed and const/volatile qualifiers stripped.
    std::string base = p.first;
    base.erase(std::remove(base.begin(), base.end(), '*'), base.end());
    std::istringstream iss(base);
    std::string tok, normalized;
    while (iss >> tok)
    {
        if (tok == "const" || tok == "volatile")
            continue;
        if (!normalized.empty())
            normalized += ' ';
        normalized += tok;
    }

    bool is_string_ptr = (stars == 1 && normalized == "char");
    return !is_string_ptr;
}

bool FunLinkEntry::has_escaping_pointer() const
{
    return std::any_of(params.begin(), params.end(), param_escapes);
}

std::set<std::string> escaping_functions(const std::vector<FunLinkEntry> &entries)
{
    std::set<std::string> names;
    for (const auto &e : entries)
        if (e.has_escaping_pointer())
            names.insert(e.name);
    return names;
}

void write_heap_hints(std::ostream &os, const std::set<std::string> &names)
{
    os << "# Heap-hints emitted by LinkPy.\n"
          "#\n"
          "# One function name per line. Blank lines and lines starting with '#' are\n"
          "# ignored. An address-taken stack object is promoted to the heap only if its\n"
          "# address flows into the pointer argument of one of the functions listed here.\n";
    for (const auto &name : names)
        os << name << '\n';
}

std::string FunLinkEntry::to_string() const
{
    std::ostringstream oss;
    oss << module_name << DELIMITER << name << DELIMITER << return_type;
    for (const auto &param : params)
    {
        oss << DELIMITER << param.first << DELIMITER << param.second;
    }
    return oss.str();
}

std::istream &operator>>(std::istream &is, FunLinkEntry &e)
{
    std::string line;
    if (std::getline(is, line))
    {
        e = FunLinkEntry(line);
    }
    return is;
}

std::ostream &operator<<(std::ostream &os, const FunLinkEntry &e)
{
    os << e.to_string();
    return os;
}

const FunLinkEntry* find_function(const std::vector<FunLinkEntry> &linkmap, const std::string &name)
{
    auto it = std::find_if(linkmap.begin(), linkmap.end(), [&name](const FunLinkEntry &entry) {
        return entry.name == name;
    });
    return (it != linkmap.end()) ? &(*it) : nullptr;
}