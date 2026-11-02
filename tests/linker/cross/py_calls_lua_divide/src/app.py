"""Python HOST calling a Lua function through linkpy (scalar crossing)."""
import sys

import linkpy

m = linkpy.load("mytest")
r = m.divide(6, 3)
print("python got", r)
sys.exit(0 if abs(r - 2.0) < 1e-6 else 1)
