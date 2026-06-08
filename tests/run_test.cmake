# Driver for a single pycallocs Python test (ported from tests/Makefile).
#
# Runs the test script, merging stdout+stderr (the old Makefile used `2>&1`).
# If EXPECTED is given, the captured output must match it byte-for-byte;
# otherwise the test only needs to exit 0.
#
# Required -D vars: PYTHON, TESTPY, OUTFILE
# Optional -D var : EXPECTED
# LD_PRELOAD / PYTHONPATH and the working directory are supplied by ctest.

execute_process(
    COMMAND "${PYTHON}" "${TESTPY}"
    OUTPUT_FILE "${OUTFILE}"
    ERROR_FILE  "${OUTFILE}"
    RESULT_VARIABLE rc)

file(READ "${OUTFILE}" actual)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Test '${TESTPY}' failed (exit ${rc}):\n${actual}")
endif()

if(DEFINED EXPECTED AND NOT EXPECTED STREQUAL "")
    file(READ "${EXPECTED}" expected)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "Output differs for '${TESTPY}'.\n"
            "--- expected (${EXPECTED}) ---\n${expected}"
            "--- actual ---\n${actual}")
    endif()
endif()
