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
target_compile_options(heap_hints_fixture PRIVATE -g)

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
