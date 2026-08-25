#!/usr/bin/env python3
import json, os, random, shutil, subprocess, sys

ORACLE = "/home/agent/oracle/executable"
MINE = "/workspace/executable"
ROOT = "/tmp/dlco"
FIX = os.path.join(ROOT, "fix")


def make_fixtures():
    shutil.rmtree(ROOT, ignore_errors=True)
    os.makedirs(FIX)

    def dump(name, feats, lname=None):
        json.dump(
            {"type": "FeatureCollection", "name": lname or name.split(".")[0],
             "features": feats},
            open(os.path.join(FIX, name), "w"))

    dump("pt.json", [{"type": "Feature", "properties": {"a": 0},
                      "geometry": {"type": "Point",
                                   "coordinates": [3.5, 4.25]}}])
    dump("pr.json", [{"type": "Feature", "properties": {"a": 0},
                      "geometry": {"type": "Point",
                                   "coordinates": [1.12345678901235,
                                                   -2.98765432109876]}},
                     {"type": "Feature", "properties": {"a": 1},
                      "geometry": {"type": "Point",
                                   "coordinates": [101.723456789,
                                                   1.000000049]}}])
    dump("pg.json", [{"type": "Feature", "properties": {"a": 1},
                      "geometry": {"type": "Polygon", "coordinates": [
                          [[0, 0], [1, 0], [1, 1], [0, 1], [0, 0]],
                          [[0.2, 0.2], [0.2, 0.4], [0.4, 0.4], [0.4, 0.2],
                           [0.2, 0.2]]]}}])
    dump("mp.json", [{"type": "Feature", "properties": {"a": 0},
                      "geometry": {"type": "Polygon", "coordinates": [
                          [[0, 0], [4, 0], [4, 4], [0, 4], [0, 0]],
                          [[1, 1], [1, 2], [2, 2], [2, 1], [1, 1]]]}}])
    dump("lines.json", [{"type": "Feature", "properties": {"a": 0},
                         "geometry": {"type": "LineString",
                                      "coordinates": [[0, 0], [1, 1]]}},
                        {"type": "Feature", "properties": {"a": 1},
                         "geometry": {"type": "LineString",
                                      "coordinates": [[2, 2], [3, 3]]}}])
    dump("nullg.json", [{"type": "Feature", "properties": {"a": 0},
                         "geometry": None},
                        {"type": "Feature", "properties": {"a": 1},
                         "geometry": {"type": "Point",
                                      "coordinates": [3, 4]}},
                        {"type": "Feature", "properties": {"a": 2},
                         "geometry": None}])
    dump("empty.json", [])
    for n in (1, 2, 5, 8, 9, 16, 17, 33, 64, 65):
        random.seed(n)
        dump("q%d.json" % n,
             [{"type": "Feature", "properties": {"a": i},
               "geometry": {"type": "Point",
                            "coordinates": [round(random.uniform(-170, 170), 6),
                                            round(random.uniform(-80, 80), 6)]}}
              for i in range(n)])
    random.seed(99)
    feats = []
    for i in range(211):
        x, y = random.uniform(-170, 170), random.uniform(-80, 80)
        if i % 3 == 0:
            g = {"type": "Point", "coordinates": [round(x, 6), round(y, 6)]}
        elif i % 3 == 1:
            g = {"type": "LineString",
                 "coordinates": [[round(x, 6), round(y, 6)],
                                 [round(x + random.uniform(0, 5), 6),
                                  round(y + random.uniform(0, 5), 6)]]}
        else:
            g = {"type": "Polygon",
                 "coordinates": [[[x, y], [x + 1, y], [x + 1, y + 1],
                                  [x, y + 1], [x, y]]]}
        feats.append({"type": "Feature", "properties": {"a": i},
                      "geometry": g})
    dump("big.json", feats)
    random.seed(7)
    dump("bigl.json",
         [{"type": "Feature", "properties": {"a": i},
           "geometry": {"type": "LineString",
                        "coordinates": [[round(random.uniform(-170, 170), 6),
                                         round(random.uniform(-80, 80), 6)],
                                        [round(random.uniform(-170, 170), 6),
                                         round(random.uniform(-80, 80), 6)]]}}
          for i in range(40)])


def runpair(args):
    res = []
    for i, exe in enumerate([ORACLE, MINE]):
        d = os.path.join(ROOT, "ab"[i])
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        for f in os.listdir(FIX):
            shutil.copy(os.path.join(FIX, f), d)
        p = subprocess.run([exe] + args, cwd=d, capture_output=True)
        files = {}
        for dirpath, dirs, fns in os.walk(d):
            for dn in dirs:
                files[os.path.relpath(os.path.join(dirpath, dn), d) + "/"] = b""
            for fn in fns:
                full = os.path.join(dirpath, fn)
                with open(full, "rb") as fh:
                    files[os.path.relpath(full, d)] = fh.read()
        res.append((p.returncode, p.stdout, p.stderr, files))
    return res


def main():
    make_fixtures()
    only = sys.argv[1] if len(sys.argv) > 1 else None
    cases = []

    for src in ["pt.json", "pr.json", "pg.json", "lines.json"]:
        cases.append(["vector", "convert", "--lco", "WRITE_BBOX=YES", src, "o.json"])
        cases.append(["vector", "convert", "--lco", "RFC7946=YES", src, "o.json"])
        cases.append(["vector", "convert", "--lco", "COORDINATE_PRECISION=2", src, "o.json"])
    cases += [
        ["vector", "convert", "--lco", "COORDINATE_PRECISION=5", "pg.json", "o.json"],
        ["vector", "convert", "--lco", "COORDINATE_PRECISION=0", "pr.json", "o.json"],
        ["vector", "convert", "--lco", "COORDINATE_PRECISION=-3", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "COORDINATE_PRECISION=", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "COORDINATE_PRECISION=abc", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=YES", "--lco", "COORDINATE_PRECISION=2", "pg.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=YES", "--lco", "WRITE_BBOX=YES", "pr.json", "o.json"],
        ["vector", "convert", "--lco", "WRITE_BBOX=YES", "--lco", "COORDINATE_PRECISION=3", "pr.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=YES", "--lco", "WRITE_BBOX=YES", "--lco", "COORDINATE_PRECISION=1", "pg.json", "o.json"],
        ["vector", "convert", "--lco", "WRITE_BBOX=NO", "pr.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=FALSE", "pr.json", "o.json"],
        ["vector", "convert", "--lco", "write_bbox=yes", "pr.json", "o.json"],
        ["vector", "convert", "--lco", "WRITE_BBOX=1", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=FOO", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=off", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=TRUE", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "ID_TYPE=Widget", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "WRITE_NON_FINITE_VALUES=YES", "pt.json", "o.json"],
        ["vector", "convert", "--lco", "WRITE_BBOX=YES", "nullg.json", "o.json"],
        ["vector", "convert", "--lco", "RFC7946=YES", "empty.json", "o.json"],
    ]
    for n in (1, 2, 5, 8, 9, 16, 17, 33, 64, 65):
        cases.append(["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES",
                      "q%d.json" % n, "o.shp"])
    cases += [
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "big.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "bigl.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "mp.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "nullg.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "empty.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=NO", "q9.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=FOO", "pt.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=1", "pt.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=no", "big.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "--lco", "SHPT=ARCM", "lines.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=YES", "--lco", "SHPT=NONE", "lines.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SPATIAL_INDEX=FOO", "pt.json", "o.shp"],
    ]
    shpt_all = ["POINT", "ARC", "POLYGON", "MULTIPOINT", "POINTZ", "ARCZ",
                "POLYGONZ", "MULTIPOINTZ", "POINTM", "ARCM", "POLYGONM",
                "MULTIPOINTM", "POINTZM", "ARCZM", "POLYGONZM",
                "MULTIPOINTZM", "MULTIPATCH", "NONE", "NULL", "BOGUS"]
    for v in shpt_all:
        cases.append(["vector", "convert", "-q", "--lco", "SHPT=" + v, "pt.json", "o.shp"])
    for v in ["ARC", "ARCZ", "ARCM", "ARCZM", "POLYGON", "MULTIPATCH", "NONE", "BOGUS"]:
        cases.append(["vector", "convert", "-q", "--lco", "SHPT=" + v, "lines.json", "o.shp"])
    for v in ["POLYGON", "POLYGONZ", "POLYGONM", "POLYGONZM", "MULTIPATCH", "ARC", "NONE"]:
        cases.append(["vector", "convert", "-q", "--lco", "SHPT=" + v, "mp.json", "o.shp"])
    cases += [
        ["vector", "convert", "--lco", "SHPT=BOGUS", "lines.json", "o.shp"],
        ["vector", "convert", "--lco", "shpt=bogus", "lines.json", "o.shp"],
        ["vector", "convert", "--lco", "SHPT=", "lines.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SHPT=MULTIPATCH", "big.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SHPT=MULTIPATCH", "qnull.json" if False else "nullg.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SHPT=POINTM", "nullg.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "SHPT=arc", "lines.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "2GB_LIMIT=MAYBE", "pt.json", "o.shp"],
        ["vector", "convert", "-q", "--lco", "DBF_DATE_LAST_UPDATE=X", "pt.json", "o.shp"],
        ["vector", "convert", "--append", "--lco", "WRITE_BBOX=YES", "pt.json", "pr.json"],
        ["vector", "concat", "--lco", "WRITE_BBOX=YES", "pt.json", "o.json"],
        ["vector", "concat", "--lco", "RFC7946=YES", "pt.json", "pt.json", "o.json"],
        ["vector", "pipeline", "read", "pt.json", "!", "write", "--lco", "WRITE_BBOX=YES", "o.json"],
        ["vector", "pipeline", "read", "q9.json", "!", "write", "-q", "--lco", "SPATIAL_INDEX=YES", "o.shp"],
    ]

    total = fail = 0
    fails = []
    for args in cases:
        label = " ".join(args)
        if only and only not in label:
            continue
        total += 1
        (rc1, so1, se1, f1), (rc2, so2, se2, f2) = runpair(args)
        ok = rc1 == rc2 and so1 == so2 and se1 == se2 and f1 == f2
        if not ok:
            fail += 1
            fails.append(label)
            print("DIFF:", label)
            if rc1 != rc2:
                print("  rc: %s vs %s" % (rc1, rc2))
            if so1 != so2:
                print("  stdout: %r vs %r" % (so1[:200], so2[:200]))
            if se1 != se2:
                print("  stderr: %r vs %r" % (se1[:300], se2[:300]))
            if f1 != f2:
                keys = sorted(set(f1) | set(f2))
                for k in keys:
                    if f1.get(k) != f2.get(k):
                        print("  file %s: %s vs %s bytes" %
                              (k, len(f1[k]) if k in f1 else "MISSING",
                               len(f2[k]) if k in f2 else "MISSING"))
    print("%d/%d ok" % (total - fail, total))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
