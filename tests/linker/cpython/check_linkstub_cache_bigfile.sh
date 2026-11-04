#!/bin/sh
# $1 = a linkpy test binary. A large file the build command names (here the
# Python library it links, copied aside) is keyed by its path, size and
# modification time, not its contents: a process start must not hash
# megabytes, and a library that was replaced (new mtime) must still miss.
set -e
main=$1
here=$(cd "$(dirname "$0")" && pwd)
cache=$(mktemp -d)
work=$(mktemp -d)
trap 'rm -rf "$cache" "$work"' EXIT
lib=$(echo "$LINKSTUB_BUILD_CMD" | tr ' ' '\n' | grep '/libpython[0-9.]*\.so$' | head -1)
[ -n "$lib" ] || { echo "the build command names no libpython*.so"; exit 1; }
cp "$lib" "$work/$(basename "$lib")"
cmd=$(echo "$LINKSTUB_BUILD_CMD" | sed "s|$lib|$work/$(basename "$lib")|")
export LINKPY_CACHE_DIR=$cache
export LINKPY_TEST_COMPILE_LOG=$cache/compiles.log
export LINKSTUB_BUILD_CMD="sh $here/logged_build.sh $cmd"
"$main" > /dev/null
"$main" > /dev/null
n=$(wc -l < "$cache/compiles.log")
echo "compiler invocations after two starts: $n"
[ "$n" -eq 1 ]
touch "$work/$(basename "$lib")"     # same contents, new modification time
"$main" > /dev/null
n=$(wc -l < "$cache/compiles.log")
echo "compiler invocations after the library's mtime changed: $n"
[ "$n" -eq 2 ]
