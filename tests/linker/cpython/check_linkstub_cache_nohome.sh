#!/bin/sh
# $1 = a linkpy test binary. With neither LINKPY_CACHE_DIR nor HOME set (a
# service started by an init system), the cache falls back to a per-user
# directory under TMPDIR, so a second process still skips the compiler.
set -e
main=$1
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
unset LINKPY_CACHE_DIR
unset HOME
export TMPDIR=$tmp
export LINKPY_TEST_COMPILE_LOG=$tmp/compiles.log
export LINKSTUB_BUILD_CMD="sh $here/logged_build.sh $LINKSTUB_BUILD_CMD"
"$main" > /dev/null
"$main" > /dev/null
n=$(wc -l < "$tmp/compiles.log")
echo "compiler invocations without HOME: $n (cache: $(ls "$tmp" | grep linkpy-cache || echo none))"
[ "$n" -eq 1 ]
