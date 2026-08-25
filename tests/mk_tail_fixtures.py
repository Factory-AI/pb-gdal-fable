#!/usr/bin/env python3
"""Fixture set for the raster tail verbs (set-type/scale/unscale).

usage: mk_tail_fixtures.py [outdir]

Standalone GTiff writer: little-endian, uncompressed, contiguous strips,
one strip. Integer values are parsed exactly (no float round trip) so the
64-bit extremes survive.
"""
import math
import os
import struct
import sys

DT = {
    'Byte': (1, 8, 'B'), 'Int8': (2, 8, 'b'),
    'UInt16': (1, 16, 'H'), 'Int16': (2, 16, 'h'),
    'UInt32': (1, 32, 'I'), 'Int32': (2, 32, 'i'),
    'UInt64': (1, 64, 'Q'), 'Int64': (2, 64, 'q'),
    'Float16': (3, 16, 'e'),
    'Float32': (3, 32, 'f'), 'Float64': (3, 64, 'd'),
    'CInt16': (5, 32, 'hh'), 'CInt32': (5, 64, 'ii'),
    'CFloat32': (6, 64, 'ff'), 'CFloat64': (6, 128, 'dd'),
}


def parse(v):
    if isinstance(v, str):
        if any(c in v for c in '.eE') or v in ('nan', 'inf', '-inf'):
            return float(v)
        return int(v)
    return v


def write_tif(out, dtype, w, h, bands, gt=None, epsg=None, nodata=None,
              gmd=None, geokeys=None):
    sf, bits, fmt = DT[dtype]
    nb = len(bands)
    for b in bands:
        assert len(b) == w * h * len(fmt), out
    px = b''
    for r in range(h):
        for c in range(w):
            for b in bands:
                for k in range(len(fmt)):
                    v = parse(b[(r * w + c) * len(fmt) + k])
                    px += struct.pack('<' + fmt[k],
                                      v if fmt[k] in 'efd' else int(v))
    tags = []
    tags.append((256, 3, [w]))
    tags.append((257, 3, [h]))
    tags.append((258, 3, [bits] * nb))
    tags.append((259, 3, [1]))
    tags.append((262, 3, [1]))
    tags.append((273, 4, [0]))
    tags.append((277, 3, [nb]))
    tags.append((278, 3, [h]))
    tags.append((279, 4, [len(px)]))
    tags.append((284, 3, [1]))
    tags.append((339, 3, [sf] * nb))
    if nb > 1:
        tags.append((338, 3, [0] * (nb - 1)))
    if gt:
        tags.append((33550, 12, [gt[1], -gt[5], 0.0]))
        tags.append((33922, 12, [0.0, 0.0, 0.0, gt[0], gt[3], 0.0]))
    if geokeys:
        tags.append((34735, 3, geokeys))
    elif epsg:
        tags.append((34735, 3, [1, 1, 0, 3, 1024, 0, 1, 2,
                                2048, 0, 1, epsg, 1025, 0, 1, 1]))
    if gmd:
        tags.append((42112, 2, list(gmd.encode() + b'\0')))
    if nodata is not None:
        tags.append((42113, 2, list((nodata + '\0').encode())))
    tags.sort()

    TYPSZ = {2: 1, 3: 2, 4: 4, 12: 8}
    nent = len(tags)
    data_off = 8 + 2 + nent * 12 + 4
    entries = b''
    ext_off = data_off
    ext_chunks = []
    for code, typ, vals in tags:
        cnt = len(vals)
        sz = TYPSZ[typ] * cnt
        if typ == 2:
            raw = bytes(vals)
        elif typ == 12:
            raw = b''.join(struct.pack('<d', v) for v in vals)
        else:
            f2 = {3: 'H', 4: 'I'}[typ]
            raw = b''.join(struct.pack('<' + f2, int(v)) for v in vals)
        if sz <= 4:
            entries += struct.pack('<HHI', code, typ, cnt) + raw.ljust(4,
                                                                       b'\0')
        else:
            entries += struct.pack('<HHII', code, typ, cnt, ext_off)
            ext_chunks.append(raw)
            ext_off += sz + (sz & 1)
    extras = b''
    for raw in ext_chunks:
        extras += raw
        if len(raw) & 1:
            extras += b'\0'
    strip_off = ext_off
    body = bytearray(struct.pack('<H', nent) + entries +
                     struct.pack('<I', 0) + extras + px)
    for idx, (code, typ, vals) in enumerate(tags):
        if code == 273:
            off = 2 + idx * 12 + 8
            body[off:off + 4] = struct.pack('<I', strip_off)
    with open(out, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, 8) + bytes(body))


def gmd_scale(pairs):
    x = '<GDALMetadata>\n'
    for sample, offset, scale in pairs:
        x += ('  <Item name="OFFSET" sample="%d" role="offset">%s</Item>\n'
              % (sample, offset))
        x += ('  <Item name="SCALE" sample="%d" role="scale">%s</Item>\n'
              % (sample, scale))
    x += '</GDALMetadata>\n'
    return x


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    gt = [100.0, 0.5, 0.0, 200.0, 0.0, -0.5]
    j = os.path.join
    write_tif(j(d, 'b.tif'), 'Byte', 4, 2,
              [[0, 1, 5, 254, 255, 128, 64, 32]], gt=gt, epsg=4326)
    write_tif(j(d, 'i16.tif'), 'Int16', 3, 2,
              [[-300, 5, 7, 32767, -32768, 0]], nodata='-300')
    write_tif(j(d, 'u16.tif'), 'UInt16', 3, 1, [[0, 777, 65535]])
    write_tif(j(d, 'i32.tif'), 'Int32', 2, 2,
              [[-2147483648, 2147483647, 42, -7]])
    write_tif(j(d, 'u32.tif'), 'UInt32', 2, 1, [[0, 4294967295]])
    write_tif(j(d, 'i8.tif'), 'Int8', 3, 1, [[-128, 127, -5]])
    write_tif(j(d, 'u64.tif'), 'UInt64', 2, 1,
              [['0', '18446744073709551615']])
    write_tif(j(d, 'i64.tif'), 'Int64', 2, 1,
              [['-9223372036854775808', '9223372036854775807']])
    write_tif(j(d, 'f32.tif'), 'Float32', 4, 1,
              [[-3.25, 0.5, math.nan, 3.25e38]])
    write_tif(j(d, 'inf.tif'), 'Float32', 3, 1,
              [[math.inf, -math.inf, 1e38]])
    write_tif(j(d, 'f32n.tif'), 'Float32', 3, 1,
              [[math.nan, 5.5, -1.0]], nodata='nan')
    write_tif(j(d, 'f64.tif'), 'Float64', 3, 1,
              [[-1.5, 2.25e-300, 1e300]])
    write_tif(j(d, 'cs.tif'), 'Byte', 3, 1, [[10, 20, 200]],
              gmd=gmd_scale([(0, '10', '2')]))
    write_tif(j(d, 'cs0.tif'), 'Float32', 3, 1, [[1.5, -8, 100]],
              gmd=gmd_scale([(0, '-3', '0.5')]))
    write_tif(j(d, 'csid.tif'), 'Byte', 2, 1, [[7, 9]],
              gmd=gmd_scale([(0, '0', '1')]))
    write_tif(j(d, 'ci16.tif'), 'CInt16', 2, 1, [[3, -4, 100, 7]])
    write_tif(j(d, 'cf32.tif'), 'CFloat32', 2, 1,
              [[1.5, -2.5, math.nan, 0.0]])
    write_tif(j(d, 'm3.tif'), 'Byte', 2, 2,
              [[1, 2, 3, 4], [10, 20, 30, 40], [5, 5, 5, 5]],
              gmd=gmd_scale([(1, '100', '3')]))
    write_tif(j(d, 'cn.tif'), 'Byte', 2, 2, [[9, 9, 9, 9]])
    write_tif(j(d, 'an.tif'), 'Int16', 2, 1, [[5, 5]], nodata='5')
    write_tif(j(d, 'tmpl.tif'), 'Byte', 2, 1, [[1, 2]],
              gt=[100.5, 0.5, 0.0, 200.0, 0.0, -0.5], epsg=4326)
    write_tif(j(d, 'gtns.tif'), 'Byte', 4, 2, [[1, 2, 3, 4, 5, 6, 7, 8]],
              gt=gt)
    write_tif(j(d, 'bn.tif'), 'Byte', 4, 2,
              [[0, 1, 5, 254, 255, 128, 64, 32]], gt=gt, epsg=4326,
              nodata='255')
    write_tif(j(d, 'ck16.tif'), 'CInt16', 3, 2,
              [[3, -4, 100, 7, -20, 5, 0, 0, 9, -9, 32000, -32000]])
    write_tif(j(d, 'ck32.tif'), 'CInt32', 3, 2,
              [[3, -4, 100000, 7, -20, 5, 0, 0, 9, -9,
                2000000, -2000000]])
    write_tif(j(d, 'ckf32.tif'), 'CFloat32', 3, 2,
              [[3.5, -4.25, 100.5, 7, -20, 5.75, 0, 0, 9.5, -9,
                320.25, -320]])
    write_tif(j(d, 'ckf64.tif'), 'CFloat64', 3, 2,
              [[3.5, -4.25, 100.5, 7, -20, 5.75, 0, 0, 9.5, -9,
                320.25, -320]])
    write_tif(j(d, 'ckfn.tif'), 'CFloat32', 3, 2,
              [['nan', 1, 'inf', 2, -3.5, '-inf', 1e30, -1e30,
                0.5, -0.5, 65504.0, 7]])
    write_tif(j(d, 'ckh.tif'), 'Float16', 3, 2,
              [[7.5, -1.25, 0.000061, 65504.0, -65504.0, 2.5]])
    write_tif(j(d, 'ckbig.tif'), 'Float64', 3, 1,
              [['nan', 1e300, -1e300]])
    write_tif(j(d, 'nd64.tif'), 'Int64', 2, 1, [[1, 2]],
              nodata='9223372036854775807')
    write_tif(j(d, 'ndu64.tif'), 'UInt64', 2, 1, [[1, 2]],
              nodata='18446744073709551614')
    write_tif(j(d, 'ndu7.tif'), 'UInt64', 2, 1, [[1, 2]], nodata='7')
    write_tif(j(d, 'ndh.tif'), 'Float16', 2, 1, [[7.5, 1]], nodata='7.5')
    write_tif(j(d, 'ndc.tif'), 'CInt16', 2, 1, [[3, -4, 100, 7]],
              nodata='32768')
    write_tif(j(d, 'nde.tif'), 'Int64', 2, 1, [[1, 2]],
              nodata='9.2233720368547758e+18')
    for name, src, dt, w, h in (
            ('cv16.vrt', 'ck16.tif', 'CInt16', 3, 2),
            ('cvh.vrt', 'ckh.tif', 'Float16', 3, 2),
            ('cvf64.vrt', 'ckf64.tif', 'CFloat64', 3, 2)):
        with open(j(d, name), 'w') as f:
            f.write(f'''<VRTDataset rasterXSize="{w}" rasterYSize="{h}">
  <Metadata domain="IMAGE_STRUCTURE">
    <MDI key="INTERLEAVE">BAND</MDI>
  </Metadata>
  <VRTRasterBand dataType="{dt}" band="1">
    <ColorInterp>Gray</ColorInterp>
    <SimpleSource>
      <SourceFilename relativeToVRT="1">{src}</SourceFilename>
      <SourceBand>1</SourceBand>
      <SourceProperties RasterXSize="{w}" RasterYSize="{h}" DataType="{dt}" BlockXSize="{w}" BlockYSize="{h}" />
      <SrcRect xOff="0" yOff="0" xSize="{w}" ySize="{h}" />
      <DstRect xOff="0" yOff="0" xSize="{w}" ySize="{h}" />
    </SimpleSource>
  </VRTRasterBand>
</VRTDataset>
''')


if __name__ == '__main__':
    main()
