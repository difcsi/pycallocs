cmake_minimum_required(VERSION 3.22)
project(pycallocs C)

# ---------------------------------------------------------------------------
# pycallocs builds a CPython extension module (`allocs`) that provides an
# invisible FFI on top of liballocs meta-information.
#
# liballocs and alaska are submodules under contrib/. By default they are
# built from source as part of this build (BUILD_LIBALLOCS / BUILD_ALASKA);
# with those off they are consumed as pre-built dependencies, and the build
# fails with build instructions when they are missing or not built.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# NOTE: the VM extensions are always built with the system compiler; the
# allocscc / alaska (yukon) toolchains apply only to test fixtures.
option(USE_ALASKA   "Use the Alaska handle runtime (fixtures transformed; extension handle-aware)" ON)
option(DEBUG        "Build the extension with -O0 and assertions enabled"            OFF)
option(SPECIALISE_CONVERSION
    "Generate & dlopen specialised PyObject_to_T<T> translators for by-pointer PyObject->struct conversion (needs g++-16 at runtime)"
    OFF)
option(INJECT_CONVERSION
    "DRAFT: like SPECIALISE_CONVERSION, but synthesise the translators via P3294 token-sequence injection instead of string-templated codegen (needs a P3294-capable g++-16 at runtime; mutually exclusive with SPECIALISE_CONVERSION)"
    OFF)
option(BUILD_LIBALLOCS
    "Build the liballocs submodule (contrib/liballocs) from source as part of this build, instead of consuming a pre-built copy"
    ON)
option(BUILD_ALASKA
    "Build the alaska submodule (contrib/alaska, the preset matching ALASKA_OPT_TREE) from source as part of this build, instead of consuming a pre-built copy"
    ON)
option(BUILD_BENCHMARKS
    "Build the benchmark fixtures + native baseline and expose the bench* run targets (benchmarks/)"
    OFF)

# ---------------------------------------------------------------------------
# Paths: liballocs, alaska and linkpy
# (LIBALLOCS_DIR / ALASKA_DIR / ALASKA_OPT_TREE must be set before
# add_subdirectory(contrib) so contrib can build them; linkpy is first-party
# code living in linker/)
# ---------------------------------------------------------------------------
set(LIBALLOCS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/contrib/liballocs"
    CACHE PATH "liballocs submodule root")
set(ALASKA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/contrib/alaska"
    CACHE PATH "alaska submodule root")
# Which of alaska's opt/ install trees to compile and run against (and, with
# BUILD_ALASKA, to build: tree alaska-<preset> comes from alaska's <preset>
# CMake preset). The trees differ in enabled runtime features (see alaska's
# CMakePresets.json):
#   alaska-anchorage             compaction only (no refcount API)
#   alaska-refcount-anchorage    + handle refcounts (no cycle collection)
#   alaska-refcount-gc-anchorage + refcounts, stack scan and cycle collection
#                                  (the Yukon configuration)
set(ALASKA_OPT_TREE "alaska-anchorage"
    CACHE STRING "alaska opt/ tree to use (e.g. alaska-refcount-gc-anchorage)")

set(LINKPY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/linker"
    CACHE PATH "linkpy (linker) root")
set(LINKPY_AUDIT_SO "${LINKPY_DIR}/dist/audit.so")

# ---------------------------------------------------------------------------
# Third-party builds: cpython, liballocs and alaska
# contrib/CMakeLists.txt drives the build of each subproject and
# exports _RUNTIME_DEP_TARGETS and _CPYTHON_DEP_TARGET to this scope.
# ---------------------------------------------------------------------------
add_subdirectory(contrib)

# ---------------------------------------------------------------------------
# linkpy (linker/ — build from source only when audit.so is absent)
# ---------------------------------------------------------------------------
set(_LINKPY_BUILD_TARGET "")
if(NOT EXISTS "${LINKPY_AUDIT_SO}")
    if(NOT EXISTS "${LINKPY_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "linkpy not found at ${LINKPY_DIR} and ${LINKPY_AUDIT_SO} is missing.")
    endif()
    message(STATUS "linkpy: audit.so not found — building from source")
    add_subdirectory("${LINKPY_DIR}" linkpy)
    set(_LINKPY_BUILD_TARGET audit)
else()
    message(STATUS "linkpy: using pre-built ${LINKPY_AUDIT_SO}")
endif()

# ---------------------------------------------------------------------------
# liballocs (contrib/liballocs submodule)
# ---------------------------------------------------------------------------
message(STATUS "Looking for liballocs in ${LIBALLOCS_DIR}")

# When BUILD_LIBALLOCS is on these artifacts are produced by the `liballocs`
# target during the build, so they legitimately don't exist at configure time.
if(NOT BUILD_LIBALLOCS)
    if(NOT EXISTS "${LIBALLOCS_DIR}/config.mk")
        message(FATAL_ERROR
            "liballocs not configured at ${LIBALLOCS_DIR}.\n"
            "Initialise its submodules and build it:\n"
            "  git submodule update --init --recursive contrib/liballocs\n"
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


# ---------------------------------------------------------------------------
# alaska (contrib/alaska submodule; ALASKA_OPT_TREE is set with the paths above)
# ---------------------------------------------------------------------------
set(ALASKA "${ALASKA_DIR}/opt/${ALASKA_OPT_TREE}/bin/alaska"
    CACHE FILEPATH "alaska compiler wrapper")
set(ALASKA_CLANG "${ALASKA_DIR}/opt/llvm/bin/clang"
    CACHE FILEPATH "alaska's bundled clang (without alaska transformation passes)")

if(USE_ALASKA)
    message(STATUS "Looking for alaska at ${ALASKA}")
    if(NOT BUILD_ALASKA AND NOT EXISTS "${ALASKA}")
        message(FATAL_ERROR
            "alaska not found at ${ALASKA}.\n"
            "Build it in ${ALASKA_DIR} first or set -DUSE_ALASKA=OFF.")
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
# Whether the Alaska runtime is in use. The handle transform (and the
# allocscc/alaska toolchains generally) apply ONLY to the test fixtures
# (tests/), whose malloc() must yield uniqtypes/real handles; the extension
# manipulates handles explicitly (ss_translate) and is always built with the
# system compiler. _ALASKA_ENABLED drives the SS_HAVE_ALASKA macro
# (handle_query.h), libalaska linking, and the transformed-fixture /
# relocate-test wiring.
# ---------------------------------------------------------------------------
# The VM extensions themselves are ALWAYS built with the system compiler --
# never with liballocs' allocscc nor with alaska's clang (yukon). Those
# toolchains apply only to the test fixtures (tests/), whose malloc() must
# yield uniqtypes/handles. USE_ALASKA still makes the extension handle-AWARE
# (SS_HAVE_ALASKA + libalaska link) without changing its compiler.
set(_ALASKA_ENABLED FALSE)
if(USE_ALASKA)
    set(_ALASKA_ENABLED TRUE)
    if(NOT BUILD_ALASKA AND NOT EXISTS "${ALASKA_CLANG}")
        message(FATAL_ERROR
            "alaska's clang not found at ${ALASKA_CLANG} (needed to compile "
            "alaska_translate.bc).\nBuild alaska first or set -DUSE_ALASKA=OFF.")
    endif()
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
                   "(gcc>=15 / clang>=19), "
                   "and g++-16 must be on PATH at runtime for translator compilation.")
endif()
if(INJECT_CONVERSION)
    list(APPEND _build_env "INJECT_CONVERSION=1")
    message(STATUS "INJECT_CONVERSION on (DRAFT P3294 token-injection back end): the "
                   "extension #embed's pycpputils.hpp and pyc_inject.hpp, so it must be "
                   "built with a C23 #embed-capable C compiler (gcc>=15 / clang>=19), "
                   "and a P3294-capable g++-16 "
                   "must be on PATH at runtime for translator compilation.")
endif()
if(_ALASKA_ENABLED)
    # Alaska runtime in use: make the extension handle-aware (handle_query.h keys
    # off SS_HAVE_ALASKA) and link libalaska so ss_translate() resolves. The
    # extension is still built WITHOUT the transform, by the system compiler.
    list(APPEND _build_env "SS_HAVE_ALASKA=1")
    list(APPEND _build_env "ALASKA_OPT_TREE=${ALASKA_OPT_TREE}")

    # alaska_translate()/pin/unpin are normally INLINED by the Alaska transform, so
    # libalaska exports no callable symbols for them. The extension is NOT
    # transformed, so it needs native definitions: compile Alaska's shipped
    # alaska_translate.bc (LLVM bitcode) to an object with alaska's clang and link
    # it into the extension (the "translate-native" mechanism in handle_query.h).
    set(_ALASKA_TRANSLATE_BC "${ALASKA_DIR}/opt/${ALASKA_OPT_TREE}/lib/alaska_translate.bc")
    set(_ALASKA_TRANSLATE_OBJ "${CMAKE_CURRENT_BINARY_DIR}/alaska_translate_native.o")
    add_custom_command(
        OUTPUT "${_ALASKA_TRANSLATE_OBJ}"
        COMMAND "${ALASKA_CLANG}" -c -fPIC -O2 "${_ALASKA_TRANSLATE_BC}"
                -o "${_ALASKA_TRANSLATE_OBJ}"
        DEPENDS "${_ALASKA_TRANSLATE_BC}"
        COMMENT "Compiling Alaska native translate object (alaska_translate.bc -> .o)"
        VERBATIM)
    add_custom_target(alaska-translate-obj DEPENDS "${_ALASKA_TRANSLATE_OBJ}")
    list(APPEND _build_env "ALASKA_TRANSLATE_OBJ=${_ALASKA_TRANSLATE_OBJ}")
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
if(_RUNTIME_DEP_TARGETS)
    add_dependencies(build-python ${_RUNTIME_DEP_TARGETS})
endif()
if(_ALASKA_ENABLED)
    add_dependencies(build-python alaska-translate-obj)
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

if(_RUNTIME_DEP_TARGETS)
    add_dependencies(basetypes-provider ${_RUNTIME_DEP_TARGETS})
endif()

# ---------------------------------------------------------------------------
# Tests (each tests/*.py is registered as an individual ctest test).
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# The test fixture libs are compiled with allocscc -> alaska, so they too need
# the freshly-built liballocs/alaska when BUILD_LIBALLOCS / BUILD_ALASKA are on.
if(_RUNTIME_DEP_TARGETS AND TARGET test-libs)
    add_dependencies(test-libs ${_RUNTIME_DEP_TARGETS})
endif()

# Convenience `check` target: build the extension, the base-types provider and
# the test fixture libs, then run the suite (matches the old automake `check`).
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS build-python test-libs basetypes-provider
    USES_TERMINAL)

# ---------------------------------------------------------------------------
# Benchmarks (opt-in). Added after tests so BASETYPES_PROVIDER / LINKPY_AUDIT_SO /
# _LINKPY_BUILD_TARGET / _RUNTIME_DEP_TARGETS are all in scope.
# ---------------------------------------------------------------------------
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
