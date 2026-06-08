#!/bin/sh
# Build the base-types uniqtype provider shared object.
#
# Usage: gen_provider.sh LIBALLOCS_DIR SRC_DIR OUT_DIR
#
#   LIBALLOCS_DIR  liballocs source/build root (has tools/Makefile.meta)
#   SRC_DIR        this directory (basetypes.c, aliases.inc.c)
#   OUT_DIR        build output directory
#
# Produces $OUT_DIR/libbasetypes_provider.so, which defines the base-type
# uniqtype symbols the pycallocs extension imports. Preload it (after
# liballocs_preload.so) so those symbols resolve at dlopen time.
set -e

LIBALLOCS="$1"
SRC_DIR="$2"
OUT_DIR="$3"
: "${CC:=cc}"

mkdir -p "$OUT_DIR"

# 1. Compile the base-type listing with DWARF-4 (liballocs' dwarftypes can't
#    handle DWARF-5).
"$CC" -gdwarf-4 -fPIC -shared -o "$OUT_DIR/basetypes.so" "$SRC_DIR/basetypes.c"
ABS="$(readlink -f "$OUT_DIR/basetypes.so")"

# 2. Run liballocs' meta generator to get ABI-correct canonical uniqtypes,
#    keeping all metadata inside the build tree (not the system /usr/lib/meta).
META_BASE="$OUT_DIR/meta"
mkdir -p "$META_BASE"
LIBALLOCS="$LIBALLOCS" META_BASE="$META_BASE" \
    make -f "$LIBALLOCS/tools/Makefile.meta" "$META_BASE$ABS-meta.so"

# 3. Provider TU = canonical definitions (dwarftypes.c) + spelled-out aliases,
#    in one TU so the alias targets are in scope. roottypes.c supplies 'void'.
GEN="$META_BASE$(dirname "$ABS")"
cat "$GEN/basetypes.so-dwarftypes.c" "$SRC_DIR/aliases.inc.c" > "$OUT_DIR/provider.c"

"$CC" -shared -fPIC -gdwarf-4 -I"$LIBALLOCS/include" \
    "$OUT_DIR/provider.c" "$GEN/basetypes.so-roottypes.c" \
    -o "$OUT_DIR/libbasetypes_provider.so"

echo "built $OUT_DIR/libbasetypes_provider.so"
