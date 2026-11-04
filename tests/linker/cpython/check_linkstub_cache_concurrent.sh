#!/bin/sh
# $1 = a linkpy test binary. Several processes starting on a cold cache at
# once: one compiles, the others wait for it, so the compiler runs once and
# every process works.
set -e
main=$1
here=$(cd "$(dirname "$0")" && pwd)
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT
export LINKPY_CACHE_DIR=$cache
export LINKPY_TEST_COMPILE_LOG=$cache/compiles.log
export LINKSTUB_BUILD_CMD="sh $here/logged_build.sh $LINKSTUB_BUILD_CMD"
fails=0
for i in 1 2 3 4; do
    ( "$main" > /dev/null 2>"$cache/err.$i" || echo fail > "$cache/fail.$i" ) &
done
wait
for i in 1 2 3 4; do [ -e "$cache/fail.$i" ] && fails=$((fails + 1)); done
n=$(wc -l < "$cache/compiles.log")
echo "compiler invocations for 4 concurrent cold starts: $n, failures: $fails"
[ "$n" -eq 1 ] && [ "$fails" -eq 0 ]
