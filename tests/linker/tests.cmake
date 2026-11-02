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
# Python additionally carries the by-reference (ptr_arg) and heap-hints tests,
# which exercise pointer arguments escaping into the VM via a pycallocs proxy --
# a Python/liballocs-only path with no Lua analogue (Lua passes pointers as opaque
# userdata), so they live under tests/linker/cpython/ only.
#
# Paths are anchored on LINKPY_ROOT / LINKPY_TESTS_DIR (set by the includer),
# not CMAKE_SOURCE_DIR: the latter points at the superproject when the linker
# tree is consumed via add_subdirectory.
# ---------------------------------------------------------------------------

set(LINKPY_TEMPLATE "${LINKPY_ROOT}/cpython/template.cpp")
set(LINKPY_LUA_TEMPLATE "${LINKPY_ROOT}/lua/template_lua.cpp")

# Shared, language-agnostic lmap-mode check (driven with -DTOOL/-DSRC).
set(LINKPY_LMAP_CHECK "${LINKPY_TESTS_DIR}/check_typecheck_lmap.cmake")

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
        set(lang_dir "${LINKPY_ROOT}/lua")
        set(lang_cflags "${LUA_CFLAGS_STR}")
        set(lang_libs "${LUA_LDFLAGS_STR}")
    else()
        set(ext "py")
        set(hgen "${LINKPY_ROOT}/cpython/pyhgen.py")
        set(audit_lib audit)
        set(template "${LINKPY_TEMPLATE}")
        set(marshal_src "${LINKPY_ROOT}/cpython/pycpputils.cpp")
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
    # function signatures from the embedded DWARF (see INIT_DWARF_PATH below).
    set_target_properties(${test_name}_init PROPERTIES
        OUTPUT_NAME init
        LIBRARY_OUTPUT_DIRECTORY "${work_dir}")
    target_compile_options(${test_name}_init PRIVATE -g)
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
    set(linkstub_cmd "${CMAKE_CXX_COMPILER} -std=c++26 -freflection -g -shared -fPIC \
-o liblinkstub.so linkstub.cpp ${marshal_src} \
-I${LINKPY_ROOT}/common -I${lang_dir} -I${include_dir} ${lang_cflags} ${lang_libs}")

    add_test(NAME ${test_name} COMMAND ${test_name}_main)
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${work_dir}"
        ENVIRONMENT "LD_AUDIT=$<TARGET_FILE:${audit_lib}>;TEMPLATE_PATH=${template};LINKSTUB_BUILD_CMD=${linkstub_cmd};INIT_DWARF_PATH=$<TARGET_FILE:${test_name}_init>")
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
set(LINKPY_LIBALLOCS_DIR "${PYCALLOCS_ROOT}/contrib/stackscan/contrib/liballocs"
    CACHE PATH "liballocs root for by-reference tests")
set(LINKPY_ALLOCSCC "${LINKPY_LIBALLOCS_DIR}/tools/lang/c/bin/allocscc"
    CACHE FILEPATH "allocscc compiler for typed allocations")
set(LINKPY_LIBALLOCS_PRELOAD "${LINKPY_LIBALLOCS_DIR}/lib/liballocs_preload.so"
    CACHE FILEPATH "liballocs preload .so")
set(LINKPY_BASETYPES_PROVIDER "${PYCALLOCS_ROOT}/build/basetypes/libbasetypes_provider.so"
    CACHE FILEPATH "base-types uniqtype provider .so")
set(LINKPY_ALLOCS_PYTHONPATH "${PYCALLOCS_ROOT}"
    CACHE PATH "directory containing the importable `allocs` extension")

# Like add_linkpy_test, but builds main with allocscc (typed heap) and runs it
# under the liballocs preload with `allocs` importable, so the trampoline can
# build a proxy from the pointer argument. Always Python.
function(add_linkpy_byref_test name)
    set(test_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    set(gen_dir "${work_dir}/gen")
    set(include_dir "${test_dir}/include")

    file(MAKE_DIRECTORY "${work_dir}")
    file(CREATE_LINK "${test_dir}/src" "${work_dir}/src" SYMBOLIC)

    file(GLOB headers CONFIGURE_DEPENDS "${include_dir}/*.py.h")
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
        COMMENT "linkpy byref test ${name}: generating stubs"
        VERBATIM)

    add_library(python_${name}_init SHARED ${gen_sources})
    set_target_properties(python_${name}_init PROPERTIES
        OUTPUT_NAME init
        LIBRARY_OUTPUT_DIRECTORY "${work_dir}")
    target_compile_options(python_${name}_init PRIVATE -g)
    target_include_directories(python_${name}_init PRIVATE "${include_dir}")

    # Build main with allocscc so its malloc'd struct carries a liballocs type.
    set(main_exe "${work_dir}/python_${name}_main")
    add_custom_command(
        OUTPUT "${main_exe}"
        COMMAND ${CMAKE_COMMAND} -E env "LIBALLOCS=${LINKPY_LIBALLOCS_DIR}"
                "${LINKPY_ALLOCSCC}" -std=gnu11 -g
                "-I${include_dir}" "${test_dir}/src/main.c"
                -o "${main_exe}" "-L${work_dir}" -linit "-Wl,-rpath,${work_dir}"
        DEPENDS "${test_dir}/src/main.c" python_${name}_init
        COMMENT "linkpy byref test ${name}: building main with allocscc"
        VERBATIM)
    add_custom_target(python_${name}_main ALL DEPENDS "${main_exe}")

    set(linkstub_cmd "${CMAKE_CXX_COMPILER} -std=c++26 -freflection -g -shared -fPIC \
-o liblinkstub.so linkstub.cpp ${LINKPY_ROOT}/cpython/pycpputils.cpp \
-I${LINKPY_ROOT}/common -I${LINKPY_ROOT}/cpython -I${include_dir} -I${PY_INCLUDE} -I${PY_PLATINCLUDE} \
${PYTHON_LIB} ${PY_SYSLIBS_STR}")

    add_test(NAME python_${name} COMMAND "${main_exe}")
    set_tests_properties(python_${name} PROPERTIES
        WORKING_DIRECTORY "${work_dir}"
        ENVIRONMENT "LD_AUDIT=$<TARGET_FILE:audit>;TEMPLATE_PATH=${LINKPY_TEMPLATE};LINKSTUB_BUILD_CMD=${linkstub_cmd};INIT_DWARF_PATH=${work_dir}/libinit.so;LD_PRELOAD=${LINKPY_LIBALLOCS_PRELOAD}:${LINKPY_BASETYPES_PROVIDER};PYTHONPATH=${LINKPY_ALLOCS_PYTHONPATH};LD_LIBRARY_PATH=${LINKPY_LIBALLOCS_DIR}/lib;GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288")
endfunction()
