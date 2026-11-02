// heaphints: a standalone generator for Yukon's heap-promotion hints.
//
// Where pystubgen derives the hints from the .py.h stub headers at stub-gen
// time, this tool derives them from the DWARF embedded in an already-compiled
// stub library/executable (the same source the auditor reads at runtime). It
// lists the functions whose pointer arguments escape into Python, i.e. the ones
// the trampoline passes by reference as a pycallocs proxy. Both tools share
// escaping_functions()/write_heap_hints() so they emit byte-identical files.

#include <fstream>
#include <iostream>
#include <string>

#include "linkmap.hpp"
#include "dwarfmap.hpp"

static void print_help()
{
    std::cout
        << "Usage: heaphints <binary> [output]\n"
        << "  Reads DWARF from <binary> (a stub library/executable built with -g)\n"
        << "  and emits Yukon heap-promotion hints: the functions whose pointer\n"
        << "  arguments escape into Python (one name per line).\n"
        << "  Only linkpy stub functions (declared in a .py.* file) are considered.\n"
        << "  output: file to write (default: stdout).\n"
        << "  -h, --help: show this help and exit.\n";
}

int main(int argc, char *argv[])
{
    std::string binary, output;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            print_help();
            return 0;
        }
        else if (binary.empty())
            binary = arg;
        else if (output.empty())
            output = arg;
    }
    if (binary.empty())
    {
        print_help();
        return 1;
    }

    auto names = escaping_functions(load_dwarf_map(binary));

    if (output.empty())
    {
        write_heap_hints(std::cout, names);
    }
    else
    {
        std::ofstream out(output);
        if (!out)
        {
            std::cerr << "heaphints: cannot open " << output << " for writing\n";
            return 1;
        }
        write_heap_hints(out, names);
    }
    return 0;
}
