# Run a heap-hints generator and assert the emitted function names match.
#
# Invoked via `cmake -P`. Inputs (via -D):
#   EXPECTED   ";"-separated list of expected function names
#   OUT_DIR    directory to write heap_hints.txt into
# and exactly one generator selector:
#   PYSTUBGEN + INCLUDE_DIR   header-based: run `pystubgen INCLUDE_DIR OUT_DIR`
#   HEAPHINTS + BINARY        DWARF-based:  run `heaphints BINARY OUT_DIR/heap_hints.txt`
#
# The comparison ignores blank lines and '#' comments, so the file's header
# comment doesn't make the assertion brittle.

file(MAKE_DIRECTORY "${OUT_DIR}")
set(hints_file "${OUT_DIR}/heap_hints.txt")

if(DEFINED PYSTUBGEN)
    execute_process(
        COMMAND "${PYSTUBGEN}" "${INCLUDE_DIR}" "${OUT_DIR}/"
        RESULT_VARIABLE rc)
elseif(DEFINED HEAPHINTS)
    execute_process(
        COMMAND "${HEAPHINTS}" "${BINARY}" "${hints_file}"
        RESULT_VARIABLE rc)
else()
    message(FATAL_ERROR "check_heap_hints: set PYSTUBGEN or HEAPHINTS")
endif()

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "heap-hints generator exited ${rc}")
endif()
if(NOT EXISTS "${hints_file}")
    message(FATAL_ERROR "no heap_hints.txt produced at ${hints_file}")
endif()

file(STRINGS "${hints_file}" lines)
set(got "")
foreach(line ${lines})
    string(STRIP "${line}" line)
    if(line STREQUAL "")
        continue()
    endif()
    if(line MATCHES "^#")
        continue()
    endif()
    list(APPEND got "${line}")
endforeach()
list(SORT got)

set(expected ${EXPECTED})
list(SORT expected)

if(NOT got STREQUAL expected)
    message(FATAL_ERROR "heap-hints mismatch:\n  got:      [${got}]\n  expected: [${expected}]")
endif()
message(STATUS "heap-hints OK: [${got}]")
