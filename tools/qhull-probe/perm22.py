#!/usr/bin/env python3
"""2x2 lattice (one cocircular quad): diagonal for every input permutation."""
import itertools, sys
from probe import probe_lattice

ox = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
oy = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
step = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

# lattice idx: 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1)
res = {}
for perm in itertools.permutations(range(4)):
    d = probe_lattice(2, 2, ox=ox, oy=oy, step=step, seed=5,
                      order=list(perm), tag="perm")
    res[perm] = d[(0, 0)]
for perm, v in res.items():
    print("".join(map(str, perm)), v)
