# Baseline routes of the Lua -> Python per-call benchmark
# (tests/linker/bench/cross_bench.py, README.md there). Each is a ctest that
# runs benchloop.lua against the callee of the by-value cross fixture and
# prints the same `bench <workload>: <us> us/call` lines as the PolyLink
# routes (cross_bench_lua_py*, tests/linker/cross/). None of them runs under
# the auditor or the liballocs preload.
#
#   bench_lua_py_hand   a Lua C module written against the CPython API
#   bench_lua_py_cffi   a Lua C module calling a library cffi generated in
#                       embedding mode (needs cffi for ${PYTHON})
#   bench_lua_py_lupa   Python hosting Lua through lupa; the loop still runs in
#                       Lua and calls Python (needs lupa, see
#                       LINKPY_BENCH_LUPA_PYTHON; run by pyrun.c)

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

# cffi, embedding mode: cffi_build.py generates and compiles libshapes_cffi.so,
# which exports the callee's functions as C; cffishim.so is the Lua half.
execute_process(COMMAND ${PYTHON} -c "import cffi"
    RESULT_VARIABLE bench_no_cffi OUTPUT_QUIET ERROR_QUIET)
if(bench_no_cffi)
    message(STATUS "linkpy: bench_lua_py_cffi skipped (${PYTHON} cannot import cffi; pip install cffi)")
else()
    set(bench_cffi_lib "${CMAKE_CURRENT_BINARY_DIR}/libshapes_cffi.so")
    add_custom_command(
        OUTPUT "${bench_cffi_lib}"
        COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/cffi_build.py"
                --out "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cffi_build.py"
        COMMENT "linkpy bench: generating libshapes_cffi.so with cffi"
        VERBATIM)
    add_custom_target(bench_shapes_cffi DEPENDS "${bench_cffi_lib}")
    add_bench_lua_module(bench_cffishim cffishim cffishim.c)
    add_dependencies(bench_cffishim bench_shapes_cffi)
    target_link_libraries(bench_cffishim PRIVATE "${bench_cffi_lib}")
    add_bench_lua_route(bench_lua_py_cffi bench_cffishim app_cffi.lua)
endif()

# lupa: the process is Python and Lua is the guest, but the timed loop is the
# same Lua loop calling the same Python functions. lupa is rarely installed
# system-wide, so the interpreter is configurable; the default is a venv in
# the build tree:
#   python3 -m venv <build>/bench/venv && <build>/bench/venv/bin/pip install lupa
set(LINKPY_BENCH_LUPA_PYTHON "${CMAKE_BINARY_DIR}/bench/venv/bin/python" CACHE FILEPATH
    "Python interpreter that can import lupa, for bench_lua_py_lupa")
set(bench_no_lupa TRUE)
if(EXISTS "${LINKPY_BENCH_LUPA_PYTHON}")
    execute_process(COMMAND "${LINKPY_BENCH_LUPA_PYTHON}" -c "import lupa"
        RESULT_VARIABLE bench_no_lupa OUTPUT_QUIET ERROR_QUIET)
endif()
if(bench_no_lupa)
    message(STATUS "linkpy: bench_lua_py_lupa skipped (${LINKPY_BENCH_LUPA_PYTHON} cannot import lupa; "
                   "create the venv and pip install lupa, or set LINKPY_BENCH_LUPA_PYTHON)")
else()
    # LINKPY_BENCH_LUPA_PYTHON only says where lupa is installed. The test is
    # run by pyrun, a python launcher linked against the libpython the other
    # routes embed: a distribution's python3 may be linked statically, and
    # then runs the same callee measurably faster (about 1.6x on this
    # callee). pyrun prints `info python=shared`, which the driver requires.
    execute_process(
        COMMAND "${LINKPY_BENCH_LUPA_PYTHON}" -c
                "import lupa, os; print(os.path.dirname(os.path.dirname(lupa.__file__)))"
        OUTPUT_VARIABLE bench_lupa_site OUTPUT_STRIP_TRAILING_WHITESPACE)
    add_executable(bench_pyrun pyrun.c)
    set_target_properties(bench_pyrun PROPERTIES
        OUTPUT_NAME pyrun RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(bench_pyrun PRIVATE -O2 -g)
    target_include_directories(bench_pyrun PRIVATE ${PY_INCLUDES})
    target_link_libraries(bench_pyrun PRIVATE ${PYTHON_LIB} ${PY_SYSLIBS})
    add_test(NAME bench_lua_py_lupa
        COMMAND bench_pyrun "${CMAKE_CURRENT_SOURCE_DIR}/app_lupa.py")
    set_tests_properties(bench_lua_py_lupa PROPERTIES
        ENVIRONMENT "PYTHONPATH=${bench_callee_dir}:${bench_lupa_site}")
endif()
