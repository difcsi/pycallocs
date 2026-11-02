# Lua test suite. Mirror image of ../cpython/: the same value tests and
# typecheck tests, registered through the same helpers (add_linkpy_test /
# add_linkpy_typecheck_tests defined in ../tests.cmake) with LANG lua. Only
# wired when liblua was found (guarded in linker/CMakeLists.txt).
#
# Each value test runs a C main, linked against a Lua stub library, that calls Lua
# functions through the JIT-compiled liblinkstub.so under LD_AUDIT=audit_lua.so:
#   lua_simple_divide — scalar args + number return.
#   lua_struct        — by-value struct RETURN (table -> struct, reflection).
#   lua_struct_arg    — by-value struct ARGUMENT (struct -> table, reflection).
#   lua_generated_header — header derived from ---@ hints by luahgen (no
#                          hand-written .lua.h): the full Lua pipeline end to end.

add_linkpy_test(simple_divide LANG lua)
# dlopen_divide: the Lua mirror of the Python dlopen test -- resolves divide() at
# runtime via dlopen + dlsym instead of load-time linking (see ../cpython).
add_linkpy_test(dlopen_divide LANG lua DLOPEN)
add_linkpy_test(struct LANG lua)
add_linkpy_test(struct_arg LANG lua)
add_linkpy_test(generated_header LANG lua GENERATE_HEADERS)

add_linkpy_typecheck_tests(LANG lua)
