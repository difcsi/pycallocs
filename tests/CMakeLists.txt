# ---------------------------------------------------------------------------
# pycallocs test suite (ported from tests/Makefile + tests/libs/Makefile)
#
# Each tests/*.py becomes an individual ctest test. Its merged stdout/stderr is
# compared against the matching *.out when one exists, otherwise the test only
# has to exit 0 (matching the old Makefile's %.py rule).
#
# The tests load shared objects from libs/, compiled with liballocs' allocscc.
# ---------------------------------------------------------------------------

set(TESTS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(LIBS_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/libs")
# All build artifacts (the fixture .so files and per-test stdout/stderr captures)
# live under the build tree so the source tree stays clean and ctest can run
# entirely from the build folder.
set(LIBS_BIN_DIR  "${CMAKE_CURRENT_BINARY_DIR}/libs")
set(ALLOCSCC          "${LIBALLOCS_DIR}/tools/lang/c/bin/allocscc")
set(LIBALLOCS_PRELOAD "${LIBALLOCS_DIR}/lib/liballocs_preload.so")

# Alaska toolchain/runtime locations (used to build the relocate fixture with the
# Alaska transform, and to load it at run time). Mirrors stackscan's test recipe.
# The tree is selected by the top-level ALASKA_OPT_TREE cache variable.
set(_ALASKA_ANCHORAGE "${ALASKA_DIR}/opt/${ALASKA_OPT_TREE}")
set(_ALASKA_LLVM      "${ALASKA_DIR}/opt/llvm")
set(_ALASKA_GLLVM     "${ALASKA_DIR}/opt/gllvm")
# liballocs writes per-binary type metadata (-meta.so) under META_BASE; the default
# /usr/lib/meta is not writable, so keep it in the build tree (set at both compile
# and run time so the metadata is generated and then found).
set(_META_BASE        "${CMAKE_CURRENT_BINARY_DIR}/meta")

# ---------------------------------------------------------------------------
# Build the test fixture shared objects with allocscc (was tests/libs/Makefile).
# Each lib lives in its own subfolder: libs/<name>/<name>.c is the source; the
# compiled libs/<name>/<name>.so is written into the build tree (LIBS_BIN_DIR).
# As before, the alaska runtime environment must be in place; LIBALLOCS is passed
# through so allocscc can locate its toolchain. Tests find the .so files via the
# PYCALLOCS_TEST_LIBS env var (set below), pointing at LIBS_BIN_DIR.
# ---------------------------------------------------------------------------
file(GLOB LIB_SOURCES "${LIBS_SRC_DIR}/*/*.c")
# Drop allocscc-generated intermediates (*.cil.c, *.allocstubs.c, *.usedtypes.c).
list(FILTER LIB_SOURCES EXCLUDE REGEX "\\.(cil|allocstubs|usedtypes)\\.")

# The relocate fixture hard-depends on the Alaska runtime (hrealloc/alaska_translate),
# so its .so is unloadable without Alaska. Only build it when Alaska is enabled,
# matching the gated relocate test below.
if(NOT _ALASKA_ENABLED)
    list(FILTER LIB_SOURCES EXCLUDE REGEX "/relocate/")
endif()

# When Alaska is enabled, build EVERY fixture through the Alaska transform so the
# whole suite exercises handles: allocscc delegates codegen to ALLOCSCC_CC=alaska,
# giving both liballocs uniqtypes (from cilly, needed so the proxy layer can type
# the allocation) AND handles (from the alaska backend). This mirrors stackscan's
# ALLOCSCC_ALASKA_ENV: CC_IS_CLANG makes CIL parse clang's _Float* types and drop
# gcc-only flags, META_BASE captures each fixture's type meta-DSO in the build tree,
# and PATH/LD_LIBRARY_PATH expose gclang/opt/llc/get-bc and the runtime libs to the
# build. Without Alaska, plain allocscc (default backend, no transform) as before.
set(_extra_cc_env)
set(_cc_label "allocscc")
if(_ALASKA_ENABLED)
    set(_extra_cc_env
        "ALLOCSCC_CC=${ALASKA}"
        "CC_IS_CLANG=1"
        "ALASKA=${_ALASKA_ANCHORAGE}"
        "ALASKA_CLANG=${_ALASKA_LLVM}/bin/clang"
        "ALASKA_GCLANG=${_ALASKA_GLLVM}/bin/gclang"
        "LLVM_COMPILER=clang"
        "LDFLAGS=-fuse-ld=bfd"
        "PATH=${_ALASKA_LLVM}/bin:${_ALASKA_GLLVM}/bin:${_ALASKA_ANCHORAGE}/bin:$ENV{PATH}"
        "LD_LIBRARY_PATH=${_ALASKA_ANCHORAGE}/lib:${_ALASKA_LLVM}/lib:${LIBALLOCS_DIR}/lib:$ENV{LD_LIBRARY_PATH}")
    set(_cc_label "allocscc+alaska")
endif()

set(_lib_sos "")
foreach(src ${LIB_SOURCES})
    get_filename_component(name "${src}" NAME_WE)
    set(so "${LIBS_BIN_DIR}/${name}/${name}.so")

    add_custom_command(
        OUTPUT "${so}"
        # Ensure the per-lib output subfolder exists (allocscc won't create it).
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LIBS_BIN_DIR}/${name}"
        # ALLOCSCC_TRAP_PTR_WRITES=1: run allocscc's trapptrwrites CIL pass so
        # C-side pointer/aggregate stores call __notify_ptr_write/__notify_copy.
        # Without it liballocs' GC lifetime extension can't see writes the compiler
        # makes (e.g. save_hw storing a heap pointer into a global) -> struct_global.
        COMMAND ${CMAKE_COMMAND} -E env "LIBALLOCS=${LIBALLOCS_DIR}"
                "ALLOCSCC_TRAP_PTR_WRITES=1"
                "META_BASE=${_META_BASE}"
                ${_extra_cc_env}
                "${ALLOCSCC}" -std=gnu11 -g -fPIC -shared
                "-I${LIBALLOCS_DIR}/include" "${src}" -o "${so}"
        # allocscc drops intermediates next to the .so (*.so.allocstubs.{c,i,o},
        # *.so.usedtypes.{c,o}, *.so.linked.o). Only the .so is needed at runtime,
        # so remove the rest to keep the build's libs/<name>/ folder tidy.
        COMMAND ${CMAKE_COMMAND} -E rm -f
                "${so}.allocstubs.c" "${so}.allocstubs.i" "${so}.allocstubs.o"
                "${so}.usedtypes.c" "${so}.usedtypes.o" "${so}.linked.o"
        DEPENDS "${src}"
        COMMENT "Building test lib ${name}.so with ${_cc_label}"
        VERBATIM)
    list(APPEND _lib_sos "${so}")
endforeach()

add_custom_target(test-libs DEPENDS ${_lib_sos})

# A ctest fixture so that `ctest` on its own (not just `make check`) builds the
# fixture libs before the python tests run. Also builds linkpy's audit target
# when it is being compiled from source (i.e. audit.so was absent at configure time).
add_test(NAME build-test-libs
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
            --target test-libs basetypes-provider ${_LINKPY_BUILD_TARGET})
set_tests_properties(build-test-libs PROPERTIES FIXTURES_SETUP testlibs)

# ---------------------------------------------------------------------------
# Register each test as an individual ctest test. Each test lives in its own
# subfolder: py/<name>/<name>.py (with an optional py/<name>/<name>.out).
# ---------------------------------------------------------------------------
file(GLOB TEST_SCRIPTS "${TESTS_SRC_DIR}/py/*/*.py")

foreach(script ${TEST_SCRIPTS})
    get_filename_component(name "${script}" NAME_WE)

    # `relocate` proves a proxy survives an Alaska backing move (hrealloc). It only
    # holds under the full Alaska handle transform: without it, a realloc move would
    # invalidate the proxy's pointer. Skip registering it in non-Alaska builds.
    # (_ALASKA_ENABLED is set by the top-level CMakeLists before add_subdirectory.)
    if(name STREQUAL "relocate" AND NOT _ALASKA_ENABLED)
        continue()
    endif()

    get_filename_component(testdir "${script}" DIRECTORY)
    set(expected "${testdir}/${name}.out")
    set(_expected_arg "")
    if(EXISTS "${expected}")
        set(_expected_arg "-DEXPECTED=${expected}")
    endif()

    # META_BASE is used unconditionally: every fixture (Alaska or plain allocscc)
    # writes its type meta-DSO into the build tree, and liballocs must look there to
    # resolve it at run time. The libalaska search path is only needed when Alaska is
    # enabled (then every fixture is transformed and its dlopen pulls in libalaska).
    set(_test_ld_lib "${LIBALLOCS_DIR}/lib")
    set(_test_extra_env ";META_BASE=${_META_BASE}")
    if(_ALASKA_ENABLED)
        set(_test_ld_lib "${_ALASKA_ANCHORAGE}/lib:${LIBALLOCS_DIR}/lib")
    endif()

    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
                "-DPYTHON=${PYTHON}"
                "-DTESTPY=${script}"
                ${_expected_arg}
                "-DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/${name}.actual"
                -P "${TESTS_SRC_DIR}/run_test.cmake")

    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        FIXTURES_REQUIRED testlibs
        TIMEOUT 300
        # GLIBC_TUNABLES: the liballocs preload uses initial-exec TLS for its
        # malloc-hook reentrancy guard (so reading it never calls __tls_get_addr,
        # which would recurse into malloc during DTV allocation). That forces the
        # preload's whole TLS block into the static TLS area, which does not fit
        # the default surplus under LD_AUDIT, so raise it here.
        #
        # LD_LIBRARY_PATH: the fixture .so files are built with allocscc, which
        # adds a DT_NEEDED on liballocs_dummyweaks.so (weak liballocs stubs,
        # overridden by the real preload at runtime) but no RPATH. dlopen still
        # has to find that file, so put the liballocs lib dir on the search path.
        #
        # PYCALLOCS_TEST_LIBS: where the fixture .so files were built (the build
        # tree, not the source tree). Each test appends the lib subfolders it uses
        # to elflib's search path from this root.
        # PYTHONPATH carries both the repo root (for elflib/allocs) and the tests
        # dir (for the shared pyc_testlib helper that the tests import).
        ENVIRONMENT "LD_PRELOAD=${LIBALLOCS_PRELOAD}:${BASETYPES_PROVIDER};PYTHONPATH=${CMAKE_SOURCE_DIR}:${TESTS_SRC_DIR};LD_AUDIT=${LINKPY_AUDIT_SO};LD_LIBRARY_PATH=${_test_ld_lib};GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288;PYCALLOCS_TEST_LIBS=${LIBS_BIN_DIR}${_test_extra_env}")
endforeach()
