#include <link.h>
#include <unistd.h>
#include <string.h>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
// One auditor source, two artifacts: audit.so embeds CPython (default), and
// audit_lua.so (built with -DLINKPY_LANG_LUA) embeds Lua. The codegen below is
// language-neutral -- only the embedded interpreter used by init/verify/finalize
// differs, abstracted behind lang_init()/lang_finalize()/verify_function().
#ifdef LINKPY_LANG_LUA
#include "luacpputils.hpp"
#else
#include "pycpputils.hpp"
#endif
#include "linkmap.hpp"
#include "dwarfmap.hpp"
#ifdef LINKPY_INJECT_STUBS
#include "codegen_inject.hpp" // DRAFT P3294 token-injection codegen back end
#endif

#define DEBUG
#define PY_VERIFY
#define TEMPLATE_DEFAULT_PATH "./template.cpp"
// #define ABORT_ON_PY_NOT_FOUND false

#ifdef LINKPY_LANG_LUA
static void lang_init() { init_lua(); }
static void lang_finalize() { close_lua(); }
[[maybe_unused]] static const char *LANG_NAME = "Lua";
#else
static void lang_init() { init_py(); }
static void lang_finalize() { Py_Finalize(); }
[[maybe_unused]] static const char *LANG_NAME = "Python";
#endif

#ifdef DEBUG
std::vector<FunLinkEntry> linkmap;
#endif


bool INIT = false;

std::string g_workdir;

static void cleanup_workdir()
{
    if (!g_workdir.empty())
    {
        std::error_code ec;
        std::filesystem::remove_all(g_workdir, ec);
    }
}

static const std::string &linkstub_workdir()
{
    if (g_workdir.empty())
    {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec)
            base = "/tmp";
        std::string tmpl = (base / "linkpy.XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data()) != nullptr)
        {
            g_workdir = buf.data();
            atexit(cleanup_workdir);
        }
        else
        {
            // Couldn't make a private dir; fall back to the temp root so we still
            // compile somewhere rather than failing outright (no auto-cleanup then).
            g_workdir = base.string();
        }
#ifdef DEBUG
        std::cout << "linkpy work dir: " << g_workdir << std::endl;
#endif
    }
    return g_workdir;
}

void gen_code(std::ostream &out, const FunLinkEntry &fun)
{
    const std::size_t paramc = fun.params.size();

    // The trampoline forwards to run_function<T> (or run_function_void for void).
    // Build the call expression in a single buffer, appending in place.
    std::string call;
    auto it = std::back_inserter(call);
    if (fun.return_type == "void")
        std::format_to(it, R"(run_function_void("{}", "{}", {})", fun.module_name, fun.name, paramc);
    else
        std::format_to(it, R"(return run_function<{}>("{}", "{}", {})",
                       fun.return_type, fun.module_name, fun.name, paramc);
    if (paramc > 0)
        std::format_to(it, ", {}", fun.params_args());
    call += ')';

    out << std::format(R"(extern "C" {} {}({})
{{
#ifdef DEBUG
    printf("Hello from {}!\n");
#endif
    {};
}}

)",
                       fun.return_type, fun.name, fun.params_decl(), fun.name, call);
}

void verify_function(FunLinkEntry &fun)
{
    // Search the embedded interpreter for fun.name in module fun.module_name; if
    // found, set fun.is_loaded so the link-time report can flag a stub whose
    // implementation is missing. The actual marshalling happens later in the
    // JIT-compiled linkstub, in a separate VM.
#ifdef LINKPY_LANG_LUA
    if (function_exists(lua_state(), fun.module_name, fun.name))
        fun.is_loaded = true;
#ifdef ABORT_ON_PY_NOT_FOUND
    else
        abort();
#endif
#else
    PyObject *pModule = import_module(fun.module_name.c_str());
    if (pModule == NULL)
    {
        PyErr_Print();
#ifdef ABORT_ON_PY_NOT_FOUND
        abort();
#endif
        return;
    }
    PyObject *pFunc = import_function(pModule, fun.name.c_str());
    if (pFunc == NULL)
    {
        PyErr_Print();
#ifdef ABORT_ON_PY_NOT_FOUND
        abort();
#endif
        return;
    }
    fun.is_loaded = true;
#endif
}

int compile_stubs()
{
    const char *build_cmd = getenv("LINKSTUB_BUILD_CMD");
    std::string command = "cd '" + linkstub_workdir() +
                          "' && env -u LD_AUDIT -u LD_DEBUG ";
    command += (build_cmd != NULL) ? build_cmd : "make liblinkstub";

    #ifndef DEBUG
    command += " >/dev/null 2>/dev/null";
    #endif

    // The build command shells out to a compiler. When the program being audited
    // runs under the liballocs LD_PRELOAD (required for the by-reference path,
    // where C objects must carry liballocs type info), that preload must NOT be
    // inherited by the compiler: liballocs' syscall trapping crashes cc1plus.
    // The `env -u` prefix only unsets variables for the final program, but the
    // /bin/sh that system() spawns would already start under the preload. So
    // drop LD_PRELOAD from our own environment for the duration of the call
    // (liballocs is already loaded in this process, so this does not affect us)
    // and restore it afterwards.
    const char *saved_preload = getenv("LD_PRELOAD");
    std::string preload = saved_preload ? saved_preload : "";
    bool had_preload = (saved_preload != NULL);
    if (had_preload) unsetenv("LD_PRELOAD");

    int rc = system(command.c_str());

    if (had_preload) setenv("LD_PRELOAD", preload.c_str(), 1);

    return rc;
}

// Build the link map from the DWARF embedded in the stub init library, whose
// path is supplied via INIT_DWARF_PATH (set by the build harness, mirroring
// TEMPLATE_PATH / LINKSTUB_BUILD_CMD). DWARF is the sole source of types; the
// former signatures.lmap text fallback has been removed.
std::vector<FunLinkEntry> load_linkmap()
{
    std::vector<FunLinkEntry> linkmap;

    const char *dwarf_path = getenv("INIT_DWARF_PATH");
    if (dwarf_path != NULL && std::filesystem::exists(dwarf_path))
    {
        linkmap = load_dwarf_map(dwarf_path);
#ifdef DEBUG
        std::cout << "Loaded " << linkmap.size()
                  << " function(s) from DWARF in " << dwarf_path << std::endl;
#endif
    }

#ifdef PY_VERIFY
    for (auto &entry : linkmap)
        verify_function(entry);
#endif

    return linkmap;
}

void process_lmap(const std::string &path, const std::string &templatepath, const std::string& outpath)
{
#ifndef DEBUG
    auto linkmap = load_linkmap();
#else
    linkmap = load_linkmap();
    std::cout << "Linkmap:" << std::endl;
    for (const auto &entry : linkmap)
    {
        std::cout << entry.to_string() << std::endl;
    }
#endif

    // The trampolines name struct types by value (e.g. `struct Point`), so their
    // definitions must precede them. Recover them from the same stub-library
    // DWARF the link map came from.
    std::vector<StructDef> structs;
    const char *dwarf_path = getenv("INIT_DWARF_PATH");
    if (dwarf_path != NULL && std::filesystem::exists(dwarf_path))
        structs = load_dwarf_structs(dwarf_path);

#ifdef LINKPY_INJECT_STUBS
    // DRAFT alternative: emit a compile-time descriptor + a single consteval
    // block; linkpy::inject_stubs() expands it into the struct definitions and
    // trampolines via P3294 token-sequence injection at linkstub's compile time,
    // replacing the printed-source codegen below. Downstream is identical.
    (void) gen_code; // unused on this path
    write_linkstub_injected(templatepath, outpath, linkmap, structs);
#else
    std::ifstream in(templatepath);
    std::ofstream out(outpath);
    out << in.rdbuf();
    in.close();
    out << std::endl;

    // Emit the recovered struct definitions ahead of the generated trampolines.
    for (const auto &sd : structs)
    {
        out << "struct " << sd.name << "\n{\n";
        for (const auto &field : sd.fields)
            out << "    " << field.first << ' ' << field.second << ";\n";
        out << "};\n\n";
    }

    for (const auto &entry : linkmap)
    {
        gen_code(out, entry);
    }
    out.close();
#endif
}

int init()
{
#ifdef DEBUG
    std::cout << "Linker looked for libinit, initialising\n";
#endif
    lang_init();

#ifndef LINKPY_LANG_LUA
    PyObject *sysPath = PySys_GetObject("path");
    PyList_Append(sysPath, PyUnicode_FromString("."));
#endif
    std::string template_path;
    const char *template_env = getenv("TEMPLATE_PATH");

    if (template_env != NULL)
    {
        template_path = std::string(template_env);
    }  else {
        template_path = TEMPLATE_DEFAULT_PATH;
    }
    
    process_lmap(".", template_path, linkstub_workdir() + "/linkstub.cpp");

    if (compile_stubs() != 0)
    {
#ifdef DEBUG
        std::cerr << "Failed to compile linkstub" << std::endl;
#endif
        return 0;
    }
#ifdef DEBUG
    else
    {
        std::cout << "Compiled linkstub" << std::endl;
    }
#endif

    lang_finalize();
    return 1;
}

unsigned int la_version(unsigned int version)
{
    // Here, we have to generate entrypoints for the linker from the above linkstub template
    return LAV_CURRENT;
}

static bool is_init_lib(const char *name)
{
    return std::filesystem::path(name).filename() == "libinit.so";
}

char *la_objsearch(const char *name, uintptr_t *cookie, unsigned int flag)
{
    if (is_init_lib(name))
    {
        if (!INIT)
        {
            init();
            INIT = true;
        }
        std::string name_str = linkstub_workdir() + "/liblinkstub.so";
        char *new_name = new char[name_str.length() + 1];
        strcpy(new_name, name_str.c_str());
        return new_name;
    }
    return (char *)name;
}

#ifdef DEBUG
uintptr_t *liblinkstub = NULL;

std::vector<std::string> get_symbols_from_link_map(link_map *map)
{
    ElfW(Dyn) *dyn = map->l_ld;
    ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    size_t syment = sizeof(ElfW(Sym));
    size_t strsz = 0;

    for (; dyn->d_tag != DT_NULL; ++dyn)
    {
        switch (dyn->d_tag)
        {
        case DT_SYMTAB:
            symtab = reinterpret_cast<ElfW(Sym) *>(dyn->d_un.d_ptr);
            break;
        case DT_STRTAB:
            strtab = reinterpret_cast<const char *>(dyn->d_un.d_ptr);
            break;
        case DT_SYMENT:
            syment = dyn->d_un.d_val;
            break;
        case DT_STRSZ:
            strsz = dyn->d_un.d_val;
            break;
        }
    }

    if (!symtab || !strtab)
    {
        std::cerr << "Failed to locate symbol or string table.\n";
        return {};
    }

    std::vector<std::string> symbolnames;

    size_t i = 0;
    while (true)
    {
        ElfW(Sym) *sym = reinterpret_cast<ElfW(Sym) *>(
            reinterpret_cast<char *>(symtab) + i * syment);

        // Heuristic: stop if we go beyond the string table
        if (reinterpret_cast<const char *>(sym) >= strtab)
            break;

        const char *name = strtab + sym->st_name;
        if (sym->st_name != 0 && name && name[0] != '\0' && name[0] != '_')
        {
            symbolnames.push_back(name);
        }

        ++i;
    }
    return symbolnames;
}

std::vector<std::string> symbols;

unsigned int la_objopen(struct link_map *map, Lmid_t lmid, uintptr_t *cookie)
{
    if (strcmp(map->l_name, "") == 0)
    {
        return LA_FLG_BINDFROM;
    }
    if (strcmp(map->l_name + strlen(map->l_name) - strlen("liblinkstub.so"), "liblinkstub.so") == 0)
    {
        liblinkstub = cookie;

        std::cout << "Linker found liblinkstub! " << cookie << " Yay!\n";
        std::cout << "Liblinkstub at 0x" << std::hex << map->l_addr << " 0x" << std::hex << map->l_ld->d_un.d_val << std::endl;
        symbols = get_symbols_from_link_map(map);
        // print out symbols that are in symbols and in link map
        std::cout << "Foreign Functions in liblinkstub:" << std::endl;
        for (const auto &symbol : symbols)
        {
            auto fn = find_function(linkmap, symbol);
            if(fn != nullptr)
            {
                std::cout << LANG_NAME << " foreign function:" << fn->module_name <<"." << fn->name << std::endl;
            } else {
                std::cout << "Junk symbol: " << symbol << std::endl;
            }
        }

    
        return LA_FLG_BINDTO;
    }
    return 0;
}

unsigned int la_objclose(uintptr_t *cookie)
{
    if (cookie == liblinkstub)
    {
        std::cout << "Linker closing liblinkstub! Bye!\n";
        return 0;
    }
    return 0;
}

uintptr_t la_symbind64(Elf64_Sym *sym, unsigned int ndx, uintptr_t *refcook, uintptr_t *defcook, unsigned int *flags, const char *symname)
{
    auto fn = find_function(linkmap, symname);
    if ( fn != nullptr)
    {
        std::cout<< "Linker is binding function " << symname << ".  This is a " << LANG_NAME << " function!" << std::endl;
        std::cout << fn->to_string() << " ";
        if(!fn->is_loaded){
            std::cout << "Function " << fn->module_name << "." << fn->name << " is not loaded, should crash or abort" << std::endl;
        } else {
            std::cout << "Function " << fn->module_name << "." << fn->name << " is loaded, all good" << std::endl;
        }
    }

    return sym->st_value;
}
#endif