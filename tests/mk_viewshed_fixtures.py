#!/usr/bin/env python3
"""Fixture set for raster viewshed cases.

usage: mk_viewshed_fixtures.py [outdir]
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif  # noqa: E402

d = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(d, exist_ok=True)
j = os.path.join
utm = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
G1 = [0.0, 1.0, 0.0, 10.0, 0.0, -1.0]

# flat 10x10 DEM, all zeros, 1m pixels
write_tif(j(d, 'flat.tif'), 'Float32', 10, 10, [[0.0] * 100],
          gt=G1, geokeys=utm)
# ramp in x
write_tif(j(d, 'rampx.tif'), 'Float32', 10, 10,
          [[float(c) for r in range(10) for c in range(10)]],
          gt=G1, geokeys=utm)
# single peak in middle of flat
pk = [0.0] * 100
pk[5 * 10 + 5] = 10.0
write_tif(j(d, 'peak.tif'), 'Float32', 10, 10, [pk], gt=G1, geokeys=utm)
# wall: column 5 high
wall = [0.0] * 100
for r in range(10):
    wall[r * 10 + 5] = 5.0
write_tif(j(d, 'wall.tif'), 'Float32', 10, 10, [wall], gt=G1, geokeys=utm)

# random terrains
rng = random.Random(42)
write_tif(j(d, 'rnd.tif'), 'Float32', 17, 17,
          [[round(rng.uniform(0, 6), 2) for _ in range(17 * 17)]],
          gt=[0.0, 1.0, 0.0, 17.0, 0.0, -1.0], geokeys=utm)
rng = random.Random(7)
write_tif(j(d, 'rnd2.tif'), 'Float32', 21, 13,
          [[round(rng.uniform(0, 8), 2) for _ in range(21 * 13)]],
          gt=[0.0, 1.0, 0.0, 13.0, 0.0, -1.0], geokeys=utm)
# anisotropic resolution (2 x 0.5)
rng = random.Random(19)
write_tif(j(d, 'rndr.tif'), 'Float32', 16, 18,
          [[round(rng.uniform(0, 10), 2) for _ in range(16 * 18)]],
          gt=[100.0, 2.0, 0.0, 500.0, 0.0, -0.5], geokeys=utm)

# two-band raster
b1 = [float((r * 5 + c) % 7) for r in range(5) for c in range(5)]
b2 = [float((r * 3 + c * 2) % 5) for r in range(5) for c in range(5)]
write_tif(j(d, 'mb.tif'), 'Float32', 5, 5, [b1, b2],
          gt=[0.0, 1.0, 0.0, 5.0, 0.0, -1.0], geokeys=utm)

# small ramp, with and without nodata declaration
ramp5 = [float((r * 5 + c) % 7) for r in range(5) for c in range(5)]
write_tif(j(d, 'e.tif'), 'Float64', 5, 5, [ramp5],
          gt=[0.0, 1.0, 0.0, 5.0, 0.0, -1.0], geokeys=utm)
write_tif(j(d, 'nd.tif'), 'Float64', 5, 5, [ramp5],
          gt=[0.0, 1.0, 0.0, 5.0, 0.0, -1.0], geokeys=utm, nodata='3')
# no geotransform, no SRS
write_tif(j(d, 'nogt.tif'), 'Float64', 5, 5, [[0.0] * 25])

with open(j(d, 'exists.tif'), 'w') as f:
    f.write('existing')
