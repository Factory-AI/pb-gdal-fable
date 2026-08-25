#!/usr/bin/env python3
"""Capture static completion catalogs from the reference executable into
spec/misc/complete.json.  Run offline during development; the result is
embedded into the binary by tools/embed.py."""

import json
import os
import shutil
import subprocess
import sys
import tempfile

ORACLE = sys.argv[1] if len(sys.argv) > 1 else "/home/agent/oracle/executable"
OUT = os.path.join(os.path.dirname(__file__), "..", "spec", "misc", "complete.json")


def run(args, cwd=None):
    r = subprocess.run([ORACLE, "completion"] + args, capture_output=True,
                       text=True, cwd=cwd)
    return r.stdout


data = {}

data["configkeys"] = run(["gdal", "--config"])

universes = {
    "raster_out": run(["gdal", "raster", "convert", "--of"]),
    "raster_in": run(["gdal", "raster", "info", "--if"]),
    "vector_out": run(["gdal", "vector", "convert", "--of"]),
    "vector_in": run(["gdal", "vector", "info", "--if"]),
    "mdim_out": run(["gdal", "mdim", "convert", "--of"]),
    "mdim_in": run(["gdal", "mdim", "info", "--if"]),
    "dataset_format": run(["gdal", "dataset", "copy", "s", "d", "--format"]),
}
data["universes"] = universes

# driver creation/open/layer-creation option catalogs
drvopts = {}


def capture_opts(kind, driver, prefix_args, optflag):
    keys = run(prefix_args + [optflag, ""])
    if not keys or keys.startswith("**"):
        return None
    ent = {"keys": keys, "values": {}}
    for tok in keys.split(" "):
        if not tok.endswith("="):
            continue
        k = tok[:-1]
        v = run(prefix_args + [optflag, k + "="])
        ent["values"][k] = v
    return ent


def add_drv(kind, driver, prefix_args, optflag):
    ent = capture_opts(kind, driver, prefix_args, optflag)
    if ent is not None:
        drvopts.setdefault(kind, {})[driver] = ent


for d in ["GTiff", "COG", "VRT", "MEM", "GDALG"]:
    add_drv("raster_co", d, ["gdal", "raster", "convert", "--of", d], "--co")
for d in ["GTiff", "COG", "VRT", "MEM"]:
    add_drv("raster_oo", d, ["gdal", "raster", "convert", "--if", d], "--oo")
for d in ["MEM", "ESRI Shapefile", "GeoJSON", "GeoJSONSeq", "GDALG"]:
    add_drv("vector_co", d, ["gdal", "vector", "convert", "--of", d], "--co")
    add_drv("vector_lco", d, ["gdal", "vector", "convert", "--of", d], "--lco")
for d in ["MEM", "ESRI Shapefile", "GeoJSON", "GeoJSONSeq", "ESRIJSON", "TopoJSON"]:
    add_drv("vector_oo", d, ["gdal", "vector", "convert", "--if", d], "--oo")
data["drvopts"] = drvopts

# ---- pipeline capture ---------------------------------------------------
# context dir with marker files to classify dynamic file completions
ctx = tempfile.mkdtemp(prefix="gcpl")
open(os.path.join(ctx, "zzmark.tif"), "w").close()
open(os.path.join(ctx, "zzmark.txt"), "w").close()
open(os.path.join(ctx, "zzmdim.vrt"), "w").close()
# make a real shapefile marker via a temp geojson
gj = os.path.join(ctx, "zzmark.geojson")
with open(gj, "w") as f:
    f.write('{"type":"FeatureCollection","features":[{"type":"Feature",'
            '"properties":{},"geometry":{"type":"Point","coordinates":[1,2]}}]}')
subprocess.run([ORACLE, "vector", "convert", "-q", "--of", "ESRI Shapefile",
                gj, os.path.join(ctx, "zzmark.shp")], capture_output=True)
# raster context input
subprocess.run([ORACLE, "raster", "create", "-q", "--size", "2,2",
                os.path.join(ctx, "zzras.tif")], capture_output=True)

CFG = data["configkeys"]


def classify(out):
    if out == "":
        return {"k": "empty"}
    if out.startswith("** "):
        return {"k": "static", "v": out}
    if out == CFG:
        return {"k": "config"}
    if out.startswith("none EPSG: "):
        return {"k": "crs", "v": "none"}
    if out.startswith("pixel dataset EPSG: "):
        return {"k": "crs", "v": "pixel dataset"}
    if out.startswith("EPSG: "):
        return {"k": "crs", "v": ""}
    toks = out.split(" ")
    has_tif = "zzmark.tif" in toks or "zzras.tif" in toks
    has_shp = "zzmark.shp" in toks
    has_txt = "zzmark.txt" in toks
    has_vrt = "zzmdim.vrt" in toks
    if has_txt:
        return {"k": "files", "v": "all"}
    if has_tif and has_shp:
        return {"k": "files", "v": "both"}
    if has_tif:
        return {"k": "files", "v": "raster"}
    if has_shp:
        return {"k": "files", "v": "vector"}
    if has_vrt:
        return {"k": "files", "v": "mdim"}
    return {"k": "static", "v": out}


def cap_steps(entry_words, flavors):
    ent = {}
    ent["first"] = run(entry_words)
    firstopts = {}
    firstvals = {}
    for step in ent["first"].split(" "):
        if not step:
            continue
        ol = run(entry_words + [step, "--"])
        firstopts[step] = ol
        vals = {}
        vals[""] = classify(run(entry_words + [step, ""], cwd=ctx))
        for opt in ol.split(" "):
            if not opt:
                continue
            vals[opt] = classify(run(entry_words + [step, opt], cwd=ctx))
        firstvals[step] = vals
    ent["firstopts"] = firstopts
    ent["firstvals"] = firstvals
    mid = {}
    opts = {}
    optvals = {}
    for flavor, readarg in flavors.items():
        pre = entry_words + ["read", readarg, "!"]
        steplist = run(pre, cwd=ctx)
        mid[flavor] = steplist
        opts[flavor] = {}
        optvals[flavor] = {}
        for step in steplist.split(" "):
            if not step:
                continue
            ol = run(pre + [step, "--"], cwd=ctx)
            opts[flavor][step] = ol
            vals = {}
            vals[""] = classify(run(pre + [step, ""], cwd=ctx))
            for opt in ol.split(" "):
                if not opt:
                    continue
                vals[opt] = classify(run(pre + [step, opt], cwd=ctx))
            optvals[flavor][step] = vals
        ent["mid"] = mid
        ent["opts"] = opts
        ent["optvals"] = optvals
    return ent


data["pipeline"] = {
    "pipeline": cap_steps(["gdal", "pipeline"],
                          {"gen": "zznope", "ras": "zzras.tif", "vec": "zzmark.shp"}),
    "raster_pipeline": cap_steps(["gdal", "raster", "pipeline"],
                                 {"gen": "zznope", "ras": "zzras.tif"}),
    "vector_pipeline": cap_steps(["gdal", "vector", "pipeline"],
                                 {"gen": "zznope", "vec": "zzmark.shp"}),
}

# ---- per-command capture: bare output, option list, per-arg records -------
tree = json.load(open(os.path.join(os.path.dirname(__file__), "..",
                                   "spec", "tree.json")))
cmds = {}
for cid, c in sorted(tree.items()):
    path = [] if cid == "ROOT" else c.get("path", [])
    words = ["gdal"] + path
    ent = {}
    if cid != "ROOT":
        ent["bare"] = run(words, cwd=ctx)
    ent["opts"] = run(words + ["--"], cwd=ctx)
    args = {}
    for a in c.get("args", []):
        args[a["name"]] = classify(run(words + ["--" + a["name"]], cwd=ctx))
    ent["args"] = args
    cmds[cid] = ent
data["cmds"] = cmds

shutil.rmtree(ctx)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    json.dump(data, f, indent=1, sort_keys=True)
    f.write("\n")
print("wrote", OUT, os.path.getsize(OUT), "bytes")
