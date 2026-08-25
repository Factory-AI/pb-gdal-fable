#!/usr/bin/env python3
"""Shapefile encoding fixtures (pure python, no GDAL needed).

Creates in the target dir:
  base ldid-87 shapefile (fields nam\xe9/b latin-1, value caf\xe9) as
  t_l87.*, plus patched variants (LDID byte / .cpg files) and a few json
  inputs for write-side cases.
"""
import os
import shutil
import struct
import sys

d = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(d, exist_ok=True)


def mk_base(stem):
    # 1 point feature (1,2), fields: nam\xe9 C80 = 'caf\xe9', b N9 = 1
    shp_rec = struct.pack(">ii", 1, 10) + struct.pack("<i2d", 1, 1.0, 2.0)
    shp_hdr = struct.pack(">i5ii", 9994, 0, 0, 0, 0, 0, (100 + len(shp_rec)) // 2)
    shp_hdr += struct.pack("<ii8d", 1000, 1, 1.0, 2.0, 1.0, 2.0, 0, 0, 0, 0)
    open(os.path.join(d, stem + ".shp"), "wb").write(shp_hdr + shp_rec)
    shx_rec = struct.pack(">ii", 50, 10)
    shx_hdr = struct.pack(">i5ii", 9994, 0, 0, 0, 0, 0, (100 + len(shx_rec)) // 2)
    shx_hdr += struct.pack("<ii8d", 1000, 1, 1.0, 2.0, 1.0, 2.0, 0, 0, 0, 0)
    open(os.path.join(d, stem + ".shx"), "wb").write(shx_hdr + shx_rec)
    fields = [(b"nam\xe9", b"C", 80, 0), (b"b", b"N", 9, 0)]
    hdr = bytearray(32)
    hdr[0] = 3
    hdr[1:4] = bytes((126, 7, 28))
    struct.pack_into("<I", hdr, 4, 1)
    struct.pack_into("<H", hdr, 8, 32 + 32 * len(fields) + 1)
    struct.pack_into("<H", hdr, 10, 1 + sum(f[2] for f in fields))
    hdr[29] = 87
    dbf = bytes(hdr)
    for name, t, w, dec in fields:
        fd = bytearray(32)
        fd[0:len(name)] = name
        fd[11:12] = t
        fd[16] = w
        fd[17] = dec
        dbf += bytes(fd)
    dbf += b"\x0d"
    dbf += b" " + b"caf\xe9".ljust(80) + b"        1"
    dbf += b"\x1a"
    open(os.path.join(d, stem + ".dbf"), "wb").write(dbf)
    prj = ('GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",'
           '6378137.0,298.257223563]],PRIMEM["Greenwich",0.0],'
           'UNIT["Degree",0.0174532925199433]]')
    open(os.path.join(d, stem + ".prj"), "w").write(prj)


def variant(stem, ldid=None, cpg=None, upper=False):
    for ext in (".shp", ".shx", ".dbf", ".prj"):
        shutil.copy(os.path.join(d, "t_l87" + ext), os.path.join(d, stem + ext))
    if ldid is not None:
        p = os.path.join(d, stem + ".dbf")
        b = bytearray(open(p, "rb").read())
        b[29] = ldid
        open(p, "wb").write(bytes(b))
    if cpg is not None:
        open(os.path.join(d, stem + (".CPG" if upper else ".cpg")), "wb").write(cpg)


mk_base("t_l87")
variant("t_l0", ldid=0)
variant("t_l38", ldid=38)
variant("t_l4", ldid=4)
variant("t_l45", ldid=45)
variant("t_cpgutf8", cpg=b"UTF-8")
variant("t_cpg1251", cpg=b"CP1251")
variant("t_cpggarb", cpg=b"FOOBAR")
variant("t_cpg88591", ldid=0, cpg=b"ISO-8859-1")
variant("t_cpg1252", ldid=0, cpg=b"1252")
variant("t_cpgnum", ldid=0, cpg=b"8859-1\nrest ignored")
variant("t_cpgempty", cpg=b"")
variant("t_cpgldid", cpg=b"LDID/38")
variant("t_cpgup", ldid=0, cpg=b"CP866", upper=True)

with open(os.path.join(d, "fv.json"), "wb") as f:
    f.write(b'{"type":"FeatureCollection","features":[{"type":"Feature",'
            b'"properties":{"name":"caf\xc3\xa9 \xd0\xb9","b":1},'
            b'"geometry":{"type":"Point","coordinates":[1,2]}}]}')
with open(os.path.join(d, "fc.json"), "wb") as f:
    f.write(b'{"type":"FeatureCollection","features":[{"type":"Feature",'
            b'"properties":{"\xd0\xb8\xd0\xbc\xd1\x8f":"caf\xc3\xa9","b":1},'
            b'"geometry":{"type":"Point","coordinates":[1,2]}}]}')
with open(os.path.join(d, "ap.json"), "wb") as f:
    f.write(b'{"type":"FeatureCollection","features":[{"type":"Feature",'
            b'"properties":{"nam\xc3\xa9":"\xd0\xb9x","b":2},'
            b'"geometry":{"type":"Point","coordinates":[1,2]}}]}')
with open(os.path.join(d, "gonly.json"), "wb") as f:
    f.write(b'{"type":"FeatureCollection","features":[{"type":"Feature",'
            b'"properties":{},"geometry":{"type":"Point",'
            b'"coordinates":[1,2]}}]}')
