#!/bin/bash
# unit_bench.sh [-r rounds] [name=unitd]...
#
# NGINX Unit end to end: tools/linkpy-bench.py of UNIT_DIR (default
# ~/Develop/unit) against each named unitd (the real binary or a linkpy-unitd
# wrapper, see unit_wrapper.sh), on the `empty` app and on `body_generate`
# with a 64 KiB body, the configurations interleaved within each round and
# everything pinned to BENCH_CPUS (default 0-3, the P-cores here). Reports
# min and median p50, the best req/s and the fastest app start. Round to
# round spread is 15 us or so even pinned: six rounds rank coarsely.
#
# Defaults: upstream=~/Develop/unit-baseline/build/sbin/unitd and
# current=UNIT_DIR/build/linkpy-unitd.
set -u
UNIT=${UNIT_DIR:-$HOME/Develop/unit}; rounds=6
while getopts "r:" o; do case $o in r) rounds=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1))
[ $# -gt 0 ] || set -- "upstream=$HOME/Develop/unit-baseline/build/sbin/unitd" "current=$UNIT/build/linkpy-unitd"
[ -f "$UNIT/tools/linkpy-bench.py" ] || { echo "unit_bench: $UNIT/tools/linkpy-bench.py not found" >&2; exit 1; }
lines=$(mktemp); cd "$UNIT"
for r in $(seq 1 "$rounds"); do
    for cfg in "$@"; do
        name=${cfg%%=*}; bin=${cfg#*=}
        for app in "empty" "body_generate --header X-Length=65536"; do
            line=$(taskset -c "${BENCH_CPUS:-0-3}" python3 tools/linkpy-bench.py "$bin" test/python/$app --label x 2>/dev/null | grep latency)
            echo "$r $name ${app%% *} $(echo "$line" | sed -n 's/.*= \([0-9]*\) req\/s; latency p50 \([0-9]*\)us.*app start) \([0-9]*\)ms.*/\2 \1 \3/p')" >> "$lines"
            sleep 1   # let the previous unitd release the port
        done
    done
done
python3 - "$lines" <<'PY'
import sys, collections, statistics
d = collections.defaultdict(list); order = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 6:
        d[(p[1], p[2])].append((int(p[3]), int(p[4]), int(p[5])))
        if p[1] not in order: order.append(p[1])
    elif len(p) >= 3:
        print(f"round {p[0]} {p[1]} {p[2]}: no result (unitd failed to start or configure)")
print(f"{'configuration':16s}{'app':15s}{'p50 min':>8s}{'p50 med':>8s}{'req/s best':>11s}{'start ms':>9s}{'rounds':>7s}")
for name in order:
    for app in ['empty', 'body_generate']:
        v = d[(name, app)]
        if v: print(f"{name:16s}{app:15s}{min(x[0] for x in v):8d}{statistics.median(x[0] for x in v):8.0f}{max(x[1] for x in v):11d}{min(x[2] for x in v):9d}{len(v):7d}")
PY
rm -f "$lines"
