#!/bin/bash
# Build and run the standalone liballocs bug reproducers.
# See docs/LIBALLOCS_BUGS.md for the analysis of each.
#
#   ./run.sh [LIBALLOCS_HOME]
#
# LIBALLOCS_HOME defaults to the pycallocs liballocs submodule. Each
# case prints REPRODUCED (the bug is present) or NOT-REPRODUCED.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
DEFAULT_LA="$HERE/../../contrib/liballocs"
LA="${1:-${LIBALLOCS_HOME:-$DEFAULT_LA}}"

PRELOAD="$LA/lib/liballocs_preload.so"
DUMMY="$LA/lib/liballocs_dummyweaks.so"
INCS="-I$LA/include -I$LA/contrib/libsystrap/contrib/librunt/include -I$LA/contrib/liballocstool/include"

[ -e "$PRELOAD" ] || { echo "no liballocs_preload.so at $PRELOAD" >&2; exit 2; }
CC="${CC:-cc}"

# link the meta stubs so undefined weak refs resolve; -Wl,-E mirrors
# the real client link (liballocs symbols exported for interposition)
LINK="-Wl,-E $PRELOAD $DUMMY"

echo "liballocs: $LA"
$CC -O1 -o "$HERE/bug1_mmap_realloc" "$HERE/bug1_mmap_realloc.c" $LINK || exit 2
$CC -O1 -o "$HERE/query"             "$HERE/query.c" $INCS $LINK 2>/dev/null || exit 2
$CC -O1 -o "$HERE/bug4_policy_oob"   "$HERE/bug4_policy_oob.c" $LINK || exit 2

echo
echo "== bug 1: realloc of an mmap-served chunk =="
"$HERE/bug1_mmap_realloc" >/dev/null 2>&1
[ $? -eq 139 ] && echo "  REPRODUCED (SIGSEGV)" || echo "  NOT-REPRODUCED"

echo
echo "== bug 2: LIBALLOCS_ERR=/dev/null crashes at startup =="
echo "  (ASLR-sensitive: run natively, not under gdb/setarch -R)"
LIBALLOCS_ERR=/dev/null "$HERE/query" >/dev/null 2>&1
[ $? -eq 139 ] && echo "  REPRODUCED (SIGSEGV)" || echo "  NOT-REPRODUCED"

echo
echo "== bug 3: empty environment crashes at startup =="
env -i "$HERE/query" >/dev/null 2>&1
[ $? -eq 139 ] && echo "  REPRODUCED (SIGSEGV)" || echo "  NOT-REPRODUCED"

echo
echo "== bug 4: register_gc_policy off-by-one =="
if "$HERE/bug4_policy_oob" >/dev/null 2>&1; then
    echo "  REPRODUCED (4th registration returned id 4, an out-of-bounds slot)"
else
    echo "  NOT-REPRODUCED"
fi

echo
echo "== bug 5: debug output cannot be silenced =="
n=$(LIBALLOCS_DEBUG_LEVEL=0 "$HERE/query" 2>&1 >/dev/null \
        | grep -cE 'lazily mapped|MMAP:')
if [ "$n" -gt 0 ]; then
    echo "  REPRODUCED ($n lines to stderr with LIBALLOCS_DEBUG_LEVEL=0)"
else
    echo "  NOT-REPRODUCED"
fi
