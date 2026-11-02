// linkchk -- the dwarfmap/linker-side front end for the typecheck tools. Built
// twice: `linkpychk` drives tools/pytypecheck.py, `lualinkchk` drives
// tools/luatypecheck.py (the tool path is baked in by CMake via LINKCHK_TOOL).
//
// Given a compiled stub library (one carrying DWARF, i.e. INIT_DWARF_PATH) and
// the directory of scripting sources it is meant to bind, decide whether the
// module is a good candidate. It recovers the C function signatures from DWARF
// with load_dwarf_map() -- the very same reader the auditor uses at link time --
// serialises each entry with FunLinkEntry::to_string(), and pipes the result
// into the typecheck tool (--lmap -), which checks the signatures against the
// modules' type annotations. The colon-delimited form is an in-memory
// interchange only: no signatures.lmap file is written or read.
//
//   linkpychk|lualinkchk <init_dwarf.so> <src_dir> [extra typecheck args...]
//
// Exit code is the tool's own: 0 = good candidate, 1 = incompatible,
// 2 = usage/parse error. Extra args are forwarded verbatim (e.g. --strict,
// --json, -q), so the same verdict the auditor would reach can be obtained in
// machine-readable form for the linker extension.
//
// Only function signatures travel through to_string(); struct *layouts* are not
// part of that serialisation, so struct-field checks (available in header mode)
// are not performed on this path.

#include "dwarfmap.hpp"
#include "linkmap.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

// The typecheck tool this runner drives is baked in by CMake (pytypecheck.py for
// linkpychk, luatypecheck.py for lualinkchk). Both speak the same --lmap stdin
// protocol, so the runner itself is language-agnostic.
#ifndef LINKCHK_TOOL
#define LINKCHK_TOOL "tools/pytypecheck.py"
#endif
#ifndef PYTHON_EXE
#define PYTHON_EXE "python3"
#endif

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0]
                  << " <init_dwarf.so> <src_dir> [extra pytypecheck args...]\n";
        return 2;
    }
    const std::string dwarf_path = argv[1];
    const char *src_dir = argv[2];

    std::vector<FunLinkEntry> linkmap = load_dwarf_map(dwarf_path);
    if (linkmap.empty())
    {
        std::cerr << argv[0] << ": no stub functions recovered from DWARF in "
                  << dwarf_path << " (cannot assess candidacy)\n";
        return 2;
    }

    // The interpreter and tool location are baked in by CMake; allow a runtime
    // override so the runner is relocatable.
    const char *py = getenv("PYTHON");
    if (py == nullptr || *py == '\0') py = PYTHON_EXE;
    const char *tool = getenv("LINKCHK_TOOL");
    if (tool == nullptr || *tool == '\0') tool = LINKCHK_TOOL;

    // argv for: <py> <tool> --src <src_dir> --lmap - [extra...]
    std::vector<char *> args;
    args.push_back(const_cast<char *>(py));
    args.push_back(const_cast<char *>(tool));
    args.push_back(const_cast<char *>("--src"));
    args.push_back(const_cast<char *>(src_dir));
    args.push_back(const_cast<char *>("--lmap"));
    args.push_back(const_cast<char *>("-"));
    for (int i = 3; i < argc; ++i)
        args.push_back(argv[i]);
    args.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0)
    {
        perror("linkchk: pipe");
        return 2;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("linkchk: fork");
        return 2;
    }
    if (pid == 0)
    {
        // Child: read the signatures from the pipe as stdin, then exec.
        if (dup2(fds[0], STDIN_FILENO) < 0)
            _exit(127);
        close(fds[0]);
        close(fds[1]);
        execvp(py, args.data());
        perror("linkchk: execvp");
        _exit(127);
    }

    // Parent: stream the DWARF-recovered signatures to the child's stdin.
    close(fds[0]);
    std::string blob;
    for (const auto &e : linkmap)
    {
        blob += e.to_string();
        blob += '\n';
    }
    std::size_t off = 0;
    while (off < blob.size())
    {
        ssize_t w = write(fds[1], blob.data() + off, blob.size() - off);
        if (w < 0)
        {
            if (errno == EINTR) continue;
            perror("linkchk: write");
            break;
        }
        off += static_cast<std::size_t>(w);
    }
    close(fds[1]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
    {
        perror("linkchk: waitpid");
        return 2;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 2;
}
