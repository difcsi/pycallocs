#pragma once
#include <string>
#include <vector>
#include "linkmap.hpp"
#include "dwarfmap.hpp"

// DRAFT (LINKPY_INJECT_STUBS): alternative to auditor.cpp's gen_code()/struct
// printing. Writes the link-stub source (linkstub.cpp, in the auditor's work dir)
// as the linkstub template followed by a
// compile-time *descriptor* and a single consteval injection block, so that the
// struct definitions and `extern "C"` trampolines are synthesised by
// linkpy::inject_stubs() (include/linkject.hpp) via P3294 token-sequence
// injection rather than by std::format-ing C++ source from the auditor.
//
// Drop-in for the codegen part of process_lmap(): same inputs (template path,
// output path, the link map, and the by-value structs recovered from DWARF),
// same output file, same downstream compile_stubs().
void write_linkstub_injected(const std::string &templatepath,
                             const std::string &outpath,
                             const std::vector<FunLinkEntry> &linkmap,
                             const std::vector<StructDef> &structs);
