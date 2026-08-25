#!/usr/bin/env python3
# generic `gdal convert` dispatch matrix
import subprocess, os, shutil

O = "/home/agent/oracle/executable"
M = "/workspace/executable"
T = "/tmp/ds/a.tif"
J = "/tmp/vc/in.json"

CASES = [
    "$T o.tif", "$J o.json", "$J o.shp", "$T o.json", "$J o.tif",
    "-q $J o.json", "--of GeoJSON $J noext", "--of GTiff $T noext2",
    "gz.json og.json", "/nope.json o9.json",
    "--output-layer ren $J o2.json", "--output-layer ren $T ol.tif",
    "--output-layer x /nope.json o9.json", "--output-layer x gz.json o7.json",
    "$T b.tif c.tif d.tif", "--input $T x.tif y.tif", "--output om.tif $T",
    "--input $J --output oo.json", "$J on.json extra.json",
    "--nosuch V $J o.json", "--nosuch $J o.json", "-Z $J o.json",
    "--if GTiff $T oi.tif", "--if GeoJSON $J oiv.json",
    "--if GeoJSON $T oif.tif", "$T e.tif", "$J e.json",
    "--overwrite $T e.tif", "--overwrite $J e.json",
    "$J g.gdalg.json", "$T r.gdalg.json", "--of MEM $J m1", "--of MEM $T m2",
    "-l in $J o5.json", "-l nosuch $J o6.json", "--update $J e.json",
    "--append $J ap.json", "$J", "$T",
]

def main():
    fails = 0
    tot = 0
    for c in CASES:
        args = c.replace("$T", T).replace("$J", J).split()
        res = []
        for i, exe in enumerate([O, M]):
            d = "/tmp/gcd/" + "ab"[i]
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
            with open(d + "/gz.json", "w") as f:
                f.write("garbage{")
            shutil.copy(T, d + "/e.tif")
            shutil.copy("/tmp/vc/out.json", d + "/e.json")
            p = subprocess.run([exe, "convert"] + args, cwd=d,
                               capture_output=True)
            files = {}
            for dp, _, fns in os.walk(d):
                for fn in fns:
                    full = os.path.join(dp, fn)
                    with open(full, "rb") as fh:
                        files[os.path.relpath(full, d)] = fh.read()
            res.append((p.returncode, p.stdout, p.stderr, files))
        tot += 1
        if res[0] != res[1]:
            fails += 1
            (r1, s1, e1, f1), (r2, s2, e2, f2) = res
            print("FAIL: convert", c)
            if r1 != r2:
                print("  rc", r1, r2)
            if s1 != s2:
                print("  out", repr(s1[:100]), repr(s2[:100]))
            if e1 != e2:
                print("  err\n   O:", repr(e1[:220]), "\n   M:", repr(e2[:220]))
            if f1 != f2:
                k1, k2 = set(f1), set(f2)
                if k1 != k2:
                    print("  files O-only", k1 - k2, "M-only", k2 - k1)
                for k in sorted(k1 & k2):
                    if f1[k] != f2[k]:
                        print("  filediff", k, len(f1[k]), len(f2[k]))
    print(f"{tot-fails}/{tot} passed")

if __name__ == "__main__":
    main()
