# Cross-language (VM host -> VM callee) fixtures, registered through
# add_linkpy_cross_test (tests/linker/tests.cmake). The whole directory is only
# added when Lua was found (linker/CMakeLists.txt): every combination needs Lua
# on one side or the other.

add_linkpy_cross_test(py_calls_lua_divide HOST python CALLEE lua)
add_linkpy_cross_test(py_calls_lua_point HOST python CALLEE lua)
add_linkpy_cross_test(lua_calls_py_divide HOST lua CALLEE python)
add_linkpy_cross_test(lua_calls_py_point HOST lua CALLEE python)

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
else()
    message(STATUS "linkpy: cross by-reference tests disabled; "
                   "-DLINKPY_BYREF_TESTS=ON (with liballocs paths) to enable")
endif()
