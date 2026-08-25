#!/usr/bin/env python3
# pipeline (raster/vector/generic + `gdal read` alias + GDALG) matrix
import subprocess, os, shutil, json

O = "/home/agent/oracle/executable"
M = "/workspace/executable"
T = "/tmp/ds/a.tif"
J = "/tmp/vc/in.json"
SHP = "/tmp/vc/in2.shp"

# each case: full gdal argv (after the executable) with placeholders
CASES = [
    # core read/write/info
    "pipeline read $T ! write o1.tif",
    "raster pipeline read $T ! write o2.tif",
    "vector pipeline read $J ! write o3.json",
    "pipeline read $J ! write o4.json",
    "pipeline read $J ! write o5.shp",
    "pipeline read $T ! info",
    "pipeline read $J ! info",
    "raster pipeline read $T ! info --format text",
    "vector pipeline read $J ! info --of json",
    "vector pipeline read $J ! info --of bogus",
    "pipeline read $T ! write o6.tif ! info",
    "read $T ! write a1.tif",
    "read $J ! write a2.json",
    "read $T ! info",
    "read $J ! info",
    "read $T",
    "read $J",
    "read nosuch.tif ! write a3.tif",
    # gdalg round trips
    "pipeline read $T ! write g1.gdalg.json",
    "vector pipeline read $J ! write g2.gdalg.json",
    # errors: counts / ordering / names
    "pipeline read $T",
    "raster pipeline read $T",
    "vector pipeline read $J",
    "pipeline nosuchstep ! write x.tif",
    "pipeline read $T ! nosuchstep ! write x.tif",
    "pipeline write x.tif ! write y.tif",
    "pipeline read $T ! read $T ! write x.tif",
    "pipeline info ! write x.tif",
    "raster pipeline write o.tif ! info",
    "vector pipeline write o.json ! info",
    "pipeline read $T ! info ! write x.tif",
    "pipeline concat ! write x.tif",
    "pipeline read $T ! concat ! write x.tif",
    # step option errors + suggestions
    "pipeline read --inptu $T ! write x.tif",
    "pipeline read $T ! write --ovewrite x.tif",
    "pipeline read $T ! write --nosuchopt x.tif",
    "pipeline read $T ! write -Z x.tif",
    "pipeline read $T ! info --formt text",
    "raster pipeline read $T extra ! write x.tif",
    "pipeline read ! write x.tif",
    "pipeline read $T ! write",
    # pre-open validation
    "pipeline read nosuch.xyz ! write x.tif",
    "vector pipeline read gz.json ! write x.json",
    "raster pipeline read gz.json ! write x.tif",
    "pipeline read gz.json ! write x.json",
    # vector update family in pipeline
    "vector pipeline read $J ! write --append e.json",
    "vector pipeline read $J ! write --overwrite e.json",
    "vector pipeline read $J ! write --update e.json",
    "vector pipeline read $SHP ! write --append e2.shp",
    # creation options + warnings
    "pipeline read $T ! write --co COMPRESS=LZW c1.tif",
    "pipeline read $T ! write --co NOSUCH=YES c2.tif",
    "vector pipeline read $J ! write --lco NOSUCH=YES c3.json",
    "vector pipeline read $J ! write --lco RFC7946=NO c4.json",
    "vector pipeline read $J ! write --oo NOSUCH=YES c5.json",
    "pipeline read $T ! write --of MEM m1",
    "vector pipeline read $J ! write --of MEM m2",
    "vector pipeline read $J ! write --of stream streamed_dataset",
    "pipeline read $T ! write --of stream streamed_dataset",
    # step help
    "raster pipeline read --help",
    "raster pipeline read $T --help",
    "raster pipeline reproject --help",
    "vector pipeline swap-xy --help",
    "pipeline concat -h",
    "pipeline tile --help",
    "raster pipeline tile --help",
    "vector pipeline info --help",
    "pipeline read in.json --help",
    "pipeline read --input in.json -h",
    "raster pipeline nosuchstep --help",
    "pipeline read $T --help ! write h1.tif",
    "pipeline read $T ! write h2.tif --help",
    "pipeline read $T ! reproject --help ! write h3.tif",
    "pipeline nosuchstep ! write --help",
    "pipeline read in.json ! write h4.json --help",
    "vector pipeline read in.json --help ! info",
    "read $T --help ! write h5.tif",
    "read in.json --help ! write h6.json",
    "read in.json --help",
    "read $T --help",
    "read $T --json-usage ! write h7.tif",
    "read $T --help-doc ! write h8.tif",
    "raster pipeline --help-doc",
    "pipeline --help-doc",
    "raster pipeline write --help-doc",
    "pipeline --json-usage",
    "raster pipeline --json-usage",
    "vector pipeline --json-usage",
    "pipeline --help",
    "vector pipeline -h",
    # single-token pipelines
    ["pipeline", "read $T ! write w1.tif"],
    ["vector", "pipeline", "read $J ! info"],
    ["pipeline", "read $T", "!", "write", "w2.tif"],
    ["raster", "pipeline", "read", "$T x", "!", "write", "w3.tif"],
    # brackets
    "pipeline [ read $T ] ! write b1.tif",
    "pipeline read [ $T ] ! write b2.tif",
    "pipeline read $T [ ! write b3.tif",
    "pipeline read $T ] ! write b4.tif",
    "pipeline concat [ read ] ! write b5.json",
    "pipeline concat [ read $T b ] ! write b6.tif",
    "pipeline concat [ nosuch ] ! write b7.tif",
    "pipeline read $T ! write [ b8.tif",
    "vector pipeline read [ read ] ! write b9.json",
    "pipeline ] read $T",
    "pipeline concat [ read --badopt x ] ! write b10.tif",
    # input-format validation
    "vector pipeline read --if MEM $J ! write i1.json",
    "vector pipeline read --if VRT $J ! write i2.json",
    "vector pipeline read --if GNMFile $J ! write i3.json",
    ["raster", "pipeline", "read", "--if", "ESRI Shapefile", "$T", "!",
     "write", "i4.tif"],
    "raster pipeline read --if GNMDatabase $T ! write i5.tif",
    "raster pipeline read --if COG $T ! write i6.tif",
    "raster pipeline read --if NoSuchDrv $T ! write i7.tif",
    "raster pipeline read --if gtiff $T ! write i8.tif",
    "pipeline read --if GTiff $T ! write i9.tif",
    "pipeline read --if GeoJSON $J ! write i10.json",
    "pipeline read --if TopoJSON zz.json ! write i11.json",
    "pipeline read --if GTiff $T",
    # middle steps and concat
    "pipeline read $T ! read ! write mr1.tif",
    "vector pipeline read $J ! read x ! info",
    "vector pipeline read $J ! read ! info",
    "pipeline read $T ! info ! info",
    "raster pipeline read $T ! info ! write mr2.tif",
    "vector pipeline read $J ! info ! write mr3.json",
    "pipeline concat --badopt ! write cc1.tif",
    "pipeline concat --mode single ! write cc2.tif",
    "pipeline concat $T ! write cc3.tif",
    "vector pipeline concat $J ! write cc4.json",
    "raster pipeline concat $T ! write cc6.tif",
    "pipeline read $T ! write --if GTiff cc7.tif",
    "vector pipeline concat nosuch.json ! write cm1.json",
    "vector pipeline concat gz.json ! write cm2.json",
    "pipeline concat nosuch.json ! write cm3.json",
    "vector pipeline read $J ! concat ! write cm4.json",
    "vector pipeline concat --mode bogus $J ! write cm5.json",
    "vector pipeline concat --output-layer x $J ! write cm6.json",
    "vector pipeline concat --if GTiff $J ! write cm7.json",
    "vector pipeline concat --oo NOSUCH=Y $J ! write cm8.json",
    "vector pipeline concat -l in $J ! write cm9.json",
    "vector pipeline concat --of GeoJSON $J ! write cm10.json",
    "vector pipeline concat $J ! info",
    "-q vector pipeline concat nosuch.json ! write cm11.json",
    "pipeline concat in.json ! write cm12.json",
    # concat leaf
    "vector concat $J k1.json",
    "vector concat $J k2.shp",
    "vector concat -q $J k3.json",
    "vector concat $J e.json",
    "vector concat --overwrite $J e.json",
    "vector concat --append $J e.json",
    "vector concat --update $J e.json",
    "vector concat --update $J e2.shp",
    "vector concat --update $J missing.json",
    "vector concat $J noext",
    "vector concat $J k4.gdalg.json",
    "vector concat --of MEM $J kmem",
    "vector concat nosuch.json k5.json",
    "vector concat gz.json k6.json",
    "vector concat e2.shp k7.json",
    "vector concat -l in $J k8.json",
    "vector concat -l nosuch $J k9.json",
    "vector concat -l nosuch $J k9.shp",
    "vector concat --lco NOSUCH=Y $J k10.json",
    "vector concat --if GTiff $J k11.json",
    "vector concat --if NoSuch $J k12.json",
    "vector concat $J",
    "vector concat",
    "vector concat --mode single $J k13.json",
    "vector concat --mode single --output-layer X $J k14.json",
    "vector concat --mode stack $J k15.json",
    "vector concat --mode bogus $J k16.json",
    "vector concat --output-layer X $J k17.json",
    "vector concat -q --output-layer X $J k18.json",
    "vector concat $J $J k19.json",
    "vector concat --of Bogus $J k20.json",
    "concat $J k21.json",
    "raster concat $T k22.tif",
    # concat leaf multi-input
    "vector concat $J $J w1.json",
    "vector concat $J $J w2.shp",
    "-q vector concat $J $J w3.json",
    "vector concat $J e2.shp w4.json",
    "vector concat e2.shp e2.shp w5.json",
    "vector concat e2.shp e2.shp w6.shp",
    "vector concat nosuch.json $J w7.json",
    "vector concat $J gz.json w8.json",
    "vector concat --mode single $J $J w9.json",
    "vector concat --mode single --output-layer L $J $J w10.json",
    "vector concat -l in $J $J w11.json",
    "vector concat -l nosuch $J $J w12.json",
    "vector concat -l nosuch $J $J w12.shp",
    "vector concat $J $J e.json",
    "vector concat --overwrite $J $J e.json",
    "vector concat --append $J $J e.json",
    "vector concat --append e2.shp e2.shp e.json",
    "vector concat --append e2.shp e.json",
    "vector concat --overwrite-layer $J e.json",
    "vector concat $J $J noext",
    "vector concat --of MEM $J $J wmem",
    "vector concat --field-strategy Intersection $J $J w13.json",
    "vector concat --of Bogus $J $J w14.json",
    "vector concat --if GTiff $J $J w15.json",
    "vector concat $J $J $J w16.json",
    # root info-attempt fallback
    "nosuch in.json",
    "in.json extra",
    "in.json in.json",
    "a.tif in.json",
    "a.tif -q",
    "a.tif --nosuchopt",
    "in.json --summary",
    "a.tif --summary",
    "a.tif --format text",
    "a.tif --format=text",
    "a.tif --format bogus",
    "a.tif --format text extra",
    "in.json --limit",
    "in.json --limit 1",
    "a.tif --subdataset 1",
    "a.tif --hist -q",
    "nosuch.json in.json",
    "in.json nosuch.json",
    "nosuchcmd xarg yarg",
    "nosuch.json",
    # root option info attempts
    "-q in.json",
    "-q a.tif",
    "-q nosuch",
    "--bogus in.json",
    "-q vector info in.json",
    "-q in.json in.json a.tif",
    "--format text a.tif",
    "--summary in.json",
    "--format bogus a.tif",
    "--drivers in.json",
    "--drivers a.tif",
    "--config FOO=BAR in.json",
    "--format text in.json extra",
    "--format text a.tif in.json",
    "--format text in.json in.json",
]

GDALG = [
    '{', '{"command_line"', '"abc', 'tru', 'nulx', 'trux', 'falsx',
    '{"a" 1}', '{"a":1 "b":2}', '[1 2]', '{5:1}', '-x', '{"a":+5}',
    '"a\\q"', '{"a":01}', '{"a":1,}', '[1,2,]', '{"a":.5}', '{,}', '[',
    '{"a"', '5x', '{}', '[]', '"x"', 'null', '{"a":"\\u00e9"}',
    '{"command_line":Infinity}', '{"command_line":NaN}',
    '{"command_line":5}', '{"command_line":3.5}', '{"command_line":true}',
    '{"command_line":null}', '{"command_line":["a"]}',
    '{"command_line":{"x":1}}', '{"command_line":""}',
    '{"command_line":"   "}', '{"command_line":"read"} garbage',
    '{"command_line": "read /tmp/ds/a.tif", }',
    '{"command_line":"gdal PIPELINE read a.tif"}',
    '{"command_line":"gdal vector convert --input in.json"}',
    '{"command_line":"gdal pipeline read a.tif"}',
    '{"command_line":"gdal raster pipeline read a.tif ! write gw.tif"}',
    '{"command_line":"gdal"}', '{"command_line":"gdal pipeline"}',
    '{"other":1,"command_line":"read a.tif ! info"}',
    '{"command_line":"read in.json ! write ov.json"}',
    '{"command_line":"read a.tif ! info"}',
]


def prep(d, jf=None):
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy(T, d + "/a.tif")
    shutil.copy(J, d + "/in.json")
    shutil.copy("/tmp/vc/out.json", d + "/e.json")
    for ext in ("shp", "shx", "dbf", "prj"):
        shutil.copy("/tmp/vc/in2." + ext, d + "/e2." + ext)
    with open(d + "/gz.json", "w") as f:
        f.write("garbage{")
    if jf is not None:
        with open(d + "/t.json", "w") as f:
            f.write(jf)


def run(argv, jf=None):
    res = []
    for i, exe in enumerate([O, M]):
        d = "/tmp/pl/h" + "ab"[i]
        prep(d, jf)
        p = subprocess.run([exe] + argv, cwd=d, capture_output=True)
        files = {}
        for dp, dns, fns in os.walk(d):
            for dn in dns:
                files[os.path.relpath(os.path.join(dp, dn), d) + "/"] = b""
            for fn in fns:
                full = os.path.join(dp, fn)
                with open(full, "rb") as fh:
                    files[os.path.relpath(full, d)] = fh.read()
        res.append((p.returncode, p.stdout, p.stderr, files))
    return res


def report(label, res):
    (r1, s1, e1, f1), (r2, s2, e2, f2) = res
    print("FAIL:", label)
    if r1 != r2:
        print("  rc", r1, r2)
    if s1 != s2:
        print("  out", repr(s1[:120]), repr(s2[:120]))
    if e1 != e2:
        print("  err\n   O:", repr(e1[:220]), "\n   M:", repr(e2[:220]))
    if f1 != f2:
        keys = set(f1) | set(f2)
        for k in sorted(keys):
            if f1.get(k) != f2.get(k):
                print("  file", k, len(f1.get(k, b"")), len(f2.get(k, b"")))


def main():
    fails = tot = 0
    for c in CASES:
        argv = c if isinstance(c, list) else c.split()
        argv = [a.replace("$T", T).replace("$J", J).replace("$SHP", SHP)
                for a in argv]
        res = run(argv)
        tot += 1
        if res[0] != res[1]:
            fails += 1
            report(" ".join(argv), res)
    for jf in GDALG:
        res = run(["pipeline", "t.json"], jf)
        tot += 1
        if res[0] != res[1]:
            fails += 1
            report("gdalg " + json.dumps(jf), res)
    print(f"{tot - fails}/{tot} OK")


if __name__ == "__main__":
    main()
