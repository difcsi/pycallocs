#!/bin/sh
# $1 = a linkpy test binary. Start it with large environment entries placed
# FIRST, together bigger than the auditor's snapshot buffer (256 KiB; a
# single entry cannot exceed the kernel's 128 KiB): the one that does not
# fit is skipped with a note on stderr and the settings after it are still
# captured, so the program works.
set -e
main=$1
cache=$(mktemp -d)
trap 'rm -rf "$cache"' EXIT
big=$(head -c 100000 /dev/zero | tr '\0' x)
out=$(env -i BIG1="$big" BIG2="$big" BIG3="$big" LD_AUDIT="$LD_AUDIT" TEMPLATE_PATH="$TEMPLATE_PATH" \
    INIT_DWARF_PATH="$INIT_DWARF_PATH" LINKSTUB_BUILD_CMD="$LINKSTUB_BUILD_CMD" \
    LINKPY_CACHE_DIR="$cache" PATH="$PATH" HOME="$HOME" "$main" 2>&1)
echo "$out" | grep -i 'truncat' > /dev/null || { echo "no truncation notice:"; echo "$out" | tail -3; exit 1; }
echo "oversized entry skipped with a notice; program ran"
