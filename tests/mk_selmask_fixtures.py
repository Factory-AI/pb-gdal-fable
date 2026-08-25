#!/usr/bin/env python3
"""Fixtures for raster select --mask (c8 stub upgrade).

usage: mk_selmask_fixtures.py [outdir]
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif


def vals(n, seed):
    rnd = random.Random(seed)
    return [float(rnd.randrange(256)) for _ in range(n)]


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)

    # generation is CPU-heavy and deterministic: memoize per script hash
    import hashlib
    import shutil
    jj = os.path.join
    with open(os.path.abspath(__file__), 'rb') as fp:
        key = hashlib.sha256(fp.read()).hexdigest()[:16]
    cache = '/tmp/selmaskfix_' + key
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
    gt = [2.0, 0.002, 0.0, 46.0, 0.0, -0.002]
    n = 200
    write_tif(j(d, 's2.tif'), 'Byte', 20, 10,
              [vals(n, 41), vals(n, 42)], gt=gt, epsg=4326)
    write_tif(j(d, 's2n.tif'), 'Byte', 20, 10,
              [vals(n, 43), vals(n, 44)], gt=gt, epsg=4326, nodata='7')
    write_tif(j(d, 's3.tif'), 'Byte', 20, 10,
              [vals(n, 45), vals(n, 46), vals(n, 47)], gt=gt,
              epsg=4326)
    nb = 600 * 400
    write_tif(j(d, 'sbig.tif'), 'Byte', 600, 400,
              [vals(nb, 51), vals(nb, 52)], gt=gt, epsg=4326,
              nodata='7')


if __name__ == '__main__':
    main()
