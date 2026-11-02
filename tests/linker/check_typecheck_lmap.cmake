# Drive a typecheck tool in --lmap stdin mode (the dwarfmap/linker path) and
# assert the loaded module is a good candidate. Language-agnostic: the tool and
# source directory are passed in, so both pytypecheck and luatypecheck use it.
#
# Invoked via `cmake -P`. Inputs (via -D):
#   PYTHON   python interpreter
#   TOOL     path to the typecheck tool (pytypecheck.py / luatypecheck.py)
#   SRC      directory of the sources to check
#
# The signatures below are the colon-delimited FunLinkEntry::to_string() form the
# auditor / dwarfmap recover from DWARF for the shared `good` fixture (the Python
# and Lua good fixtures define the same add/make_point/bump contract), fed in on
# stdin exactly as the *linkchk runner would pipe them.

set(lmap
"mod:add:int:int:a:int:b
mod:make_point:struct Point:int:x:int:y
mod:bump:void:struct Point *:p
")

# execute_process can only redirect stdin from a file, so stage the signatures.
set(lmap_file "${CMAKE_CURRENT_BINARY_DIR}/typecheck_lmap.in")
file(WRITE "${lmap_file}" "${lmap}")

execute_process(
    COMMAND "${PYTHON}" "${TOOL}" --src "${SRC}" --lmap - --strict
    INPUT_FILE "${lmap_file}"
    RESULT_VARIABLE rc)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "typecheck (lmap mode) exited ${rc}; expected a good candidate")
endif()
message(STATUS "typecheck lmap OK")
