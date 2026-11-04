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
#include "linkstub_cache.hpp"
#include <dlfcn.h>
#include <sys/wait.h>
#include <malloc.h>
#include "hostgen.hpp"
#ifdef LINKPY_INJECT_STUBS
#include "codegen_inject.hpp" // DRAFT P3294 token-injection codegen back end
#endif

#define PY_VERIFY
#define TEMPLATE_DEFAULT_PATH "./template.cpp"
// #define ABORT_ON_PY_NOT_FOUND false

#ifdef LINKPY_LANG_LUA
static void lang_init() { linkpy::lua::init_lua(); }
static void lang_finalize() { linkpy::lua::close_lua(); }
[[maybe_unused]] static const char *LANG_NAME = "Lua";
// Namespace qualifier gen_code emits before run_function[_void], selecting the
// callee-language marshaller in the (possibly two-marshaller) linkstub TU.
static const char *CALLEE_NS = "linkpy::lua";
#else
static void lang_init() { linkpy::py::init_py(); }
static void lang_finalize() { Py_Finalize(); }
[[maybe_unused]] static const char *LANG_NAME = "Python";
static const char *CALLEE_NS = "linkpy::py";
#endif

// The stub decl-file language marker this auditor's callee corresponds to
// (FunLinkEntry::lang values are "py"/"lua"/"").
#ifdef LINKPY_LANG_LUA
static const char *CALLEE_LANG = "lua";
#else
static const char *CALLEE_LANG = "py";
#endif

// Host language of the audited process (env LINKPY_HOST_LANG): who *calls* the
// foreign functions. Default "c" keeps today's plain extern "C" surface;
// "python"/"lua" additionally make process_lmap emit host-language bindings
// (wrapper functions + module registration) into the linkstub so the host VM
// can call the callee VM through the C-struct ABI pivot.
enum class HostLang { C, Python, Lua };

// Opt-in for by-reference CROSS-language arguments (env LINKPY_CROSS_BYREF):
// pointer parameters cross as proxy handoffs -- the host wrapper extracts the
// raw address from the host VM's proxy, and the callee marshaller re-proxies
// it -- instead of being rejected. Needs the liballocs runtime and both proxy
// extensions at runtime, hence opt-in. Pointer returns stay rejected.
static bool cross_byref_enabled()
{
    const char *env = linkpy_getenv("LINKPY_CROSS_BYREF");
    return env != NULL && *env != '\0' && strcmp(env, "0") != 0;
}

static HostLang host_lang()
{
    const char *env = linkpy_getenv("LINKPY_HOST_LANG");
    if (env == NULL || *env == '\0' || strcmp(env, "c") == 0)
        return HostLang::C;
    if (strcmp(env, "python") == 0)
        return HostLang::Python;
    if (strcmp(env, "lua") == 0)
        return HostLang::Lua;
    std::cerr << "linkpy: ERROR: unknown LINKPY_HOST_LANG '" << env
              << "' (expected c, python or lua)" << std::endl;
    // Controlled fatal diagnostics exit() rather than abort(): the exit code is
    // deterministic and ctest's PASS_REGULAR_EXPRESSION can assert on the
    // message (a SIGABRT would be reported as an exception instead).
    exit(1);
}

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
        const char *tmpdir = linkpy_getenv("TMPDIR");
        std::filesystem::path base = (tmpdir != nullptr && *tmpdir != '\0')
                                         ? std::filesystem::path(tmpdir)
                                         : std::filesystem::temp_directory_path(ec);
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

    // The trampoline forwards to run_function<T> (or run_function_void for void)
    // with its call-site record: one static per trampoline holding the module
    // and function names it was generated with, which the marshaller fills
    // with what it needs to resolve the callable cheaply on every call.
    // Build the call expression in a single buffer, appending in place.
    std::string call;
    auto it = std::back_inserter(call);
    if (fun.return_type == "void")
        std::format_to(it, R"({}::run_function_void(site, {})", CALLEE_NS, paramc);
    else
        std::format_to(it, R"(return {}::run_function<{}>(site, {})", CALLEE_NS, fun.return_type, paramc);
    if (paramc > 0)
        std::format_to(it, ", {}", fun.params_args());
    call += ')';

    out << std::format(R"(extern "C" {} {}({})
{{
#ifdef DEBUG
    printf("Hello from {}!\n");
#endif
    static {}::CallSite site{{"{}", "{}"}};
    {};
}}

)",
                       fun.return_type, fun.name, fun.params_decl(), fun.name,
                       CALLEE_NS, fun.module_name, fun.name, call);
}

void verify_function(FunLinkEntry &fun)
{
    // Search the embedded interpreter for fun.name in module fun.module_name; if
    // found, set fun.is_loaded so the link-time report can flag a stub whose
    // implementation is missing. The actual marshalling happens later in the
    // JIT-compiled linkstub, in a separate VM.
#ifdef LINKPY_LANG_LUA
    if (linkpy::lua::function_exists(linkpy::lua::lua_state(), fun.module_name, fun.name))
        fun.is_loaded = true;
#ifdef ABORT_ON_PY_NOT_FOUND
    else
        abort();
#endif
#else
    PyObject *pModule = linkpy::py::import_module(fun.module_name.c_str());
    if (pModule == NULL)
    {
        PyErr_Print();
#ifdef ABORT_ON_PY_NOT_FOUND
        abort();
#endif
        return;
    }
    PyObject *pFunc = linkpy::py::import_function(pModule, fun.name.c_str());
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
    const char *build_cmd = linkpy_getenv("LINKSTUB_BUILD_CMD");
    std::string command = "cd '" + linkstub_workdir() + "' && ";
    command += (build_cmd != NULL) ? build_cmd : "make liblinkstub";

    // Quiet by default: the compiler's chatter goes away, its diagnostics
    // (stderr) stay, because a failed JIT compile must be explainable.
#ifndef DEBUG
    command += " >/dev/null";
#endif

    // The build command shells out to a compiler, which must see neither the
    // auditor (LD_AUDIT) nor a liballocs LD_PRELOAD (its syscall trapping
    // crashes cc1plus): the child gets the captured environment without them.
    std::vector<std::string> env = linkpy_child_environment({"LD_AUDIT", "LD_DEBUG", "LD_PRELOAD"});
    std::vector<char *> envp;
    for (auto &entry : env)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        const char *argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
        execve("/bin/sh", const_cast<char **>(argv), envp.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Build the link map from the DWARF embedded in the stub init library, whose
// path is supplied via INIT_DWARF_PATH (set by the build harness, mirroring
// TEMPLATE_PATH / LINKSTUB_BUILD_CMD). DWARF is the sole source of types; the
// former signatures.lmap text fallback has been removed.
std::vector<FunLinkEntry> load_linkmap()
{
    std::vector<FunLinkEntry> linkmap;

    const char *dwarf_path = linkpy_getenv("INIT_DWARF_PATH");
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

    // The linkstub is single-callee-language: every DWARF entry must carry this
    // auditor's marker. A mixed init library would need per-entry marshaller
    // dispatch, which v1 does not generate.
    for (const auto &entry : linkmap)
    {
        if (!entry.lang.empty() && entry.lang != CALLEE_LANG)
        {
            std::cerr << "linkpy: ERROR: " << entry.module_name << "." << entry.name
                      << " comes from a ." << entry.lang << ". stub but this auditor embeds "
                      << LANG_NAME << "; mixed-language init libraries are not supported"
                      << std::endl;
            exit(1);
        }
    }

    const HostLang host = host_lang();
    if (host != HostLang::C)
    {
#ifdef LINKPY_LANG_LUA
        const bool same_lang = (host == HostLang::Lua);
#else
        const bool same_lang = (host == HostLang::Python);
#endif
        if (same_lang)
        {
            std::cerr << "linkpy: ERROR: LINKPY_HOST_LANG matches the callee language ("
                      << LANG_NAME << "); a same-language linkstub would alias two "
                         "interpreters in one process" << std::endl;
            exit(1);
        }
        // Default crossing is by value only (pointer params reject); with
        // LINKPY_CROSS_BYREF pointer params cross as proxy handoffs instead.
        // Reject offending signatures up front -- all of them, so the user
        // sees the full list before the exit.
        const bool allow_byref = cross_byref_enabled();
        bool rejected = false;
        for (const auto &entry : linkmap)
        {
            std::string reason = cross_reject_reason(entry, allow_byref);
            if (!reason.empty())
            {
                std::cerr << "linkpy: ERROR: cross-language call "
                          << entry.module_name << "." << entry.name << ": " << reason
                          << "; by-reference does not cross language boundaries (v1) -- "
                             "only scalars, char* strings and by-value structs may cross"
                          << std::endl;
                rejected = true;
            }
        }
        if (rejected)
            exit(1);
    }

    // The trampolines name struct types by value (e.g. `struct Point`), so their
    // definitions must precede them. Recover them from the same stub-library
    // DWARF the link map came from.
    std::vector<StructDef> structs;
    const char *dwarf_path = linkpy_getenv("INIT_DWARF_PATH");
    if (dwarf_path != NULL && std::filesystem::exists(dwarf_path))
        structs = load_dwarf_structs(dwarf_path);

#ifdef LINKPY_INJECT_STUBS
    // DRAFT alternative: emit a compile-time descriptor + a single consteval
    // block; linkpy::inject_stubs() expands it into the struct definitions and
    // trampolines via P3294 token-sequence injection at linkstub's compile time,
    // replacing the printed-source codegen below. Downstream is identical.
    if (host != HostLang::C)
    {
        std::cerr << "linkpy: ERROR: LINKPY_INJECT_STUBS does not support "
                     "host-language bindings (LINKPY_HOST_LANG != c)" << std::endl;
        exit(1);
    }
    (void) gen_code; // unused on this path
    write_linkstub_injected(templatepath, outpath, linkmap, structs);
#else
    std::ifstream in(templatepath);
    std::ofstream out(outpath);
    out << in.rdbuf();
    in.close();
    out << std::endl;

    // When the host is a VM, splice in the host-language prologue right after
    // the callee template, so both marshallers are in scope before any
    // generated code. The prologue must not start a VM or define constructors
    // (see template_host_py.cpp / template_host_lua.cpp).
    if (host != HostLang::C)
    {
        const char *host_template = linkpy_getenv("HOST_TEMPLATE_PATH");
        if (host_template == NULL || !std::filesystem::exists(host_template))
        {
            std::cerr << "linkpy: ERROR: LINKPY_HOST_LANG is set but "
                         "HOST_TEMPLATE_PATH does not name a readable file"
                      << std::endl;
            exit(1);
        }
        std::ifstream hin(host_template);
        out << hin.rdbuf();
        out << std::endl;
    }

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

    // Host-language wrappers + registration come last: they call the extern
    // "C" trampolines above by name.
    if (host == HostLang::Python)
        gen_host_python(out, linkmap);
    else if (host == HostLang::Lua)
        gen_host_lua(out, linkmap);

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
    // This interpreter's libc may no longer see PYTHONPATH (see la_version);
    // give it the captured one, in order, ahead of the defaults.
    const char *pythonpath = linkpy_getenv("PYTHONPATH");
    if (pythonpath != nullptr)
    {
        std::string entries(pythonpath);
        std::vector<std::string> list;
        std::size_t start = 0;
        while (start <= entries.size())
        {
            std::size_t colon = entries.find(':', start);
            if (colon == std::string::npos)
                colon = entries.size();
            if (colon > start)
                list.push_back(entries.substr(start, colon - start));
            start = colon + 1;
        }
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            PyList_Insert(sysPath, 0, PyUnicode_FromString(it->c_str()));
    }
#endif
    std::string template_path;
    const char *template_env = linkpy_getenv("TEMPLATE_PATH");

    if (template_env != NULL)
    {
        template_path = std::string(template_env);
    }  else {
        template_path = TEMPLATE_DEFAULT_PATH;
    }
    
    process_lmap(".", template_path, linkstub_workdir() + "/linkstub.cpp");

    if (compile_stubs() != 0)
    {
        std::cerr << "linkpy: failed to compile the linkstub" << std::endl;
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

static std::string g_linkstub_path;

// Everything the generated linkstub depends on, for the cache key: the
// auditor itself (its code generator), the VM template(s), the stub library's
// DWARF (signatures and struct layouts) and the build command.
static std::vector<std::string> linkstub_cache_material()
{
    std::vector<std::string> m;
    m.push_back("linkpy-linkstub-cache-1");
    m.push_back(LANG_NAME);
    // The auditor itself (its code generator) by its build-id, which the
    // linker derived from its contents; hashing the file instead was most of a
    // cache-hit start once the binary carried debug info.
    const std::string build_id = linkstub_cache_build_id(reinterpret_cast<void *>(&init));
    if (!build_id.empty())
        m.push_back("auditor-build-id:" + build_id);
    else
    {
        Dl_info self;
        if (dladdr(reinterpret_cast<void *>(&init), &self) != 0 && self.dli_fname != nullptr)
            m.push_back(std::string("@") + self.dli_fname);
    }
    const char *tmpl = linkpy_getenv("TEMPLATE_PATH");
    m.push_back(std::string("@") + (tmpl != nullptr ? tmpl : TEMPLATE_DEFAULT_PATH));
    const char *host = linkpy_getenv("LINKPY_HOST_LANG");
    m.push_back(host != nullptr ? host : "");
    const char *host_tmpl = linkpy_getenv("HOST_TEMPLATE_PATH");
    m.push_back(host_tmpl != nullptr ? std::string("@") + host_tmpl : "");
    const char *dwarf = linkpy_getenv("INIT_DWARF_PATH");
    m.push_back(dwarf != nullptr ? std::string("@") + dwarf : "");
    const char *cmd = linkpy_getenv("LINKSTUB_BUILD_CMD");
    m.push_back(cmd != nullptr ? cmd : "make liblinkstub");
    return m;
}

// The linkstub to hand the loader: a cached one when this exact generation
// has been compiled before on this machine, otherwise a fresh JIT compile
// (init), stored for the next process.
static std::string linkstub_locate()
{
    const bool caching = !linkstub_cache_dir().empty();
    const std::string key = caching ? linkstub_cache_key(linkstub_cache_material()) : "";
    std::string path = caching ? linkstub_cache_lookup(key) : "";
    if (!path.empty())
    {
#ifdef DEBUG
        std::cout << "linkpy: linkstub cache hit " << path << std::endl;
#endif
        return path;
    }
    const std::string built = linkstub_workdir() + "/liblinkstub.so";
    if (init() == 0)
        return built; // missing; the loader reports it, as before
    path = caching ? linkstub_cache_store(key, built, linkstub_workdir() + "/linkstub.cpp") : "";
    return path.empty() ? built : path;
}

extern char **environ;

unsigned int la_version(unsigned int version)
{
    // la_version runs when the process starts, while the environment block is
    // still intact: everything the auditor reads from the environment later
    // comes from this snapshot (linkpy_getenv), see linkstub_cache.hpp.
    linkpy_capture_environment(environ);

    // The auditor has its own copy of libc, whose malloc shares the process's
    // brk heap with the host's malloc. A host runtime that indexes that heap
    // (liballocs) assumes every chunk in it is its own, so the auditor's arena
    // must never grow once the host is running: reserve its growth now, in
    // one piece, before anything of the host has started.
    mallopt(M_TOP_PAD, 256 * 1024 * 1024);
    free(malloc(16));
    mallopt(M_TOP_PAD, 128 * 1024);
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
            g_linkstub_path = linkstub_locate();
            INIT = true;
        }
        char *new_name = new char[g_linkstub_path.length() + 1];
        strcpy(new_name, g_linkstub_path.c_str());
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