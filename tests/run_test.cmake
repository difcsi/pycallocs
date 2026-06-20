# Driver for a single pycallocs Python test (ported from tests/Makefile).
#
# The preloaded liballocs runtime writes a lot of diagnostic tracing to stderr
# (MMAP traces, lazy pageindex mapping, the exit-time query summary, assorted
# warnings). That noise is deliberately kept -- it is invaluable when debugging
# -- but it must NOT affect the pass/fail comparison. The test programs print
# their real results to stdout, so we capture stdout and stderr SEPARATELY:
#   * stdout  -> OUTFILE, compared byte-for-byte against EXPECTED
#   * stderr  -> OUTFILE.stderr, retained as a sidecar for inspection
# (The old Makefile merged them with 2>&1, from before liballocs was this
# chatty.)
#
# Required -D vars: PYTHON, TESTPY, OUTFILE
# Optional -D var : EXPECTED
# LD_PRELOAD / LD_AUDIT / LD_LIBRARY_PATH / PYTHONPATH and the working
# directory are supplied by ctest.

set(ERRFILE "${OUTFILE}.stderr")

execute_process(
    COMMAND "${PYTHON}" "${TESTPY}"
    OUTPUT_FILE "${OUTFILE}"
    ERROR_FILE  "${ERRFILE}"
    RESULT_VARIABLE rc)

file(READ "${OUTFILE}" actual)

if(NOT rc EQUAL 0)
    # A crash/exception: the Python traceback is on stderr (amid the liballocs
    # trace), so surface it here to make the failure diagnosable.
    file(READ "${ERRFILE}" errtext)
    message(FATAL_ERROR
        "Test '${TESTPY}' failed (exit ${rc}).\n"
        "--- stdout ---\n${actual}"
        "--- stderr (liballocs diagnostics + any traceback) ---\n${errtext}")
endif()

if(DEFINED EXPECTED AND NOT EXPECTED STREQUAL "")
    file(READ "${EXPECTED}" expected)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "Output differs for '${TESTPY}'.\n"
            "--- expected (${EXPECTED}) ---\n${expected}"
            "--- actual (stdout) ---\n${actual}"
            "--- liballocs stderr kept at: ${ERRFILE} ---\n")
    endif()
endif()
