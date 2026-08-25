#!/usr/bin/env python3
"""Fixtures for cases_asfeatures.txt and cases_cleancollar.txt."""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

d = sys.argv[1] if len(sys.argv) > 1 else '.'
rng = random.Random(4242)

# ---- clean-collar inputs ----
write_tif(d + '/cc1.tif', 'Byte', 4, 2,
          [[5.0, 200, 30, 5, 5, 60, 70, 5]],
          gt=[10.0, 1.0, 0.0, 20.0, 0.0, -1.0])
write_tif(d + '/cc2.tif', 'Byte', 7, 5,
          [[rng.choice([0, 3, 14, 16, 40, 100, 200, 240, 250, 255])
            for _ in range(35)]], gt=[0.0, 1.0, 0.0, 5.0, 0.0, -1.0])
write_tif(d + '/ccrgb.tif', 'Byte', 5, 4,
          [[rng.randint(0, 255) for _ in range(20)] for _ in range(3)],
          gt=[0.0, 1.0, 0.0, 4.0, 0.0, -1.0])
write_tif(d + '/ccrgb2.tif', 'Byte', 6, 4,
          [[rng.choice([0, 5, 10, 200, 250, 255]) for _ in range(24)]
           for _ in range(3)], gt=[0.0, 1.0, 0.0, 4.0, 0.0, -1.0])
write_tif(d + '/ccb2.tif', 'Byte', 5, 3,
          [[rng.choice([0, 8, 100, 255]) for _ in range(15)]
           for _ in range(2)], gt=[0.0, 1.0, 0.0, 3.0, 0.0, -1.0])
write_tif(d + '/ccu16.tif', 'UInt16', 4, 2,
          [[5.0, 30, 500, 65535, 0, 240, 100, 20]],
          gt=[0.0, 1.0, 0.0, 2.0, 0.0, -1.0])
write_tif(d + '/cci16.tif', 'Int16', 4, 1, [[-300.0, 100, 200, 10]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0])
write_tif(d + '/ccf32.tif', 'Float32', 5, 1,
          [[0.0, 200.4, 200.5, 300.2, 5.4]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0])
write_tif(d + '/ccsrs.tif', 'Byte', 3, 2, [[5.0, 200, 5, 5, 90, 0]],
          gt=[10.0, 1.0, 0.0, 20.0, 0.0, -1.0], epsg=32631)
write_tif(d + '/ccnogt.tif', 'Byte', 3, 1, [[5.0, 200, 5]])
write_tif(d + '/ccnd.tif', 'Byte', 3, 2, [[5.0, 30, 200, 5, 99, 0]],
          gt=[10.0, 1.0, 0.0, 20.0, 0.0, -1.0], nodata='99')
write_tif(d + '/cctall.tif', 'Byte', 4000, 10,
          [[rng.choice([0, 200]) for _ in range(40000)]],
          gt=[0.0, 1.0, 0.0, 10.0, 0.0, -1.0])
write_tif(d + '/ccbig.tif', 'Byte', 21, 17,
          [[rng.choice([0, 0, 10, 16, 100, 200, 250, 255])
            for _ in range(357)]], gt=[0.0, 1.0, 0.0, 17.0, 0.0, -1.0])
write_tif(d + '/ccsame.tif', 'Byte', 4, 2, [[9.0] * 8],
          gt=[10.0, 1.0, 0.0, 20.0, 0.0, -1.0])

# ---- as-features inputs ----
write_tif(d + '/afa.tif', 'Byte', 2, 2, [[10.0, 20.0, 30.0, 40.0]],
          gt=[100.0, 10.0, 0.0, 200.0, 0.0, -10.0], epsg=32631)
write_tif(d + '/afm.tif', 'Float32', 3, 2,
          [[0.1, 2.5, float('nan'), -5.0, float('inf'), 7.0],
           [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]],
          gt=[0.0, 1.0, 0.0, 2.0, 0.0, -1.0], nodata='4')
write_tif(d + '/afg.tif', 'Byte', 2, 1, [[1.0, 2.0]])
write_tif(d + '/afw.tif', 'Byte', 2, 1, [[1.0, 2.0]],
          gt=[10.0, 0.5, 0.0, 45.0, 0.0, -0.5], epsg=4326)
write_tif(d + '/afu.tif', 'Byte', 2, 1, [[1.0, 2.0]],
          gt=[500000.0, 10.0, 0.0, 4649776.0, 0.0, -10.0], epsg=32631)
write_tif(d + '/afn.tif', 'Byte', 2, 2,
          [[4.0, 1.0, 4.0, 2.0], [9.0, 4.0, 4.0, 3.0]],
          gt=[0.0, 1.0, 0.0, 2.0, 0.0, -1.0], nodata='4')
write_tif(d + '/afnn.tif', 'Float32', 2, 1, [[float('nan'), 5.0]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0], nodata='nan')
write_tif(d + '/afi16.tif', 'Int16', 2, 1, [[-3.0, 300.0]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0])
write_tif(d + '/afrot.tif', 'Byte', 2, 2, [[1.0, 2.0, 3.0, 4.0]],
          gt=[10.0, 2.0, 0.5, 90.0, 0.25, -3.0])
write_tif(d + '/aff64.tif', 'Float64', 2, 1, [[0.1, 1e300]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0])
write_tif(d + '/afi64.tif', 'Int64', 2, 1, [[-4.0, 9007199254740992.0]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0])

# ---- error-path targets ----
open(d + '/garb.tif', 'w').write('not a tiff')
open(d + '/empty.tif', 'w').close()
os.makedirs(d + '/adir.tif', exist_ok=True)
os.makedirs(d + '/dirj.json', exist_ok=True)
write_tif(d + '/exist.tif', 'Byte', 2, 1, [[7.0, 8.0]],
          gt=[0.0, 1.0, 0.0, 1.0, 0.0, -1.0])
open(d + '/y.gdalg.json', 'w').write('{}')

# ---- pre-existing outputs for refusal / append flows ----
open(d + '/ex.json', 'w').write('')
open(d + '/ex2.json', 'w').write(
    '{"type":"FeatureCollection","features":[]}')
open(d + '/apx.json', 'w').write('''{
"type": "FeatureCollection",
"name": "pixels",
"features": [
{"type":"Feature","properties":{"BAND_1":7.0},"geometry":null}
]
}
''')
# two lines so the sequence driver (not plain GeoJSON) claims the file
open(d + '/apx.geojsonl', 'w').write(
    '{"type":"Feature","properties":{"BAND_1":7.0},"geometry":null}\n'
    '{"type":"Feature","properties":{"BAND_1":8.0},"geometry":null}\n')
