import shutil
import struct
import sys

sys.path.insert(0, '/workspace/tests')
from mk_tail_fixtures import write_tif
from mk_color_fixtures import write_pal_tif


def write_ovr_tif(out, w, h, px, ow, oh, opx):
    def ifd(entries, nxt):
        b = struct.pack('<H', len(entries))
        for t, ty, c, v in entries:
            b += struct.pack('<HHII', t, ty, c, v)
        return b + struct.pack('<I', nxt)

    main = bytes(px)
    ovr = bytes(opx)
    off_main = 8
    ifd1_off = off_main + len(main)
    ifd1_len = 2 + 12 * 8 + 4
    off_ovr = ifd1_off + ifd1_len
    ifd2_off = off_ovr + len(ovr)
    e1 = [(256, 3, 1, w), (257, 3, 1, h), (258, 3, 1, 8), (259, 3, 1, 1),
          (262, 3, 1, 1), (273, 4, 1, off_main), (278, 3, 1, h),
          (279, 4, 1, len(main))]
    e2 = [(254, 4, 1, 1), (256, 3, 1, ow), (257, 3, 1, oh), (258, 3, 1, 8),
          (259, 3, 1, 1), (262, 3, 1, 1), (273, 4, 1, off_ovr),
          (278, 3, 1, oh), (279, 4, 1, len(ovr))]
    with open(out, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, ifd1_off))
        f.write(main)
        f.write(ifd(e1, ifd2_off))
        f.write(ovr)
        f.write(ifd(e2, 0))


def make():
    seq = [str(i) for i in range(12)]
    gt = (0, 1, 0, 3, 0, -1)
    write_tif('a.tif', 'Byte', 4, 3, [seq], gt=gt, epsg=32633)
    shutil.copy('a.tif', 'a_copy.tif')
    write_tif('b_pix.tif', 'Byte', 4, 3,
              [[s if s != '5' else '9' for s in seq]], gt=gt, epsg=32633)
    write_tif('b_size.tif', 'Byte', 3, 3, [seq[:9]], gt=gt, epsg=32633)
    write_tif('b_dt.tif', 'UInt16', 4, 3, [seq], gt=gt, epsg=32633)
    write_tif('dt_pix.tif', 'UInt16', 4, 3,
              [[s if s != '5' else '900' for s in seq]], gt=gt, epsg=32633)
    write_tif('b_gt.tif', 'Byte', 4, 3, [seq], gt=(5, 1, 0, 3, 0, -1),
              epsg=32633)
    write_tif('b_nogt.tif', 'Byte', 4, 3, [seq])
    write_tif('b_crs.tif', 'Byte', 4, 3, [seq], gt=gt, epsg=32634)
    write_tif('b_nd.tif', 'Byte', 4, 3, [seq], gt=gt, epsg=32633,
              nodata='7')
    write_tif('nd5a.tif', 'Byte', 3, 1, [['1', '2', '3']], nodata='5')
    write_tif('nd6b.tif', 'Byte', 3, 1, [['1', '2', '3']], nodata='6')
    md = '<GDALMetadata>\n<Item name="FOO">%s</Item>\n</GDALMetadata>\n'
    write_tif('b_md.tif', 'Byte', 4, 3, [seq], gt=gt, epsg=32633,
              gmd=md % 'bar')
    write_tif('mdfoo1.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gmd=md % 'bar')
    write_tif('mdfoo2.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gmd=md % 'baz')
    write_tif('mdmix.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gt=(0, 1, 0, 3, 0, -1), epsg=32633,
              gmd='<GDALMetadata>\n<Item name="FOO">bar</Item>\n'
                  '<Item name="AREA_OR_POINT">Point</Item>\n'
                  '<Item name="ZZZ">z</Item>\n</GDALMetadata>\n')
    write_tif('mdpoint.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gmd='<GDALMetadata>\n<Item name="AREA_OR_POINT">Point</Item>'
                  '\n</GDALMetadata>\n')
    write_tif('b_2b.tif', 'Byte', 4, 3,
              [seq, [str(i * 2 % 256) for i in range(12)]], gt=gt,
              epsg=32633)
    write_tif('m2a.tif', 'Byte', 3, 2,
              [[str(i) for i in range(6)], [str(i + 10) for i in range(6)]])
    write_tif('m2b.tif', 'Byte', 3, 2,
              [[str(i if i != 2 else 5) for i in range(6)],
               [str(i + 10 if i != 4 else 0) for i in range(6)]])
    write_tif('f32a.tif', 'Float32', 3, 1, [['0.5', '1.25', '-3.5']])
    write_tif('f32b.tif', 'Float32', 3, 1, [['0.5', '1.5', '-13.75']])
    write_tif('fnan_a.tif', 'Float32', 3, 1, [['nan', '2', '3']])
    write_tif('fnan_b.tif', 'Float32', 3, 1, [['nan', '2', '4']])
    write_tif('if_a.tif', 'Byte', 3, 1, [['1', '2', '3']])
    write_tif('if_b.tif', 'Float32', 3, 1, [['1', '2', '4.5']])
    write_tif('h_a.tif', 'Byte', 3, 2, [[str(i) for i in range(6)]])
    write_tif('h_b.tif', 'Byte', 3, 3, [[str(i) for i in range(9)]])
    write_tif('crsonly_a.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gt=(0, 1, 0, 3, 0, -1), epsg=32633)
    write_tif('crsonly_b.tif', 'Byte', 3, 1, [['1', '9', '3']],
              gt=(0, 1, 0, 3, 0, -1))
    write_tif('gtonly_a.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gt=(0, 1, 0, 3, 0, -1))
    write_tif('gtonly_b.tif', 'Byte', 3, 1, [['1', '9', '3']])
    write_tif('bg_a.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gt=(0, 1, 0, 3, 0, -1), epsg=32633)
    write_tif('bg_b.tif', 'Byte', 3, 1, [['1', '9', '3']])
    write_tif('gray83.tif', 'Byte', 8, 3,
              [[str(i * 10 % 256) for i in range(24)]])
    write_pal_tif('pal4c.tif', 8, 3,
                  [(x + y) % 4 for y in range(3) for x in range(8)],
                  [(0, 0, 0), (255, 0, 0), (0, 255, 0), (10, 20, 30)])
    write_pal_tif('palA.tif', 4, 2, [0, 1, 2, 3, 0, 1, 2, 3],
                  [(0, 0, 0), (255, 0, 0), (0, 255, 0), (10, 20, 30)])
    write_pal_tif('palB.tif', 4, 2, [0, 1, 2, 3, 0, 1, 2, 3],
                  [(0, 0, 0), (255, 0, 0), (0, 250, 0), (10, 20, 30)])
    write_ovr_tif('ovr2.tif', 4, 3, range(12), 2, 2, [0, 2, 8, 10])
    write_tif('ovrbase.tif', 'Byte', 4, 3, [seq])
    write_tif('mdzba.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gmd='<GDALMetadata><Item name="ZZZ">z</Item>'
                  '<Item name="BBB">b</Item>'
                  '<Item name="AAA">a</Item></GDALMetadata>')
    write_tif('mdempty.tif', 'Byte', 3, 1, [['1', '2', '3']])
    write_tif('mdooo.tif', 'Byte', 3, 1, [['1', '2', '3']],
              gmd='<GDALMetadata><Item name="ZZZ">o</Item>'
                  '<Item name="BBB">o</Item>'
                  '<Item name="AAA">o</Item></GDALMetadata>')
    write_tif('dim_nd.tif', 'Byte', 5, 5, [['1'] * 25], nodata='3')
    write_tif('dim_sm.tif', 'Byte', 3, 2, [['1'] * 6])
    write_tif('dim_w.tif', 'Byte', 3, 5, [['1'] * 15], nodata='7')
    write_tif('dim_full.tif', 'Byte', 5, 5, [['1'] * 25],
              gt=(0, 1, 0, 5, 0, -1), epsg=4326,
              gmd='<GDALMetadata><Item name="FOO">bar</Item>'
                  '</GDALMetadata>')
    write_tif('dim_f2.tif', 'Byte', 3, 2, [['1'] * 6],
              gt=(0, 2, 0, 5, 0, -2), epsg=32631, nodata='3')
    write_tif('dim_2b.tif', 'Byte', 3, 2, [['1'] * 6, ['2'] * 6])
    write_tif('dim_i2b.tif', 'Int16', 5, 5, [['1'] * 25, ['2'] * 25],
              nodata='3')
    write_tif('dim_w2.tif', 'Byte', 5, 2, [['1'] * 10])
    write_tif('dim_crs1b.tif', 'Byte', 5, 5, [['1'] * 25],
              gt=(0, 1, 0, 5, 0, -1), epsg=4326)
    write_tif('dim_no2b.tif', 'Byte', 5, 5, [['1'] * 25, ['2'] * 25])
    with open('a.vrt', 'w') as f:
        f.write('''<VRTDataset rasterXSize="4" rasterYSize="3">
  <SRS dataAxisToSRSAxisMapping="1,2">EPSG:32633</SRS>
  <GeoTransform>  0.0000000000000000e+00,  1.0000000000000000e+00,  0.0000000000000000e+00,  3.0000000000000000e+00,  0.0000000000000000e+00, -1.0000000000000000e+00</GeoTransform>
  <VRTRasterBand dataType="Byte" band="1">
    <SimpleSource>
      <SourceFilename relativeToVRT="1">a.tif</SourceFilename>
      <SourceBand>1</SourceBand>
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
''')
