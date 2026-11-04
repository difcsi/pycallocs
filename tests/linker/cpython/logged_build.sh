#!/bin/sh
# Records one line per invocation in $LINKPY_TEST_COMPILE_LOG, then runs the
# real linkstub build command. The log's path is not part of the command, so
# the cache key (which covers files named in the command) stays stable.
echo "$*" >> "$LINKPY_TEST_COMPILE_LOG"
exec sh -c "$*"
