# Drive builds of third-party subprojects: cpython, stackscan (+ liballocs, alaska), linkpy.
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
# stackscan (builds liballocs + alaska from source when BUILD_STACKSCAN=ON)
# ---------------------------------------------------------------------------
set(_STACKSCAN_DEP_TARGETS "")
if(BUILD_STACKSCAN)
    add_subdirectory(stackscan)
    foreach(_t liballocs alaska stackscan)
        if(TARGET ${_t})
            list(APPEND _STACKSCAN_DEP_TARGETS ${_t})
        endif()
    endforeach()
endif()
set(_STACKSCAN_DEP_TARGETS "${_STACKSCAN_DEP_TARGETS}" PARENT_SCOPE)

# ---------------------------------------------------------------------------
# linkpy (build from source only when audit.so is absent)
# ---------------------------------------------------------------------------
set(_LINKPY_BUILD_TARGET "")
if(NOT EXISTS "${LINKPY_AUDIT_SO}")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/linkpy/CMakeLists.txt")
        message(FATAL_ERROR
            "linkpy not found at ${CMAKE_CURRENT_SOURCE_DIR}/linkpy and "
            "${LINKPY_AUDIT_SO} is missing.\n"
            "Initialise the submodule: git submodule update --init contrib/linkpy")
    endif()
    message(STATUS "linkpy: audit.so not found — building from source")
    add_subdirectory(linkpy)
    set(_LINKPY_BUILD_TARGET audit)
else()
    message(STATUS "linkpy: using pre-built ${LINKPY_AUDIT_SO}")
endif()
set(_LINKPY_BUILD_TARGET "${_LINKPY_BUILD_TARGET}" PARENT_SCOPE)
