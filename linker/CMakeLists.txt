cmake_minimum_required(VERSION 3.22)

# The C++ side uses C++26 static reflection (P2996) to marshal aggregates, which
# currently requires g++-16's -freflection. Pin the C++ compiler accordingly
# (overridable via -DCMAKE_CXX_COMPILER). Must precede project().
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER g++-16)
endif()

project(linkpy C CXX)

# ---------------------------------------------------------------------------
# linkpy: builds `pystubgen` (an executable) and `audit.so` (an LD_AUDIT
# library), both embedding CPython. Ported from the old hand-written Makefile.
#
# Targets the Python given by -DPYTHON=<interpreter> (defaults to python3),
# querying it via sysconfig so it works against an in-tree (non-installed)
# CPython build such as the bundled contrib/cpython.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable static reflection (P2996) for all C++ translation units. PyObject_to_T
# in pycpputils.hpp uses reflection syntax, so every TU that includes it needs
# the flag, even if it doesn't instantiate the aggregate path.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-freflection>)

# Quiet by default. The auditor's and the linkstub's progress chatter (one
# line per call, among others) is for debugging LinkPy itself; a server
# hosting Python cannot have it. ON defines DEBUG for the tools here; the
# runtime linkstub gets it by adding -DDEBUG to LINKSTUB_BUILD_CMD.
option(LINKPY_DEBUG "Print LinkPy's own debug chatter (auditor, stub generator)" OFF)
if(LINKPY_DEBUG)
    add_compile_definitions(DEBUG)
endif()

set(PYTHON "python3" CACHE STRING "Python interpreter to build against")

# --- Query the interpreter for version + include/lib layout ----------------
execute_process(COMMAND "${PYTHON}" -c
    "import sys,sysconfig;\
print('%d.%d'%sys.version_info[:2]);\
print(sysconfig.get_path('include'));\
print(sysconfig.get_path('platinclude'));\
print(sysconfig.get_config_var('LIBPL') or '');\
print((sysconfig.get_config_var('LIBS') or '')+' '+(sysconfig.get_config_var('SYSLIBS') or ''))"
    OUTPUT_VARIABLE _py_info OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _py_rc)
if(NOT _py_rc EQUAL 0)
    message(FATAL_ERROR "linkpy: cannot query Python interpreter '${PYTHON}'")
endif()

string(REPLACE "\n" ";" _py_lines "${_py_info}")
list(GET _py_lines 0 PY_VERSION)        # e.g. 3.15
list(GET _py_lines 1 PY_INCLUDE)        # .../Include
list(GET _py_lines 2 PY_PLATINCLUDE)    # build root (holds pyconfig.h)
list(GET _py_lines 3 PY_LIBPL)          # install libdir (may be empty/non-existent for in-tree)
list(GET _py_lines 4 PY_SYSLIBS)        # extra system libs needed to embed

separate_arguments(PY_SYSLIBS)

# --- Locate libpython<ver>.a -----------------------------------------------
# For an in-tree build it sits in the build root (== platinclude); for an
# installed interpreter it is under LIBPL. Search both.
find_library(PYTHON_LIB
    NAMES "python${PY_VERSION}" "python${PY_VERSION}m"
    HINTS "${PY_PLATINCLUDE}" "${PY_LIBPL}"
    NO_DEFAULT_PATH)
if(NOT PYTHON_LIB)
    find_library(PYTHON_LIB NAMES "python${PY_VERSION}" "python${PY_VERSION}m")
endif()
if(NOT PYTHON_LIB)
    message(FATAL_ERROR "linkpy: libpython${PY_VERSION} not found (searched ${PY_PLATINCLUDE}, ${PY_LIBPL})")
endif()

message(STATUS "linkpy: Python ${PY_VERSION}; lib ${PYTHON_LIB}")

# --- libdw (elfutils): read function types from the stub library's DWARF ----
# The auditor recovers function signatures from the DWARF the compiler embeds in
# the stub init library. DWARF is the sole source of types (the former
# signatures.lmap text file has been removed).
find_library(DW_LIB NAMES dw)
if(NOT DW_LIB)
    message(FATAL_ERROR "linkpy: libdw not found (install libdw-dev / elfutils-devel)")
endif()
message(STATUS "linkpy: libdw ${DW_LIB}")

# --- liblua (optional): embed a Lua VM for the Lua marshalling path ----------
# When found, linkpy additionally builds audit_lua.so (the Lua LD_AUDIT library)
# and registers the Lua runtime tests; the Python path is unaffected when it is
# absent. PUC-Lua 5.3+ is required (the integer subtype maps to C int); the
# pkg-config module name varies by distro, so try a few in order.
find_package(PkgConfig QUIET)
set(LUA_FOUND FALSE)
if(PkgConfig_FOUND)
    foreach(_lua_pc lua5.5 lua5.4 lua-5.4 lua5.3 lua-5.3 lua)
        pkg_check_modules(LUA QUIET IMPORTED_TARGET ${_lua_pc})
        if(LUA_FOUND)
            break()
        endif()
    endforeach()
endif()
if(LUA_FOUND)
    # Flag strings for the runtime linkstub compile command (a shell string).
    string(JOIN " " LUA_CFLAGS_STR ${LUA_CFLAGS})
    string(JOIN " " LUA_LDFLAGS_STR ${LUA_LDFLAGS})
    message(STATUS "linkpy: Lua ${LUA_VERSION}; cflags '${LUA_CFLAGS_STR}' libs '${LUA_LDFLAGS_STR}'")
else()
    message(STATUS "linkpy: liblua not found; Lua auditor + tests disabled "
                   "(install liblua5.4-dev or set PKG_CONFIG_PATH)")
endif()

set(PY_INCLUDES "${PY_INCLUDE}" "${PY_PLATINCLUDE}")

# Project-wide shared code (pycpputils.hpp, tokenject.hpp) lives in the
# superproject's common/ folder, one level above this tree.
get_filename_component(PYCALLOCS_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common" ABSOLUTE)

# Output both artifacts into dist/ (the layout other tools expect).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dist")

# --- pystubgen executable ---------------------------------------------------
add_executable(pystubgen cpython/pystubgen.cpp common/linkmap.cpp)
target_include_directories(pystubgen PRIVATE common ${PY_INCLUDES})
target_link_libraries(pystubgen PRIVATE ${PYTHON_LIB} ${PY_SYSLIBS})

# --- heaphints executable ---------------------------------------------------
# Standalone Yukon heap-hints generator: reads the DWARF in a compiled stub
# library (the same source the auditor uses) and emits the functions whose
# pointer arguments escape into Python. Needs libdw but not CPython.
add_executable(heaphints common/heaphints.cpp common/linkmap.cpp common/dwarfmap.cpp)
target_include_directories(heaphints PRIVATE common)
target_link_libraries(heaphints PRIVATE ${DW_LIB})

# --- linkpychk executable ---------------------------------------------------
# The dwarfmap/linker-side front end for tools/pytypecheck.py: recovers the C
# function signatures from a stub library's DWARF (load_dwarf_map, the same
# source the auditor uses), serialises them with FunLinkEntry::to_string(), and
# pipes them into pytypecheck.py (--lmap -) to decide whether the loaded Python
# is a good candidate to bind. No signatures.lmap file is ever written. Needs
# libdw but not CPython. The interpreter and tool paths are baked in (env
# PYTHON / PYTYPECHECK_PY override at runtime).
add_executable(linkpychk common/pytypecheck_runner.cpp common/linkmap.cpp common/dwarfmap.cpp)
target_include_directories(linkpychk PRIVATE common)
target_link_libraries(linkpychk PRIVATE ${DW_LIB})
target_compile_definitions(linkpychk PRIVATE
    PYTHON_EXE="${PYTHON}"
    LINKCHK_TOOL="${CMAKE_CURRENT_SOURCE_DIR}/cpython/pytypecheck.py")

# --- lualinkchk executable --------------------------------------------------
# The Lua sibling of linkpychk: the same runner (src/pytypecheck_runner.cpp),
# but with tools/luatypecheck.py baked in. Recovers C signatures from a stub
# library's DWARF (the `.lua.` decl-file filter accepts them) and pipes them
# into the Lua type checker. Needs libdw but not any interpreter.
add_executable(lualinkchk common/pytypecheck_runner.cpp common/linkmap.cpp common/dwarfmap.cpp)
target_include_directories(lualinkchk PRIVATE common)
target_link_libraries(lualinkchk PRIVATE ${DW_LIB})
target_compile_definitions(lualinkchk PRIVATE
    PYTHON_EXE="${PYTHON}"
    LINKCHK_TOOL="${CMAKE_CURRENT_SOURCE_DIR}/lua/luatypecheck.py")

# DRAFT: synthesise the runtime linkstub's structs + trampolines via P3294
# token-sequence injection (codegen_inject.cpp + include/linkject.hpp) instead of
# std::format-ing C++ source in the auditor. Off by default; the printed-codegen
# path remains the default. Needs a P3294-capable g++-16 at runtime.
option(LINKPY_INJECT_STUBS
    "DRAFT: emit a descriptor + consteval injector for the linkstub instead of printed trampolines (P3294)" OFF)

# --- audit.so (LD_AUDIT library) -------------------------------------------
set(_audit_sources common/auditor.cpp common/hostgen.cpp cpython/pycpputils.cpp common/linkmap.cpp common/dwarfmap.cpp common/linkstub_cache.cpp)
if(LINKPY_INJECT_STUBS)
    list(APPEND _audit_sources common/codegen_inject.cpp)
endif()
add_library(audit MODULE ${_audit_sources})
target_include_directories(audit PRIVATE common cpython "${PYCALLOCS_COMMON_DIR}" ${PY_INCLUDES})
target_link_libraries(audit PRIVATE ${PYTHON_LIB} ${PY_SYSLIBS} ${DW_LIB})
if(LINKPY_INJECT_STUBS)
    target_compile_definitions(audit PRIVATE LINKPY_INJECT_STUBS)
endif()
set_target_properties(audit PROPERTIES PREFIX "" OUTPUT_NAME "audit" SUFFIX ".so")

# --- audit_lua.so (LD_AUDIT library, Lua) -----------------------------------
# The Lua sibling of audit.so: the SAME auditor source, compiled with
# LINKPY_LANG_LUA so init/verify/finalize embed Lua (luacpputils.cpp) instead of
# CPython. The trampoline codegen is identical. Only built when liblua is found.
if(LUA_FOUND)
    add_library(audit_lua MODULE
        common/auditor.cpp common/hostgen.cpp lua/luacpputils.cpp common/linkmap.cpp common/dwarfmap.cpp common/linkstub_cache.cpp)
    target_include_directories(audit_lua PRIVATE common lua)
    target_link_libraries(audit_lua PRIVATE PkgConfig::LUA ${DW_LIB})
    target_compile_definitions(audit_lua PRIVATE LINKPY_LANG_LUA)
    set_target_properties(audit_lua PROPERTIES
        PREFIX "" OUTPUT_NAME "audit_lua" SUFFIX ".so")
endif()

# --- tests ------------------------------------------------------------------
# The suite lives in the project-wide tests/ folder (tests/linker/): the shared,
# language-parameterized helpers in tests.cmake, one fixture tree per VM.
# LINKPY_ROOT anchors the helpers' paths (CMAKE_SOURCE_DIR would point at the
# superproject when this tree is consumed via add_subdirectory). Skipped when
# the tests folder is absent (linker tree used standalone).
enable_testing()
set(LINKPY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(LINKPY_TESTS_DIR "${LINKPY_ROOT}/../tests/linker" ABSOLUTE)
if(EXISTS "${LINKPY_TESTS_DIR}/tests.cmake")
    include("${LINKPY_TESTS_DIR}/tests.cmake")
    add_subdirectory("${LINKPY_TESTS_DIR}/cpython" tests/cpython)
    if(LUA_FOUND)
        add_subdirectory("${LINKPY_TESTS_DIR}/lua" tests/lua)
        # Cross-language (VM host -> VM callee) tests need Lua on one side or
        # the other in every combination, so they gate on it as a whole.
        add_subdirectory("${LINKPY_TESTS_DIR}/cross" tests/cross)
    else()
        message(STATUS "linkpy: Lua test suite disabled (liblua not found)")
    endif()
else()
    message(STATUS "linkpy: test suite not found at ${LINKPY_TESTS_DIR}; skipping")
endif()
