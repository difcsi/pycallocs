#!/bin/bash
# wall_bench.sh [-B build_dir] [-r rounds] [label[=header_dir]]...
#
# Microseconds per call of the benchmark fixtures, pinned to one core
# (BENCH_CPU, default 2: a P-core on the machine this was written on), the
# named variants interleaved within each round, min and median over the
# rounds. A variant is a label, optionally with a directory whose
# pycpputils.hpp shadows the tree's (see cg_bench.sh -H); the bare label
# `current` measures the build as it is. Take the numbers on a quiet machine
# (load about 1) and compare runs made the same way only.
#
#   wall_bench.sh -r 6 current                 # the build, six rounds
#   wall_bench.sh -r 5 current old=/tmp/hdr    # against a header variant
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=$here/../../../linker/build; rounds=5
while getopts "B:r:" o; do case $o in B) B=$OPTARG;; r) rounds=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1)); [ $# -gt 0 ] || set -- current
B=$(cd "$B" && pwd) || exit 2; out=$B/bench; mkdir -p "$out"; cpu=${BENCH_CPU:-2}
lines=$out/wall-$$.txt; : > "$lines"

one() { # label, header dir or ""
    local label=$1 HDR=$2 t
    for t in python_bench_divide python_bench_structs; do
        local info; info=$(ctest --test-dir "$B" -N -V -R "^$t\$" 2>/dev/null)
        local cmd wd; cmd=$(echo "$info" | sed -n 's/^[0-9]*: Test command: //p'); wd=$(echo "$info" | sed -n 's/^[0-9]*: Working Directory: //p')
        [ -n "$cmd" ] || { echo "wall_bench: no test $t in $B" >&2; exit 1; }
        (
            cd "$wd" && while IFS= read -r line; do export "$line"; done < <(echo "$info" | sed -n 's/^[0-9]*:  \([A-Z_]*=.*\)$/\1/p')
            export LINKPY_CACHE_DIR=$out/cache
            [ -n "$HDR" ] && export LINKSTUB_BUILD_CMD="${LINKSTUB_BUILD_CMD/ -fPIC / -fPIC -I$HDR }"
            taskset -c "$cpu" $cmd 2>/dev/null | sed -n "s/^bench \(.*\): \([0-9.]*\) us\/call.*/$label \1 \2/p"
        ) >> "$lines"
    done
}
for r in $(seq 1 "$rounds"); do
    for v in "$@"; do
        label=${v%%=*}; HDR=""; [ "$v" != "$label" ] && HDR=$(cd "${v#*=}" && pwd)
        one "$label" "$HDR"
    done
done
python3 - "$lines" <<'PY'
import sys, collections, statistics
d = collections.defaultdict(list)
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) >= 3: d[(p[0], " ".join(p[1:-1]))].append(float(p[-1]))
labels, shapes = [], []
for l, s in d:
    if l not in labels: labels.append(l)
    if s not in shapes: shapes.append(s)
print(f"{'variant':14s}" + "".join(f"{s:>17s}" for s in shapes) + "   min / median us per call")
for l in labels:
    print(f"{l:14s}" + "".join(f"{min(d[(l,s)]):8.3f}/{statistics.median(d[(l,s)]):<8.3f}" if (l, s) in d else f"{'-':>17s}" for s in shapes)
          + f"  n={len(d[(l, shapes[0])])}")
PY
rm -f "$lines"
