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
set(ALLOCSCC          "${LIBALLOCS_DIR}/tools/lang/c/bin/allocscc")
set(LIBALLOCS_PRELOAD "${LIBALLOCS_DIR}/lib/liballocs_preload.so")

# ---------------------------------------------------------------------------
# Build the test fixture shared objects with allocscc (was tests/libs/Makefile).
# Each libs/*.c -> libs/*.so. As before, the alaska runtime environment must be
# in place; LIBALLOCS is passed through so allocscc can locate its toolchain.
# The .so files are written back into the source libs/ dir because the test
# scripts load them via the relative path "libs/".
# ---------------------------------------------------------------------------
file(GLOB LIB_SOURCES "${LIBS_SRC_DIR}/*.c")
# Drop allocscc-generated intermediates (*.cil.c, *.allocstubs.c, *.usedtypes.c).
list(FILTER LIB_SOURCES EXCLUDE REGEX "\\.(cil|allocstubs|usedtypes)\\.")

set(_lib_sos "")
foreach(src ${LIB_SOURCES})
    get_filename_component(name "${src}" NAME_WE)
    set(so "${LIBS_SRC_DIR}/${name}.so")
    add_custom_command(
        OUTPUT "${so}"
        COMMAND ${CMAKE_COMMAND} -E env "LIBALLOCS=${LIBALLOCS_DIR}"
                "${ALLOCSCC}" -std=gnu11 -g -fPIC -shared
                "-I${LIBALLOCS_DIR}/include" "${src}" -o "${so}"
        DEPENDS "${src}"
        COMMENT "Building test lib ${name}.so with allocscc"
        VERBATIM)
    list(APPEND _lib_sos "${so}")
endforeach()

add_custom_target(test-libs DEPENDS ${_lib_sos})

# A ctest fixture so that `ctest` on its own (not just `make check`) builds the
# fixture libs before the python tests run.
add_test(NAME build-test-libs
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target test-libs basetypes-provider)
set_tests_properties(build-test-libs PROPERTIES FIXTURES_SETUP testlibs)

# ---------------------------------------------------------------------------
# Register each *.py as a ctest test (was tests/Makefile).
# ---------------------------------------------------------------------------
file(GLOB TEST_SCRIPTS "${TESTS_SRC_DIR}/*.py")

foreach(script ${TEST_SCRIPTS})
    get_filename_component(name "${script}" NAME_WE)
    set(expected "${TESTS_SRC_DIR}/${name}.out")
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
        WORKING_DIRECTORY "${TESTS_SRC_DIR}"
        FIXTURES_REQUIRED testlibs
        TIMEOUT 300
        ENVIRONMENT "LD_PRELOAD=${LIBALLOCS_PRELOAD}:${BASETYPES_PROVIDER};PYTHONPATH=${CMAKE_SOURCE_DIR}")
endforeach()
