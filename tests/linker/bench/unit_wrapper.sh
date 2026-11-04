#!/bin/bash
# unit_wrapper.sh <linkpy_tree> <out_dir>
#
# Writes <out_dir>/linkpy-unitd: NGINX Unit's Python module run under the
# LinkPy auditor of another tree (a worktree of an older commit, say), with
# its own linkstub cache, for A/B comparisons with unit_bench.sh. It is
# derived from the wrapper Unit's own `configure python --linkpy=...` wrote
# (UNIT_DIR/build/linkpy-unitd, UNIT_DIR defaulting to ~/Develop/unit), so it
# follows Unit's build settings; only the LinkPy tree and the cache differ.
# PYTHONPATH names Unit's modules directory for trees that predate the
# module-beside-libinit search (harmless otherwise). The `lib` link beside
# the wrapper is what Unit's tools/linkpy-bench.py uses to find the modules.
set -eu
[ $# = 2 ] || { echo "usage: unit_wrapper.sh <linkpy_tree> <out_dir>" >&2; exit 2; }
tree=$(cd "$1" && pwd); out=$2; UNIT=${UNIT_DIR:-$HOME/Develop/unit}
src=$UNIT/build/linkpy-unitd
[ -f "$src" ] || { echo "unit_wrapper: $src not found (run Unit's configure python --linkpy=...)" >&2; exit 1; }
[ -f "$tree/linker/dist/audit.so" ] || { echo "unit_wrapper: $tree/linker/dist/audit.so not found (build that tree)" >&2; exit 1; }
mkdir -p "$out"; out=$(cd "$out" && pwd)
python3 - "$src" "$out/linkpy-unitd" "$tree" "$out" <<'PY'
import sys, re
src, dst, tree, out = sys.argv[1:]
s = open(src).read()
s, n = re.subn(r'^LINKPY=.*$', f'LINKPY="{tree}/linker"', s, count=1, flags=re.M)
assert n == 1, "no LINKPY= line in the wrapper"
s = s.replace('export LD_AUDIT=', f'export LINKPY_CACHE_DIR="{out}/cache"\nexport PYTHONPATH="$BUILD/lib/unit/modules"\nexport LD_AUDIT=', 1)
s = s.replace('# Runs unitd with the LinkPy auditor', f'# A/B wrapper written by tests/linker/bench/unit_wrapper.sh for the LinkPy tree\n# {tree}; otherwise Unit\'s own wrapper.\n# Runs unitd with the LinkPy auditor', 1)
open(dst, 'w').write(s)
PY
chmod +x "$out/linkpy-unitd"; ln -sfn "$UNIT/build/lib" "$out/lib"
echo "$out/linkpy-unitd"
