#!/bin/bash
# v8proxy test runner. Each tests/js/<name>/<name>.js runs under `node
# --expose-gc` with the liballocs preload; stdout is byte-compared against the
# sibling <name>.out when present, otherwise the script only has to exit 0.
# stderr is kept as a sidecar under tests/vmextensions/v8/build/out.
#
# Same fixtures as the Python/Lua suites: the allocscc-compiled .so files in
# the main build tree, the basetypes provider meta-DSO and the shared
# META_BASE. Set PYCALLOCS_BUILD to point elsewhere.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

PYCALLOCS_BUILD="${PYCALLOCS_BUILD:-$ROOT/build}"
LIBALLOCS="${LIBALLOCS:-$ROOT/contrib/liballocs}"
V8PROXY_MODULE="${V8PROXY_MODULE:-$ROOT/vmextensions/v8/build/v8proxy.node}"

BASETYPES_PROVIDER="$PYCALLOCS_BUILD/basetypes/libbasetypes_provider.so"
META_BASE="$PYCALLOCS_BUILD/tests/meta"
PYCALLOCS_TEST_LIBS="$PYCALLOCS_BUILD/tests/libs"
JS_TESTS_DIR="$ROOT/tests/js"

die() { echo "run_tests.sh: $*" >&2; exit 2; }
[ -e "$V8PROXY_MODULE" ] || die "v8proxy.node not built (expected $V8PROXY_MODULE)"
[ -e "$BASETYPES_PROVIDER" ] || die "basetypes provider missing: $BASETYPES_PROVIDER"
[ -d "$PYCALLOCS_TEST_LIBS" ] || die "fixture dir missing: $PYCALLOCS_TEST_LIBS"
[ -d "$JS_TESTS_DIR" ] || die "no JS tests at $JS_TESTS_DIR"

export LD_PRELOAD="$LIBALLOCS/lib/liballocs_preload.so:$BASETYPES_PROVIDER"
export LD_LIBRARY_PATH="$LIBALLOCS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288
export META_BASE
export PYCALLOCS_TEST_LIBS
export V8PROXY_MODULE

mkdir -p "$HERE/build/out"
pass=0; fail=0
for script in "$JS_TESTS_DIR"/*/*.js; do
    [ -e "$script" ] || die "no test scripts in $JS_TESTS_DIR"
    name="$(basename "${script%.js}")"
    if [ $# -gt 0 ]; then
        keep=""
        for want in "$@"; do [ "$want" = "$name" ] && keep=1; done
        [ -n "$keep" ] || continue
    fi
    out="$HERE/build/out/$name.stdout"
    errf="$HERE/build/out/$name.stderr"
    node --expose-gc "$script" >"$out" 2>"$errf"
    status=$?
    expected="$(dirname "$script")/$name.out"
    if [ $status -ne 0 ]; then
        echo "FAIL $name (exit $status; stderr: $errf)"
        grep -vE "lazily mapped|^====|liballocs summary|^-----|^queries|unrecognised heap|ld-linux|xed could not" "$errf" | sed -n 1,5p | sed 's/^/    /'
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
