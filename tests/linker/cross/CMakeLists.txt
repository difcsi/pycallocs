# Cross-language (VM host -> VM callee) fixtures, registered through
# add_linkpy_cross_test (tests/linker/tests.cmake). The whole directory is only
# added when Lua was found (linker/CMakeLists.txt): every combination needs Lua
# on one side or the other.

add_linkpy_cross_test(py_calls_lua_divide HOST python CALLEE lua)
add_linkpy_cross_test(py_calls_lua_point HOST python CALLEE lua)
add_linkpy_cross_test(lua_calls_py_divide HOST lua CALLEE python)
add_linkpy_cross_test(lua_calls_py_point HOST lua CALLEE python)

# Benchmark, not a behaviour test: prints `bench <workload>: <us> us/call` for
# the by-value route of tests/linker/bench/cross_bench.py and passes on exit
# code (the loop checks every warm-up result).
add_linkpy_cross_test(bench_lua_py HOST lua CALLEE python)

# Negative: a pointer parameter cannot cross the language boundary by default;
# the auditor must reject it before spending a JIT compile (asserted on the
# diagnostic via PASS_REGULAR_EXPRESSION -- the auditor exits nonzero).
add_linkpy_cross_test(py_calls_lua_reject_ptr HOST python CALLEE lua
    PASS_REGEX "by-reference does not cross language boundaries")

# By-reference crossing (LINKPY_CROSS_BYREF=1): the host VM's proxy hands its
# address through the C trampoline and the callee VM re-proxies it -- one live
# C object, two VMs. Needs the liballocs runtime + both proxy extensions, so
# gated like the C-host byref tests.
if(LINKPY_BYREF_TESTS)
    add_linkpy_cross_test(py_calls_lua_bump HOST python CALLEE lua BYREF)
    add_linkpy_cross_test(lua_calls_py_bump HOST lua CALLEE python BYREF)

    # Benchmarks (tests/linker/bench/cross_bench.py): one by-reference app,
    # run under stock Lua with the luaproxy extension and under the contrib/lua
    # fork, where the object handed over is a native foreign value.
    add_linkpy_cross_test(bench_lua_py_ref HOST lua CALLEE python BYREF)
    if(EXISTS "${LINKPY_POLYLUA_INTERP}")
        add_linkpy_cross_test(bench_lua_py_native HOST lua CALLEE python BYREF
            FIXTURE bench_lua_py_ref
            LUA_INTERP "${LINKPY_POLYLUA_INTERP}"
            LUA_HOST_CFLAGS "-I${PYCALLOCS_ROOT}/contrib/lua")
    else()
        message(STATUS "linkpy: cross_bench_lua_py_native skipped (no ${LINKPY_POLYLUA_INTERP})")
    endif()
else()
    message(STATUS "linkpy: cross by-reference tests disabled; "
                   "-DLINKPY_BYREF_TESTS=ON (with liballocs paths) to enable")
endif()
