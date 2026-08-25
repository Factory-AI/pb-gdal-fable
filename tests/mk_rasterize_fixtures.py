#!/usr/bin/env python3
"""Fixtures for gdal vector rasterize differential cases."""
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

out = sys.argv[1] if len(sys.argv) > 1 else '.'
os.makedirs(out, exist_ok=True)


def P(name):
    return os.path.join(out, name)


def gj(name, feats):
    fc = {"type": "FeatureCollection", "features": feats}
    with open(P(name), 'w') as f:
        json.dump(fc, f)


def feat(gtype, coords, props=None):
    return {"type": "Feature", "properties": props or {},
            "geometry": {"type": gtype, "coordinates": coords}}


# ------------------------------------------------------------------
# vector fixtures
# ------------------------------------------------------------------
gj('poly.geojson', [feat("Polygon", [[[2, 2], [8, 2], [8, 8], [2, 8],
                                      [2, 2]]], {"v": 7})])
gj('two.geojson', [
    feat("Polygon", [[[0, 0], [6, 0], [6, 6], [0, 6], [0, 0]]],
         {"v": 3, "w": 1.5}),
    feat("Polygon", [[[4, 4], [10, 4], [10, 10], [4, 10], [4, 4]]],
         {"v": 9, "w": 2.5})])
gj('pconcave.geojson', [feat("Polygon", [[[1, 1], [9, 1], [9, 8.5],
                                          [5, 3.5], [1, 8.5], [1, 1]]])])
gj('phole.geojson', [feat("Polygon", [
    [[1, 1], [9, 1], [9, 9], [1, 9], [1, 1]],
    [[3, 3], [6, 3], [6, 6], [3, 6], [3, 3]]])])
gj('line.geojson', [feat("LineString", [[1, 1], [9, 9]], {"v": 4})])
gj('line3d.geojson', [feat("LineString", [[0.5, 0.5, 10],
                                          [9.5, 9.5, 90]])])
gj('x2.geojson', [
    feat("LineString", [[0.5, 0.5, 10], [9.5, 9.5, 90]]),
    feat("LineString", [[0.5, 9.5, 200], [9.5, 0.5, 400]])])
gj('mp3.geojson', [feat("MultiPolygon", [
    [[[1, 1, 40], [4, 1, 41], [4, 4, 42], [1, 4, 43], [1, 1, 40]]],
    [[[6, 6, 50], [9, 6, 51], [9, 9, 52], [6, 9, 53], [6, 6, 50]]]])])
gj('gc.geojson', [{"type": "Feature", "properties": {},
                   "geometry": {"type": "GeometryCollection",
                                "geometries": [
    {"type": "Point", "coordinates": [1.2, 1.2]},
    {"type": "Polygon", "coordinates": [[[4.4, 4.5], [7.5, 4.5],
                                         [7.5, 7.5], [4.4, 7.5],
                                         [4.4, 4.5]]]}]}}])
gj('mix.geojson', [
    feat("LineString", [[1, 1], [9, 9]]),
    feat("LineString", [[1, 9], [9, 1]]),
    feat("Polygon", [[[2, 2], [8, 2], [8, 8], [2, 8], [2, 2]]])])
gj('pt.geojson', [
    feat("Point", [2.4, 2.6]),
    feat("MultiPoint", [[7.5, 7.5], [1.1, 8.8]])])
gj('strf.geojson', [
    feat("Polygon", [[[1, 1], [5, 1], [5, 5], [1, 5], [1, 1]]],
         {"v": "12.75", "w": None}),
    feat("Polygon", [[[6, 6], [9, 6], [9, 9], [6, 9], [6, 6]]],
         {"v": "abc", "w": 3})])
gj('pbnd.geojson', [
    feat("Polygon", [[[8.237, 1.5], [4.2, 0.0], [9.105, 3.5],
                      [12.871, 1.0], [8.237, 1.5]]]),
    feat("Polygon", [[[1.2, 6.0], [1.2, 3.0], [2.6, 4.5],
                      [1.2, 6.0]]])])
gj('z3d.geojson', [
    feat("LineString", [[11.974, 6.712, 48.43], [6.67, 5.0, 53.53],
                        [8.829, 4.725, 37.76], [11.511, 0.5, 20.25],
                        [8.237, 0.5, 39.63]]),
    feat("LineString", [[13.768, 7.767, 18.05], [1.155, 6.024, 50.99],
                        [0.207, 1.794, 5.31], [0.207, 6.117, 45.72],
                        [0.207, 2.0, 34.0]])])
gj('zclip.geojson', [
    feat("LineString", [[1.162, 6.677, 486.76], [17.117, 10.0, 286.05]]),
    feat("LineString", [[6.991, 3.0, 780.78], [14.437, 9.498, 327.73]]),
    feat("LineString", [[0.883, 8.0, 9.0], [5.426, 11.261, 9.0]]),
    feat("LineString", [[12.0, 6.684, 44.2], [12.0, 10.696, 15.38]]),
    feat("LineString", [[-5.0, 5.25, 100.0], [6.0, 5.25, 7.0]]),
    feat("LineString", [[16.279, 14.0, 412.49], [-1.492, 2.233, 557.88]]),
    feat("LineString", [[5.573, 9.247, 23.95], [16.389, 13.0, 512.55]])])
gj('egeom.geojson', [feat("Polygon", [])])
gj('tinypoly.geojson', [feat("Polygon", [[[2, 2], [40, 2], [40, 20],
                                          [2, 20], [2, 2]]])])

# ------------------------------------------------------------------
# raster update targets
# ------------------------------------------------------------------
GEOG = [1, 1, 0, 3, 1024, 0, 1, 2, 2048, 0, 1, 4326, 1025, 0, 1, 1]
UTM31 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32631]
GT10 = (0, 1, 0, 10, 0, -1)

write_tif(P('u8.tif'), 'Byte', 10, 10, [[0] * 100], gt=GT10,
          geokeys=GEOG, nodata='0')
write_tif(P('u16.tif'), 'Int16', 10, 10,
          [[100] * 100, [200] * 100], gt=GT10, geokeys=GEOG,
          nodata='-99')
write_tif(P('uf64.tif'), 'Float64', 10, 10, [[1.5] * 100], gt=GT10,
          geokeys=GEOG, nodata='0')
write_tif(P('unosrs.tif'), 'Byte', 10, 10, [[0] * 100], gt=GT10,
          nodata='0')
write_tif(P('uutm.tif'), 'Float64', 10, 10, [[1] * 100],
          gt=(2, 0.6, 0, 8, 0, -0.6), geokeys=UTM31, nodata='0')


DT2 = {'Byte': (1, 8, 'B'), 'UInt16': (1, 16, 'H'),
       'Float64': (3, 64, 'd')}


def write_tif2(path, dtype, w, h, value, gt, geokeys=None, nodata='0',
               deflate=False, rps=None, tile=None, planar=1, nbands=1):
    """single-purpose writer: constant-fill gtiff with optional deflate
    compression, strip height, 256x256 tiles or separate planes"""
    sf, bits, fmt = DT2[dtype]
    bsz = bits // 8
    px1 = struct.pack('<' + fmt, value)

    blocks = []
    if tile:
        tw, th = tile
        across = (w + tw - 1) // tw
        down = (h + th - 1) // th
        row = px1 * (tw if planar == 1 else tw) * (
            nbands if planar == 1 else 1)
        raw = row * th
        nb = across * down * (nbands if planar == 2 else 1)
        blocks = [raw] * nb
    else:
        rps = rps or h
        per = (h + rps - 1) // rps
        for band in range(nbands if planar == 2 else 1):
            for s in range(per):
                rows = min(rps, h - s * rps)
                raw = px1 * (w * (nbands if planar == 1 else 1)) * rows
                blocks.append(raw)
    if deflate:
        blocks = [zlib.compress(b, 6) for b in blocks]

    tags = []
    tags.append((256, 3, [w]))
    tags.append((257, 3, [h]))
    tags.append((258, 3, [bits] * nbands))
    tags.append((259, 3, [8 if deflate else 1]))
    tags.append((262, 3, [1]))
    tags.append((277, 3, [nbands]))
    if tile:
        tags.append((322, 3, [tile[0]]))
        tags.append((323, 3, [tile[1]]))
        tags.append((324, 4, [0] * len(blocks)))
        tags.append((325, 4, [len(b) for b in blocks]))
    else:
        tags.append((273, 4, [0] * len(blocks)))
        tags.append((278, 3, [rps]))
        tags.append((279, 4, [len(b) for b in blocks]))
    tags.append((284, 3, [planar]))
    tags.append((339, 3, [sf] * nbands))
    if nbands > 1:
        tags.append((338, 3, [0] * (nbands - 1)))
    if gt:
        tags.append((33550, 12, [gt[1], -gt[5], 0.0]))
        tags.append((33922, 12, [0.0, 0.0, 0.0, gt[0], gt[3], 0.0]))
    if geokeys:
        tags.append((34735, 3, geokeys))
    if nodata is not None:
        tags.append((42113, 2, list((nodata + '\0').encode())))
    tags.sort()

    TYPSZ = {2: 1, 3: 2, 4: 4, 12: 8}
    nent = len(tags)
    data_off = 8 + 2 + nent * 12 + 4
    ext_off = data_off
    ext = []
    slots = {}
    for code, typ, vals in tags:
        sz = TYPSZ[typ] * len(vals)
        if sz > 4:
            slots[code] = ext_off
            ext_off += sz + (sz & 1)
    blk_off = ext_off
    offs = []
    for b in blocks:
        offs.append(blk_off)
        blk_off += len(b)
    for i, (code, typ, vals) in enumerate(tags):
        if code in (273, 324):
            tags[i] = (code, typ, offs)

    entries = b''
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
            entries += struct.pack('<HHI', code, typ,
                                   cnt) + raw.ljust(4, b'\0')
        else:
            entries += struct.pack('<HHII', code, typ, cnt, slots[code])
            ext.append((slots[code], raw))
    body = bytearray(blk_off)
    body[0:2 + nent * 12 + 4 - 2] = (struct.pack('<H', nent) + entries +
                                     struct.pack('<I', 0))
    for pos, raw in ext:
        body[pos - 8:pos - 8 + len(raw)] = raw
    for pos, b in zip(offs, blocks):
        body[pos - 8:pos - 8 + len(b)] = b
    with open(path, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, 8) + bytes(body))


# deflate strips: small blocks stay on the libdeflate encoder when
# rewritten, the wide one exercises the zlib fallback
write_tif2(P('udz.tif'), 'Float64', 10, 10, 0.0, GT10, geokeys=GEOG,
           deflate=True)
write_tif2(P('udzm.tif'), 'Byte', 24, 24, 3, (0, 0.25, 0, 8, 0, -0.25),
           geokeys=GEOG, deflate=True, rps=5)
write_tif2(P('udzw.tif'), 'Byte', 2000, 4, 3,
           (0, 0.025, 0, 22, 0, -5.0), geokeys=GEOG, deflate=True,
           rps=4)
write_tif2(P('udzt.tif'), 'UInt16', 40, 40, 5,
           (0, 0.25, 0, 10, 0, -0.25), geokeys=GEOG, deflate=True,
           tile=(256, 256))
write_tif2(P('utiled.tif'), 'Byte', 40, 40, 0,
           (0, 0.25, 0, 10, 0, -0.25), geokeys=GEOG, tile=(256, 256))
write_tif2(P('uplan.tif'), 'Float64', 10, 10, 0.0, GT10, geokeys=GEOG,
           planar=2, nbands=2)
