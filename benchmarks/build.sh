#!/usr/bin/env bash
# Build the benchmark fixtures without a full CMake configure. Produces:
#   libs/bench/bench.so              via allocscc (DWARF+uniqtypes, for pycallocs)
#   libs/bench/libbench_plain.so     via gcc -O2  (for ctypes / cffi / native)
#   backends/native/benchnative*.so  the hand-written extension (the ceiling)
#
# (CMake's `-DBUILD_BENCHMARKS=ON` + target `bench-libs` does the same thing.)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LA="$ROOT/contrib/stackscan/contrib/liballocs"
ALLOCSCC="$LA/tools/lang/c/bin/allocscc"
LIBDIR="$HERE/libs/bench"
PY="${PYTHON:-python3}"

echo "[1/3] bench.so (allocscc)"
LIBALLOCS="$LA" ALLOCSCC_TRAP_PTR_WRITES=1 "$ALLOCSCC" \
    -std=gnu11 -g -fPIC -shared -I"$LA/include" "$LIBDIR/bench.c" -o "$LIBDIR/bench.so"

echo "[2/3] libbench_plain.so (gcc -O2)"
gcc -O2 -g -fPIC -shared "$LIBDIR/bench.c" -o "$LIBDIR/libbench_plain.so"

echo "[3/3] native extension"
PY_INC="$($PY -c 'import sysconfig; print(sysconfig.get_path("include"))')"
EXT="$($PY -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX") or ".so")')"
gcc -O2 -g -fPIC -shared -I"$PY_INC" -I"$LIBDIR" \
    "$HERE/backends/native/benchnative.c" -L"$LIBDIR" -lbench_plain \
    -Wl,-rpath,"$LIBDIR" -o "$HERE/backends/native/benchnative$EXT"

echo "done. fixtures in $LIBDIR and backends/native/"
