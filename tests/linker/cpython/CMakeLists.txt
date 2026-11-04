# Python test suite. Mirrors ../lua/ — the value tests and typecheck tests are
# registered through the same helpers (add_linkpy_test / add_linkpy_typecheck_tests
# defined in ../tests.cmake) with LANG python. Python additionally carries the
# by-reference and heap-hints tests, which have no Lua analogue (see ../tests.cmake).

add_linkpy_test(simple_divide LANG python)
# dlopen_divide mirrors simple_divide but resolves divide() at RUNTIME via
# dlopen("./libinit.so") + dlsym instead of linking libinit.so at load time. The
# auditor's la_objsearch redirects the dlopen to liblinkstub.so (matched by
# basename), so the dlsym'd trampoline marshals into Python -- covering the
# runtime-loading path the same auditor already drives for load-time deps.
add_linkpy_test(dlopen_divide LANG python DLOPEN)
add_linkpy_test(struct LANG python)
# struct_arg passes a struct *argument* from C into Python (exercises
# T_to_PyObject + the reflexive argument marshalling), the mirror of struct.
add_linkpy_test(struct_arg LANG python)
# generated_header ships no hand-written .py.h: pyhgen.py derives it from the
# python type hints (incl. a struct), exercising header generation end to end.
add_linkpy_test(generated_header LANG python GENERATE_HEADERS)

add_linkpy_typecheck_tests(LANG python)

# ---------------------------------------------------------------------------
# Heap-hints tests (Python only): linkpy emits the Yukon heap-promotion hint list
# -- the functions whose pointer arguments escape into Python. Build-time checks
# (they run the generators directly, not main under LD_AUDIT). The shared hh
# fixture covers every case of the escape rule; both the header-derived
# (pystubgen) and DWARF-derived (heaphints) paths must agree: argv_dump, mirror, sum.
# ---------------------------------------------------------------------------
add_test(NAME python_heap_hints_headers COMMAND ${CMAKE_COMMAND}
    -DPYSTUBGEN=$<TARGET_FILE:pystubgen>
    -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/heap_hints/include/
    -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/heap_hints/headers
    "-DEXPECTED=argv_dump;mirror;sum"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_heap_hints.cmake)

# DWARF path: compile the fixture (with -g, named *.py.c so the stub decl-file
# filter accepts it) into a shared lib and run the standalone heaphints on it.
add_library(heap_hints_fixture SHARED heap_hints/dwarf/hh.py.c)
set_target_properties(heap_hints_fixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/heap_hints/dwarf")
# -O0 whatever the build type: at -O2 the DWARF no longer carries the escaping
# pointer parameters this fixture exists to show.
target_compile_options(heap_hints_fixture PRIVATE -g -O0)

add_test(NAME python_heap_hints_dwarf COMMAND ${CMAKE_COMMAND}
    -DHEAPHINTS=$<TARGET_FILE:heaphints>
    -DBINARY=$<TARGET_FILE:heap_hints_fixture>
    -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/heap_hints/dwarf
    "-DEXPECTED=argv_dump;mirror;sum"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_heap_hints.cmake)

# ---------------------------------------------------------------------------
# By-reference (ptr_arg) test: bump(struct Point *) -- Python adds 100 to p.x
# through the proxy; main asserts the write is visible back in C. Needs liballocs,
# so OFF by default (-DLINKPY_BYREF_TESTS=ON with the liballocs paths to enable).
# ---------------------------------------------------------------------------
if(LINKPY_BYREF_TESTS)
    add_linkpy_byref_test(ptr_arg)
else()
    message(STATUS "linkpy: by-reference (ptr_arg) test disabled; "
                   "-DLINKPY_BYREF_TESTS=ON (with liballocs paths) to enable")
endif()

# bytes_array: fixed-size char/unsigned char arrays inside by-value structs
# cross as Python bytes in both directions (embedded NULs survive; the
# Python -> C copy is zero-filled so C sees a NUL-terminated char[N]).
add_linkpy_test(bytes_array LANG python)
# str_field: char* / const char* FIELDS of a by-value struct arrive in Python
# as str (NULL -> None), the same mapping top-level char* arguments get.
add_linkpy_test(str_field LANG python)

# Benchmarks, run as ordinary tests (they pass on exit code and print their
# numbers; see them with `ctest -R python_bench -V`, LINKPY_BENCH_CALLS sets the
# iteration count). bench_divide is the fixed cost of one trampoline call;
# bench_structs the by-value structs carrying fixed-capacity arrays in both
# directions plus a flat struct of strings as an argument.
add_linkpy_test(bench_divide LANG python)
add_linkpy_test(bench_structs LANG python)

# rebind: each call sees the module as it is at that moment -- a function
# rebound in the module's namespace (what importlib.reload does) and a module
# object replaced in sys.modules -- so the callable may not be kept across calls.
add_linkpy_test(rebind LANG python)

# The auditor and the linkstub stay quiet unless debugging is requested: a
# long-running host (a web server) cannot afford a line printed per call.
set_tests_properties(python_simple_divide PROPERTIES
    FAIL_REGULAR_EXPRESSION "Hello from (divide|phelloworld)!|PyVM Initialized|Running function|Linker looked for libinit")

# gil_release: a Python thread started in one trampoline call keeps running
# while C works between calls, i.e. the linkstub parks the GIL when idle.
add_linkpy_test(gil_release LANG python)

# linkstub_cache: the compiled linkstub is cached by content hash, so a second
# process loading the same init library never invokes the compiler.
get_test_property(python_simple_divide ENVIRONMENT _simple_divide_env)
add_test(NAME python_linkstub_cache
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_linkstub_cache.sh"
            $<TARGET_FILE:python_simple_divide_main>)
set_tests_properties(python_linkstub_cache PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/simple_divide"
    ENVIRONMENT "${_simple_divide_env}")

# environ_moved: the host rewrites its environment strings (process-title
# style) before loading the init library; the auditor must have captured its
# settings at process start.
add_linkpy_test(environ_moved LANG python DLOPEN)

# env_after_load: the host sets PYTHONPATH only after loading the init
# library, so the linkstub's interpreter must start at the first call, not
# at load time (the module is only reachable through that PYTHONPATH).
add_linkpy_test(env_after_load LANG python DLOPEN)
set_property(TEST python_env_after_load APPEND PROPERTY ENVIRONMENT
    "LATE_MODULE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/env_after_load/elsewhere")
