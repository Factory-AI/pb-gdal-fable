#!/usr/bin/env python3
"""Fixtures for cases_rpolyfoot.txt (raster polygonize / raster footprint)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

out = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)
def P(n): return os.path.join(out, n)

UTM11 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
G10 = (0.0, 10.0, 0.0, 50.0, 0.0, -10.0)
G1 = (0.0, 1.0, 0.0, 0.0, 0.0, -1.0)

# srsless 3x2, two regions of 5 around a column of 2
write_tif(P('plain.tif'), 'Byte', 3, 2, [[5, 5, 2, 2, 5, 5]])
# 6x5 UTM11: ring of 5s with a 7-frame holding a hole
write_tif(P('hole.tif'), 'Byte', 6, 5,
          [[5,5,5,5,5,5,5,7,7,7,7,5,5,7,5,5,7,5,5,7,7,7,7,5,5,5,5,5,5,5]],
          gt=G10, geokeys=UTM11)
# 6x5 UTM11: four staircase regions
write_tif(P('r1.tif'), 'Byte', 6, 5,
          [[1,1,2,2,3,3,1,1,2,2,3,3,1,4,4,2,3,3,1,4,4,2,2,3,1,1,4,2,2,3]],
          gt=G10, geokeys=UTM11)
# same pixels without any georeferencing
write_tif(P('rnogt.tif'), 'Byte', 6, 5,
          [[1,1,2,2,3,3,1,1,2,2,3,3,1,4,4,2,3,3,1,4,4,2,2,3,1,1,4,2,2,3]])
# disconnected corner blocks separated by nodata 0
write_tif(P('disc.tif'), 'Byte', 6, 5,
          [[1,1,0,0,2,2,1,1,0,0,2,2,0,0,0,0,0,0,3,3,0,0,4,4,3,3,0,0,4,4]],
          gt=G10, geokeys=UTM11, nodata='0')
# region with two holes punched by nodata 9
write_tif(P('twohole.tif'), 'Byte', 6, 3,
          [[1,1,1,1,1,1,1,0,1,0,1,1,1,1,1,1,1,1]], gt=G1, geokeys=UTM11,
          nodata='9')
# pinch: regions touching at corners (emission-order discriminator)
write_tif(P('pinch.tif'), 'Byte', 4, 4,
          [[1,1,1,1,1,0,1,0,1,1,0,1,0,0,1,1]], gt=G1, geokeys=UTM11)
# Int32 carrying the GP_NODATA_MARKER value plus nodata 99
write_tif(P('mark.tif'), 'Int32', 6, 1, [[1,1,-51502112,-51502112,99,99]],
          gt=G1, geokeys=UTM11, nodata='99')
# Float32 marker collision plus float nodata
write_tif(P('markf.tif'), 'Float32', 6, 1,
          [[1.5,1.5,-51502112.0,-51502112.0,2.5,2.5]], gt=G1, geokeys=UTM11,
          nodata='2.5')
# Float32 NaN runs
write_tif(P('fnan.tif'), 'Float32', 6, 1, [['nan','nan',3.5,3.5,'nan','nan']],
          gt=G1, geokeys=UTM11)
# srsless Float32 with NaN and Inf (warning-order discriminator)
write_tif(P('fnanns.tif'), 'Float32', 3, 2, [['nan',1.0,1.0,2.0,'inf',2.0]],
          gt=G10)
# lossy 32-bit integer values
write_tif(P('u32b.tif'), 'UInt32', 4, 1, [[4294967295,7,4000000000,7]],
          gt=G1, geokeys=UTM11)
# srsless lossy UInt32 (warning-order discriminator)
write_tif(P('u32ns.tif'), 'UInt32', 3, 2, [[4294967295,1,1,2,4000000000,2]],
          gt=G10)
write_tif(P('i64b.tif'), 'Int64', 4, 1,
          [[9223372036854775807,9223372036854775806,5,5]], gt=G1,
          geokeys=UTM11)
write_tif(P('u64b.tif'), 'UInt64', 4, 1,
          [[18446744073709551615,18446744073709551614,5,5]], gt=G1,
          geokeys=UTM11)
write_tif(P('d64.tif'), 'Float64', 6, 1,
          [[1.0,1.000000000000001,50.0,2.0,2.0000000000000067,60.0]], gt=G1,
          geokeys=UTM11)
# 2-band mask fixture for --combine-bands / --src-nodata
write_tif(P('mb2.tif'), 'Byte', 4, 2, [[0,1,1,0,0,1,1,0],[5,5,0,0,5,5,0,0]],
          gt=G1, geokeys=UTM11, nodata='0')
# 2-band value fixture for -b selection
write_tif(P('mb3.tif'), 'Byte', 3, 2, [[1,2,3,4,5,6],[9,9,8,8,7,7]], gt=G1,
          geokeys=UTM11)
# complex dtype refusal
write_tif(P('cplx.tif'), 'CFloat32', 2, 1, [[1.0,0.0,2.0,0.0]], gt=G1,
          geokeys=UTM11)
# SRS present but no geotransform
write_tif(P('srsnogt.tif'), 'Byte', 3, 2, [[1,1,1,1,1,1]], geokeys=UTM11)
# uniform rasters (single-feature outputs)
write_tif(P('uni.tif'), 'Byte', 3, 2, [[7,7,7,7,7,7]], gt=G10)
write_tif(P('unisrs.tif'), 'Byte', 3, 2, [[7,7,7,7,7,7]], gt=G10,
          geokeys=UTM11)
# overview base (overviews added by the oracle in setup)
write_tif(P('ovr.tif'), 'Byte', 8, 8, [[7]*64], nodata='7')
# footprint nodata staircase
write_tif(P('nd.tif'), 'Byte', 8, 6,
          [[7,3,6,9,12,15,18,21,24,7,30,33,36,39,42,45,48,51,54,57,60,63,66,
            69,72,75,78,81,84,87,90,93,96,99,102,105,108,111,114,117,120,123,
            126,129,132,135,138,141]], gt=(100.0,10.0,0.0,200.0,0.0,-10.0),
          geokeys=UTM11, nodata='7')
