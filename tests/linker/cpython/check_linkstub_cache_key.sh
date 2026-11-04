#!/bin/sh
# $1 = a linkpy test binary. The cache key must cover the marshalling source
# the build command compiles: edit it and the next process compiles again.
# Entries older than the eviction age disappear when a new one is stored.
set -e
main=$1
here=$(cd "$(dirname "$0")" && pwd)
cache=$(mktemp -d)
work=$(mktemp -d)
trap 'rm -rf "$cache" "$work"' EXIT
src=$(echo "$LINKSTUB_BUILD_CMD" | tr ' ' '\n' | grep '/pycpputils\.cpp$' | head -1)
cp "$src" "$work/pycpputils.cpp"
cmd=$(echo "$LINKSTUB_BUILD_CMD" | sed "s|$src|$work/pycpputils.cpp|")
export LINKPY_CACHE_DIR=$cache
export LINKPY_TEST_COMPILE_LOG=$cache/compiles.log
export LINKSTUB_BUILD_CMD="sh $here/logged_build.sh $cmd"
"$main" > /dev/null
mkdir -p "$cache/0000000000000000000000000000000000000000000000000000000000000000"
touch -d '40 days ago' "$cache/0000000000000000000000000000000000000000000000000000000000000000/liblinkstub.so" \
    "$cache/0000000000000000000000000000000000000000000000000000000000000000"
echo "// edited" >> "$work/pycpputils.cpp"
"$main" > /dev/null
n=$(wc -l < "$cache/compiles.log")
echo "compiler invocations after editing the marshaller source: $n"
[ "$n" -eq 2 ]
if [ -e "$cache/0000000000000000000000000000000000000000000000000000000000000000" ]; then
    echo "stale entry not evicted"; exit 1
fi
echo "stale entry evicted"
