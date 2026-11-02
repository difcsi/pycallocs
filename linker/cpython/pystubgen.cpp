#include <filesystem>
#include <fstream>
#include <string>
#include <regex>
#include <vector>
#include <iostream>
#include <cctype>
#include <format>
#include <set>
#include "linkmap.hpp"
// #define DEBUG
// #define MOD_PREFIX

namespace fs = std::filesystem;

// Recognise a linkpy stub header by suffix and return its module name (the part
// before the suffix), or "" if it is not one. Both the Python (.py.h) and Lua
// (.lua.h) suffixes are accepted; the generated stub keeps the same infix, so a
// <mod>.py.h yields <mod>.py.c and a <mod>.lua.h yields <mod>.lua.c, which is
// exactly what dwarfmap's decl-file filter looks for.
static std::string stub_module_name(const std::string &filename)
{
    for (const std::string suffix : {".py.h", ".lua.h"})
    {
        if (filename.size() > suffix.size() &&
            filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0)
            return filename.substr(0, filename.size() - suffix.size());
    }
    return "";
}

std::vector<std::string> extractSignatures(const std::string &filepath)
{
    std::vector<std::string> signatures;
    std::ifstream file(filepath);
    if (!file.is_open())
    {
#ifdef DEBUG
        std::cerr << "Failed to open file: " << filepath << std::endl;
#endif
        return signatures;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Remove comments and normalize whitespace
    content = std::regex_replace(content, std::regex("/\\*[\\s\\S]*?\\*/"), "");
    content = std::regex_replace(content, std::regex("//.*"), "");
    std::replace(content.begin(), content.end(), '\n', ' ');

    // Match function declarations
    std::regex funcPattern(R"((extern)\s+[\w\*&:<>\s]+\s+[\w:]+\s*\([^;{]*\)\s*;)");

    auto words_begin = std::sregex_iterator(content.begin(), content.end(), funcPattern);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i)
    {
        std::string signature = (*i).str();
        signature = std::regex_replace(signature, std::regex("\\s*;"), "");
        signature = std::regex_replace(signature, std::regex("extern"), "");
        signature = std::regex_replace(signature, std::regex("\\s+"), " ");
        signatures.push_back(signature);
    }

    return signatures;
}

void gen_stubs(const std::string &filepath, const std::string &header, std::vector<FunLinkEntry> &signatures)
{
    std::ofstream out(filepath);
    if (!out.is_open())
    {
#ifdef DEBUG
        std::cerr << "Failed to open file for writing: " << filepath << std::endl;
#endif
        return;
    }
    out << std::format(R"(#include "{}"
#include <stdio.h>
#include <stdlib.h>
)",
                       header);

    for (const auto &sig : signatures)
    {
#ifdef MOD_PREFIX
        std::string name = sig.module_name + "_" + sig.name;
#else
        const std::string &name = sig.name;
#endif
        out << std::format(R"(__attribute__((noinline)) {} {}({})
{{
    fprintf(stderr, "OOPS: forgot to load auditor!\n");
    abort();
}}
)",
                           sig.return_type, name, sig.params_decl());
    }
}

const std::string get_function_name(const std::string &signature, const std::string &module_name)
{
    std::regex namePattern(R"((\w+)\s*\([^;{]*\))");
    std::smatch match;
    if (std::regex_search(signature, match, namePattern))
    {
#ifndef MOD_PREFIX
        if (match[1].str().find(module_name + "_") == 0)
        {
            std::cerr << "Module name prefix found in function name" << std::endl;
            throw std::runtime_error("Module name prefix found in function name");
        }
        return match[1].str();
#else
        std::string functionName = match[1].str();
        if (!functionName.find(module_name + "_") == 0)
        {
            std::cerr << "Module name prefix not found in function name" << std::endl;
            throw std::runtime_error("Module name prefix not found in function name");
        }
        functionName = functionName.substr(module_name.length() + 1);
#endif
    }
    return "";
}

const std::string get_function_return_type(const std::string &signature)
{
    // The return type is everything preceding the function name, which is the
    // last identifier before the parameter list '('. Capturing the whole span
    // (rather than just the single word before the name) preserves multi-word
    // return types such as `struct HelloWorld` or `unsigned int`.
    std::size_t paren = signature.find('(');
    if (paren == std::string::npos)
        return "";

    std::string head = signature.substr(0, paren);
    while (!head.empty() && std::isspace(static_cast<unsigned char>(head.back())))
        head.pop_back();

    std::size_t name_start = head.find_last_of(" \t");
    if (name_start == std::string::npos)
        return "";

    std::string returnType = head.substr(0, name_start);
    while (!returnType.empty() && std::isspace(static_cast<unsigned char>(returnType.back())))
        returnType.pop_back();
    std::size_t first = returnType.find_first_not_of(" \t");
    if (first == std::string::npos)
        return "";
    return returnType.substr(first);
}

const std::string get_function_parameters(const std::string &signature)
{
    std::regex paramPattern(R"(\(([^)]+)\))");
    std::smatch match;
    if (std::regex_search(signature, match, paramPattern))
    {
        return match[1].str();
    }
    return "";
}

std::vector<FunLinkEntry> gen_lmap(const std::string &module, const std::vector<std::string> &signatures)
{
    std::vector<FunLinkEntry> ret;
    ret.reserve(signatures.size());
    for (const auto &sig : signatures)
    {
        std::vector<std::pair<std::string, std::string>> params;
        std::string paramStr = get_function_parameters(sig);

        auto trim = [](std::string s) -> std::string {
            std::size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos)
                return "";
            return s.substr(b, s.find_last_not_of(" \t") - b + 1);
        };

        std::istringstream iss(paramStr);
        std::string one;
        while (std::getline(iss, one, ','))
        {
            one = trim(one);
            if (one.empty())
                continue;
            // The parameter name is the last whitespace-separated token; the
            // rest is the (possibly multi-word) type, e.g. "struct HelloWorld hw".
            std::size_t sp = one.find_last_of(" \t");
            if (sp == std::string::npos)
                continue; // a bare type with no name — skip
            params.emplace_back(trim(one.substr(0, sp)), one.substr(sp + 1));
        }
        FunLinkEntry entry(module, get_function_name(sig, module), get_function_return_type(sig), params);
        ret.push_back(entry);
    }
    return ret;
}

void print_help()
{
    std::cout << "Usage: pystubgen [include_path] [gen_path]" << std::endl;
    std::cout << "  include_path: Path to the directory containing .py.h files (default: ./include/)" << std::endl;
    std::cout << "  gen_path: Path to the directory where generated files will be saved (default: ./gen/)" << std::endl;
    std::cout << "  -h, --help: Show this help message and exit" << std::endl;
    std::cout << std::endl;
    std::cout << "Always writes <gen_path>/heap_hints.txt: the functions whose pointer arguments" << std::endl;
    std::cout << "escape into Python (one name per line), for Yukon's heap-promotion pass." << std::endl;
}

int main(int argc, char *argv[])
{

    std::string includePath = "./include/";
    std::string genPath = "./gen/";

    // Positional args (include_path, gen_path).
    std::vector<std::string> positionals;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            print_help();
            return 0;
        }
        else
        {
            positionals.push_back(arg);
        }
    }
    if (positionals.size() > 0)
        includePath = positionals[0];
    if (positionals.size() > 1)
        genPath = positionals[1];

    try
    {
        if (!fs::exists(includePath) || !fs::is_directory(includePath))
        {
#ifdef DEBUG
            std::cerr << "Error: " << includePath << " does not exist or is not a directory." << std::endl;
#endif
            return 1;
        }
        // Functions whose pointer arguments escape into Python; collected across
        // all modules and de-duplicated. Written to heap_hints.txt below.
        std::set<std::string> heap_hints;
        for (const auto &entry : fs::recursive_directory_iterator(includePath))
        {
            if (entry.is_regular_file())
            {
                std::string filename = entry.path().filename().string();
                std::string module = stub_module_name(filename);
                if (!module.empty())
                {
                    auto modlmap = gen_lmap(module, extractSignatures(entry.path().string()));
                    // Strip the trailing ".h" and append ".c": <mod>.py.h -> <mod>.py.c,
                    // <mod>.lua.h -> <mod>.lua.c.
                    gen_stubs(genPath + filename.substr(0, filename.size() - 2) + ".c", filename, modlmap);
                    auto escaping = escaping_functions(modlmap);
                    heap_hints.insert(escaping.begin(), escaping.end());

#ifdef DEBUG

                    std::cout << "=== " << filename << " ===" << std::endl;
                    if (signatures.empty())
                    {
                        std::cout << "No function signatures found." << std::endl;
                    }
                    else
                    {
                        for (const auto &sig : signatures)
                        {
                            std::cout << sig << std::endl;
                        }
                    }
                    std::cout << std::endl;
#endif
                }
            }
        }

        // Yukon heap-promotion hints: the functions whose pointer arguments
        // escape into Python (so an address-taken stack object flowing into one
        // of them must be promoted to the heap). Always emitted.
        std::ofstream hints(genPath + "heap_hints.txt");
        write_heap_hints(hints, heap_hints);
    }
    catch (const std::exception &e)
    {
#ifdef DEBUG
        std::cerr << "Error: " << e.what() << std::endl;
#endif
        return 1;
    }

    return 0;
}