#!/usr/bin/env python3
"""Tight loop of a single pycallocs workload, for perf to sample.

Invoked by profile_hotspots.sh under the liballocs env. Warms up first so the
SPECIALISE compile (if any) doesn't pollute the profile, then hammers the chosen
workload so perf samples concentrate on the steady-state hot path
(funproxy_call, the converters, Proxy_* in src/).
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))            # repo root (allocs, elflib)
sys.path.insert(0, _HERE)                             # benchmarks
sys.path.insert(0, os.path.join(_HERE, "backends"))  # bench_pycallocs

import bench_pycallocs as B

name = sys.argv[1] if len(sys.argv) > 1 else "byval_ret"
iters = int(sys.argv[2]) if len(sys.argv) > 2 else 2_000_000

fn = B.RUNNERS[name]()
for _ in range(50):       # warm: trigger any one-time compile/cache fill
    fn()
for _ in range(iters):
    fn()
