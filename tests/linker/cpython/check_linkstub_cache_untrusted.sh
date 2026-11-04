#!/bin/sh
# $1 = a linkpy test binary. Populate a fresh cache, then make the stored
# linkstub writable by others: the auditor must refuse to load it and compile
# again (so the compiler runs twice), and the program must still work.
set -e
main=$1
here=$(cd "$(dirname "$0")" && pwd)
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT
export LINKPY_CACHE_DIR=$cache
export LINKPY_TEST_COMPILE_LOG=$cache/compiles.log
export LINKSTUB_BUILD_CMD="sh $here/logged_build.sh $LINKSTUB_BUILD_CMD"
"$main" > /dev/null
chmod o+w "$cache"/*/liblinkstub.so
"$main" > /dev/null
n=$(wc -l < "$cache/compiles.log")
echo "compiler invocations with a tampered cache entry: $n"
[ "$n" -eq 2 ]
