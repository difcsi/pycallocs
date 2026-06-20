# ---------------------------------------------------------------------------
# pycallocs benchmark suite (opt-in: -DBUILD_BENCHMARKS=ON).
#
# Builds the fixture library two ways from one source and the hand-written native
# extension, then exposes run targets. Reuses the same path/env discovery as the
# test suite (LIBALLOCS_DIR, ALLOCSCC, BASETYPES_PROVIDER, LINKPY_AUDIT_SO, PYTHON
# are all set by the parent CMakeLists before this is added).
# ---------------------------------------------------------------------------
set(BENCH_DIR     "${CMAKE_CURRENT_SOURCE_DIR}")
set(BENCH_LIB_DIR "${BENCH_DIR}/libs/bench")
set(BENCH_SRC     "${BENCH_LIB_DIR}/bench.c")
set(ALLOCSCC      "${LIBALLOCS_DIR}/tools/lang/c/bin/allocscc")

set(BENCH_ALLOCS_SO "${BENCH_LIB_DIR}/bench.so")              # for pycallocs (DWARF+uniqtypes)
set(BENCH_PLAIN_SO  "${BENCH_LIB_DIR}/libbench_plain.so")     # for ctypes / cffi / native

# bench.so via allocscc -- identical recipe to tests/CMakeLists.txt so it loads
# the same way under the liballocs runtime.
add_custom_command(
    OUTPUT "${BENCH_ALLOCS_SO}"
    COMMAND ${CMAKE_COMMAND} -E env "LIBALLOCS=${LIBALLOCS_DIR}"
            "ALLOCSCC_TRAP_PTR_WRITES=1"
            "${ALLOCSCC}" -std=gnu11 -g -fPIC -shared
            "-I${LIBALLOCS_DIR}/include" "${BENCH_SRC}" -o "${BENCH_ALLOCS_SO}"
    DEPENDS "${BENCH_SRC}"
    COMMENT "Building bench.so with allocscc (pycallocs fixture)"
    VERBATIM)

# libbench_plain.so via the plain C compiler -- the baselines' natural build.
add_custom_command(
    OUTPUT "${BENCH_PLAIN_SO}"
    COMMAND "${CMAKE_C_COMPILER}" -O2 -g -fPIC -shared
            "${BENCH_SRC}" -o "${BENCH_PLAIN_SO}"
    DEPENDS "${BENCH_SRC}"
    COMMENT "Building libbench_plain.so with ${CMAKE_C_COMPILER} -O2 (baseline fixture)"
    VERBATIM)

# Hand-written CPython extension (the ceiling), linked against the plain fixture.
execute_process(COMMAND "${PYTHON}" -c "import sysconfig;print(sysconfig.get_path('include'))"
                OUTPUT_VARIABLE PY_INC OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${PYTHON}" -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))"
                OUTPUT_VARIABLE PY_EXT_SUFFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT PY_EXT_SUFFIX)
    set(PY_EXT_SUFFIX ".so")
endif()
set(NATIVE_SRC "${BENCH_DIR}/backends/native/benchnative.c")
set(NATIVE_SO  "${BENCH_DIR}/backends/native/benchnative${PY_EXT_SUFFIX}")
add_custom_command(
    OUTPUT "${NATIVE_SO}"
    COMMAND "${CMAKE_C_COMPILER}" -O2 -g -fPIC -shared
            "-I${PY_INC}" "-I${BENCH_LIB_DIR}"
            "${NATIVE_SRC}" -L"${BENCH_LIB_DIR}" -lbench_plain
            "-Wl,-rpath,${BENCH_LIB_DIR}" -o "${NATIVE_SO}"
    DEPENDS "${NATIVE_SRC}" "${BENCH_PLAIN_SO}"
    COMMENT "Building native baseline extension benchnative${PY_EXT_SUFFIX}"
    VERBATIM)

add_custom_target(bench-libs DEPENDS
    "${BENCH_ALLOCS_SO}" "${BENCH_PLAIN_SO}" "${NATIVE_SO}")

# The allocscc fixture needs the freshly-built liballocs/alaska when building them
# from source (BUILD_STACKSCAN=ON).
if(_STACKSCAN_DEP_TARGETS)
    add_dependencies(bench-libs ${_STACKSCAN_DEP_TARGETS})
endif()

# ---------------------------------------------------------------------------
# Run targets. All depend on the extension, the fixtures, the base-types provider
# and (when built from source) the linkpy auditor -- the same prerequisites the
# tests have. The orchestrator owns per-backend environment setup.
# ---------------------------------------------------------------------------
set(_bench_deps bench-libs build-python basetypes-provider ${_LINKPY_BUILD_TARGET})
set(_bench_run "${PYTHON}" "${BENCH_DIR}/run_benchmarks.py" --build-dir "${CMAKE_BINARY_DIR}")

add_custom_target(bench
    COMMAND ${_bench_run} --out "${BENCH_DIR}/report.md"
    WORKING_DIRECTORY "${BENCH_DIR}" DEPENDS ${_bench_deps}
    COMMENT "Running full pycallocs benchmark suite" USES_TERMINAL)

add_custom_target(bench-quick
    COMMAND ${_bench_run} --quick --out "${BENCH_DIR}/report-quick.md"
    WORKING_DIRECTORY "${BENCH_DIR}" DEPENDS ${_bench_deps}
    COMMENT "Running quick pycallocs benchmark smoke run" USES_TERMINAL)

add_custom_target(bench-check
    COMMAND ${_bench_run} --check
    WORKING_DIRECTORY "${BENCH_DIR}" DEPENDS ${_bench_deps}
    COMMENT "Validating identical results across all benchmark backends" USES_TERMINAL)

add_custom_target(bench-coldstart
    COMMAND ${_bench_run} --coldstart
    WORKING_DIRECTORY "${BENCH_DIR}" DEPENDS ${_bench_deps}
    COMMENT "Measuring pycallocs cold-start / first-call latency" USES_TERMINAL)
