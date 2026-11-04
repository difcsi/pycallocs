#!/bin/bash
# cg_bench.sh [-B build_dir] [-H header_dir] [label]
#
# Instructions per call (callgrind, inclusive) of the two benchmark fixtures
# python_bench_divide and python_bench_structs of a LinkPy build. Instruction
# counts do not move with machine load, so this is what ranks a marshaller
# change on a busy machine; wall_bench.sh gives the microseconds. The one-off
# interpreter start (ensure_vm) lands inside the first timed symbol of each
# fixture and is subtracted. `take` and `give` are the average over their two
# shapes (64 KiB and 100 B in; 64 KiB and empty out).
#
#   -B build_dir   the LinkPy CMake build (default: ../../../linker/build)
#   -H header_dir  a directory whose pycpputils.hpp shadows the tree's, put
#                  first on the JIT include path: measures a header variant
#                  without touching the tree (the cache key sees the change)
#   label          prefix of the output lines and of the callgrind files
#
# Output files go under <build_dir>/bench/; the linkstubs compiled for the
# measurement use their own cache there, not the tests' one.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
B=$here/../../../linker/build; HDR=""
while getopts "B:H:" o; do case $o in B) B=$OPTARG;; H) HDR=$(cd "$OPTARG" && pwd);; *) exit 2;; esac; done
shift $((OPTIND - 1)); label=${1:-current}
B=$(cd "$B" && pwd) || exit 2; out=$B/bench; mkdir -p "$out/cgb"
command -v valgrind >/dev/null || { echo "cg_bench: valgrind not found" >&2; exit 2; }

run() { # test name, calls, warm-up calls, symbol:calls-per-iteration...
    local t=$1 n=$2 warm=$3; shift 3
    local info; info=$(ctest --test-dir "$B" -N -V -R "^$t\$" 2>/dev/null)
    local cmd wd; cmd=$(echo "$info" | sed -n 's/^[0-9]*: Test command: //p'); wd=$(echo "$info" | sed -n 's/^[0-9]*: Working Directory: //p')
    [ -n "$cmd" ] || { echo "cg_bench: no test $t in $B (configure and build the tests first)" >&2; return 1; }
    (
        cd "$wd" && while IFS= read -r line; do export "$line"; done < <(echo "$info" | sed -n 's/^[0-9]*:  \([A-Z_]*=.*\)$/\1/p')
        export LINKPY_BENCH_CALLS=$n LINKPY_CACHE_DIR=$out/cache
        [ -n "$HDR" ] && export LINKSTUB_BUILD_CMD="${LINKSTUB_BUILD_CMD/ -fPIC / -fPIC -I$HDR }"
        $cmd > /dev/null 2>&1   # the linkstub is compiled and cached before profiling
        ${BENCH_CG_CPUS:+taskset -c $BENCH_CG_CPUS} valgrind --tool=callgrind --callgrind-out-file="$out/cgb/$label-$t.out" $cmd > /dev/null 2>&1
    )
    local ann; ann=$(callgrind_annotate --auto=no --threshold=100 --inclusive=yes "$out/cgb/$label-$t.out" 2>/dev/null)
    local vm; vm=$(echo "$ann" | grep -E "linkstub.cpp:linkpy::(py|lua)::ensure_vm\(\)" | head -1 | awk '{gsub(",","",$1); print $1}')
    local first=1
    for sym in "$@"; do
        local name=${sym%%:*} mult=${sym##*:}
        local ir; ir=$(echo "$ann" | grep -E "linkstub.cpp:$name " | head -1 | awk '{gsub(",","",$1); print $1}')
        [ $first = 1 ] && { ir=$(( ${ir:-0} - ${vm:-0} )); first=0; }
        printf "%s %s: %d Ir/call\n" "$label" "$name" $(( ${ir:-0} / ((n + warm) * mult) ))
    done
}
run python_bench_divide 20000 1000 divide:1
run python_bench_structs 1000 100 take:2 give:2 ask:1
