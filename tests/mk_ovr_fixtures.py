#!/usr/bin/env python3
"""Fixtures for cases_ovr.txt: strip-organized single-IFD GTiffs in several
data types/sizes, with and without nodata, for raster overview testing."""
import struct
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else '.'

FMT = {'Byte': (8, 1, 'B'), 'UInt16': (16, 1, 'H'), 'Int16': (16, 2, 'h'),
       'UInt32': (32, 1, 'I'), 'Int32': (32, 2, 'i'),
       'Float32': (32, 3, 'f'), 'Float64': (64, 3, 'd')}


def mktif(path, w, h, dtype, nodata=None, spp=1):
    bits, sfmt, pf = FMT[dtype]
    vals = []
    for i in range(w * h * spp):
        if pf in 'fd':
            v = ((i * 37) % 251) * 1.375 - 100.0 + (i % 7) * 0.03125
        else:
            v = (i * 37) % 251
            if pf in 'hi':
                v -= 100
        vals.append(v)
    data = b''.join(struct.pack('<' + pf, v) for v in vals)

    entries = [(256, 3, 1, struct.pack('<HH', w, 0)),
               (257, 3, 1, struct.pack('<HH', h, 0)),
               (258, 3, 1, struct.pack('<HH', bits, 0)),
               (259, 3, 1, struct.pack('<HH', 1, 0)),
               (262, 3, 1, struct.pack('<HH', 2 if spp == 3 else 1, 0)),
               (273, 4, 1, struct.pack('<I', 8)),
               (277, 3, 1, struct.pack('<HH', spp, 0)),
               (278, 3, 1, struct.pack('<HH', h, 0)),
               (279, 4, 1, struct.pack('<I', len(data))),
               (339, 3, 1, struct.pack('<HH', sfmt, 0))]
    extra = b''
    if nodata is not None:
        s = nodata.encode() + b'\0'
        if len(s) <= 4:
            entries.append((42113, 2, len(s), s.ljust(4, b'\0')))
        else:
            entries.append((42113, 2, len(s), None))
            extra = s
    entries.sort(key=lambda e: e[0])
    extra_off = 8 + len(data)
    ifd_off = extra_off + len(extra)
    body = b''
    for t, ty, c, v in entries:
        if v is None:
            body += struct.pack('<HHII', t, ty, c, extra_off)
        else:
            body += struct.pack('<HHI4s', t, ty, c, v)
    ifd = struct.pack('<H', len(entries)) + body + struct.pack('<I', 0)
    with open(path, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, ifd_off) + data + extra + ifd)


import os
for dt in FMT:
    mktif(os.path.join(OUT, 'a_%s.tif' % dt), 10, 10, dt)
    mktif(os.path.join(OUT, 'c24_%s.tif' % dt), 24, 24, dt)
mktif(os.path.join(OUT, 'odd97_Float32.tif'), 9, 7, 'Float32')
mktif(os.path.join(OUT, 'odd97_Byte.tif'), 9, 7, 'Byte')
mktif(os.path.join(OUT, 'odd97_Float64.tif'), 9, 7, 'Float64')
mktif(os.path.join(OUT, 'nd_Float32.tif'), 10, 10, 'Float32',
      nodata='-58.5')
mktif(os.path.join(OUT, 'nd_Byte.tif'), 20, 20, 'Byte', nodata='7')
mktif(os.path.join(OUT, 'nd_UInt16.tif'), 20, 20, 'UInt16', nodata='7')
mktif(os.path.join(OUT, 'b20.tif'), 20, 20, 'Byte')
mktif(os.path.join(OUT, 'b300.tif'), 300, 300, 'Byte')
mktif(os.path.join(OUT, 'u16_300.tif'), 300, 300, 'UInt16')
mktif(os.path.join(OUT, 'i16_20.tif'), 20, 20, 'Int16')
mktif(os.path.join(OUT, 'f32_20.tif'), 20, 20, 'Float32')
mktif(os.path.join(OUT, 'g21.tif'), 21, 21, 'Byte')
mktif(os.path.join(OUT, 'g32.tif'), 32, 32, 'Byte')
mktif(os.path.join(OUT, 'm3.tif'), 20, 20, 'Byte', spp=3)
mktif(os.path.join(OUT, 'm3f.tif'), 20, 20, 'Float32', spp=3)
