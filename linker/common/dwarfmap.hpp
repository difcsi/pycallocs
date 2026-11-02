#pragma once
#include <string>
#include <vector>
#include <utility>
#include "linkmap.hpp"

std::vector<FunLinkEntry> load_dwarf_map(const std::string &path);

// A C struct definition recovered from DWARF: its tag name and ordered fields
// as (type-spelling, field-name) pairs, e.g. {"Point", {{"int","x"},{"int","y"}}}.
struct StructDef
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;
};

// The struct types passed/returned by value across the stub functions in `path`,
// in definition order (a struct used as another's field comes first). The auditor
// emits these into the linkstub so the generated trampolines can name the types.
std::vector<StructDef> load_dwarf_structs(const std::string &path);
