#!/usr/bin/env python3
"""Fixtures for cases_d2.txt: small strip GTiffs covering the GTiff
creation-option surface (PHOTOMETRIC, NBITS clipping, ENDIANNESS,
predictors, palette sources)."""
import struct
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else '.'

FMT = {'Byte': (8, 1, 'B'), 'UInt16': (16, 1, 'H'),
       'Float32': (32, 3, 'f')}


def mktif(path, w, h, dtype, spp=1, phot=None, extras=None, cmap=None,
          vals=None, bits=None, raw=None, gmd=None, geokeys=None):
    if raw is not None:
        sfmt = 1
        data = raw
    else:
        bits, sfmt, pf = FMT[dtype]
        if vals is None:
            vals = []
            for i in range(w * h * spp):
                if pf == 'f':
                    vals.append((i * 37) % 251 * 1.1 - 3.7)
                elif pf == 'H':
                    vals.append((i * 613) % 60001)
                else:
                    vals.append((i * 37) % 251)
        data = b''.join(struct.pack('<' + pf, v) for v in vals)
    if phot is None:
        phot = 3 if cmap else (2 if spp >= 3 else 1)

    tail = b''
    tail_off = 8 + len(data)

    def arr(fmt, items):
        nonlocal tail, tail_off
        raw = b''.join(struct.pack('<' + fmt, v) for v in items)
        if len(raw) <= 4:
            return len(items), raw.ljust(4, b'\0'), None
        off = tail_off
        tail += raw
        tail_off += len(raw)
        return len(items), None, off

    entries = [(256, 3, 1, struct.pack('<HH', w, 0), None),
               (257, 3, 1, struct.pack('<HH', h, 0), None),
               (259, 3, 1, struct.pack('<HH', 1, 0), None),
               (262, 3, 1, struct.pack('<HH', phot, 0), None),
               (273, 4, 1, struct.pack('<I', 8), None),
               (278, 3, 1, struct.pack('<HH', h, 0), None),
               (279, 4, 1, struct.pack('<I', len(data)), None),
               (277, 3, 1, struct.pack('<HH', spp, 0), None),
               (284, 3, 1, struct.pack('<HH', 1, 0), None)]
    c, v, off = arr('H', [bits] * spp)
    entries.append((258, 3, c, v, off))
    c, v, off = arr('H', [sfmt] * spp)
    entries.append((339, 3, c, v, off))
    if extras:
        c, v, off = arr('H', extras)
        entries.append((338, 3, c, v, off))
    if cmap:
        c, v, off = arr('H', cmap)
        entries.append((320, 3, c, v, off))
    if geokeys:
        c, v, off = arr('H', geokeys)
        entries.append((34735, 3, c, v, off))
    if gmd:
        g = gmd.encode() + b'\0'
        off = tail_off
        tail += g
        tail_off += len(g)
        entries.append((42112, 2, len(g), None, off))
    entries.sort(key=lambda e: e[0])
    body = b''
    for t, ty, c, v, off in entries:
        if v is None:
            body += struct.pack('<HHII', t, ty, c, off)
        else:
            body += struct.pack('<HHI4s', t, ty, c, v)
    ifd = struct.pack('<H', len(entries)) + body + struct.pack('<I', 0)
    with open(path, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, tail_off) + data + tail +
                ifd)


import os
mktif(os.path.join(OUT, 'gray.tif'), 8, 4, 'Byte')
mktif(os.path.join(OUT, 'rgb.tif'), 8, 4, 'Byte', spp=3)
mktif(os.path.join(OUT, 'g16.tif'), 8, 4, 'UInt16')
mktif(os.path.join(OUT, 'f32.tif'), 8, 4, 'Float32')
cmap = ([i * 257 for i in range(256)] + [0] * 256 +
        [65535] * 256)
mktif(os.path.join(OUT, 'pal.tif'), 8, 4, 'Byte', cmap=cmap)
mktif(os.path.join(OUT, 'two.tif'), 4, 3, 'Byte', spp=2, phot=1,
      extras=[0])
mktif(os.path.join(OUT, 'rgba.tif'), 3, 2, 'Byte', spp=4, phot=2,
      extras=[2], vals=[10, 20, 30, 40] * 6)
mktif(os.path.join(OUT, 'tall.tif'), 4, 9000, 'Byte',
      vals=[(i // 16 * 3) % 251 for i in range(4 * 9000)])

# MINISWHITE / MINISBLACK reader palette-synthesis surface
mktif(os.path.join(OUT, 'mw04.tif'), 6, 2, 'Byte', phot=0, bits=4,
      raw=bytes([0x5a, 0x3c, 0x71] * 2))
mktif(os.path.join(OUT, 'mw08.tif'), 8, 4, 'Byte', phot=0)
mktif(os.path.join(OUT, 'mw16.tif'), 8, 4, 'UInt16', phot=0)
mktif(os.path.join(OUT, 'mw32.tif'), 4, 2, 'Byte', phot=0, bits=32,
      raw=bytes([0x5a] * 32))
mktif(os.path.join(OUT, 'mb04.tif'), 6, 2, 'Byte', phot=1, bits=4,
      raw=bytes([0x5a, 0x3c, 0x71] * 2))
GMD_GRAY = ('<GDALMetadata>\n  <Item name="COLORINTERP" sample="0" '
            'role="colorinterp">Gray</Item>\n</GDALMetadata>\n')
mktif(os.path.join(OUT, 'gmd0.tif'), 8, 4, 'Byte', phot=0, gmd=GMD_GRAY)
mktif(os.path.join(OUT, 'gmd3.tif'), 8, 4, 'Byte', cmap=cmap, gmd=GMD_GRAY)

# minimal geokey directories: default geographic CRS with the custom
# "unknown" degree-sized angular unit (writer must keep 2055 + AUnits)
mktif(os.path.join(OUT, 'gk0.tif'), 4, 2, 'Byte',
      geokeys=[1, 1, 0, 1, 1024, 0, 1, 2])
mktif(os.path.join(OUT, 'gku.tif'), 4, 2, 'Byte',
      geokeys=[1, 1, 0, 2, 1024, 0, 1, 2, 2048, 0, 1, 32767])
