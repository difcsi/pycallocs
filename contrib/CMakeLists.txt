# Drive builds of third-party subprojects: cpython, liballocs, alaska.
# Path variables and validation live in the root CMakeLists.txt; only build targets
# and their dep-target lists are set here and propagated up via PARENT_SCOPE.

# ---------------------------------------------------------------------------
# cpython (bundled Python — configure + build when the in-tree binary is absent)
# ---------------------------------------------------------------------------
set(_CPYTHON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cpython")
set(_CPYTHON_PYTHON "${_CPYTHON_DIR}/python")

set(_CPYTHON_DEP_TARGET "")
if(NOT EXISTS "${_CPYTHON_PYTHON}")
    if(NOT EXISTS "${_CPYTHON_DIR}/configure")
        message(FATAL_ERROR
            "cpython binary not found and configure script missing at ${_CPYTHON_DIR}.\n"
            "Initialise the submodule: git submodule update --init contrib/cpython")
    endif()
    message(STATUS "cpython: python binary not found — will build from source")

    add_custom_command(
        OUTPUT "${_CPYTHON_DIR}/Makefile"
        COMMAND "${_CPYTHON_DIR}/configure" "--prefix=${_CPYTHON_DIR}/local" "CFLAGS=-fPIC"
        WORKING_DIRECTORY "${_CPYTHON_DIR}"
        COMMENT "Configuring CPython"
        VERBATIM)

    add_custom_command(
        OUTPUT "${_CPYTHON_PYTHON}"
        COMMAND ${CMAKE_MAKE_PROGRAM}
        COMMAND "${_CPYTHON_PYTHON}" -m ensurepip --upgrade
        COMMAND "${_CPYTHON_PYTHON}" -m pip install setuptools
        WORKING_DIRECTORY "${_CPYTHON_DIR}"
        DEPENDS "${_CPYTHON_DIR}/Makefile"
        COMMENT "Building CPython and bootstrapping pip"
        VERBATIM)

    add_custom_target(build-cpython ALL DEPENDS "${_CPYTHON_PYTHON}")
    set(_CPYTHON_DEP_TARGET build-cpython)
else()
    message(STATUS "cpython: using pre-built ${_CPYTHON_PYTHON}")
endif()
set(_CPYTHON_DEP_TARGET "${_CPYTHON_DEP_TARGET}" PARENT_SCOPE)

# ---------------------------------------------------------------------------
# liballocs + alaska (built from source when BUILD_LIBALLOCS / BUILD_ALASKA)
#
# These two subprojects have their own native build systems; CMake drives them
# through custom targets so a single `cmake --build` produces everything the
# tests need. Both targets invoke incremental builds, so re-running is cheap
# once the dependency is up to date.
#   * alaska    -> built via its own CMake preset matching ALASKA_OPT_TREE
#                  (tree alaska-<preset>), installed into
#                  contrib/alaska/opt/${ALASKA_OPT_TREE}. Requires the bundled
#                  LLVM-16 toolchain under contrib/alaska/opt (llvm/, gllvm/).
#   * liballocs -> built via autotools (configure --enable-lifetime-policies +
#                  make), producing contrib/liballocs/lib/liballocs_preload.so.
#                  Needs the OCaml 4.14 opam switch active for CIL.
# ---------------------------------------------------------------------------
set(ALASKA_BUILD_JOBS    2 CACHE STRING
    "Parallel jobs for the Alaska build (>2 can OOM the LLVM-heavy compile)")
set(LIBALLOCS_BUILD_JOBS 4 CACHE STRING
    "Parallel jobs for the liballocs make build")
set(OCAML_SWITCH         "4.14.2" CACHE STRING
    "opam switch activated for the liballocs build (CIL needs OCaml 4.14)")
set(LIBALLOCS_CONFIGURE_FLAGS "--enable-lifetime-policies" CACHE STRING
    "Flags passed to liballocs ./configure on a fresh configure")

set(_RUNTIME_DEP_TARGETS "")

if(BUILD_ALASKA)
    # Each alaska preset <preset> installs into opt/alaska-<preset>, so the tree
    # the rest of the build compiles and runs against names the preset to build.
    if(NOT ALASKA_OPT_TREE MATCHES "^alaska-(.+)$")
        message(FATAL_ERROR
            "ALASKA_OPT_TREE '${ALASKA_OPT_TREE}' is not of the form alaska-<preset> "
            "(<preset> from ${ALASKA_DIR}/CMakePresets.json); cannot build it.")
    endif()
    set(_ALASKA_PRESET    "${CMAKE_MATCH_1}")
    set(_ALASKA_OPT       "${ALASKA_DIR}/opt")
    set(_ALASKA_LLVM      "${_ALASKA_OPT}/llvm")
    set(_ALASKA_INSTALL   "${_ALASKA_OPT}/${ALASKA_OPT_TREE}")
    # PATH that exposes the bundled clang-16 and gclang to the Alaska sub-build.
    set(_ALASKA_BUILD_PATH "${_ALASKA_LLVM}/bin:${_ALASKA_OPT}/gllvm/bin:$ENV{PATH}")

    # Mirror build.sh: clang needs its resource dir on hand for some steps.
    set(_ALASKA_RESOURCE_DIR "")
    if(EXISTS "${_ALASKA_LLVM}/bin/clang")
        execute_process(
            COMMAND "${_ALASKA_LLVM}/bin/clang" -print-resource-dir
            OUTPUT_VARIABLE _ALASKA_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()

    add_custom_target(alaska ALL
        # ALASKA_LIBALLOCS_INSERT_RESERVE = sizeof(struct insert) (8). Reserves a
        # trailing liballocs insert in every sized Alaska object so handle objects
        # carry a per-object lifetime-policy mask (manual + GC). Must match the
        # _Static_assert in contrib/liballocs/src/allocators/alaska.c.
        COMMAND ${CMAKE_COMMAND} -E env
                    "PATH=${_ALASKA_BUILD_PATH}"
                    "LLVM_RESOURCE_DIR=${_ALASKA_RESOURCE_DIR}"
                ${CMAKE_COMMAND} --preset ${_ALASKA_PRESET} -S "${ALASKA_DIR}"
                    -DALASKA_LIBALLOCS_INSERT_RESERVE=8
                    # Force the preset's own values on the command line: a preset's
                    # cacheVariables only seed a FRESH cache and are silently ignored
                    # when build/<preset>/CMakeCache.txt already exists, whereas a
                    # -D entry is re-applied every configure. Without this a stale
                    # cache kept CMAKE_INSTALL_PREFIX=/usr/local -- so `--target
                    # install` tried to write /usr/local/include/alaska.h and failed
                    # with EACCES -- and ALASKA_ENABLE_TESTING=ON, rebuilding the
                    # googletest suite on every downstream build.
                    -DCMAKE_INSTALL_PREFIX=${_ALASKA_INSTALL}
                    -DALASKA_ENABLE_TESTING=OFF
        COMMAND ${CMAKE_COMMAND} -E env
                    "PATH=${_ALASKA_BUILD_PATH}"
                    "LLVM_RESOURCE_DIR=${_ALASKA_RESOURCE_DIR}"
                ${CMAKE_COMMAND} --build --preset ${_ALASKA_PRESET} --target install -j ${ALASKA_BUILD_JOBS}
        WORKING_DIRECTORY "${ALASKA_DIR}"
        COMMENT "Building Alaska (${_ALASKA_PRESET} preset) -> ${_ALASKA_INSTALL}"
        USES_TERMINAL
        VERBATIM
    )
    list(APPEND _RUNTIME_DEP_TARGETS alaska)
endif()

if(BUILD_LIBALLOCS)
    # Full liballocs build sequence (per its README) — written to a script so the
    # multi-step shell logic survives CMake's argument escaping:
    #   make -C contrib   build the bundled deps (libsystrap, liballocstool, cil, …)
    #   ./autogen.sh      regenerate ./configure        (only if not yet generated)
    #   . contrib/env.sh  export LIBSYSTRAP_DIR/CIL_INSTALL/… for configure & make
    #   ./configure ...   with --enable-lifetime-policies (only if not yet configured)
    #   make              build liballocs itself
    # autogen/configure are skipped when already done, so re-runs are incremental.
    set(_LIBALLOCS_BUILD_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/build_liballocs.sh")
    file(WRITE "${_LIBALLOCS_BUILD_SCRIPT}"
"#!/usr/bin/env bash
# Generated by CMake — builds liballocs with the lifetime-policies configuration.
set -e
cd \"${LIBALLOCS_DIR}\"
# CIL needs the OCaml 4.14 opam switch; ignore if opam is unavailable.
eval \"$(opam env --switch='${OCAML_SWITCH}' --set-switch 2>/dev/null)\" || true
make -C contrib -j${LIBALLOCS_BUILD_JOBS}
[ -f configure ] || ./autogen.sh
. contrib/env.sh
[ -f config.status ] || ./configure ${LIBALLOCS_CONFIGURE_FLAGS}
make -j${LIBALLOCS_BUILD_JOBS}
")
    add_custom_target(liballocs ALL
        COMMAND bash "${_LIBALLOCS_BUILD_SCRIPT}"
        WORKING_DIRECTORY "${LIBALLOCS_DIR}"
        COMMENT "Building liballocs (make -C contrib + autogen + configure ${LIBALLOCS_CONFIGURE_FLAGS} + make) -> ${LIBALLOCS_DIR}/lib/liballocs_preload.so"
        USES_TERMINAL
        VERBATIM
    )
    list(APPEND _RUNTIME_DEP_TARGETS liballocs)
endif()
set(_RUNTIME_DEP_TARGETS "${_RUNTIME_DEP_TARGETS}" PARENT_SCOPE)
