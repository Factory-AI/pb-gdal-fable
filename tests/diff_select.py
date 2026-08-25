#!/usr/bin/env python3
import os, shutil, subprocess, sys

ORACLE = "/home/agent/oracle/executable"
MINE = "/workspace/executable"
ROOT = "/tmp/dsel"
FIX = os.path.join(ROOT, "fix")


def make_fixtures():
    shutil.rmtree(ROOT, ignore_errors=True)
    os.makedirs(FIX)

    def orc(args):
        subprocess.run([ORACLE] + args, cwd=FIX, capture_output=True)

    orc(["raster", "create", "-q", "--size", "3,2", "--band-count", "3",
         "--burn", "10,20,30", "--crs", "EPSG:4326", "--bbox", "2,49,3,50",
         "--co", "PHOTOMETRIC=RGB", "rgb3.tif"])
    orc(["raster", "create", "-q", "--size", "3,2", "--band-count", "2",
         "--burn", "7,200", "--co", "ALPHA=YES", "al.tif"])
    orc(["raster", "create", "-q", "--size", "3,2", "--burn", "7",
         "--nodata", "7", "nd.tif"])
    orc(["raster", "create", "-q", "--size", "3,2", "--band-count", "3",
         "--burn", "1,2,3", "g3.tif"])


def runpair(args):
    res = []
    for i, exe in enumerate([ORACLE, MINE]):
        d = os.path.join(ROOT, "ab"[i])
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        for f in os.listdir(FIX):
            shutil.copy(os.path.join(FIX, f), d)
        p = subprocess.run([exe] + args, cwd=d, capture_output=True,
                           timeout=30)
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
    for b in ["1", "2,1", "1,1,3", "3,2,1", "2,1,3", "1,2,3", "2,2,2",
              "1,2,2", "2,3", "red", "RED", "green,mask:1,1", "mask",
              "mask,mask", "1,mask", "mask,1", "1,2,3,3", "1,2,3,mask",
              "9", "0", "foo"]:
        cases.append(["raster", "select", "--band", b, "rgb3.tif",
                      "o.tif", "-q"])
    cases += [
        ["raster", "select", "--band", "1", "rgb3.tif", "o.tif"],
        ["raster", "select", "--band", "2", "--band", "1", "rgb3.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "mask", "al.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "mask:1", "al.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "mask:2", "al.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "mask", "nd.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "mask:9", "al.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "2,1", "g3.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "1,1", "g3.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "2", "--exclude", "rgb3.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "mask", "--exclude", "rgb3.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "1,2,3", "--exclude", "rgb3.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "red", "nd.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "1", "missing.tif", "o.tif", "-q"],
        ["raster", "select", "--band", "2", "rgb3.tif", "o.gdalg.json", "-q"],
        ["raster", "select", "--band", "9", "rgb3.tif", "o.gdalg.json", "-q"],
        ["raster", "select", "--band", "red", "nd.tif", "o.gdalg.json", "-q"],
        ["raster", "select", "--band", "2", "--exclude", "--co", "TILED=YES", "rgb3.tif", "o.gdalg.json", "-q"],
        ["raster", "select", "--band", "2", "--of", "VRT", "rgb3.tif", "o.vrt", "-q"],
        ["raster", "select", "--band", "9", "--of", "VRT", "rgb3.tif", "o.vrt", "-q"],
        ["raster", "select", "--band", "2", "--of", "MEM", "rgb3.tif", "m1"],
        ["raster", "select", "--band", "2", "--of", "BOGUS", "rgb3.tif", "o.x", "-q"],
        ["raster", "select", "--band", "1", "--co", "COMPRESS=DEFLATE", "rgb3.tif", "o.tif", "-q"],
    ]
    total = fail = 0
    for args in cases:
        label = " ".join(args)
        if only and only not in label:
            continue
        total += 1
        (rc1, so1, se1, f1), (rc2, so2, se2, f2) = runpair(args)
        if not (rc1 == rc2 and so1 == so2 and se1 == se2 and f1 == f2):
            fail += 1
            print("DIFF:", label)
            if rc1 != rc2:
                print("  rc %s vs %s" % (rc1, rc2))
            if se1 != se2:
                print("  stderr %r vs %r" % (se1[:200], se2[:200]))
            if f1 != f2:
                for k in sorted(set(f1) | set(f2)):
                    if f1.get(k) != f2.get(k):
                        print("  file", k)
    print("%d/%d ok" % (total - fail, total))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
