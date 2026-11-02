// DRAFT alternative codegen back end for the linkpy auditor, selected with
// -DLINKPY_INJECT_STUBS (the LINKPY_INJECT_STUBS CMake option). See
// include/codegen_inject.hpp and include/linkject.hpp for the rationale.
//
// Where auditor.cpp prints the trampolines and struct definitions directly into
// linkstub.cpp, this writes a compact descriptor that linkpy::inject_stubs()
// expands into the same code via P3294 token-sequence injection at linkstub's
// own compile time. The compile step (compile_stubs) is unchanged.
//
// There is no P3294 compiler yet; this is an unverified design sketch.

#include "codegen_inject.hpp"

#include <fstream>
#include <ostream>

namespace {

// Emit one descriptor field, escaping nothing: the spellings DWARF produces here
// (C type names, identifiers) never contain the delimiters '|', ';', ':' or a
// newline, nor the raw-string terminator. A real implementation would validate
// this; for the draft we rely on the limited type set the auditor accepts.

// "type:name;type:name;" for a (type, name) list. Trailing ';' is tolerated by
// linkpy::parse_fields.
void emit_fields(std::ostream &out,
                 const std::vector<std::pair<std::string, std::string>> &fields)
{
    for (const auto &f : fields)
        out << f.first << ':' << f.second << ';';
}

} // namespace

void write_linkstub_injected(const std::string &templatepath,
                             const std::string &outpath,
                             const std::vector<FunLinkEntry> &linkmap,
                             const std::vector<StructDef> &structs)
{
    std::ifstream in(templatepath);
    std::ofstream out(outpath);

    // 1. The template prologue (Python VM init/teardown, #include <pycpputils.hpp>).
    out << in.rdbuf();
    in.close();
    out << '\n';

    // 2. Pull in the token-sequence injector.
    out << "#include \"linkject.hpp\"\n\n";

    // 3. The descriptor: one S/F record per line (see linkject.hpp grammar).
    //    Structs first, in dependency order, so they are injected before the
    //    trampolines that name them by value.
    out << "constexpr char __linkstub_desc[] = R\"DESC(\n";

    for (const StructDef &s : structs)
    {
        out << "S|" << s.name << '|';
        emit_fields(out, s.fields);
        out << '\n';
    }

    for (const FunLinkEntry &fn : linkmap)
    {
        out << "F|" << fn.module_name << '|' << fn.name << '|'
            << fn.return_type << '|';
        // FunLinkEntry stores params as (type, name) pairs, same shape as fields.
        std::vector<std::pair<std::string, std::string>> params(fn.params.begin(),
                                                                fn.params.end());
        emit_fields(out, params);
        out << '\n';
    }

    out << ")DESC\";\n\n";

    // 4. The single injection driver: at compile time this expands the descriptor
    //    into the struct definitions and extern "C" trampolines.
    out << "consteval { linkpy::inject_stubs(__linkstub_desc); }\n";
    out.close();
}
