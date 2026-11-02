"""Never runs to completion: the auditor aborts during linkpy.load's dlopen,
rejecting the pointer parameter in shapes.lua.h (v1 crossing is by value
only). The test passes on the auditor's diagnostic, not this script."""
import linkpy

linkpy.load("shapes")
raise SystemExit("unexpectedly loaded a pointer-taking cross-language module")
