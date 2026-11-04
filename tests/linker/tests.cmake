# ---------------------------------------------------------------------------
# linkpy test helpers, include()d from linker/CMakeLists.txt. The suite is
# mirrored across two languages: tests/linker/cpython/ and tests/linker/lua/
# hold the *same* set of fixtures (simple_divide, struct, struct_arg,
# generated_header, typecheck/{good,bad}), and register the *same* tests through
# the language-parameterized helpers defined here. Only the language seams differ
# (source extension, stub-header suffix, header generator, marshalling library,
# VM template, and the LD_AUDIT auditor). Registered test names are prefixed with
# the language: python_simple_divide, lua_simple_divide, ...
#
# Each fixture directory holds:
#   include/*.<lang>.h   stub headers fed to pystubgen (omitted with GENERATE_HEADERS)
#   src/*.<lang>         scripting sources
#   src/main.c           driver that links the generated init library
#
# Both languages carry a by-reference (ptr_arg) test -- pointer arguments
# escape into the VM via a proxy (pycallocs for Python, luaproxy for Lua) and
# writes propagate back to C; they need the liballocs runtime and are gated
# behind LINKPY_BYREF_TESTS. The heap-hints tests remain Python-only.
#
# Paths are anchored on LINKPY_ROOT / LINKPY_TESTS_DIR (set by the includer),
# not CMAKE_SOURCE_DIR: the latter points at the superproject when the linker
# tree is consumed via add_subdirectory.
# ---------------------------------------------------------------------------

# Compiled linkstubs are cached by content hash (linkstub_cache.cpp); the tests
# keep theirs in the build tree rather than the user's ~/.cache.
set(LINKPY_TEMPLATE "${LINKPY_ROOT}/cpython/template.cpp")
set(LINKPY_LUA_TEMPLATE "${LINKPY_ROOT}/lua/template_lua.cpp")

# Shared, language-agnostic lmap-mode check (driven with -DTOOL/-DSRC).
set(LINKPY_LMAP_CHECK "${LINKPY_TESTS_DIR}/check_typecheck_lmap.cmake")

# The linkstub cache key covers the auditor, the templates, the stub DWARF and
# the build command, but not the marshaller sources that command compiles. The
# tests put a hash of those sources into the command (a define the code never
# reads), so an edited marshaller never runs against a stale cached linkstub;
# CMake re-runs when the files change, so the hash follows every edit.
function(linkpy_marshaller_hash out)
    set(_acc "")
    foreach(f ${ARGN})
        file(SHA256 "${f}" _h)
        string(APPEND _acc "${_h}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
    endforeach()
    string(SHA256 _key "${_acc}")
    string(SUBSTRING "${_key}" 0 16 _key)
    set(${out} "${_key}" PARENT_SCOPE)
endfunction()

# Extra system libs needed to embed CPython, as a space-separated string for use
# inside the runtime compile command.
string(JOIN " " PY_SYSLIBS_STR ${PY_SYSLIBS})

# ---------------------------------------------------------------------------
# add_linkpy_test(<name> LANG <python|lua> [GENERATE_HEADERS] [DLOPEN])
#
# Build a stub library from <name>/include/*.<lang>.h (or, with GENERATE_HEADERS,
# derive the headers from the sources' type hints via the language's *hgen tool),
# build <name>/src/main.c against it, and register a ctest that runs main under
# the language's LD_AUDIT auditor so the JIT-compiled trampolines marshal C<->VM.
# The body is identical for both languages; only the seams set below differ.
#
# With DLOPEN, main does not link the init library at load time; it resolves the
# foreign functions at runtime with dlopen("libinit.so") + dlsym, which the
# auditor redirects to liblinkstub.so the same way it redirects a load-time
# dependency. Used by the dlopen_* fixtures to cover the runtime-loading path.
# ---------------------------------------------------------------------------
function(add_linkpy_test name)
    cmake_parse_arguments(ARG "GENERATE_HEADERS;DLOPEN" "LANG" "" ${ARGN})
    if(NOT ARG_LANG)
        set(ARG_LANG python)
    endif()

    if(ARG_LANG STREQUAL lua)
        set(ext "lua")
        set(hgen "${LINKPY_ROOT}/lua/luahgen.py")
        set(audit_lib audit_lua)
        set(template "${LINKPY_LUA_TEMPLATE}")
        set(marshal_src "${LINKPY_ROOT}/lua/luacpputils.cpp")
        set(marshal_hdr "${LINKPY_ROOT}/lua/luacpputils.hpp")
        set(lang_dir "${LINKPY_ROOT}/lua")
        set(lang_cflags "${LUA_CFLAGS_STR}")
        set(lang_libs "${LUA_LDFLAGS_STR}")
    else()
        set(ext "py")
        set(hgen "${LINKPY_ROOT}/cpython/pyhgen.py")
        set(audit_lib audit)
        set(template "${LINKPY_TEMPLATE}")
        set(marshal_src "${LINKPY_ROOT}/cpython/pycpputils.cpp")
        set(marshal_hdr "${LINKPY_ROOT}/../common/pycpputils.hpp")
        set(lang_dir "${LINKPY_ROOT}/cpython")
        set(lang_cflags "-I${PY_INCLUDE} -I${PY_PLATINCLUDE}")
        set(lang_libs "${PYTHON_LIB} ${PY_SYSLIBS_STR}")
    endif()

    set(test_name "${ARG_LANG}_${name}")
    set(test_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    set(gen_dir "${work_dir}/gen")

    # main runs here. The auditor now generates and compiles its link stub in a
    # private temp dir (not ./dist under the working dir), so we only need the
    # working dir itself to exist for the source symlink below.
    file(MAKE_DIRECTORY "${work_dir}")

    # The embedded interpreter searches ./src (pycpputils.cpp adds it to sys.path;
    # luacpputils.cpp adds it to package.path), so link the test's module dir in.
    file(CREATE_LINK "${test_dir}/src" "${work_dir}/src" SYMBOLIC)

    if(ARG_GENERATE_HEADERS)
        # One <module>.<lang>.h per <module>.<lang>, derived from its type hints.
        set(include_dir "${work_dir}/include")
        file(GLOB sources CONFIGURE_DEPENDS "${test_dir}/src/*.${ext}")
        set(headers "")
        foreach(s ${sources})
            get_filename_component(mod "${s}" NAME_WE)   # e.g. mymod
            list(APPEND headers "${include_dir}/${mod}.${ext}.h")
        endforeach()
        add_custom_command(
            OUTPUT ${headers}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${include_dir}"
            COMMAND ${PYTHON} "${hgen}" --src "${test_dir}/src" --out "${include_dir}"
            DEPENDS ${sources} "${hgen}" "${LINKPY_ROOT}/common/linktypes.py"
            COMMENT "linkpy ${ARG_LANG} test ${name}: generating .${ext}.h from type hints"
            VERBATIM)
    else()
        set(include_dir "${test_dir}/include")
        file(GLOB headers CONFIGURE_DEPENDS "${include_dir}/*.${ext}.h")
    endif()

    set(gen_sources "")
    foreach(header ${headers})
        get_filename_component(base "${header}" NAME)        # e.g. mytest.py.h
        string(REGEX REPLACE "\\.h$" ".c" gen_c "${base}")   # -> mytest.py.c
        list(APPEND gen_sources "${gen_dir}/${gen_c}")
    endforeach()

    # pystubgen emits the *.<lang>.c stubs and heap_hints.txt; declare the latter
    # so the build graph tracks it. DWARF is the sole source of types.
    add_custom_command(
        OUTPUT ${gen_sources} "${gen_dir}/heap_hints.txt"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
        COMMAND $<TARGET_FILE:pystubgen> "${include_dir}/" "${gen_dir}/"
        DEPENDS pystubgen ${headers}
        COMMENT "linkpy ${ARG_LANG} test ${name}: generating stubs"
        VERBATIM)

    add_library(${test_name}_init SHARED ${gen_sources})
    # Each test's init library is named libinit.so but lives in its own directory
    # (they share that SONAME). Compile with -g so the auditor recovers the
    # function signatures from the embedded DWARF (see INIT_DWARF_PATH below),
    # and at -O0 whatever the build type: the DWARF is the fixture, and the
    # optimiser rewrites it.
    set_target_properties(${test_name}_init PROPERTIES
        OUTPUT_NAME init
        LIBRARY_OUTPUT_DIRECTORY "${work_dir}")
    target_compile_options(${test_name}_init PRIVATE -g -O0)
    target_include_directories(${test_name}_init PRIVATE "${include_dir}")

    add_executable(${test_name}_main "${test_dir}/src/main.c")
    target_include_directories(${test_name}_main PRIVATE "${include_dir}")
    if(ARG_DLOPEN)
        # dlopen path: main resolves the foreign functions at RUNTIME via
        # dlopen("libinit.so") + dlsym, which the auditor's la_objsearch redirects
        # to the generated liblinkstub.so. So main must NOT link the init library
        # at load time -- it only needs libdl. The init library is still built (the
        # auditor reads its DWARF via INIT_DWARF_PATH), so depend on it to order
        # the build without creating a DT_NEEDED on it.
        target_link_libraries(${test_name}_main PRIVATE ${CMAKE_DL_LIBS})
        add_dependencies(${test_name}_main ${test_name}_init)
    else()
        target_link_libraries(${test_name}_main PRIVATE ${test_name}_init)
    endif()
    if(ARG_GENERATE_HEADERS)
        add_custom_target(${test_name}_headers DEPENDS ${headers})
        add_dependencies(${test_name}_main ${test_name}_headers)
    endif()

    # Command the audit library uses to JIT-compile linkstub.cpp into
    # liblinkstub.so, against the language's marshalling source + libs. Both are
    # named by bare filename: the auditor cd's into its private temp work dir
    # (where it wrote linkstub.cpp) before running this, so the output lands there.
    linkpy_marshaller_hash(marshaller_sha "${marshal_src}" "${marshal_hdr}")
    set(linkstub_cmd "${CMAKE_CXX_COMPILER} -std=c++26 -freflection -g -shared -fPIC \
-o liblinkstub.so linkstub.cpp ${marshal_src} \
-I${LINKPY_ROOT}/common -I${LINKPY_ROOT}/../common -I${lang_dir} -I${include_dir} ${lang_cflags} ${lang_libs} \
-DLINKPY_MARSHALLER=${marshaller_sha}")

    add_test(NAME ${test_name} COMMAND ${test_name}_main)
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${work_dir}"
        ENVIRONMENT "LINKPY_CACHE_DIR=${CMAKE_BINARY_DIR}/linkpy-cache;LD_AUDIT=$<TARGET_FILE:${audit_lib}>;TEMPLATE_PATH=${template};LINKSTUB_BUILD_CMD=${linkstub_cmd};INIT_DWARF_PATH=$<TARGET_FILE:${test_name}_init>")
endfunction()

# ---------------------------------------------------------------------------
# add_linkpy_typecheck_tests(LANG <python|lua>)
#
# Register the four typecheck tests for a language against its typecheck/ fixture:
#   <lang>_typecheck_good   — fully annotated, passes --strict.
#   <lang>_typecheck_lmap   — same fixture via the colon-delimited signatures on
#                             stdin (the form the *linkchk runner feeds in).
#   <lang>_typecheck_bad    — clashes + a missing function; expected to fail.
#   <lang>_typecheck_runner — the C++ runner end to end: recover signatures from
#                             the struct_arg stub's DWARF and pipe to the checker.
# Identical across languages bar the tool, the runner, and the fixture dir.
# ---------------------------------------------------------------------------
function(add_linkpy_typecheck_tests)
    cmake_parse_arguments(ARG "" "LANG" "" ${ARGN})
    if(ARG_LANG STREQUAL lua)
        set(tool "${LINKPY_ROOT}/lua/luatypecheck.py")
        set(runner lualinkchk)
    else()
        set(tool "${LINKPY_ROOT}/cpython/pytypecheck.py")
        set(runner linkpychk)
    endif()

    add_test(NAME ${ARG_LANG}_typecheck_good COMMAND ${PYTHON} "${tool}"
        --src "${CMAKE_CURRENT_SOURCE_DIR}/typecheck/good/src"
        --include "${CMAKE_CURRENT_SOURCE_DIR}/typecheck/good/include" --strict)

    # The lmap mode reads stdin, which add_test cannot redirect, so drive it
    # through a shell (cmake -P) that feeds the DWARF-shaped signatures in.
    add_test(NAME ${ARG_LANG}_typecheck_lmap COMMAND ${CMAKE_COMMAND}
        -DPYTHON=${PYTHON} -DTOOL=${tool}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/typecheck/good/src
        -P ${LINKPY_LMAP_CHECK})

    add_test(NAME ${ARG_LANG}_typecheck_bad COMMAND ${PYTHON} "${tool}"
        --src "${CMAKE_CURRENT_SOURCE_DIR}/typecheck/bad/src"
        --include "${CMAKE_CURRENT_SOURCE_DIR}/typecheck/bad/include")
    set_tests_properties(${ARG_LANG}_typecheck_bad PROPERTIES WILL_FAIL TRUE)

    # End-to-end runner: reuse this language's struct_arg stub library (built with
    # -g, so it carries describe(struct Point) in DWARF) and confirm its source is
    # a good candidate via the recover -> to_string -> typecheck path.
    add_test(NAME ${ARG_LANG}_typecheck_runner COMMAND $<TARGET_FILE:${runner}>
        $<TARGET_FILE:${ARG_LANG}_struct_arg_init>
        "${CMAKE_CURRENT_SOURCE_DIR}/struct_arg/src")
endfunction()

# ---------------------------------------------------------------------------
# By-reference tests (Python only): a C caller hands a *pointer* to a liballocs-
# typed struct to a Python function, which mutates the live C struct through a
# pycallocs proxy. Needs the liballocs runtime; OFF by default. Defined here,
# invoked from tests/linker/cpython/. (No Lua analogue: the Lua path has no proxy.)
# ---------------------------------------------------------------------------
option(LINKPY_BYREF_TESTS "Register linkpy by-reference (pointer-arg) tests" OFF)

set(PYCALLOCS_ROOT "${LINKPY_ROOT}/.." CACHE PATH "pycallocs project root")
set(LINKPY_LIBALLOCS_DIR "${PYCALLOCS_ROOT}/contrib/liballocs"
    CACHE PATH "liballocs root for by-reference tests")
set(LINKPY_ALLOCSCC "${LINKPY_LIBALLOCS_DIR}/tools/lang/c/bin/allocscc"
    CACHE FILEPATH "allocscc compiler for typed allocations")
set(LINKPY_LIBALLOCS_PRELOAD "${LINKPY_LIBALLOCS_DIR}/lib/liballocs_preload.so"
    CACHE FILEPATH "liballocs preload .so")
set(LINKPY_BASETYPES_PROVIDER "${PYCALLOCS_ROOT}/build/basetypes/libbasetypes_provider.so"
    CACHE FILEPATH "base-types uniqtype provider .so")
set(LINKPY_ALLOCS_PYTHONPATH "${PYCALLOCS_ROOT}"
    CACHE PATH "directory containing the importable `allocs` extension")

# allocscc locates its CIL frontend under <liballocs>/lib/ocaml by default, but
# a liballocs tree that hasn't installed its OCaml tooling only has the built
# cilly in contrib/cil -- point LIBALLOCS_OCAML_PREFIX there in that case.
if(EXISTS "${LINKPY_LIBALLOCS_DIR}/lib/ocaml/bin/cilly")
    set(_linkpy_ocaml_prefix_default "${LINKPY_LIBALLOCS_DIR}/lib/ocaml")
else()
    set(_linkpy_ocaml_prefix_default "${LINKPY_LIBALLOCS_DIR}/contrib/cil")
endif()
set(LINKPY_OCAML_PREFIX "${_linkpy_ocaml_prefix_default}"
    CACHE PATH "LIBALLOCS_OCAML_PREFIX for allocscc (where bin/cilly lives)")
set(LINKPY_LUAPROXY_DIR "${PYCALLOCS_ROOT}/vmextensions/lua/build"
    CACHE PATH "directory containing luaproxy.so (the Lua vmextension)")

# Like add_linkpy_test, but builds main with allocscc (typed heap) and runs it
# under the liballocs preload with the VM's proxy extension reachable (`allocs`
# importable for Python, luaproxy.so on LUA_CPATH for Lua), so the trampoline
# can build a proxy from the pointer argument.
function(add_linkpy_byref_test name)
    cmake_parse_arguments(ARG "" "LANG" "" ${ARGN})
    if(NOT ARG_LANG)
        set(ARG_LANG python)
    endif()

    if(ARG_LANG STREQUAL lua)
        set(ext "lua")
        set(audit_lib audit_lua)
        set(template "${LINKPY_LUA_TEMPLATE}")
        set(marshal_src "${LINKPY_ROOT}/lua/luacpputils.cpp")
        set(marshal_hdr "${LINKPY_ROOT}/lua/luacpputils.hpp")
        set(lang_dir "${LINKPY_ROOT}/lua")
        set(lang_cflags "${LUA_CFLAGS_STR}")
        set(lang_libs "${LUA_LDFLAGS_STR}")
        # The linkstub's embedded Lua require()s luaproxy for pointer args.
        set(vm_env "LUA_CPATH=${LINKPY_LUAPROXY_DIR}/?.so")
    else()
        set(ext "py")
        set(audit_lib audit)
        set(template "${LINKPY_TEMPLATE}")
        set(marshal_src "${LINKPY_ROOT}/cpython/pycpputils.cpp")
        set(marshal_hdr "${LINKPY_ROOT}/../common/pycpputils.hpp")
        set(lang_dir "${LINKPY_ROOT}/cpython")
        set(lang_cflags "-I${PY_INCLUDE} -I${PY_PLATINCLUDE}")
        set(lang_libs "${PYTHON_LIB} ${PY_SYSLIBS_STR}")
        set(vm_env "PYTHONPATH=${LINKPY_ALLOCS_PYTHONPATH}")
    endif()

    set(test_name "${ARG_LANG}_${name}")
    set(test_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    set(gen_dir "${work_dir}/gen")
    set(include_dir "${test_dir}/include")

    file(MAKE_DIRECTORY "${work_dir}")
    file(CREATE_LINK "${test_dir}/src" "${work_dir}/src" SYMBOLIC)

    file(GLOB headers CONFIGURE_DEPENDS "${include_dir}/*.${ext}.h")
    set(gen_sources "")
    foreach(header ${headers})
        get_filename_component(base "${header}" NAME)
        string(REGEX REPLACE "\\.h$" ".c" gen_c "${base}")
        list(APPEND gen_sources "${gen_dir}/${gen_c}")
    endforeach()

    add_custom_command(
        OUTPUT ${gen_sources} "${gen_dir}/heap_hints.txt"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
        COMMAND $<TARGET_FILE:pystubgen> "${include_dir}/" "${gen_dir}/"
        DEPENDS pystubgen ${headers}
        COMMENT "linkpy byref test ${test_name}: generating stubs"
        VERBATIM)

    add_library(${test_name}_init SHARED ${gen_sources})
    set_target_properties(${test_name}_init PROPERTIES
        OUTPUT_NAME init
        LIBRARY_OUTPUT_DIRECTORY "${work_dir}")
    target_compile_options(${test_name}_init PRIVATE -g -O0)
    target_include_directories(${test_name}_init PRIVATE "${include_dir}")

    # Build main with allocscc so its malloc'd struct carries a liballocs type.
    set(main_exe "${work_dir}/${test_name}_main")
    add_custom_command(
        OUTPUT "${main_exe}"
        COMMAND ${CMAKE_COMMAND} -E env "LIBALLOCS=${LINKPY_LIBALLOCS_DIR}"
                "LIBALLOCS_OCAML_PREFIX=${LINKPY_OCAML_PREFIX}"
                "${LINKPY_ALLOCSCC}" -std=gnu11 -g
                "-I${include_dir}" "${test_dir}/src/main.c"
                -o "${main_exe}" "-L${work_dir}" -linit "-Wl,-rpath,${work_dir}"
        DEPENDS "${test_dir}/src/main.c" ${test_name}_init
        COMMENT "linkpy byref test ${test_name}: building main with allocscc"
        VERBATIM)
    add_custom_target(${test_name}_main ALL DEPENDS "${main_exe}")

    linkpy_marshaller_hash(marshaller_sha "${marshal_src}" "${marshal_hdr}")
    set(linkstub_cmd "${CMAKE_CXX_COMPILER} -std=c++26 -freflection -g -shared -fPIC \
-o liblinkstub.so linkstub.cpp ${marshal_src} \
-I${LINKPY_ROOT}/common -I${LINKPY_ROOT}/../common -I${lang_dir} -I${include_dir} ${lang_cflags} ${lang_libs} \
-DLINKPY_MARSHALLER=${marshaller_sha}")

    add_test(NAME ${test_name} COMMAND "${main_exe}")
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${work_dir}"
        ENVIRONMENT "LINKPY_CACHE_DIR=${CMAKE_BINARY_DIR}/linkpy-cache;LD_AUDIT=$<TARGET_FILE:${audit_lib}>;TEMPLATE_PATH=${template};LINKSTUB_BUILD_CMD=${linkstub_cmd};INIT_DWARF_PATH=${work_dir}/libinit.so;LD_PRELOAD=${LINKPY_LIBALLOCS_PRELOAD}:${LINKPY_BASETYPES_PROVIDER};${vm_env};LD_LIBRARY_PATH=${LINKPY_LIBALLOCS_DIR}/lib;GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288")
endfunction()

# ---------------------------------------------------------------------------
# add_linkpy_cross_test(<name> HOST <python|lua> CALLEE <python|lua>
#                       [PASS_REGEX <re>])
#
# Cross-language fixture: <name>/include/*.<callee-ext>.h and
# <name>/src/*.<callee-ext> exactly as add_linkpy_test, but the caller is a
# HOST-language script (<name>/src/app.py or app.lua) instead of a C main. The
# callee auditor JIT-compiles the linkstub with host bindings appended
# (LINKPY_HOST_LANG + HOST_TEMPLATE_PATH), and the app reaches them through the
# linkpy helper (linkpy.py / linkpy.lua). The user's C header types remain the
# by-value ABI pivot between the two VMs.
#
# The linkstub compiles BOTH marshalling .cpp files (both headers are in the
# TU) but links ONLY the callee VM's library: host VM symbols stay undefined
# and resolve from the host interpreter binary (CPython is -export-dynamic;
# lua is -Wl,-E / linked against a shared liblua).
#
# PASS_REGEX registers the test as a negative one: the run is expected to die
# in the auditor with that diagnostic (PASS_REGULAR_EXPRESSION tolerates the
# abort exit, unlike WILL_FAIL).
#
# BYREF marks a by-reference fixture: any <fixture>/clib/*.c helpers are built
# into <work>/<name>.so with allocscc (typed heap + META_BASE symbol metadata,
# so the host VM can mint proxied C objects from them), and the test runs with
# LINKPY_CROSS_BYREF=1 under the liballocs preload with both proxy extensions
# reachable. Register BYREF fixtures only under LINKPY_BYREF_TESTS.
# ---------------------------------------------------------------------------
find_program(LINKPY_LUA_INTERP NAMES lua5.5 lua5.4 lua5.3 lua
    HINTS "${LINKPY_ROOT}/../contrib/lua"
    DOC "Lua interpreter used as the HOST in cross-language tests")

function(add_linkpy_cross_test name)
    cmake_parse_arguments(ARG "BYREF" "HOST;CALLEE;PASS_REGEX" "" ${ARGN})

    # Callee seams (mirror add_linkpy_test): the stub-header suffix, auditor,
    # VM template, and the VM the linkstub links.
    if(ARG_CALLEE STREQUAL lua)
        set(ext "lua")
        set(audit_lib audit_lua)
        set(template "${LINKPY_LUA_TEMPLATE}")
        set(callee_flags "${LUA_CFLAGS_STR}")
        set(callee_libs "${LUA_LDFLAGS_STR}")
    else()
        set(ext "py")
        set(audit_lib audit)
        set(template "${LINKPY_TEMPLATE}")
        set(callee_flags "-I${PY_INCLUDE} -I${PY_PLATINCLUDE}")
        set(callee_libs "${PYTHON_LIB} ${PY_SYSLIBS_STR}")
    endif()

    # Host seams: prologue template + HOST marshaller include flags (headers
    # only -- the host VM library is deliberately NOT linked), the interpreter
    # command, and the env that lets the app import/require the linkpy helper.
    if(ARG_HOST STREQUAL python)
        set(host_template "${LINKPY_ROOT}/cpython/template_host_py.cpp")
        set(host_flags "-I${PY_INCLUDE} -I${PY_PLATINCLUDE}")
        set(run_cmd "${PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/${name}/src/app.py")
        # The allocs/elflib path is only exercised by BYREF fixtures but is
        # harmless otherwise.
        set(helper_env "PYTHONPATH=${LINKPY_ROOT}/cpython:${LINKPY_ALLOCS_PYTHONPATH}")
    else()
        if(NOT LINKPY_LUA_INTERP)
            message(STATUS "linkpy: cross test ${name} skipped (no lua interpreter found)")
            return()
        endif()
        set(host_template "${LINKPY_ROOT}/lua/template_host_lua.cpp")
        set(host_flags "${LUA_CFLAGS_STR}")
        set(run_cmd "${LINKPY_LUA_INTERP}" "${CMAKE_CURRENT_SOURCE_DIR}/${name}/src/app.lua")
        # No ';;' default-path suffix: a ';' cannot survive CMake's ENVIRONMENT
        # list escaping intact, and the apps only require the linkpy helper.
        set(helper_env "LUA_PATH=${LINKPY_ROOT}/lua/?.lua")
    endif()

    set(test_name "cross_${name}")
    set(test_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    set(gen_dir "${work_dir}/gen")
    set(include_dir "${test_dir}/include")

    file(MAKE_DIRECTORY "${work_dir}")
    # The callee VM inside the linkstub searches ./src for the scripting module.
    file(CREATE_LINK "${test_dir}/src" "${work_dir}/src" SYMBOLIC)

    file(GLOB headers CONFIGURE_DEPENDS "${include_dir}/*.${ext}.h")
    set(gen_sources "")
    foreach(header ${headers})
        get_filename_component(base "${header}" NAME)        # e.g. mytest.lua.h
        string(REGEX REPLACE "\\.h$" ".c" gen_c "${base}")   # -> mytest.lua.c
        list(APPEND gen_sources "${gen_dir}/${gen_c}")
    endforeach()

    add_custom_command(
        OUTPUT ${gen_sources} "${gen_dir}/heap_hints.txt"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
        COMMAND $<TARGET_FILE:pystubgen> "${include_dir}/" "${gen_dir}/"
        DEPENDS pystubgen ${headers}
        COMMENT "linkpy cross test ${name}: generating stubs"
        VERBATIM)

    add_library(${test_name}_init SHARED ${gen_sources})
    set_target_properties(${test_name}_init PROPERTIES
        OUTPUT_NAME init
        LIBRARY_OUTPUT_DIRECTORY "${work_dir}")
    target_compile_options(${test_name}_init PRIVATE -g -O0)
    target_include_directories(${test_name}_init PRIVATE "${include_dir}")

    # By-reference extras: allocscc-built typed helper libraries the host VM
    # creates proxied C objects from, plus the liballocs runtime environment.
    set(byref_env "")
    if(ARG_BYREF)
        set(byref_meta "${work_dir}/meta")
        file(GLOB clib_sources CONFIGURE_DEPENDS "${test_dir}/clib/*.c")
        set(clib_sos "")
        foreach(csrc ${clib_sources})
            get_filename_component(cname "${csrc}" NAME_WE)
            set(cso "${work_dir}/${cname}.so")
            add_custom_command(
                OUTPUT "${cso}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${byref_meta}"
                COMMAND ${CMAKE_COMMAND} -E env "LIBALLOCS=${LINKPY_LIBALLOCS_DIR}"
                        "LIBALLOCS_OCAML_PREFIX=${LINKPY_OCAML_PREFIX}"
                        "META_BASE=${byref_meta}"
                        "${LINKPY_ALLOCSCC}" -std=gnu11 -g -fPIC -shared
                        "${csrc}" -o "${cso}"
                DEPENDS "${csrc}"
                COMMENT "linkpy cross byref ${name}: building ${cname}.so with allocscc"
                VERBATIM)
            list(APPEND clib_sos "${cso}")
        endforeach()
        add_custom_target(${test_name}_clibs ALL DEPENDS ${clib_sos})

        # LUA_CPATH serves whichever side is Lua: the host state extracting
        # addresses, or the callee state re-proxying them.
        set(byref_env ";LINKPY_CROSS_BYREF=1;LD_PRELOAD=${LINKPY_LIBALLOCS_PRELOAD}:${LINKPY_BASETYPES_PROVIDER};LD_LIBRARY_PATH=${LINKPY_LIBALLOCS_DIR}/lib;GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288;META_BASE=${byref_meta};LUA_CPATH=${LINKPY_LUAPROXY_DIR}/?.so")
    endif()

    linkpy_marshaller_hash(marshaller_sha "${LINKPY_ROOT}/cpython/pycpputils.cpp" "${LINKPY_ROOT}/../common/pycpputils.hpp"
                                          "${LINKPY_ROOT}/lua/luacpputils.cpp" "${LINKPY_ROOT}/lua/luacpputils.hpp")
    set(linkstub_cmd "${CMAKE_CXX_COMPILER} -std=c++26 -freflection -g -shared -fPIC \
-o liblinkstub.so linkstub.cpp ${LINKPY_ROOT}/cpython/pycpputils.cpp ${LINKPY_ROOT}/lua/luacpputils.cpp \
-I${LINKPY_ROOT}/common -I${LINKPY_ROOT}/../common -I${LINKPY_ROOT}/cpython -I${LINKPY_ROOT}/lua \
-I${include_dir} ${callee_flags} ${host_flags} ${callee_libs} -DLINKPY_MARSHALLER=${marshaller_sha}")

    add_test(NAME ${test_name} COMMAND ${run_cmd})
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${work_dir}"
        ENVIRONMENT "LINKPY_CACHE_DIR=${CMAKE_BINARY_DIR}/linkpy-cache;LD_AUDIT=$<TARGET_FILE:${audit_lib}>;TEMPLATE_PATH=${template};HOST_TEMPLATE_PATH=${host_template};LINKPY_HOST_LANG=${ARG_HOST};LINKSTUB_BUILD_CMD=${linkstub_cmd};INIT_DWARF_PATH=$<TARGET_FILE:${test_name}_init>;${helper_env}${byref_env}")
    if(ARG_PASS_REGEX)
        set_tests_properties(${test_name} PROPERTIES
            PASS_REGULAR_EXPRESSION "${ARG_PASS_REGEX}")
    endif()
endfunction()
