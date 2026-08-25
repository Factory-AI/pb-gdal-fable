import os
import struct
import sys

if len(sys.argv) > 1:
    os.chdir(sys.argv[1])

def mk(path, pages):
    # pages: list of dicts {w,h,bands,subtype,nosbc} ; subtype None = omit
    # tag, nosbc drops StripByteCounts
    out = bytearray()
    out += b"II*\x00" + b"\x00\x00\x00\x00"
    ifd_off_pos = 4
    for i, p in enumerate(pages):
        w, h, bands = p["w"], p["h"], p["bands"]
        subtype = p.get("subtype")
        data_off = len(out)
        out += bytes(w * h * bands)
        tags = []
        def tag(tid, ttype, count, val):
            tags.append((tid, ttype, count, val))
        tag(256, 3, 1, w)
        tag(257, 3, 1, h)
        tag(258, 3, 1, 8)
        tag(259, 3, 1, 1)
        tag(262, 3, 1, 1)
        tag(273, 4, 1, data_off)
        tag(277, 3, 1, bands)
        tag(278, 3, 1, h)
        if not p.get("nosbc"):
            tag(279, 4, 1, w * h * bands)
        tag(284, 3, 1, 1)
        if subtype is not None:
            tags.insert(0, (254, 4, 1, subtype))
        tags.sort(key=lambda t: t[0])
        if len(out) % 2:
            out += b"\x00"
        ifd_off = len(out)
        struct.pack_into("<I", out, ifd_off_pos, ifd_off)
        out += struct.pack("<H", len(tags))
        for tid, ttype, count, val in tags:
            out += struct.pack("<HHI", tid, ttype, count)
            if ttype == 3:
                out += struct.pack("<HH", val, 0)
            else:
                out += struct.pack("<I", val)
        ifd_off_pos = len(out)
        out += b"\x00\x00\x00\x00"
    open(path, "wb").write(out)

mk("pure_ovr.tif", [dict(w=10,h=10,bands=1),
                    dict(w=5,h=5,bands=1,subtype=1),
                    dict(w=3,h=3,bands=1,subtype=1)])
mk("multipage.tif", [dict(w=10,h=10,bands=1),
                     dict(w=6,h=6,bands=1,subtype=2)])
mk("mixed.tif", [dict(w=10,h=10,bands=1),
                 dict(w=5,h=5,bands=1,subtype=1),
                 dict(w=6,h=6,bands=1,subtype=2)])
mk("mixed2.tif", [dict(w=10,h=10,bands=1),
                  dict(w=6,h=6,bands=1,subtype=2),
                  dict(w=5,h=5,bands=1,subtype=1)])
mk("bandmismatch.tif", [dict(w=10,h=10,bands=1),
                        dict(w=5,h=5,bands=3,subtype=1)])
mk("bigger_ovr.tif", [dict(w=10,h=10,bands=1),
                      dict(w=12,h=12,bands=1,subtype=1)])
mk("subtype0.tif", [dict(w=10,h=10,bands=1),
                    dict(w=6,h=6,bands=1,subtype=0)])
mk("first_reduced.tif", [dict(w=10,h=10,bands=1,subtype=1),
                         dict(w=5,h=5,bands=1,subtype=1)])
mk("mask_ovr.tif", [dict(w=10,h=10,bands=1),
                    dict(w=5,h=5,bands=1,subtype=5)])
mk("warn_full.tif", [dict(w=10,h=10,bands=1),
                     dict(w=5,h=5,bands=3,subtype=2)])
mk("warn_main.tif", [dict(w=10,h=10,bands=3)])
mk("warn_ovr_ok.tif", [dict(w=10,h=10,bands=3),
                       dict(w=5,h=5,bands=3,subtype=1)])
mk("warn_mask.tif", [dict(w=10,h=10,bands=1),
                     dict(w=5,h=5,bands=3,subtype=5)])
mk("warn_sub0.tif", [dict(w=10,h=10,bands=1),
                     dict(w=5,h=5,bands=3,subtype=0)])
mk("warn_2full.tif", [dict(w=10,h=10,bands=1),
                      dict(w=5,h=5,bands=3,subtype=2),
                      dict(w=6,h=6,bands=3,subtype=2)])
mk("sbc.tif", [dict(w=4,h=4,bands=1,nosbc=True)])
# distinct-text pairs (main: photometric mismatch; extra pages: missing
# StripByteCounts) decode the true libtiff directory traffic order
mk("wd.tif", [dict(w=10,h=10,bands=2),
              dict(w=5,h=5,bands=2,subtype=1,nosbc=True)])
mk("wd2.tif", [dict(w=10,h=10,bands=2),
               dict(w=5,h=5,bands=2,subtype=1,nosbc=True),
               dict(w=3,h=3,bands=2,subtype=1,nosbc=True)])
mk("wr.tif", [dict(w=10,h=10,bands=2),
              dict(w=5,h=5,bands=3,subtype=1,nosbc=True)])
mk("wm.tif", [dict(w=10,h=10,bands=2),
              dict(w=5,h=5,bands=2,subtype=5,nosbc=True)])
mk("wf.tif", [dict(w=10,h=10,bands=2),
              dict(w=5,h=5,bands=1,subtype=2,nosbc=True)])
mk("ws.tif", [dict(w=10,h=10,bands=2),
              dict(w=5,h=5,bands=1,subtype=0,nosbc=True)])
