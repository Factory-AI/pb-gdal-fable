#!/usr/bin/env python3
"""Measured-shapefile and .shx fixtures (pure python)."""
import os
import struct
import sys

d = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(d, exist_ok=True)
NOD = -1.7976931348623157e308


def write_shp(stem, shptype, recs, bbox, zr=(0, 0), mr=(0, 0), shx=True):
    body = b""
    sx = b""
    for i, content in enumerate(recs):
        sx += struct.pack(">ii", (100 + len(body)) // 2, len(content) // 2)
        body += struct.pack(">ii", i + 1, len(content) // 2) + content
    exts = [(".shp", body)] + ([(".shx", sx)] if shx else [])
    for ext, dat in exts:
        hdr = struct.pack(">i5ii", 9994, 0, 0, 0, 0, 0, (100 + len(dat)) // 2)
        hdr += struct.pack("<ii8d", 1000, shptype, *bbox, *zr, *mr)
        open(os.path.join(d, stem + ext), "wb").write(hdr + dat)
    n = len(recs)
    hdr = bytearray(32)
    hdr[0] = 3
    hdr[1:4] = bytes((126, 7, 28))
    struct.pack_into("<I", hdr, 4, n)
    struct.pack_into("<H", hdr, 8, 65)
    struct.pack_into("<H", hdr, 10, 10)
    hdr[29] = 87
    dbf = bytes(hdr)
    fd = bytearray(32)
    fd[0:1] = b"a"
    fd[11:12] = b"N"
    fd[16] = 9
    dbf += bytes(fd) + b"\x0d"
    for i in range(n):
        dbf += b" " + ("%9d" % (i + 1)).encode()
    dbf += b"\x1a"
    open(os.path.join(d, stem + ".dbf"), "wb").write(dbf)


def arcm(m1, m2, x0=1.0):
    c = struct.pack("<i", 23) + struct.pack("<4d", x0, 2, x0 + 2, 4)
    c += struct.pack("<ii", 1, 2) + struct.pack("<i", 0)
    c += struct.pack("<2d", x0, 2.0) + struct.pack("<2d", x0 + 2, 4.0)
    c += struct.pack("<2d", min(m1, m2), max(m1, m2))
    c += struct.pack("<2d", m1, m2)
    return c


def arcnom(x0=1.0):
    c = struct.pack("<i", 23) + struct.pack("<4d", x0, 2, x0 + 2, 4)
    c += struct.pack("<ii", 1, 2) + struct.pack("<i", 0)
    c += struct.pack("<2d", x0, 2.0) + struct.pack("<2d", x0 + 2, 4.0)
    return c


write_shp("rpm", 21, [struct.pack("<i3d", 21, 1.0, 2.0, 7.5)], (1, 2, 1, 2),
          mr=(7.5, 7.5))
write_shp("rpmn", 21, [struct.pack("<i2d", 21, 1.0, 2.0) +
                       struct.pack("<d", NOD)], (1, 2, 1, 2))
write_shp("rpzm", 11, [struct.pack("<i4d", 11, 1.0, 2.0, 3.0, 9.25)],
          (1, 2, 1, 2), zr=(3, 3), mr=(9.25, 9.25))
write_shp("ram", 23, [arcm(5.5, 6.5)], (1, 2, 3, 4), mr=(5.5, 6.5))
write_shp("mix1", 23, [arcm(NOD, NOD), arcm(5.0, 6.0, x0=5)], (1, 2, 7, 4))
write_shp("mix2", 23, [arcm(5.0, 6.0), arcm(NOD, NOD, x0=5)], (1, 2, 7, 4))
write_shp("mix3", 23, [arcm(5.0, NOD)], (1, 2, 3, 4))
write_shp("mnull", 23, [struct.pack("<i", 0), arcm(5.0, 6.0, x0=5)],
          (1, 2, 7, 4))
write_shp("mshort", 23, [arcnom(), arcm(5.0, 6.0, x0=5)], (1, 2, 7, 4))
write_shp("thr", 23, [arcm(-2e38, -2e38)], (1, 2, 3, 4), mr=(-2e38, -2e38))
write_shp("e23", 23, [], (0, 0, 0, 0))

c = struct.pack("<i", 13) + struct.pack("<4d", 1, 2, 3, 4)
c += struct.pack("<ii", 1, 2) + struct.pack("<i", 0)
c += struct.pack("<2d", 1, 2) + struct.pack("<2d", 3, 4)
c += struct.pack("<2d", 7, 8) + struct.pack("<2d", 7, 8)
c += struct.pack("<2d", NOD, NOD) + struct.pack("<2d", NOD, NOD)
write_shp("zmn", 13, [c], (1, 2, 3, 4), zr=(7, 8))

c = struct.pack("<i", 28) + struct.pack("<4d", 1, 2, 3, 4)
c += struct.pack("<i", 2) + struct.pack("<4d", 1, 2, 3, 4)
c += struct.pack("<2d", 5, 6) + struct.pack("<2d", 5, 6)
write_shp("mpm", 28, [c], (1, 2, 3, 4), mr=(5, 6))

c = struct.pack("<i", 25) + struct.pack("<4d", 0, 0, 4, 4)
c += struct.pack("<ii", 1, 5) + struct.pack("<i", 0)
for x, y in [(0, 0), (0, 4), (4, 4), (4, 0), (0, 0)]:
    c += struct.pack("<2d", float(x), float(y))
c += struct.pack("<2d", 1, 5) + struct.pack("<5d", 1, 2, 3, 4, 5)
write_shp("pgm", 25, [c], (0, 0, 4, 4), mr=(1, 5))

# .shx pathology set: missing / truncated / zero-length
write_shp("ns", 23, [arcm(5.5, 6.5)], (1, 2, 3, 4), mr=(5.5, 6.5),
          shx=False)
write_shp("ts", 23, [arcm(5.5, 6.5)], (1, 2, 3, 4), mr=(5.5, 6.5))
open(os.path.join(d, "ts.shx"), "r+b").truncate(60)
write_shp("zs", 23, [arcm(5.5, 6.5)], (1, 2, 3, 4), mr=(5.5, 6.5))
open(os.path.join(d, "zs.shx"), "wb").write(b"")
for sub, stems in (("dd", ["b1", "x2"]), ("db1", ["only"])):
    os.makedirs(os.path.join(d, sub), exist_ok=True)
    for s in stems:
        write_shp(os.path.join(sub, s), 23, [arcm(5.5, 6.5)], (1, 2, 3, 4),
                  mr=(5.5, 6.5), shx=(s == "x2"))

with open(os.path.join(d, "ap1.json"), "w") as f:
    f.write('{"type":"FeatureCollection","features":[{"type":"Feature",'
            '"properties":{"a":9},"geometry":{"type":"LineString",'
            '"coordinates":[[10,11],[12,13]]}}]}')
with open(os.path.join(d, "tiny.json"), "w") as f:
    f.write('{"type":"FeatureCollection","features":[{"type":"Feature",'
            '"properties":{},"geometry":{"type":"Point","coordinates":'
            '[1.25e-08,2e120,1.5e200]}}]}')
