#!/usr/bin/env python3
"""Fixture set for raster reproject cubic/cubicspline (c8 stub upgrade).

usage: mk_cubic_fixtures.py [outdir]

Small georeferenced rasters per data type plus nodata-holed variants,
a two-band nodata-tagged pair and tiny-source degenerates. Values are
drawn from seeded Mersenne generators so both sandboxes agree.
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def gen(dt, n, seed):
    rnd = random.Random(seed)
    if dt == 'Byte':
        return [float(rnd.randrange(256)) for _ in range(n)]
    if dt == 'Int8':
        return [float(rnd.randrange(-128, 128)) for _ in range(n)]
    if dt == 'Int16':
        return [float(rnd.randrange(-32768, 32768)) for _ in range(n)]
    if dt == 'UInt16':
        return [float(rnd.randrange(65536)) for _ in range(n)]
    if dt == 'Int32':
        return [float(rnd.randrange(-2**31, 2**31)) for _ in range(n)]
    if dt == 'Float32':
        return [f32(rnd.uniform(-1000.0, 1000.0)) for _ in range(n)]
    return [rnd.uniform(-1000.0, 1000.0) for _ in range(n)]


def holes(vals, nd, seed, frac=0.08):
    rnd = random.Random(seed)
    out = list(vals)
    for k in range(len(out)):
        if rnd.random() < frac:
            out[k] = nd
    return out


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    j = os.path.join
    gt = [2.0, 0.002, 0.0, 46.0, 0.0, -0.002]
    w, h = 40, 30

    dts = ['Byte', 'Int8', 'Int16', 'UInt16', 'Int32', 'Float32',
           'Float64']
    for i, dt in enumerate(dts):
        write_tif(j(d, 'c_%s.tif' % dt), dt, w, h, [gen(dt, w * h, i + 1)],
                  gt=gt, epsg=4326)

    nds = {'Byte': 7.0, 'UInt16': 0.0, 'Int16': -999.0,
           'Float32': -999.5, 'Float64': -999.0}
    for i, (dt, nd) in enumerate(sorted(nds.items())):
        vals = holes(gen(dt, w * h, 20 + i), nd, 40 + i)
        write_tif(j(d, 'n_%s.tif' % dt), dt, w, h, [vals], gt=gt,
                  epsg=4326, nodata=repr(nd) if nd != int(nd)
                  else str(int(nd)))

    b1 = holes(gen('Int16', w * h, 61), 7.0, 62)
    b2 = holes(gen('Int16', w * h, 63), 7.0, 64)
    write_tif(j(d, 'nm2.tif'), 'Int16', w, h, [b1, b2], gt=gt,
              epsg=4326, nodata='7')

    write_tif(j(d, 't22.tif'), 'Byte', 2, 2, [gen('Byte', 4, 71)],
              gt=gt, epsg=4326)
    write_tif(j(d, 't33.tif'), 'Byte', 3, 3, [gen('Byte', 9, 72)],
              gt=gt, epsg=4326)
    write_tif(j(d, 't11.tif'), 'Byte', 1, 1, [gen('Byte', 1, 73)],
              gt=gt, epsg=4326)
    write_tif(j(d, 't33f.tif'), 'Float32', 3, 3,
              [gen('Float32', 9, 74)], gt=gt, epsg=4326)


if __name__ == '__main__':
    main()
