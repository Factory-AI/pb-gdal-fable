#!/usr/bin/env python3
"""Fixture set for the raster tile suite.

usage: mk_tile_fixtures.py [outdir]

Byte rasters with a (7x+13y)%251 pattern over a UTM grid, plus tiny
rasters with varied georeferencing:
  big600.tif  600x400  EPSG:32611, gt [500000,10,0,4000000,0,-10]
  odd601.tif  601x399  same grid (odd dims)
  w1200.tif   1200x300 same grid (3-level pyramid)
  w768.tif    768x512  same grid (exact tile multiples)
  nogt700.tif 700x300  pattern, no georeferencing
  utm.tif     6x5 sequential, EPSG:32611
  gtnosrs.tif 6x5 gt but no SRS
  nogt.tif    6x5 no georeferencing
  geo.tif     6x5 EPSG:4326, gt [5,0.05,0,44,0,-0.05]
  f32.tif     6x5 Float32 i+0.25, EPSG:32611
  rgb.tif     6x5 3-band (3i,5i,7i), EPSG:32611
  byteutm.tif 20x20 (7x+13y)%256, EPSG:32611 at the classic byte grid
  al300.tif   300x300 EPSG:3857, tile-aligned at zoom 6 res

Every file gets a fixed mtime so the stacta datetime is stable across
runs.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

MTIME = 1700000000
UTM = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]


def pat(w, h):
    return [str((7 * x + 13 * y) % 251) for y in range(h) for x in range(w)]


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)

    # generation is CPU-heavy and deterministic: memoize per script hash
    import hashlib
    import shutil
    jj = os.path.join
    with open(os.path.abspath(__file__), 'rb') as fp:
        key = hashlib.sha256(fp.read()).hexdigest()[:16]
    cache = '/tmp/tilefix_' + key
    if os.path.exists(jj(cache, '.done')):
        for f in os.listdir(cache):
            if f != '.done':
                shutil.copy2(jj(cache, f), jj(d, f))
        return
    build(d)
    tmp = cache + '.tmp%d' % os.getpid()
    os.makedirs(tmp, exist_ok=True)
    for f in os.listdir(d):
        shutil.copy2(jj(d, f), jj(tmp, f))
    with open(jj(tmp, '.done'), 'w'):
        pass
    try:
        os.rename(tmp, cache)
    except OSError:
        shutil.rmtree(tmp, ignore_errors=True)


def build(d):
    j = os.path.join
    files = []

    def tif(name, *a, **kw):
        p = j(d, name)
        write_tif(p, *a, **kw)
        files.append(p)

    gt = [500000.0, 10.0, 0.0, 4000000.0, 0.0, -10.0]
    tif('big600.tif', 'Byte', 600, 400, [pat(600, 400)], gt=gt, geokeys=UTM)
    tif('odd601.tif', 'Byte', 601, 399, [pat(601, 399)], gt=gt, geokeys=UTM)
    tif('w1200.tif', 'Byte', 1200, 300, [pat(1200, 300)], gt=gt,
        geokeys=UTM)
    tif('w768.tif', 'Byte', 768, 512, [pat(768, 512)], gt=gt, geokeys=UTM)
    tif('nogt700.tif', 'Byte', 700, 300, [pat(700, 300)])

    seq = [str(i) for i in range(30)]
    gt5 = [0.0, 10.0, 0.0, 50.0, 0.0, -10.0]
    tif('utm.tif', 'Byte', 6, 5, [seq], gt=gt5, geokeys=UTM)
    tif('gtnosrs.tif', 'Byte', 6, 5, [seq], gt=gt5)
    tif('nogt.tif', 'Byte', 6, 5, [seq])
    tif('geo.tif', 'Byte', 6, 5, [seq],
        gt=[5.0, 0.05, 0.0, 44.0, 0.0, -0.05], epsg=4326)
    tif('f32.tif', 'Float32', 6, 5, [[str(i + 0.25) for i in range(30)]],
        gt=gt5, geokeys=UTM)
    tif('rgb.tif', 'Byte', 6, 5,
        [[str(3 * i) for i in range(30)], [str(5 * i) for i in range(30)],
         [str(7 * i) for i in range(30)]], gt=gt5, geokeys=UTM)

    b20 = [str((i % 20 * 7 + i // 20 * 13) % 256) for i in range(400)]
    tif('byteutm.tif', 'Byte', 20, 20, [b20],
        gt=[440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0], geokeys=UTM)

    MERC = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 3857]
    orig = 20037508.342789244
    res6 = 156543.03392804097 / 64
    tif('al300.tif', 'Byte', 300, 300,
        [[str((97 * x + 15 * y) % 251) for y in range(300)
          for x in range(300)]],
        gt=[-orig + 40 * 256 * res6, res6, 0.0,
            orig - 30 * 256 * res6, 0.0, -res6], geokeys=MERC)

    for p in files:
        os.utime(p, (MTIME, MTIME))
    print('done')


if __name__ == '__main__':
    main()
