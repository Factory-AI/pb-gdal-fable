#!/usr/bin/env python3
"""Fixture set for raster zonal-stats cases.

usage: mk_zonal_fixtures.py [outdir]
"""
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif  # noqa: E402

d = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(d, exist_ok=True)
j = os.path.join
utm = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
utm12 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32612]
G8 = [0.0, 1.0, 0.0, 8.0, 0.0, -1.0]

rng = random.Random(1234)
vals = [float(rng.randint(0, 9)) for _ in range(64)]
write_tif(j(d, 'zin.tif'), 'Float32', 8, 8, [vals], gt=G8, geokeys=utm)
write_tif(j(d, 'zin_nd.tif'), 'Float32', 8, 8, [vals], gt=G8,
          geokeys=utm, nodata='5')
write_tif(j(d, 'zin_i16.tif'), 'Int16', 8, 8, [[int(v) for v in vals]],
          gt=G8, geokeys=utm)
# same content, different SRS (advisory warning)
write_tif(j(d, 'zin_srs2.tif'), 'Float32', 8, 8, [vals], gt=G8,
          geokeys=utm12)
# no SRS at all
write_tif(j(d, 'zin_nosrs.tif'), 'Float32', 8, 8, [vals], gt=G8)

# multi-band input
rng = random.Random(77)
mb1 = [float(rng.randint(0, 5)) for _ in range(64)]
mb2 = [round(rng.uniform(-3, 3), 1) for _ in range(64)]
mb3 = [float(rng.randint(10, 12)) for _ in range(64)]
write_tif(j(d, 'zmb.tif'), 'Float64', 8, 8, [mb1, mb2, mb3], gt=G8,
          geokeys=utm)

# raster zones on the same grid: blocks of ids 1,2,3 plus a 0 corner
zz = []
for r in range(8):
    for c in range(8):
        if r < 2 and c < 2:
            zz.append(0)
        elif c < 4:
            zz.append(1)
        elif r < 4:
            zz.append(2)
        else:
            zz.append(3)
write_tif(j(d, 'zzone.tif'), 'Byte', 8, 8, [zz], gt=G8, geokeys=utm)
# zones with nodata declared on the band
write_tif(j(d, 'zzone_nd.tif'), 'Byte', 8, 8, [zz], gt=G8, geokeys=utm,
          nodata='1')
# negative and float zone ids
zf = [(-2.5 if c < 3 else (0.25 if r < 5 else 7.0))
      for r in range(8) for c in range(8)]
write_tif(j(d, 'zzone_f.tif'), 'Float32', 8, 8, [zf], gt=G8, geokeys=utm)
# coarser zones grid (input gets resampled)
zc = [1, 1, 2, 2, 1, 1, 2, 2, 3, 3, 4, 4, 3, 3, 4, 4]
write_tif(j(d, 'zzone_c.tif'), 'Byte', 4, 4, [zc],
          gt=[0.0, 2.0, 0.0, 8.0, 0.0, -2.0], geokeys=utm)
# two-band zones raster
write_tif(j(d, 'zzone_2b.tif'), 'Byte', 8, 8, [zz, zz[::-1]], gt=G8,
          geokeys=utm)
# zones in another SRS
write_tif(j(d, 'zzone_srs2.tif'), 'Byte', 8, 8, [zz], gt=G8,
          geokeys=utm12)

# weights
rng = random.Random(5)
wv = [round(rng.uniform(0.25, 2.0), 2) for _ in range(64)]
write_tif(j(d, 'zw.tif'), 'Float32', 8, 8, [wv], gt=G8, geokeys=utm)
write_tif(j(d, 'zw_nd.tif'), 'Float32', 8, 8, [wv], gt=G8, geokeys=utm,
          nodata=str(wv[9]))
# coarse weights (get resampled)
wc = [round(rng.uniform(0.5, 1.5), 2) for _ in range(16)]
write_tif(j(d, 'zw_c.tif'), 'Float32', 4, 4, [wc],
          gt=[0.0, 2.0, 0.0, 8.0, 0.0, -2.0], geokeys=utm)
# two-band weights
write_tif(j(d, 'zw_2b.tif'), 'Float32', 8, 8, [wv, wv[::-1]], gt=G8,
          geokeys=utm)

# input with NaN cells and no nodata
import struct  # noqa: E402
nanv = list(vals)
nanv[3] = float('nan')
nanv[20] = float('nan')
write_tif(j(d, 'zin_nan.tif'), 'Float32', 8, 8, [nanv], gt=G8,
          geokeys=utm)


def fc(feats, crs=None):
    o = {'type': 'FeatureCollection', 'features': feats}
    if crs:
        o['crs'] = {'type': 'name', 'properties': {'name': crs}}
    return o


def feat(props, geom):
    return {'type': 'Feature', 'properties': props, 'geometry': geom}


def poly(*rings):
    return {'type': 'Polygon', 'coordinates': list(rings)}


sq = poly([[0.5, 3.0], [4.5, 3.0], [4.5, 7.5], [0.5, 7.5], [0.5, 3.0]])
tri = poly([[2.0, 0.0], [8.0, 0.0], [8.0, 5.0], [2.0, 0.0]])
hole = poly([[1.0, 1.0], [7.0, 1.0], [7.0, 7.0], [1.0, 7.0], [1.0, 1.0]],
            [[3.0, 3.0], [5.0, 3.0], [5.0, 5.0], [3.0, 5.0], [3.0, 3.0]])
outside = poly([[20.0, 20.0], [24.0, 20.0], [24.0, 24.0], [20.0, 20.0]])
mpoly = {'type': 'MultiPolygon', 'coordinates': [
    poly([[0.2, 0.2], [2.8, 0.2], [2.8, 2.8], [0.2, 2.8],
          [0.2, 0.2]])['coordinates'],
    poly([[5.2, 5.2], [7.9, 5.2], [7.9, 7.9], [5.2, 5.2]])['coordinates'],
]}

CRSUTM = 'urn:ogc:def:crs:EPSG::32611'
with open(j(d, 'zpoly.json'), 'w') as f:
    json.dump(fc([
        feat({'name': 'sq', 'id': 1}, sq),
        feat({'name': 'tri', 'id': 2}, tri),
        feat({'name': 'hole', 'id': 3}, hole),
    ], CRSUTM), f)
with open(j(d, 'zpoly_crs84.json'), 'w') as f:
    json.dump(fc([feat({'name': 'sq'}, sq)]), f)
with open(j(d, 'zmulti.json'), 'w') as f:
    json.dump(fc([
        feat({'name': 'mp'}, mpoly),
        feat({'name': 'out'}, outside),
        feat({'name': 'null'}, None),
    ], CRSUTM), f)
with open(j(d, 'zpoint.json'), 'w') as f:
    json.dump(fc([
        feat({'name': 'sq'}, sq),
        feat({'name': 'pt'}, {'type': 'Point', 'coordinates': [3, 3]}),
        feat({'name': 'tri'}, tri),
    ], CRSUTM), f)
# two layers cannot happen with GeoJSON; use a second file for layer tests
with open(j(d, 'zone_lyr.json'), 'w') as f:
    json.dump(fc([feat({'a': 1}, sq)], CRSUTM), f)

with open(j(d, 'exists.json'), 'w') as f:
    json.dump(fc([feat({'x': 1}, None)]), f)
with open(j(d, 'exists.txt'), 'w') as f:
    f.write('plain\n')
