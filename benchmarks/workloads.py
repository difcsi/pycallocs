"""Canonical workload list shared by every backend and the orchestrator.

A *workload* is one unit of cross-language work with a fixed, checkable result.
Each backend adapter (backends/bench_*.py) implements the subset it can express
idiomatically; cells a backend does not implement show as N/A in the report.

The single source of truth here is: the ordered names, the one-line description,
and the EXPECTED result. `--check` runs every backend once and asserts its
produced value equals EXPECTED, so a "fast" backend cannot win by doing less.
"""

# Element count for the array_index workload. Sum of 0..N-1 is the expected value.
ARRAY_N = 64

# Ordered so the report reads from cheapest/clearest to most revealing.
ORDER = [
    "scalar_int",
    "scalar_fp",
    "byval_ret",
    "byval_arg",
    "byptr_arg",
    "bigstruct_ret",
    "field_read",
    "field_write",
    "array_index",
]

DESCRIPTIONS = {
    "scalar_int":    "int triple(int) -> int           : pure per-call overhead",
    "scalar_fp":     "double mul(double,double)         : float marshalling",
    "byval_ret":     "struct make_point(int,int)        : by-value struct RETURN (SPECIALISE path)",
    "byval_arg":     "long consume_point(struct)        : by-value struct ARG (coerce path)",
    "byptr_arg":     "long sum_point(struct*)           : by-pointer arg (lazy proxy)",
    "bigstruct_ret": "struct make_wide(int) [12 fields] : conversion cost vs field count",
    "field_read":    "read two fields of a struct       : getfield descriptor",
    "field_write":   "write two fields of a struct      : setfield descriptor",
    "array_index":   f"sum {ARRAY_N} ints by indexing      : element access + bounds",
}

EXPECTED = {
    "scalar_int":    21,
    "scalar_fp":     3.0,
    "byval_ret":     7,
    "byval_arg":     7,
    "byptr_arg":     7,
    "bigstruct_ret": 66,                       # sum 0..11
    "field_read":    7,
    "field_write":   30,                       # 10 + 20
    "array_index":   (ARRAY_N * (ARRAY_N - 1)) // 2,   # sum 0..N-1 = 2016
}


def matches(name, value, *, rel=1e-9):
    """True if `value` equals the expected result for workload `name`."""
    exp = EXPECTED[name]
    if isinstance(exp, float) or isinstance(value, float):
        try:
            return abs(float(value) - float(exp)) <= rel * max(1.0, abs(float(exp)))
        except (TypeError, ValueError):
            return False
    return value == exp
