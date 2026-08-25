#!/usr/bin/env python3
"""Fixture set for the COG output driver (c8 stub upgrade).

usage: mk_cog_fixtures.py [outdir]

Small per-type rasters plus >512px ones that trigger the overview
cascade. Even dimensions keep every level an exact halving. Seeded
Mersenne values so both sandboxes agree.
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


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    j = os.path.join

    # generation is CPU-heavy and deterministic: memoize per script hash
    import hashlib
    import shutil
    with open(os.path.abspath(__file__), 'rb') as fp:
        key = hashlib.sha256(fp.read()).hexdigest()[:16]
    cache = '/tmp/cogfix_' + key
    if os.path.exists(j(cache, '.done')):
        for f in os.listdir(cache):
            if f != '.done':
                shutil.copy2(j(cache, f), j(d, f))
        return
    build(d)
    tmp = cache + '.tmp%d' % os.getpid()
    os.makedirs(tmp, exist_ok=True)
    for f in os.listdir(d):
        shutil.copy2(j(d, f), j(tmp, f))
    with open(j(tmp, '.done'), 'w'):
        pass
    try:
        os.rename(tmp, cache)
    except OSError:
        shutil.rmtree(tmp, ignore_errors=True)


def build(d):
    j = os.path.join
    gt = [2.0, 0.002, 0.0, 46.0, 0.0, -0.002]
    w, h = 20, 10

    dts = ['Byte', 'Int8', 'Int16', 'UInt16', 'Int32', 'Float32',
           'Float64']
    for i, dt in enumerate(dts):
        write_tif(j(d, 'g_%s.tif' % dt), dt, w, h,
                  [gen(dt, w * h, i + 1)], gt=gt, epsg=4326)

    write_tif(j(d, 'g3.tif'), 'Byte', w, h,
              [gen('Byte', w * h, 11), gen('Byte', w * h, 12),
               gen('Byte', w * h, 13)], gt=gt, epsg=4326)
    write_tif(j(d, 'gn_Byte.tif'), 'Byte', w, h,
              [gen('Byte', w * h, 14)], gt=gt, epsg=4326, nodata='7')
    write_tif(j(d, 'gn_Float32.tif'), 'Float32', w, h,
              [gen('Float32', w * h, 15)], gt=gt, epsg=4326,
              nodata='-999.5')
    write_tif(j(d, 'g2n.tif'), 'Int16', w, h,
              [gen('Int16', w * h, 16), gen('Int16', w * h, 17)],
              gt=gt, epsg=4326, nodata='7')
    write_tif(j(d, 'gplain.tif'), 'Byte', w, h,
              [gen('Byte', w * h, 18)])

    for dt, seed in [('Byte', 31), ('Int16', 32), ('Float32', 33)]:
        write_tif(j(d, 'big_%s.tif' % dt), dt, 600, 400,
                  [gen(dt, 240000, seed)], gt=gt, epsg=4326)
    write_tif(j(d, 'big3.tif'), 'Byte', 600, 400,
              [gen('Byte', 240000, 34), gen('Byte', 240000, 35),
               gen('Byte', 240000, 36)], gt=gt, epsg=4326)
    write_tif(j(d, 'big14.tif'), 'Byte', 1400, 900,
              [gen('Byte', 1400 * 900, 37)], gt=gt, epsg=4326)


if __name__ == '__main__':
    main()
