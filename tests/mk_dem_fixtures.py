#!/usr/bin/env python3
"""Fixture set for the DEM verb suite (slope/aspect/hillshade/roughness/
tpi/tri).

usage: mk_dem_fixtures.py [outdir]

Deterministic LCG terrain, Float32:
  dbig.tif  40x30 UTM 32611, nodata -500 with holes
  dgeo.tif  40x30 EPSG:4326 (geographic autoscale path)
  dsm.tif   6x5 UTM, plain
  dnond.tif 6x5 UTM, no nodata declared
  dnogt.tif 6x5, no georeferencing at all
  dmb.tif   6x5 UTM, two bands
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif


def lcg_vals(n, seed, lo=0.0, hi=100.0):
    s = seed
    out = []
    for _ in range(n):
        s = (s * 1103515245 + 12345) % (1 << 31)
        out.append(round(lo + (hi - lo) * (s / float(1 << 31)), 2))
    return out


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    j = os.path.join
    utm = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
    geo = [1, 1, 0, 3, 1024, 0, 1, 2, 1025, 0, 1, 1, 2048, 0, 1, 4326]

    v = lcg_vals(40 * 30, 777)
    holes = lcg_vals(60, 42, 0, 40 * 30)
    for hix in holes:
        v[int(hix)] = -500.0
    write_tif(j(d, 'dbig.tif'), 'Float32', 40, 30, [v],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm,
              nodata='-500')
    write_tif(j(d, 'dgeo.tif'), 'Float32', 40, 30, [lcg_vals(40 * 30, 5)],
              gt=[5.0, 0.05, 0.0, 44.0, 0.0, -0.05], geokeys=geo)
    sm = [10.0, 12.5, 9.0, 20.0, 30.5, 8.0,
          11.0, 15.0, 40.0, 25.0, 12.0, 7.0,
          9.5, 60.0, 35.0, 18.0, 22.0, 6.0,
          13.0, 44.0, 28.0, 16.0, 19.0, 5.0,
          14.0, 33.0, 21.0, 17.0, 23.0, 4.0]
    write_tif(j(d, 'dsm.tif'), 'Float32', 6, 5, [sm],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm,
              nodata='-9999')
    write_tif(j(d, 'dnond.tif'), 'Float32', 6, 5, [sm],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm)
    write_tif(j(d, 'dnogt.tif'), 'Float32', 6, 5, [sm])
    write_tif(j(d, 'dmb.tif'), 'Float32', 6, 5,
              [sm, [x * 2.0 + 1.0 for x in sm]],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm,
              nodata='-9999')
    hill = [round(100.0 * max(0.0, 1.0 - math.hypot((c - 15.5) / 16.0,
                                                    (r - 15.5) / 16.0)), 2)
            for r in range(32) for c in range(32)]
    write_tif(j(d, 'dhill.tif'), 'Float32', 32, 32, [hill],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm,
              nodata='-9999')


if __name__ == '__main__':
    main()
