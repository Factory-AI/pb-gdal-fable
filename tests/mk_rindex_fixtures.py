#!/usr/bin/env python3
"""Fixture set for the raster index suite.

usage: mk_rindex_fixtures.py [outdir]

Small Byte rasters with varied georeferencing:
  utm.tif     6x5 EPSG:32611, gt [0,10,0,50,0,-10]
  utm2.tif    8x4 EPSG:32611, gt [500,5,0,300,0,-5]
  utm12.tif   6x5 EPSG:32612 (different CRS, same shape)
  big.tif     40x30 EPSG:32611, gt [1000,20,0,2000,0,-20]
  geo.tif     6x5 EPSG:4326, gt [5,0.05,0,44,0,-0.05]
  nogt.tif    6x5 no georeferencing at all
  gtnosrs.tif 6x5 gt but no SRS
  srsnogt.tif 6x5 SRS 4326 but no gt
  nonsq.tif   6x5 EPSG:32611, non-square pixels 10x5
  rot.vrt     rotated geotransform over gtnosrs.tif, EPSG:32611
  nocode.vrt  custom projected WKT without an authority code
  dird/       m.tif n.tif note.txt sub/k.tif (directory expansion)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

ROT_VRT = '''<VRTDataset rasterXSize="6" rasterYSize="5">
  <SRS>EPSG:32611</SRS>
  <GeoTransform>100.0, 10.0, 2.0, 500.0, 3.0, -10.0</GeoTransform>
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="1">gtnosrs.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
'''

NOCODE_VRT = '''<VRTDataset rasterXSize="6" rasterYSize="5">
  <SRS>PROJCS["My Custom TM",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",-117.5],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],PARAMETER["false_northing",0],UNIT["metre",1]]</SRS>
  <GeoTransform>0.0, 10.0, 0.0, 50.0, 0.0, -10.0</GeoTransform>
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="1">gtnosrs.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
'''


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    j = os.path.join
    utm = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
    utm12 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32612]
    geo = [1, 1, 0, 3, 1024, 0, 1, 2, 1025, 0, 1, 1, 2048, 0, 1, 4326]
    v30 = [str(x % 250) for x in range(30)]
    v32 = [str(x % 250) for x in range(32)]
    v1200 = [str(x % 250) for x in range(1200)]

    write_tif(j(d, 'utm.tif'), 'Byte', 6, 5, [v30],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm)
    write_tif(j(d, 'utm2.tif'), 'Byte', 8, 4, [v32],
              gt=[500.0, 5.0, 0.0, 300.0, 0.0, -5.0], geokeys=utm)
    write_tif(j(d, 'utm12.tif'), 'Byte', 6, 5, [v30],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm12)
    write_tif(j(d, 'big.tif'), 'Byte', 40, 30, [v1200],
              gt=[1000.0, 20.0, 0.0, 2000.0, 0.0, -20.0], geokeys=utm)
    write_tif(j(d, 'geo.tif'), 'Byte', 6, 5, [v30],
              gt=[5.0, 0.05, 0.0, 44.0, 0.0, -0.05], geokeys=geo)
    write_tif(j(d, 'nogt.tif'), 'Byte', 6, 5, [v30])
    write_tif(j(d, 'gtnosrs.tif'), 'Byte', 6, 5, [v30],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0])
    write_tif(j(d, 'srsnogt.tif'), 'Byte', 6, 5, [v30], geokeys=geo)
    write_tif(j(d, 'nonsq.tif'), 'Byte', 6, 5, [v30],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -5.0], geokeys=utm)
    with open(j(d, 'rot.vrt'), 'w') as f:
        f.write(ROT_VRT)
    with open(j(d, 'nocode.vrt'), 'w') as f:
        f.write(NOCODE_VRT)

    dd = j(d, 'dird')
    os.makedirs(j(dd, 'sub'), exist_ok=True)
    write_tif(j(dd, 'm.tif'), 'Byte', 6, 5, [v30],
              gt=[0.0, 10.0, 0.0, 50.0, 0.0, -10.0], geokeys=utm)
    write_tif(j(dd, 'n.tif'), 'Byte', 8, 4, [v32],
              gt=[500.0, 5.0, 0.0, 300.0, 0.0, -5.0], geokeys=utm)
    with open(j(dd, 'note.txt'), 'w') as f:
        f.write('not a raster\n')
    write_tif(j(dd, 'sub', 'k.tif'), 'Byte', 6, 5, [v30],
              gt=[900.0, 10.0, 0.0, 60.0, 0.0, -10.0], geokeys=utm)


if __name__ == '__main__':
    main()
