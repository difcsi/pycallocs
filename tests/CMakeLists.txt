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
                "${ALLOCSCC}" -std=gnu11 -g -fPIC -shared
                "-I${LIBALLOCS_DIR}/include" "${src}" -o "${so}"
        # allocscc drops intermediates next to the .so (*.so.allocstubs.{c,i,o},
        # *.so.usedtypes.{c,o}, *.so.linked.o). Only the .so is needed at runtime,
        # so remove the rest to keep the build's libs/<name>/ folder tidy.
        COMMAND ${CMAKE_COMMAND} -E rm -f
                "${so}.allocstubs.c" "${so}.allocstubs.i" "${so}.allocstubs.o"
                "${so}.usedtypes.c" "${so}.usedtypes.o" "${so}.linked.o"
        DEPENDS "${src}"
        COMMENT "Building test lib ${name}.so with allocscc"
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
    get_filename_component(testdir "${script}" DIRECTORY)
    set(expected "${testdir}/${name}.out")
    set(_expected_arg "")
    if(EXISTS "${expected}")
        set(_expected_arg "-DEXPECTED=${expected}")
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
        ENVIRONMENT "LD_PRELOAD=${LIBALLOCS_PRELOAD}:${BASETYPES_PROVIDER};PYTHONPATH=${CMAKE_SOURCE_DIR}:${TESTS_SRC_DIR};LD_AUDIT=${LINKPY_AUDIT_SO};LD_LIBRARY_PATH=${LIBALLOCS_DIR}/lib;GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288;PYCALLOCS_TEST_LIBS=${LIBS_BIN_DIR}")
endforeach()
