# Simple benchmark for pycallocs

## Benchmark code

```bash
#!/bin/bash
PY=/home/zoltan/Develop/pycallocs/contrib/cpython/python
export PYTHONPATH=/home/zoltan/Develop/pycallocs:/home/zoltan/Develop/pycallocs/tests
export LD_LIBRARY_PATH=/home/zoltan/Develop/pycallocs/contrib/stackscan/contrib/liballocs/lib
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=524288
export META_BASE=/home/zoltan/Develop/pycallocs/build/tests/meta
export PYCALLOCS_TEST_LIBS=/home/zoltan/Develop/pycallocs/build/tests/libs
export LD_PRELOAD=/home/zoltan/Develop/pycallocs/contrib/stackscan/contrib/liballocs/lib/liballocs_preload.so:/home/zoltan/Develop/pycallocs/build/basetypes/libbasetypes_provider.so
export LD_AUDIT=/home/zoltan/Develop/pycallocs/contrib/linkpy/dist/audit.so
cd /home/zoltan/Develop/pycallocs/build/tests
run(){ /usr/bin/time -f "%e" "$PY" -c "$1" >/dev/null 2>/tmp/e.log; tail -1 /tmp/e.log; }
H='from pyc_testlib import add_libs; add_libs("bigrecursive")
from elflib import bigrecursive
'
N=200000
echo -n "A proxy-alloc only      gc ON : "; run "${H}
for _ in range($N): br=bigrecursive.bigrecursive()"
echo -n "B alloc + cycle write   gc ON : "; run "${H}
for _ in range($N):
 br=bigrecursive.bigrecursive(); br.ptr=br"
echo -n "C proxy-alloc only      gc OFF: "; run "import gc; gc.disable()
${H}
for _ in range($N): br=bigrecursive.bigrecursive()"
echo -n "D alloc + cycle write   gc OFF: "; run "import gc; gc.disable()
${H}
for _ in range($N):
 br=bigrecursive.bigrecursive(); br.ptr=br"
```

## Before

A proxy-alloc only      gc ON : 1.19
B alloc + cycle write   gc ON : 8.44
C proxy-alloc only      gc OFF: 1.21
D alloc + cycle write   gc OFF: 331.93
