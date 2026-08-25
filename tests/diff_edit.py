#!/usr/bin/env python3
"""Differential harness for `gdal raster edit`: for every fixture in /tmp/ds
and /tmp/dz, run each edit operation against oracle and candidate in twin
sandboxes and compare rc/stdout/stderr and all produced files byte-for-byte.

Usage: python3 tests/diff_edit.py [fixture-glob ...]
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.environ.get("ORACLE", "/home/agent/oracle/executable")
MINE = os.path.join(ROOT, "executable")

OPS = [
    ["--metadata", "A=1"],
    ["--crs", "EPSG:4326"],
    ["--nodata", "42"],
    ["--stats", "-q"],
    ["--bbox", "1,2,3,4"],
    ["--hist", "-q"],
    ["--unset-metadata", "BK"],
    ["--auxiliary", "--stats", "-q"],
    ["--gcp", "0,0,1,2"],
    ["--gcp", "0,0,1,2", "--crs", "EPSG:32631"],
    ["--scale", "3", "--offset", "2"],
    ["--color-interpretation", "gray"],
]


def run_one(fixture, op):
    with tempfile.TemporaryDirectory() as td:
        outs = {}
        for label, exe in (("a", ORACLE), ("b", MINE)):
            d = os.path.join(td, label)
            os.mkdir(d)
            shutil.copy(fixture, os.path.join(d, "t.tif"))
            aux = fixture + ".aux.xml"
            if os.path.exists(aux):
                shutil.copy(aux, os.path.join(d, "t.tif.aux.xml"))
            p = subprocess.run(
                [exe, "raster", "edit"] + op + ["t.tif"],
                cwd=d, capture_output=True)
            files = {}
            for f in sorted(os.listdir(d)):
                with open(os.path.join(d, f), "rb") as fp:
                    files[f] = fp.read()
            outs[label] = (p.returncode, p.stdout, p.stderr, files)
        a, b = outs["a"], outs["b"]
        if a[0] != b[0]:
            return "rc %d vs %d" % (a[0], b[0])
        if a[1] != b[1]:
            return "stdout diff"
        if a[2] != b[2]:
            return "stderr diff: %r vs %r" % (a[2][:120], b[2][:120])
        if set(a[3]) != set(b[3]):
            return "file set diff: %s vs %s" % (sorted(a[3]), sorted(b[3]))
        for f in a[3]:
            if a[3][f] != b[3][f]:
                return "file diff: " + f
        return None


def main():
    pats = sys.argv[1:] or ["/tmp/ds/*.tif", "/tmp/dz/*.tif"]
    fixtures = sorted(sum((glob.glob(p) for p in pats), []))
    fails = 0
    total = 0
    for fx in fixtures:
        for op in OPS:
            total += 1
            r = run_one(fx, op)
            if r:
                fails += 1
                print("FAIL %s %s: %s" % (os.path.basename(fx),
                                          " ".join(op), r))
    print("%d/%d passed" % (total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
