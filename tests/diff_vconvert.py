#!/usr/bin/env python3
import os, shutil, subprocess, sys, json

ORACLE = "/home/agent/oracle/executable"
MINE = "/workspace/executable"
ROOT = "/tmp/dvc"

VJ = "/tmp/vj"
VS = "/tmp/vs"
VC = "/tmp/vc"


def gather_inputs():
    ins = []
    if os.path.isdir(VJ):
        for f in sorted(os.listdir(VJ)):
            if f.endswith(".json"):
                ins.append(os.path.join(VJ, f))
    if os.path.isdir(VS):
        seen = set()
        for f in sorted(os.listdir(VS)):
            b, e = os.path.splitext(f)
            if e in (".shp", ".dbf") and b not in seen:
                seen.add(b)
                ins.append(os.path.join(VS, b + (".shp" if e == ".shp" or os.path.exists(os.path.join(VS, b + ".shp")) else ".dbf")))
    for f in ["in.json", "t2.json", "pgmix.json", "mpmix.json", "lspg.json",
              "mppt.json", "pgls.json", "ptmp.json", "nullg.json", "gc1.json",
              "dts2.json", "uni.json", "emp.json", "coords.json",
              "coords2.json", "one.json", "props.json", "p2.json"]:
        p = os.path.join(VC, f)
        if os.path.exists(p):
            ins.append(p)
    return ins


def runpair(args, cwd_extra=None):
    res = []
    for i, exe in enumerate([ORACLE, MINE]):
        d = os.path.join(ROOT, "ab"[i])
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        if cwd_extra:
            cwd_extra(d)
        p = subprocess.run([exe] + args, cwd=d, capture_output=True)
        files = {}
        for dirpath, _, fns in os.walk(d):
            for fn in fns:
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, d)
                with open(full, "rb") as fh:
                    files[rel] = fh.read()
        res.append((p.returncode, p.stdout, p.stderr, files))
    return res


def main():
    total = fail = 0
    fails = []
    only = sys.argv[1] if len(sys.argv) > 1 else None
    cases = []
    for inp in gather_inputs():
        for out, opts in [
            ("out.json", []),
            ("out.shp", []),
            ("out.json", ["-q"]),
            ("out.shp", ["-q"]),
        ]:
            cases.append([ "vector", "convert" ] + opts + [inp, out])
    # option and error surfaces
    E = [
        ["vector", "convert", os.path.join(VC, "in.json"), "bad.xyz"],
        ["vector", "convert", "--of", "GTiff", os.path.join(VC, "in.json"), "g.tif"],
        ["vector", "convert", "--of", "COG", os.path.join(VC, "in.json"), "g.tif"],
        ["vector", "convert", "--of", "VRT", os.path.join(VC, "in.json"), "g.vrt"],
        ["vector", "convert", "--of", "nope", os.path.join(VC, "in.json"), "g.x"],
        ["vector", "convert", "--of", "MEM", os.path.join(VC, "in.json"), "m"],
        ["vector", "convert", "--of", "GDALG", os.path.join(VC, "in.json"), "g.gdalg.json"],
        ["vector", "convert", os.path.join(VC, "in.json"), "o.gdalg.json"],
        ["vector", "convert", "--of", "GeoJSON", os.path.join(VC, "in.json"), "noext"],
        ["vector", "convert", "--of", "ESRI Shapefile", os.path.join(VC, "in.json"), "noext"],
        ["vector", "convert", VS, "multi.json"],
        ["vector", "convert", VS, "multi.shp"],
        ["vector", "convert", "-l", "simple", VS, "ml.json"],
        ["vector", "convert", "-l", "simple", "-l", "types", VS, "two.json"],
        ["vector", "convert", "-l", "simple", "-l", "types", VS, "two.shp"],
        ["vector", "convert", "-l", "nosuch", VS, "ml2.json"],
        ["vector", "convert", "-l", "nosuch", os.path.join(VC, "in.json"), "ml3.json"],
        ["vector", "convert", os.path.join(VC, "in.json"), "sub/out.json"],
        ["vector", "convert", os.path.join(VC, "in.json"), "sub/out.shp"],
        ["vector", "convert", "/tmp/g.txt", "o.json"],
        ["vector", "convert", "/nonexistent.json", "o.json"],
        ["vector", "convert", os.path.join(VC, "in.json")],
        ["vector", "convert", os.path.join(VC, "in.json"), "a.json", "b.json"],
        ["vector", "convert"],
        ["vector", "convert", "--output-layer", "renamed", os.path.join(VC, "in.json"), "o.json"],
        ["vector", "convert", "--output-layer", "renamed", os.path.join(VC, "in.json"), "o.shp"],
        ["vector", "convert", "--overwrite", os.path.join(VC, "in.json"), "exist.json"],
        ["vector", "convert", os.path.join(VC, "in.json"), "exist.json"],
        ["vector", "convert", os.path.join(VC, "in.json"), "exist.shp"],
        ["vector", "convert", "--append", os.path.join(VC, "in.json"), "exist.json"],
        ["vector", "convert", "--update", os.path.join(VC, "in.json"), "exist.json"],
        ["vector", "convert", "--overwrite-layer", os.path.join(VC, "in.json"), "exist.json"],
        ["vector", "convert", "--upsert", os.path.join(VC, "in.json"), "exist.json"],
        ["vector", "convert", "--append", os.path.join(VC, "in.json"), "valid.json"],
        ["vector", "convert", "--update", os.path.join(VC, "in.json"), "valid.json"],
        ["vector", "convert", "--overwrite-layer", os.path.join(VC, "in.json"), "valid.json"],
        ["vector", "convert", "--upsert", os.path.join(VC, "in.json"), "valid.json"],
        ["vector", "convert", "--append", "--output-layer", "renamed", os.path.join(VC, "in.json"), "valid.json"],
        ["vector", "convert", "--append", os.path.join(VC, "in.json"), "missing.json"],
        ["vector", "convert", "--update", os.path.join(VC, "in.json"), "missing.json"],
        ["vector", "convert", "--overwrite-layer", os.path.join(VC, "in.json"), "missing.json"],
        ["vector", "convert", "--upsert", os.path.join(VC, "in.json"), "missing.json"],
        ["vector", "convert", "--upsert", os.path.join(VC, "in.json"), "missing.shp"],
        ["vector", "convert", "--append", os.path.join(VC, "in.json"), "vin2.shp"],
        ["vector", "convert", "--update", os.path.join(VC, "in.json"), "vin2.shp"],
        ["vector", "convert", "--overwrite-layer", os.path.join(VC, "in.json"), "vin2.shp"],
        ["vector", "convert", "--upsert", os.path.join(VC, "in.json"), "vin2.shp"],
        ["vector", "convert", "--append", os.path.join(VC, "t2.json"), "vin2.shp"],
        ["vector", "convert", "--append", "-q", os.path.join(VC, "in.json"), "vin2.shp"],
        ["vector", "convert", "-l", "simple", "-l", "types", VS, "nodir/two.shp"],
        ["vector", "convert", "-l", "types", "--of", "ESRI Shapefile", VS, "t1d"],
        ["vector", "convert", "-l", "types", VS, "t1s.shp"],
        ["vector", "convert", "-l", "numfmt", VS, "nf.shp"],
        ["vector", "convert", "--of", "ESRI Shapefile", os.path.join(VC, "props.json"), "jdir"],
    ]

    def mk_exist(d):
        with open(os.path.join(d, "exist.json"), "w") as f:
            f.write("{}")
        with open(os.path.join(d, "exist.shp"), "w") as f:
            f.write("x")
        shutil.copy(os.path.join(VC, "out.json"), os.path.join(d, "valid.json"))
        for e in ("shp", "shx", "dbf", "prj"):
            shutil.copy(os.path.join(VC, "in2." + e), os.path.join(d, "vin2." + e))

    for args in cases + E:
        key = " ".join(args)
        if only and only not in key:
            continue
        total += 1
        (rc1, so1, se1, f1), (rc2, so2, se2, f2) = runpair(args, mk_exist)
        ok = rc1 == rc2 and so1 == so2 and se1 == se2 and f1 == f2
        if not ok:
            fail += 1
            fails.append(key)
            if len(fails) <= 12:
                print("FAIL:", key)
                if rc1 != rc2:
                    print("  rc", rc1, rc2)
                if so1 != so2:
                    print("  stdout", repr(so1[-120:]), repr(so2[-120:]))
                if se1 != se2:
                    print("  stderr\n   O:", repr(se1[:400]), "\n   M:", repr(se2[:400]))
                if f1 != f2:
                    k1, k2 = set(f1), set(f2)
                    if k1 != k2:
                        print("  files only-oracle", k1 - k2, "only-mine", k2 - k1)
                    for k in sorted(k1 & k2):
                        if f1[k] != f2[k]:
                            print("  file差", k, len(f1[k]), len(f2[k]))
    print(f"{total-fail}/{total} passed")
    if fails:
        print("failures:", len(fails))
        for f in fails[:40]:
            print(" -", f)
        sys.exit(1)


if __name__ == "__main__":
    main()
