#!/bin/sh
# $1 = a linkpy test binary. Run it twice with a fresh cache directory and a
# build command that logs every compiler invocation: the second process must
# find the linkstub in the cache, so the compiler runs exactly once.
set -e
main=$1
here=$(cd "$(dirname "$0")" && pwd)
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT
export LINKPY_CACHE_DIR=$cache
export LINKSTUB_BUILD_CMD="sh $here/logged_build.sh $cache/compiles.log $LINKSTUB_BUILD_CMD"
"$main" > /dev/null
"$main" > /dev/null
n=$(wc -l < "$cache/compiles.log")
echo "compiler invocations across two processes: $n"
[ "$n" -eq 1 ]
