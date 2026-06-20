cmake_minimum_required(VERSION 3.22)
project(pycallocs C)

# ---------------------------------------------------------------------------
# pycallocs builds a CPython extension module (`allocs`) that provides an
# invisible FFI on top of liballocs meta-information.
#
# liballocs and alaska are NOT submodules of pycallocs; they belong to the
# stackscan submodule and are consumed here as pre-built dependencies. This
# build locates them under contrib/stackscan/contrib/ and fails with build
# instructions when they are missing or not built.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(USE_ALLOCSCC "Use liballocs' allocscc compiler to generate uniqtype symbols" OFF)
option(USE_ALASKA   "Use the alaska compiler"                                        ON)
option(USE_ALASKA_CLANG
    "Build the extension with alaska's bundled clang (no alaska transformation passes)"
    ON)
option(DEBUG        "Build the extension with -O0 and assertions enabled"            OFF)
option(SPECIALISE_CONVERSION
    "Generate & dlopen specialised PyObject_to_T<T> translators for by-pointer PyObject->struct conversion (needs g++-16 at runtime)"
    OFF)
option(INJECT_CONVERSION
    "DRAFT: like SPECIALISE_CONVERSION, but synthesise the translators via P3294 token-sequence injection instead of string-templated codegen (needs a P3294-capable g++-16 at runtime; mutually exclusive with SPECIALISE_CONVERSION)"
    OFF)
option(BUILD_STACKSCAN
    "Build the stackscan submodule and its own submodules (alaska, liballocs) from source as part of this build, instead of consuming pre-built copies"
    ON)
option(BUILD_BENCHMARKS
    "Build the benchmark fixtures + native baseline and expose the bench* run targets (benchmarks/)"
    OFF)

# ---------------------------------------------------------------------------
# Paths: stackscan and linkpy
# (must be set before add_subdirectory(contrib) so contrib can read them)
# ---------------------------------------------------------------------------
set(STACKSCAN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/contrib/stackscan"
    CACHE PATH "stackscan submodule root")

set(LINKPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/contrib/linkpy"
    CACHE PATH "linkpy root")
set(LINKPY_AUDIT_SO "${LINKPY_DIR}/dist/audit.so")

# ---------------------------------------------------------------------------
# Third-party builds: stackscan and linkpy
# contrib/CMakeLists.txt drives add_subdirectory for each subproject and
# exports _STACKSCAN_DEP_TARGETS and _LINKPY_BUILD_TARGET to this scope.
# ---------------------------------------------------------------------------
add_subdirectory(contrib)

# ---------------------------------------------------------------------------
# liballocs (pre-built, owned by stackscan)
# ---------------------------------------------------------------------------
set(LIBALLOCS_DIR "${STACKSCAN_DIR}/contrib/liballocs"
    CACHE PATH "liballocs source/build root (pre-built)")

message(STATUS "Looking for liballocs in ${LIBALLOCS_DIR}")

# When BUILD_STACKSCAN is on these artifacts are produced by the `liballocs`
# target during the build, so they legitimately don't exist at configure time.
if(NOT BUILD_STACKSCAN)
    if(NOT EXISTS "${LIBALLOCS_DIR}/config.mk")
        message(FATAL_ERROR
            "liballocs not configured at ${LIBALLOCS_DIR}.\n"
            "Initialise stackscan's submodules and build it:\n"
            "  git submodule update --init --recursive\n"
            "  cd ${LIBALLOCS_DIR} && ./configure --enable-lifetime-policies && make")
    endif()

    if(NOT EXISTS "${LIBALLOCS_DIR}/lib/liballocs.so")
        message(FATAL_ERROR
            "liballocs not built at ${LIBALLOCS_DIR}.\n"
            "  cd ${LIBALLOCS_DIR} && make")
    endif()

    file(STRINGS "${LIBALLOCS_DIR}/config.h" _liballocs_lifetime
         REGEX "LIFETIME_POLICIES")
    if(NOT _liballocs_lifetime)
        message(FATAL_ERROR
            "liballocs must be built with --enable-lifetime-policies.\n"
            "  cd ${LIBALLOCS_DIR} && ./configure --enable-lifetime-policies && make")
    endif()
endif()

# Paths derived from liballocs.
set(LIBRUNT_DIR       "${LIBALLOCS_DIR}/contrib/libsystrap/contrib/librunt")
set(LIBSYSTRAP_DIR    "${LIBALLOCS_DIR}/contrib/libsystrap")
set(LIBALLOCSTOOL_DIR "${LIBALLOCS_DIR}/contrib/liballocstool")
set(ALLOCSCC          "${LIBALLOCS_DIR}/tools/lang/c/bin/allocscc")

if(USE_ALLOCSCC AND NOT BUILD_STACKSCAN AND NOT EXISTS "${ALLOCSCC}")
    message(FATAL_ERROR
        "allocscc not found at ${ALLOCSCC}. Build liballocs first or set -DUSE_ALLOCSCC=OFF")
endif()

# ---------------------------------------------------------------------------
# alaska (pre-built, owned by stackscan)
# ---------------------------------------------------------------------------
set(ALASKA_DIR "${STACKSCAN_DIR}/contrib/alaska"
    CACHE PATH "alaska source/build root (pre-built)")
set(ALASKA "${ALASKA_DIR}/opt/alaska-anchorage/bin/alaska"
    CACHE FILEPATH "alaska compiler wrapper")
set(ALASKA_CLANG "${ALASKA_DIR}/opt/llvm/bin/clang"
    CACHE FILEPATH "alaska's bundled clang (without alaska transformation passes)")

if(USE_ALASKA)
    message(STATUS "Looking for alaska at ${ALASKA}")
    if(NOT BUILD_STACKSCAN AND NOT EXISTS "${ALASKA}")
        message(FATAL_ERROR
            "alaska not found at ${ALASKA}.\n"
            "Build it inside stackscan first or set -DUSE_ALASKA=OFF.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Python interpreter
# ---------------------------------------------------------------------------
# Prefer the bundled cpython submodule when present, else the system python3.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/contrib/cpython/python")
    set(_python_default "${CMAKE_CURRENT_SOURCE_DIR}/contrib/cpython/python")
else()
    set(_python_default "python3")
endif()
set(PYTHON "${_python_default}" CACHE STRING "Python interpreter to use (path or name on PATH)")

execute_process(COMMAND "${PYTHON}" --version
                RESULT_VARIABLE _py_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _py_rc EQUAL 0)
    message(FATAL_ERROR "Python interpreter '${PYTHON}' not usable. Set -DPYTHON=/path/to/python3")
endif()

# ---------------------------------------------------------------------------
# Choose the compiler driver the extension is built with.
#   USE_ALLOCSCC     -> emits uniqtype symbols (delegates codegen to alaska)
#   USE_ALASKA_CLANG -> alaska's bundled clang, no transformation passes (default)
#   USE_ALASKA       -> full alaska compiler with handle-based transformation
# ---------------------------------------------------------------------------
if(USE_ALLOCSCC)
    set(EXT_CC "${ALLOCSCC}")
elseif(USE_ALASKA_CLANG)
    set(EXT_CC "${ALASKA_CLANG}")
    if(NOT BUILD_STACKSCAN AND NOT EXISTS "${ALASKA_CLANG}")
        message(FATAL_ERROR
            "alaska's clang not found at ${ALASKA_CLANG}.\n"
            "Build stackscan first or set -DUSE_ALASKA_CLANG=OFF.")
    endif()
elseif(USE_ALASKA)
    set(EXT_CC "${ALASKA}")
else()
    set(EXT_CC "")  # default system compiler chosen by setuptools
endif()

# ---------------------------------------------------------------------------
# Environment shared by the extension build and the test run.
# ---------------------------------------------------------------------------
set(_build_env
    "LIBALLOCS=${LIBALLOCS_DIR}"
    "LIBRUNT=${LIBRUNT_DIR}"
    "LIBSYSTRAP=${LIBSYSTRAP_DIR}"
    "LIBALLOCSTOOL=${LIBALLOCSTOOL_DIR}"
    "ALASKA=${ALASKA_DIR}"
)
if(USE_ALLOCSCC)
    list(APPEND _build_env "USE_ALLOCSCC=yes")
endif()
if(DEBUG)
    list(APPEND _build_env "DEBUG=1")
endif()
if(SPECIALISE_CONVERSION AND INJECT_CONVERSION)
    message(FATAL_ERROR "SPECIALISE_CONVERSION and INJECT_CONVERSION are mutually "
                        "exclusive: both define Specialise_Convert/Specialise_FromValue. "
                        "Enable at most one.")
endif()
if(SPECIALISE_CONVERSION)
    list(APPEND _build_env "SPECIALISE_CONVERSION=1")
    message(STATUS "SPECIALISE_CONVERSION on: the extension #embed's pycpputils.hpp, "
                   "so it must be built with a C23 #embed-capable C compiler "
                   "(gcc>=15 / clang>=19, e.g. -DUSE_ALASKA_CLANG=OFF -DUSE_ALASKA=OFF), "
                   "and g++-16 must be on PATH at runtime for translator compilation.")
endif()
if(INJECT_CONVERSION)
    list(APPEND _build_env "INJECT_CONVERSION=1")
    message(STATUS "INJECT_CONVERSION on (DRAFT P3294 token-injection back end): the "
                   "extension #embed's pycpputils.hpp and pyc_inject.hpp, so it must be "
                   "built with a C23 #embed-capable C compiler (gcc>=15 / clang>=19, e.g. "
                   "-DUSE_ALASKA_CLANG=OFF -DUSE_ALASKA=OFF), and a P3294-capable g++-16 "
                   "must be on PATH at runtime for translator compilation.")
endif()
if(EXT_CC)
    list(APPEND _build_env "CC=${EXT_CC}")
endif()

# ---------------------------------------------------------------------------
# Build the Python extension via pip/setuptools (setup.py drives the actual
# compilation, mirroring the previous automake `build-python` target).
# ---------------------------------------------------------------------------
add_custom_target(build-python ALL
    COMMAND ${CMAKE_COMMAND} -E env ${_build_env}
            "${PYTHON}" -m pip install -e . --no-build-isolation
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building and installing the pycallocs Python extension (editable)"
    USES_TERMINAL VERBATIM)

if(_CPYTHON_DEP_TARGET)
    add_dependencies(build-python ${_CPYTHON_DEP_TARGET})
endif()
if(_STACKSCAN_DEP_TARGETS)
    add_dependencies(build-python ${_STACKSCAN_DEP_TARGETS})
endif()

# ---------------------------------------------------------------------------
# Install: build a wheel/sdist install into the configured prefix.
# ---------------------------------------------------------------------------
install(CODE "
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env ${_build_env}
                \"${PYTHON}\" -m pip install --prefix=\"${CMAKE_INSTALL_PREFIX}\" .
        WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR \"pip install failed\")
    endif()
")

# ---------------------------------------------------------------------------
# Base-types uniqtype provider.
#
# The extension imports base-type uniqtype symbols (e.g. __uniqtype__signed_char$$8)
# that liballocs would normally supply via the C library's meta-DSO -- which we
# can't generate here (DWARF-5 libc crashes liballocs' dwarftypes). Instead we
# generate a small provider .so and preload it (after liballocs_preload.so) when
# running the tests. See basetypes/.
# ---------------------------------------------------------------------------
set(BASETYPES_PROVIDER "${CMAKE_CURRENT_BINARY_DIR}/basetypes/libbasetypes_provider.so")
add_custom_command(
    OUTPUT "${BASETYPES_PROVIDER}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/basetypes/gen_provider.sh"
            "${LIBALLOCS_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/basetypes"
            "${CMAKE_CURRENT_BINARY_DIR}/basetypes"
    DEPENDS basetypes/basetypes.c basetypes/aliases.inc.c basetypes/gen_provider.sh
    COMMENT "Building base-types uniqtype provider"
    VERBATIM)
add_custom_target(basetypes-provider ALL DEPENDS "${BASETYPES_PROVIDER}")

if(_STACKSCAN_DEP_TARGETS)
    add_dependencies(basetypes-provider ${_STACKSCAN_DEP_TARGETS})
endif()

# ---------------------------------------------------------------------------
# Tests (each tests/*.py is registered as an individual ctest test).
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# The test fixture libs are compiled with allocscc -> alaska, so they too need
# the freshly-built liballocs/alaska when BUILD_STACKSCAN is on.
if(_STACKSCAN_DEP_TARGETS AND TARGET test-libs)
    add_dependencies(test-libs ${_STACKSCAN_DEP_TARGETS})
endif()

# Convenience `check` target: build the extension, the base-types provider and
# the test fixture libs, then run the suite (matches the old automake `check`).
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS build-python test-libs basetypes-provider
    USES_TERMINAL)

# ---------------------------------------------------------------------------
# Benchmarks (opt-in). Added after tests so BASETYPES_PROVIDER / LINKPY_AUDIT_SO /
# _LINKPY_BUILD_TARGET / _STACKSCAN_DEP_TARGETS are all in scope.
# ---------------------------------------------------------------------------
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
