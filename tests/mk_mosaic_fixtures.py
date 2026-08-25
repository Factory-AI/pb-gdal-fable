#!/usr/bin/env python3
"""Fixtures for gdal raster mosaic / stack differential cases."""
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

# a: 3x2 Byte, ulx=0 uly=10, res 1
write_tif(P('a.tif'), 'Byte', 3, 2, [[1, 2, 3, 4, 5, 6]],
          gt=(0, 1, 0, 10, 0, -1), geokeys=UTM31)
# b: 3x2 Byte, shifted right by 2 (overlaps a by 1 col)
write_tif(P('b.tif'), 'Byte', 3, 2, [[10, 20, 30, 40, 50, 60]],
          gt=(2, 1, 0, 10, 0, -1), geokeys=UTM31)
# c: disjoint 2x2, offset (10, 4)
write_tif(P('c.tif'), 'Byte', 2, 2, [[7, 8, 9, 11]],
          gt=(10, 1, 0, 4, 0, -1), geokeys=UTM31)
# d: different resolution 0.5
write_tif(P('d.tif'), 'Byte', 4, 4, [list(range(16))],
          gt=(0, 0.5, 0, 10, 0, -0.5), geokeys=UTM31)
# e: 2-band Byte
write_tif(P('e.tif'), 'Byte', 3, 2,
          [[1, 2, 3, 4, 5, 6], [11, 12, 13, 14, 15, 16]],
          gt=(0, 1, 0, 10, 0, -1), geokeys=UTM31)
# f: Float32 with nodata
write_tif(P('f.tif'), 'Float32', 3, 2,
          [['1.5', '2.5', '3.5', '4.5', '5.5', '6.5']],
          gt=(0, 1, 0, 10, 0, -1), geokeys=UTM31, nodata='-999')
# g: no CRS
write_tif(P('g.tif'), 'Byte', 3, 2, [[1, 2, 3, 4, 5, 6]],
          gt=(0, 1, 0, 10, 0, -1))
# h: different CRS
write_tif(P('h.tif'), 'Byte', 3, 2, [[1, 2, 3, 4, 5, 6]],
          gt=(2, 1, 0, 10, 0, -1), geokeys=UTM32)
# i: no geotransform
write_tif(P('i.tif'), 'Byte', 3, 2, [[1, 2, 3, 4, 5, 6]])
# j: UInt16
write_tif(P('j.tif'), 'UInt16', 3, 2, [[100, 200, 300, 400, 500, 600]],
          gt=(2, 1, 0, 10, 0, -1), geokeys=UTM31)
# k: Byte with nodata=5
write_tif(P('k.tif'), 'Byte', 3, 2, [[1, 2, 3, 4, 5, 6]],
          gt=(2, 1, 0, 10, 0, -1), geokeys=UTM31, nodata='5')
# weird resolutions for float formatting paths
write_tif(P('w1.tif'), 'Byte', 3, 3, [list(range(9))],
          gt=(1.0 / 3, 1.0 / 3, 0, 10.123456789, 0, -1.0 / 7),
          geokeys=UTM31)
write_tif(P('w2.tif'), 'Byte', 3, 3, [list(range(10, 19))],
          gt=(2.0 / 3, 1.0 / 6, 0, 10, 0, -1.0 / 11), geokeys=UTM31)
