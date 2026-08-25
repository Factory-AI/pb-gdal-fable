#!/usr/bin/env python3
"""Hand-rolled 2x2 GeoTIFFs exercising datum/ensemble geokey reconstruction:
known-code geographic/geocentric routes, user-defined citation variants,
custom projections over coded bases, unit lookups, ellipsoid-identify
flattening quirks, and geokey bounds validation."""
import os
import struct
import sys

if len(sys.argv) > 1:
    os.chdir(sys.argv[1])


def mk(path, keys, dbls=(), ascii_s=b"", rev=0):
    keys = sorted(keys)
    w = h = 2
    out = bytearray(b"II*\x00\x00\x00\x00\x00")
    data_off = len(out)
    out += bytes(w * h)
    gk = [1, 1, rev, len(keys)]
    for kid, loc, cnt, val in keys:
        gk += [kid, loc, cnt, val]
    tags = [
        (256, 3, 1, w), (257, 3, 1, h), (258, 3, 1, 8), (259, 3, 1, 1),
        (262, 3, 1, 1), (273, 4, 1, data_off), (277, 3, 1, 1),
        (278, 3, 1, h), (279, 4, 1, w * h), (284, 3, 1, 1),
    ]
    gkbytes = struct.pack("<%dH" % len(gk), *gk)
    dblbytes = struct.pack("<%dd" % len(dbls), *dbls)
    tags.append((34735, 3, len(gk), ("ext", gkbytes)))
    if dbls:
        tags.append((34736, 12, len(dbls), ("ext", dblbytes)))
    if ascii_s:
        tags.append((34737, 2, len(ascii_s), ("ext", ascii_s)))
    tags.sort(key=lambda t: t[0])
    if len(out) % 2:
        out += b"\x00"
    ifd_off = len(out)
    struct.pack_into("<I", out, 4, ifd_off)
    out += struct.pack("<H", len(tags))
    ext_start = ifd_off + 2 + 12 * len(tags) + 4
    extdata = bytearray()
    for tid, ttype, count, val in tags:
        out += struct.pack("<HHI", tid, ttype, count)
        if isinstance(val, tuple):
            payload = val[1]
            if len(payload) <= 4:
                out += payload + bytes(4 - len(payload))
            else:
                if len(extdata) % 2:
                    extdata += b"\x00"
                out += struct.pack("<I", ext_start + len(extdata))
                extdata += payload
        elif ttype == 3:
            out += struct.pack("<HH", val, 0)
        else:
            out += struct.pack("<I", val)
    out += b"\x00\x00\x00\x00"
    out += extdata
    open(path, "wb").write(out)


AREA = [(1024, 0, 1, 2), (1025, 0, 1, 1)]
GRS = (298.257222101, 6378137.0)
NWL = (298.25, 6378145.0)


def geog_user(datum, dbls=GRS, cit=b"", extra=()):
    keys = list(AREA)
    if cit:
        keys.append((2049, 34737, len(cit), 0))
    keys += [(2048, 0, 1, 32767), (2050, 0, 1, datum), (2054, 0, 1, 9102)]
    if dbls:
        keys += [(2057, 34736, 1, 1), (2059, 34736, 1, 0)]
    keys += list(extra)
    return keys, dbls, cit


# known-code geographic and geocentric routes (ensemble, plain, dynamic)
for code, mt in [(4258, 2), (4267, 2), (9000, 2), (4936, 3), (7789, 3)]:
    mk("d1_code%d.tif" % code,
       [(1024, 0, 1, mt), (1025, 0, 1, 1), (2048, 0, 1, code)])

# user-defined geographic: citation variants over a static-frame datum
for name, cit in [
    ("d1_cit_datum", b"Datum = MyDatum|\x00"),
    ("d1_cit_ell", b"junk|Ellipsoid = MyEll|\x00"),
    ("d1_cit_gcs", b"GCS Name = MyGCS|\x00"),
    ("d1_cit_raw", b"a|b|c|\x00"),
    ("d1_cit_empty", b"\x00"),
]:
    k, d, c = geog_user(6258, cit=cit)
    mk(name + ".tif", k, d, c)

# datum resolution classes: hardcoded, static db, dynamic, unknown, user
for name, datum, dbls in [
    ("d1_dat6267", 6267, GRS),
    ("d1_dat6258", 6258, GRS),
    ("d1_dat1165", 1165, GRS),
    ("d1_dat6760", 6760, NWL),
    ("d1_dat32767", 32767, NWL),
    ("d1_datbad", 9999, GRS),
    ("d1_dat6326_arow", 6326, (298.25, 6378137.0)),
    ("d1_dat6326_nwl", 6326, NWL),
    ("d1_datwgs", 6326, (298.257223563, 6378137.0)),
]:
    k, d, c = geog_user(datum, dbls=dbls)
    mk(name + ".tif", k, d, c)

# semi-minor key variant of the unique-ellipsoid match
mk("d1_dat6760b.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 32767),
    (2050, 0, 1, 6760), (2054, 0, 1, 9102), (2057, 34736, 1, 1),
    (2058, 34736, 1, 0)], (6356759.769488684, 6378145.0))

# user-defined geocentric: GeogLinearUnits handling and name fallbacks
geoc = [(1024, 0, 1, 3), (1025, 0, 1, 1), (2048, 0, 1, 32767),
        (2050, 0, 1, 6258), (2054, 0, 1, 9102), (2057, 34736, 1, 1),
        (2059, 34736, 1, 0)]
mk("d1_geoc_9036.tif", geoc + [(2052, 0, 1, 9036)], GRS)
mk("d1_geoc_nolin.tif", geoc, GRS)
mk("d1_geoc_badlin.tif", geoc + [(2052, 0, 1, 9999)], GRS)
mk("d1_geoc_name.tif", geoc + [(1026, 34737, 8, 0)], GRS, b"MyGeoc|\x00")
mk("d1_geoc_name2.tif", geoc + [(2049, 34737, 5, 0)], GRS, b"Alt|\x00")

# custom projections (mt=1, user PCS, UTM zone 31N) over coded bases
proj = [(1024, 0, 1, 1), (1025, 0, 1, 1), (3072, 0, 1, 32767),
        (3074, 0, 1, 16031)]
mk("d1_proj4326.tif", proj + [(2048, 0, 1, 4326), (3076, 0, 1, 9001)])
mk("d1_proj4807.tif", proj + [(2048, 0, 1, 4807), (3076, 0, 1, 9001)])
mk("d1_proj9000.tif", proj + [(2048, 0, 1, 9000), (3076, 0, 1, 9001)])
mk("d1_proj29999.tif", proj + [(2048, 0, 1, 29999), (3076, 0, 1, 9001)])
mk("d1_proj_km.tif", proj + [(2048, 0, 1, 4326), (3076, 0, 1, 9036)])
mk("d1_proj_badunit.tif", proj + [(2048, 0, 1, 4326), (3076, 0, 1, 9999)])
mk("d1_proj_keys.tif",
   proj + [(2048, 0, 1, 9000), (2054, 0, 1, 9102), (2057, 34736, 1, 1),
           (2059, 34736, 1, 0), (3076, 0, 1, 9001)],
   (298.257223563, 6378137.0))
mk("d1_projuser_ft.tif",
   proj + [(2048, 0, 1, 32767), (2050, 0, 1, 6326), (2054, 0, 1, 9102),
           (3076, 0, 1, 9002)])
mk("d1_proj_lunits.tif",
   proj + [(2048, 0, 1, 32767), (2050, 0, 1, 6326), (2054, 0, 1, 9102),
           (3073, 34737, 15, 0), (3076, 0, 1, 32767),
           (3077, 34736, 1, 0)],
   (0.3,), b"LUnits = myft|\x00")

# model/code edge routing: absent 2048 composes, geocentric codes under
# mt=2 rebuild textually, mt=3 codes contribute only their datum
mk("d1_geoc_no2048.tif",
   [(1024, 0, 1, 3), (1025, 0, 1, 1), (2050, 0, 1, 6267)])
mk("d1_geog_no2048.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2050, 0, 1, 6267)])
mk("d1_geog4936.tif", [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 4936)])
mk("d1_geoc29999.tif", [(1024, 0, 1, 3), (1025, 0, 1, 1), (2048, 0, 1, 29999)])
mk("d1_geoc4326.tif", [(1024, 0, 1, 3), (1025, 0, 1, 1), (2048, 0, 1, 4326)])
mk("d1_geoc4936d.tif",
   [(1024, 0, 1, 3), (1025, 0, 1, 1), (2048, 0, 1, 4936), (2050, 0, 1, 6267)])

# ESRI PE strings only load under a user-defined model type; other model
# types keep the citation as a plain name
PE = (b'ESRI PE String = PROJCS["Custom",GEOGCS["GCS_WGS_1984",'
      b'DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137.0,298.257223563]],'
      b'PRIMEM["Greenwich",0.0],UNIT["Degree",0.0174532925199433]],'
      b'PROJECTION["Mercator"],PARAMETER["False_Easting",0.0],'
      b'PARAMETER["False_Northing",0.0],PARAMETER["Central_Meridian",0.0],'
      b'PARAMETER["Standard_Parallel_1",0.0],UNIT["Meter",1.0]]\x00')
mk("d1_pe_mt327.tif",
   [(1024, 0, 1, 32767), (1025, 0, 1, 1), (3073, 34737, len(PE), 0),
    (3076, 0, 1, 32767)], (), PE)
mk("d1_pe_proj.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (3072, 0, 1, 32767),
    (3073, 34737, len(PE), 0), (3075, 0, 1, 1), (3076, 0, 1, 9001)], (), PE)

# method-less projected directories become citation-named local CRSs
CIT = b"Hello World|\x00"
mk("d1_eng_cit.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (3073, 34737, len(CIT), 0),
    (3076, 0, 1, 9001)], (), CIT)
CIT2 = b"LUnits = myft|\x00"
mk("d1_eng_lunits.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (3073, 34737, len(CIT2), 0),
    (3076, 0, 1, 9001)], (), CIT2)

# PCS citation naming, LUnits-vs-coded-unit conflict, axis name and order
CIT3 = b"PCS Name = MyPCS|\x00"
mk("d1_prj_pcsname.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (3072, 0, 1, 32767),
    (3073, 34737, len(CIT3), 0), (3075, 0, 1, 1), (3076, 0, 1, 9001)],
   (), CIT3)
CIT4 = b"a|b|c|\x00"
mk("d1_prj_seg.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (3072, 0, 1, 32767),
    (3073, 34737, len(CIT4), 0), (3075, 0, 1, 1), (3076, 0, 1, 9001)],
   (), CIT4)
mk("d1_prj_lunits9002.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (3072, 0, 1, 32767),
    (3073, 34737, len(CIT2), 0), (3075, 0, 1, 1), (3076, 0, 1, 9002)],
   (), CIT2)
mk("d1_proj_noang.tif",
   [(1024, 0, 1, 1), (1025, 0, 1, 1), (2048, 0, 1, 32767),
    (2050, 0, 1, 6326), (3072, 0, 1, 32767), (3074, 0, 1, 16031),
    (3076, 0, 1, 9001)])

# geokey bounds validation: corrupt directories drop the whole key set
mk("d1_bad_ascii.tif",
   [(1024, 0, 1, 3), (1025, 0, 1, 1), (1026, 34737, 8, 0),
    (2048, 0, 1, 32767), (2049, 34737, 21, 8), (2050, 0, 1, 6258),
    (2054, 0, 1, 9102), (2057, 34736, 1, 1), (2059, 34736, 1, 0)],
   GRS, b"FromGT|\x00GCS Name = FromGeog|\x00")
mk("d1_bad_dbl.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 32767),
    (2050, 0, 1, 6258), (2054, 0, 1, 9102), (2057, 34736, 2, 1)],
   (1.0, 6378137.0))
mk("d1_bad_dir.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 32767),
    (2050, 0, 1, 6267), (2054, 0, 1, 9102), (2062, 34735, 3, 100)])
mk("d1_ok_nulcount.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 32767),
    (2049, 34737, 5, 0), (2050, 0, 1, 6267), (2054, 0, 1, 9102)],
   (), b"abc|\x00")
mk("d1_ok_overrun.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 32767),
    (2049, 34737, 9, 0), (2050, 0, 1, 6267), (2054, 0, 1, 9102)],
   (), b"abc|\x00xyz|\x00")

# coded-PCS flavor rebuilds: GTCitation-gated ensemble demote, unit and
# angular overrides, short override keys forcing component recomposition,
# registry-comparison warnings
P = [(1024, 0, 1, 1), (1025, 0, 1, 1)]
CIT_X = b"x|\x00"
mk("d1x_utm.tif", P + [(3072, 0, 1, 32611)])
mk("d1x_utmcit.tif",
   P + [(3072, 0, 1, 32611), (1026, 34737, len(CIT_X), 0)], (), CIT_X)
mk("d1x_etrscit.tif",
   P + [(3072, 0, 1, 25831), (1026, 34737, len(CIT_X), 0)], (), CIT_X)
mk("d1x_units9002.tif", P + [(3072, 0, 1, 32611), (3076, 0, 1, 9002)])
mk("d1x_ang9104.tif", P + [(3072, 0, 1, 32611), (2054, 0, 1, 9104)])
mk("d1x_gcs4326.tif", P + [(3072, 0, 1, 32611), (2048, 0, 1, 4326)])
mk("d1x_dat6326.tif", P + [(3072, 0, 1, 32611), (2050, 0, 1, 6326)])
mk("d1x_gcs4258.tif", P + [(3072, 0, 1, 32611), (2048, 0, 1, 4258)])
mk("d1x_gcsproj.tif", P + [(3072, 0, 1, 32611), (2048, 0, 1, 32611)])
mk("d1x_userdat.tif", P + [(3072, 0, 1, 32611), (2050, 0, 1, 32767)])
mk("d1x_gcsuser.tif", P + [(3072, 0, 1, 32611), (2048, 0, 1, 32767)])
mk("d1x_utmzone.tif", P + [(3072, 0, 1, 32611), (3074, 0, 1, 32767)])
mk("d1x_parm.tif", P + [(3072, 0, 1, 32611), (3075, 0, 1, 1)])
mk("d1x_lcc2154.tif", P + [(3072, 0, 1, 2154), (3074, 0, 1, 18085)])
mk("d1x_laea3035.tif", P + [(3072, 0, 1, 3035), (2048, 0, 1, 4258)])
mk("d1x_ups32661.tif", P + [(3072, 0, 1, 32661), (2048, 0, 1, 4326)])
mk("d1x_merc3857.tif", P + [(3072, 0, 1, 3857), (3074, 0, 1, 3856)])
mk("d1x_utm26911.tif", P + [(3072, 0, 1, 26911), (2048, 0, 1, 4269)])

# GeoTIFF 1.0/1.1 vertical directories: revision-gated compound builds,
# citation recognition, coded vertical datum, geographic-3D promotion
CIT_M = b"WGS 84 / UTM zone 11N + EGM96 height|\x00"
CIT_N = b"X+Y|\x00"
CIT_H = b"hello|\x00"
CIT_U = b"WGS 84 / UTM zone 11N|\x00"
mk("d1x_v5773.tif", P + [(3072, 0, 1, 32611), (4096, 0, 1, 5773)])
mk("d1x_v5773cit.tif",
   P + [(3072, 0, 1, 32611), (4096, 0, 1, 5773),
        (1026, 34737, len(CIT_M), 0)], (), CIT_M)
mk("d1x_v5773citx.tif",
   P + [(3072, 0, 1, 32611), (4096, 0, 1, 5773),
        (1026, 34737, len(CIT_N), 0)], (), CIT_N)
mk("d1x_v4097.tif",
   P + [(3072, 0, 1, 32611), (4096, 0, 1, 5773),
        (4097, 34737, len(CIT_H), 0)], (), CIT_H)
mk("d1x_vunit9003.tif", P + [(3072, 0, 1, 32611), (4099, 0, 1, 9003)])
mk("d1x_vuser.tif", P + [(3072, 0, 1, 32611), (4096, 0, 1, 32767)])
mk("d1x_v5773r1.tif", P + [(3072, 0, 1, 32611), (4096, 0, 1, 5773)],
   rev=1)
mk("d1x_v5773r1cit.tif",
   P + [(3072, 0, 1, 32611), (4096, 0, 1, 5773),
        (1026, 34737, len(CIT_H), 0)], (), CIT_H, rev=1)
mk("d1x_vunitr1.tif", P + [(3072, 0, 1, 32611), (4099, 0, 1, 9001)],
   rev=1)
mk("d1x_vdat5100r1.tif", P + [(3072, 0, 1, 32611), (4098, 0, 1, 5100)],
   rev=1)
mk("d1x_p4979.tif", P + [(3072, 0, 1, 32611), (4096, 0, 1, 4979)])
mk("d1x_p4979r1.tif", P + [(3072, 0, 1, 32611), (4096, 0, 1, 4979)],
   rev=1)
mk("d1x_p4979cit.tif",
   P + [(3072, 0, 1, 32611), (4096, 0, 1, 4979),
        (1026, 34737, len(CIT_U), 0)], (), CIT_U)
mk("d1x_p4979r1cit.tif",
   P + [(3072, 0, 1, 32611), (4096, 0, 1, 4979),
        (1026, 34737, len(CIT_U), 0)], (), CIT_U, rev=1)
mk("d1x_geog3d.tif",
   [(1024, 0, 1, 2), (1025, 0, 1, 1), (2048, 0, 1, 4326),
    (4096, 0, 1, 4979)])

# mt=1 fallbacks with only a coded GeographicType: projected codes warn,
# geocentric ones spell out the local axis names
mk("d1x_eng_gcsproj.tif", P + [(2048, 0, 1, 32611)])
mk("d1x_eng_geoc.tif", P + [(2048, 0, 1, 4978)])
mk("d1x_eng_geog.tif", P + [(2048, 0, 1, 4267)])
