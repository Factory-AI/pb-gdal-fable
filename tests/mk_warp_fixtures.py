#!/usr/bin/env python3
"""Fixture set for raster reproject (leaf + pipeline + warped VRT).

usage: mk_warp_fixtures.py [outdir]

Reuses the standalone GTiff writer from mk_tail_fixtures. u1.tif keeps the
geographic-slot geokey layout carrying a projected code (32631), which the
reference answers with the GTIFF_SRS_SOURCE warning; p1.tif is the clean
projected layout.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

W_VRT = '''<VRTDataset rasterXSize="468" rasterYSize="336" subClass="VRTWarpedDataset">
  <SRS dataAxisToSRSAxisMapping="1,2">PROJCS["WGS 84 / UTM zone 31N",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",3],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],PARAMETER["false_northing",0],UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH],AUTHORITY["EPSG","32631"]]</SRS>
  <GeoTransform>  4.2185666655367491e+05,  1.7099394848929458e+02,  0.0000000000000000e+00,  5.0945335911050942e+06,  0.0000000000000000e+00, -1.7099394848929458e+02</GeoTransform>
  <Metadata>
    <MDI key="AREA_OR_POINT">Area</MDI>
  </Metadata>
  <VRTRasterBand dataType="Int32" band="1" subClass="VRTWarpedRasterBand">
    <NoDataValue>42</NoDataValue>
    <ColorInterp>Gray</ColorInterp>
  </VRTRasterBand>
  <BlockXSize>468</BlockXSize>
  <BlockYSize>128</BlockYSize>
  <GDALWarpOptions>
    <WarpMemoryLimit>6.71089e+07</WarpMemoryLimit>
    <ResampleAlg>NearestNeighbour</ResampleAlg>
    <WorkingDataType>Int32</WorkingDataType>
    <Option name="NUM_THREADS">32</Option>
    <Option name="INIT_DEST">NO_DATA</Option>
    <Option name="ERROR_OUT_IF_EMPTY_SOURCE_WINDOW">FALSE</Option>
    <SourceDataset relativeToVRT="1">big.tif</SourceDataset>
    <Transformer>
      <ApproxTransformer>
        <MaxError>0.125</MaxError>
        <BaseTransformer>
          <GenImgProjTransformer>
            <SrcGeoTransform>2,0.002,0,46,0,-0.002</SrcGeoTransform>
            <SrcInvGeoTransform>-1000,500,0,23000,0,-500</SrcInvGeoTransform>
            <DstGeoTransform>421856.66655367491,170.99394848929458,0,5094533.5911050942,0,-170.99394848929458</DstGeoTransform>
            <DstInvGeoTransform>-2467.0853575855408,0.0058481601766310872,0,29793.648466010174,0,-0.0058481601766310872</DstInvGeoTransform>
            <ReprojectTransformer>
              <ReprojectionTransformer>
                <SourceSRS>GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AXIS["Latitude",NORTH],AXIS["Longitude",EAST],AUTHORITY["EPSG","4326"]]</SourceSRS>
                <TargetSRS>PROJCS["WGS 84 / UTM zone 31N",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4326"]],PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",3],PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],PARAMETER["false_northing",0],UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH],AUTHORITY["EPSG","32631"]]</TargetSRS>
                <Options>
                  <Option key="CENTER_LONG">2.512</Option>
                  <Option key="AREA_OF_INTEREST">2,45.488,3.024,46</Option>
                </Options>
              </ReprojectionTransformer>
            </ReprojectTransformer>
          </GenImgProjTransformer>
        </BaseTransformer>
      </ApproxTransformer>
    </Transformer>
    <BandList>
      <BandMapping src="1" dst="1">
        <SrcNoDataReal>5</SrcNoDataReal>
        <SrcNoDataImag>0</SrcNoDataImag>
        <DstNoDataReal>42</DstNoDataReal>
        <DstNoDataImag>0</DstNoDataImag>
      </BandMapping>
    </BandList>
  </GDALWarpOptions>
</VRTDataset>
'''

WG_GDALG = ('{\n'
            '  "type":"gdal_streamed_alg",\n'
            '  "command_line":"gdal raster reproject --input big.tif '
            '--quiet --dst-crs EPSG:32631 --output-format stream '
            '--output streamed_dataset",\n'
            '  "gdal_version":"3130000"\n'
            '}')


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    j = os.path.join

    w, h = 512, 256
    vals = [(r * 7 + c * 13) % 2000 - 300 for r in range(h)
            for c in range(w)]
    write_tif(j(d, 'big.tif'), 'Int32', w, h, [vals],
              gt=[2.0, 0.002, 0.0, 46.0, 0.0, -0.002], epsg=4326)

    w, h = 10, 8
    vals = [(r * w + c) % 256 for r in range(h) for c in range(w)]
    write_tif(j(d, 'g1.tif'), 'Byte', w, h, [vals],
              gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326)

    w2, h2 = 7, 5
    v2 = [(r * 37 + c * 11) % 256 for r in range(h2) for c in range(w2)]
    v2[8] = 99
    write_tif(j(d, 'g2.tif'), 'Byte', w2, h2, [v2],
              gt=[-1.0, 0.05, 0.0, 44.5, 0.0, -0.05], epsg=4326,
              nodata='99')

    w3, h3 = 9, 6
    v3 = [(r * 53 + c * 7) % 256 for r in range(h3) for c in range(w3)]
    write_tif(j(d, 'u1.tif'), 'Byte', w3, h3, [v3],
              gt=[450000.0, 100.0, 0.0, 5000000.0, 0.0, -100.0],
              epsg=32631)
    write_tif(j(d, 'p1.tif'), 'Byte', w3, h3, [v3],
              gt=[450000.0, 100.0, 0.0, 5000000.0, 0.0, -100.0],
              geokeys=[1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1,
                       3072, 0, 1, 32631])

    v4a = [(r * 13 + c) % 1000 - 300 for r in range(h) for c in range(w)]
    v4b = [(r + c * 17) % 500 for r in range(h) for c in range(w)]
    write_tif(j(d, 'm2.tif'), 'Int16', w, h, [v4a, v4b],
              gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326)
    write_tif(j(d, 'm2n.tif'), 'Int16', w, h, [v4a, v4b],
              gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326, nodata='7')

    v4c = [(r * 5 + c * 3) % 400 - 100 for r in range(h) for c in range(w)]
    v4d = [(r * 11 + c * 29) % 300 for r in range(h) for c in range(w)]
    write_tif(j(d, 'mb3.tif'), 'Int16', w, h, [v4a, v4b, v4c],
              gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326)
    write_tif(j(d, 'mb4.tif'), 'Int16', w, h, [v4a, v4b, v4c, v4d],
              gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326)

    vt = [(r * w + c) * 0.5 - 3.0 for r in range(h) for c in range(w)]
    vt[3] = 7.0
    write_tif(j(d, 'tt.tif'), 'Float64', w, h, [vt],
              gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326, nodata='7')

    vf = [0.5, 1.25, -2.0, 3.75, 4.0, 5.5, -6.25, 7.0, 8.5, 9.0, 10.25,
          -11.5]
    write_tif(j(d, 'f1.tif'), 'Float32', 4, 3, [vf],
              gt=[2.0, 0.25, 0.0, 46.0, 0.0, -1.0 / 3.0], epsg=4326)

    write_tif(j(d, 'nosrs.tif'), 'Byte', 4, 3,
              [list(range(12))], gt=[0.0, 1.0, 0.0, 3.0, 0.0, -1.0])
    write_tif(j(d, 'nogt.tif'), 'UInt32', 4, 3, [list(range(12))])

    gt1 = [2.0, 0.1, 0.0, 46.0, 0.0, -0.1]
    n = w * h
    tv = {
        'Byte': [(i * 3) % 256 for i in range(n)],
        'Int8': [(i * 3) % 250 - 125 for i in range(n)],
        'UInt16': [(i * 997) % 65536 for i in range(n)],
        'Int16': [(i * 997) % 60000 - 30000 for i in range(n)],
        'UInt32': [(i * 100003) % 4294967296 for i in range(n)],
        'Int32': [(i * 100003) % 4000000000 - 2000000000
                  for i in range(n)],
        'UInt64': [i * 123456789012 for i in range(n)],
        'Int64': [(i - 40) * 98765432101 for i in range(n)],
        'Float32': [i * 0.25 - 3.5 for i in range(n)],
        'Float64': [i * 1e-3 + 0.123456789 for i in range(n)],
    }
    for dt, v in tv.items():
        write_tif(j(d, 't_%s.tif' % dt), dt, w, h, [v], gt=gt1, epsg=4326)

    with open(j(d, 'w.vrt'), 'w') as f:
        f.write(W_VRT)
    with open(j(d, 'wg.gdalg.json'), 'w') as f:
        f.write(WG_GDALG)


if __name__ == '__main__':
    main()
