#!/usr/bin/env python3
"""Fixtures for gdal raster blend differential cases."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

out = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)


def P(name):
    return os.path.join(out, name)


UTM31 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32631]
GT = (0, 1, 0, 16, 0, -1)
W = H = 16
N = W * H


def lcg(seed):
    s = seed
    vals = []
    for _ in range(N):
        s = (s * 1103515245 + 12345) & 0x7fffffff
        vals.append((s >> 16) & 255)
    return vals


# color inputs
write_tif(P('cgray.tif'), 'Byte', W, H, [lcg(1)], gt=GT, geokeys=UTM31)
write_tif(P('cga.tif'), 'Byte', W, H, [lcg(2), lcg(3)], gt=GT, geokeys=UTM31)
write_tif(P('crgb.tif'), 'Byte', W, H, [lcg(4), lcg(5), lcg(6)],
          gt=GT, geokeys=UTM31)
write_tif(P('crgba.tif'), 'Byte', W, H, [lcg(7), lcg(8), lcg(9), lcg(10)],
          gt=GT, geokeys=UTM31)
# overlays
write_tif(P('ogray.tif'), 'Byte', W, H, [lcg(11)], gt=GT, geokeys=UTM31)
write_tif(P('oga.tif'), 'Byte', W, H, [lcg(12), lcg(13)],
          gt=GT, geokeys=UTM31)
write_tif(P('orgb.tif'), 'Byte', W, H, [lcg(14), lcg(15), lcg(16)],
          gt=GT, geokeys=UTM31)
write_tif(P('orgba.tif'), 'Byte', W, H, [lcg(17), lcg(18), lcg(19), lcg(20)],
          gt=GT, geokeys=UTM31)
# rgb color for hsv-value cases (seeds picked clear of half-LSB
# float boundaries in the oracle's HSV chain)
write_tif(P('crgbh.tif'), 'Byte', W, H, [lcg(130), lcg(131), lcg(132)],
          gt=GT, geokeys=UTM31)
# size mismatch overlay
write_tif(P('osmall.tif'), 'Byte', 8, 8, [lcg(21)[:64]],
          gt=GT, geokeys=UTM31)
# 5-band input (unsupported band count)
write_tif(P('c5b.tif'), 'Byte', W, H,
          [lcg(22), lcg(23), lcg(24), lcg(25), lcg(26)],
          gt=GT, geokeys=UTM31)
# pre-existing output
write_tif(P('ex.tif'), 'Byte', 2, 2, [[1, 2, 3, 4]], gt=GT, geokeys=UTM31)
