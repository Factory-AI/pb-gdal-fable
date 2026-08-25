#!/usr/bin/env python3
"""Fixtures for gdal raster pansharpen differential cases."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

out = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)


def P(name):
    return os.path.join(out, name)


UTM31 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32631]
UTM32 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32632]


def lcg(seed, n, mod=256):
    s = seed
    vals = []
    for _ in range(n):
        s = (s * 1103515245 + 12345) & 0x7fffffff
        vals.append((s >> 16) % mod)
    return vals


# pan: 16x16 Byte res 1
write_tif(P('pan.tif'), 'Byte', 16, 16, [lcg(31, 256)],
          gt=(0, 1, 0, 16, 0, -1), geokeys=UTM31)
# ms: 8x8 3-band Byte res 2, same extent
write_tif(P('ms.tif'), 'Byte', 8, 8, [lcg(32, 64), lcg(33, 64), lcg(34, 64)],
          gt=(0, 2, 0, 16, 0, -2), geokeys=UTM31)
# msa: 8x8 4-band
write_tif(P('msa.tif'), 'Byte', 8, 8,
          [lcg(35, 64), lcg(36, 64), lcg(37, 64), lcg(38, 64)],
          gt=(0, 2, 0, 16, 0, -2), geokeys=UTM31)
# ms1: single band
write_tif(P('ms1.tif'), 'Byte', 8, 8, [lcg(39, 64)],
          gt=(0, 2, 0, 16, 0, -2), geokeys=UTM31)
# mss: shifted extent (partial overlap) for extent adjustment
write_tif(P('mss.tif'), 'Byte', 8, 8, [lcg(40, 64), lcg(41, 64)],
          gt=(4, 2, 0, 12, 0, -2), geokeys=UTM31)
# msc: different CRS -> warning
write_tif(P('msc.tif'), 'Byte', 8, 8, [lcg(42, 64), lcg(43, 64)],
          gt=(0, 2, 0, 16, 0, -2), geokeys=UTM32)
# UInt16 pair for bit-depth cases
write_tif(P('pan16.tif'), 'UInt16', 16, 16, [lcg(44, 256, 4096)],
          gt=(0, 1, 0, 16, 0, -1), geokeys=UTM31)
write_tif(P('ms16.tif'), 'UInt16', 8, 8,
          [lcg(45, 64, 4096), lcg(46, 64, 4096)],
          gt=(0, 2, 0, 16, 0, -2), geokeys=UTM31)
# pre-existing output
write_tif(P('ex.tif'), 'Byte', 2, 2, [[1, 2, 3, 4]],
          gt=(0, 1, 0, 2, 0, -1), geokeys=UTM31)
