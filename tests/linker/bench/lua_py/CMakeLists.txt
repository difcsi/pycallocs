# Baseline routes of the Lua -> Python per-call benchmark
# (tests/linker/bench/cross_bench.py, README.md there). Each is a ctest that
# runs benchloop.lua against the callee of the by-value cross fixture and
# prints the same `bench <workload>: <us> us/call` lines as the PolyLink
# routes (cross_bench_lua_py*, tests/linker/cross/). None of them runs under
# the auditor or the liballocs preload.
#
#   bench_lua_py_hand   a Lua C module written against the CPython API

if(NOT LINKPY_LUA_INTERP)
    message(STATUS "linkpy: bench_lua_py_* baselines skipped (no lua interpreter found)")
    return()
endif()

set(bench_callee_dir "${LINKPY_TESTS_DIR}/cross/bench_lua_py/src")

# A Lua C module <name>.so in this build directory, compiled as the linkstub
# is (-O2 -g). Like every Lua C module it leaves the lua_* symbols to the
# interpreter that loads it.
function(add_bench_lua_module target name)
    add_library(${target} MODULE ${ARGN})
    set_target_properties(${target} PROPERTIES
        PREFIX "" OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(${target} PRIVATE -O2 -g)
    target_include_directories(${target} PRIVATE ${LUA_INCLUDE_DIRS})
endfunction()

# add_bench_lua_route(<test> <module target> <app.lua>)
function(add_bench_lua_route test target app)
    add_test(NAME ${test}
        COMMAND "${LINKPY_LUA_INTERP}" "${CMAKE_CURRENT_SOURCE_DIR}/${app}")
    set_tests_properties(${test} PROPERTIES
        ENVIRONMENT "PYTHONPATH=${bench_callee_dir};BENCH_MODULE_DIR=$<TARGET_FILE_DIR:${target}>")
endfunction()

add_bench_lua_module(bench_handbridge handbridge handbridge.c)
target_include_directories(bench_handbridge PRIVATE ${PY_INCLUDES})
target_link_libraries(bench_handbridge PRIVATE ${PYTHON_LIB} ${PY_SYSLIBS})
add_bench_lua_route(bench_lua_py_hand bench_handbridge app_hand.lua)
