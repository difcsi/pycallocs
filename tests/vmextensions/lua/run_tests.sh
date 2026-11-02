#!/bin/bash
# luaproxy test runner. Mirrors the discipline of tests/run_test.cmake: each
# tests/lua/<name>/<name>.lua (in the top-level pycallocs tests/ folder) runs
# under luahost with the liballocs preload; its stdout is byte-compared
# against the sibling <name>.out when present, otherwise the script only has
# to exit 0. stderr is kept as a sidecar file under tests/vmextensions/lua/build/out.
#
# Uses the same fixtures as the Python suite: the allocscc-compiled .so files
# in the main build tree (build/tests/libs), the basetypes provider meta-DSO
# and the shared META_BASE directory. Set PYCALLOCS_BUILD to point elsewhere.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

# --stock: run through the vendored contrib/lua interpreter + require
# "luaproxy" (package.cpath -> the luaproxy build dir) instead of the embedded luahost.
USE_STOCK=0
if [ "${1:-}" = "--stock" ]; then USE_STOCK=1; shift; fi

PYCALLOCS_BUILD="${PYCALLOCS_BUILD:-$ROOT/build}"
STACKSCAN="${STACKSCAN:-$ROOT/contrib/stackscan}"
LIBALLOCS="${LIBALLOCS:-$STACKSCAN/contrib/liballocs}"
LUAPROXY_BUILD="$ROOT/vmextensions/lua/build"
LUAHOST="${LUAHOST:-$LUAPROXY_BUILD/luahost}"

BASETYPES_PROVIDER="$PYCALLOCS_BUILD/basetypes/libbasetypes_provider.so"
META_BASE="$PYCALLOCS_BUILD/tests/meta"
PYCALLOCS_TEST_LIBS="$PYCALLOCS_BUILD/tests/libs"
LUA_TESTS_DIR="$ROOT/tests/lua"

die() { echo "run_tests.sh: $*" >&2; exit 2; }
[ -x "$LUAHOST" ] || die "luahost not built (expected $LUAHOST; run cmake first)"
[ -e "$BASETYPES_PROVIDER" ] || die "basetypes provider missing: $BASETYPES_PROVIDER"
[ -d "$PYCALLOCS_TEST_LIBS" ] || die "fixture dir missing: $PYCALLOCS_TEST_LIBS"
[ -d "$LUA_TESTS_DIR" ] || die "no Lua tests at $LUA_TESTS_DIR"

export LD_PRELOAD="$LIBALLOCS/lib/liballocs_preload.so:$BASETYPES_PROVIDER"
export LD_LIBRARY_PATH="$LIBALLOCS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288
export META_BASE
export PYCALLOCS_TEST_LIBS

mkdir -p "$HERE/build/out"
pass=0; fail=0
for script in "$LUA_TESTS_DIR"/*/*.lua; do
    [ -e "$script" ] || die "no test scripts in $LUA_TESTS_DIR"
    name="$(basename "${script%.lua}")"
    # Only run tests explicitly listed as arguments, when any are given
    if [ $# -gt 0 ]; then
        keep=""
        for want in "$@"; do [ "$want" = "$name" ] && keep=1; done
        [ -n "$keep" ] || continue
    fi
    out="$HERE/build/out/$name.stdout"
    errf="$HERE/build/out/$name.stderr"
    if [ $USE_STOCK -eq 1 ]; then
        LUA_CPATH="$LUAPROXY_BUILD/?.so" "$ROOT/contrib/lua/lua" "$script" >"$out" 2>"$errf"
    else
        "$LUAHOST" "$script" >"$out" 2>"$errf"
    fi
    status=$?
    expected="$(dirname "$script")/$name.out"
    if [ $status -ne 0 ]; then
        echo "FAIL $name (exit $status; stderr: $errf)"
        sed -n 1,5p "$errf" | sed 's/^/    /'
        fail=$((fail+1))
    elif [ -e "$expected" ] && ! cmp -s "$out" "$expected"; then
        echo "FAIL $name (output mismatch)"
        diff "$expected" "$out" | sed -n 1,10p | sed 's/^/    /'
        fail=$((fail+1))
    else
        echo "PASS $name"
        pass=$((pass+1))
    fi
done

echo "----"
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
