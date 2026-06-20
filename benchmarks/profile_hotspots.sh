#!/usr/bin/env bash
# C-level hotspot profile of one pycallocs workload via Linux perf.
#
# The per-call work lives in the extension's C (src/function_proxy.c:funproxy_call,
# the foreign_basetype.c converters, src/proxy.c) where Python profilers can't see,
# so we sample with perf. Sets up the liballocs/linkpy environment, runs a tight
# loop (_profile_driver.py), and prints perf stat + the top symbols.
#
# Usage:  ./profile_hotspots.sh [workload] [iters]
#   workload defaults to byval_ret ; iters defaults to 2,000,000
#   override the build dir with PYCALLOCS_BUILD_DIR=/path/to/build
#
# Needs perf access: sysctl kernel.perf_event_paranoid<=1 (or run with sudo -E).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LA="$ROOT/contrib/stackscan/contrib/liballocs"
BUILD="${PYCALLOCS_BUILD_DIR:-$ROOT/build}"

export LD_PRELOAD="$LA/lib/liballocs_preload.so:$BUILD/basetypes/libbasetypes_provider.so"
export LD_AUDIT="$ROOT/contrib/linkpy/dist/audit.so"
export LD_LIBRARY_PATH="$LA/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GLIBC_TUNABLES="glibc.rtld.optional_static_tls=524288"
export PYTHONPATH="$ROOT:$HERE${PYTHONPATH:+:$PYTHONPATH}"

WORKLOAD="${1:-byval_ret}"
ITERS="${2:-2000000}"
PY="${PYTHON:-python3}"
DRIVER="$HERE/_profile_driver.py"
PERF_DATA="$HERE/perf-$WORKLOAD.data"

echo "## perf stat -- $WORKLOAD x $ITERS  (cycles/instructions/cache per run)"
perf stat -- "$PY" "$DRIVER" "$WORKLOAD" "$ITERS" || true
echo
echo "## perf record/report -- top symbols for $WORKLOAD"
perf record -g --call-graph dwarf -o "$PERF_DATA" -- "$PY" "$DRIVER" "$WORKLOAD" "$ITERS"
perf report -i "$PERF_DATA" --stdio 2>/dev/null | sed -n '1,40p'
echo
echo "(full profile: perf report -i $PERF_DATA)"
