#!/bin/bash
# start_time.sh [-B build_dir] [-c] [label]
#
# What a process start costs with a warm linkstub cache: the wall time of a
# one-call python_bench_divide process, five runs pinned to BENCH_CPU
# (default 2), sorted; with -c also a callgrind profile of that start, with
# the entries that matter (the cache key, the interpreter start, the loader).
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=$here/../../../linker/build; cg=0
while getopts "B:c" o; do case $o in B) B=$OPTARG;; c) cg=1;; *) exit 2;; esac; done
shift $((OPTIND - 1)); label=${1:-current}
B=$(cd "$B" && pwd) || exit 2; out=$B/bench; mkdir -p "$out/cgb"
info=$(ctest --test-dir "$B" -N -V -R '^python_bench_divide$' 2>/dev/null)
cmd=$(echo "$info" | sed -n 's/^[0-9]*: Test command: //p'); wd=$(echo "$info" | sed -n 's/^[0-9]*: Working Directory: //p')
[ -n "$cmd" ] || { echo "start_time: no python_bench_divide in $B" >&2; exit 1; }
cd "$wd" && while IFS= read -r line; do export "$line"; done < <(echo "$info" | sed -n 's/^[0-9]*:  \([A-Z_]*=.*\)$/\1/p')
export LINKPY_BENCH_CALLS=1
$cmd > /dev/null 2>&1 || { echo "$label: the fixture failed" >&2; exit 1; }
for i in 1 2 3 4 5; do /usr/bin/time -f "%e" taskset -c "${BENCH_CPU:-2}" $cmd 2>&1 > /dev/null | tail -1; done \
    | sort -n | paste -sd' ' | sed "s/^/$label cache-hit start, seconds (5 runs sorted): /"
if [ $cg = 1 ]; then
    valgrind --tool=callgrind --callgrind-out-file="$out/cgb/start-$label.out" $cmd > /dev/null 2>&1
    callgrind_annotate --auto=no --threshold=100 --inclusive=yes "$out/cgb/start-$label.out" 2>/dev/null \
        | grep -E "PROGRAM TOTALS|linkstub_cache_key\(|linkstub_locate|ensure_vm|init_py|dl_main" \
        | sed 's/ \[.*\]//; s/(std::__cxx11.*//' | awk '{printf "  %14s %s\n", $1, substr($0, index($0,$3))}' | head -8
fi
