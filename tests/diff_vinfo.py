#!/usr/bin/env python3
"""Differential harness for `gdal vector info`: run oracle and candidate on
each fixture under /tmp/vj (plus any given globs) with several option sets
and compare rc/stdout/stderr byte-for-byte.

Usage: python3 tests/diff_vinfo.py [fixture-glob ...]
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORACLE = os.environ.get("ORACLE", "/home/agent/oracle/executable")
MINE = os.path.join(ROOT, "executable")

OPTS = [
    [],
    ["--of", "json"],
    ["--features"],
    ["--of", "json", "--features"],
    ["--summary"],
    ["--of", "json", "--summary"],
    ["--fid", "0"],
    ["--fid", "1"],
    ["--fid", "99"],
    ["--of", "json", "--fid", "1"],
    ["--features", "--limit", "1"],
    ["--of", "json", "--features", "--limit", "1"],
]

# option sets where LAYER is replaced by the fixture's first layer name
LAYER_OPTS = [
    ["--layer", "LAYER"],
    ["--of", "json", "--layer", "LAYER"],
    ["--summary", "--layer", "LAYER"],
    ["--of", "json", "--summary", "--layer", "LAYER"],
    ["--features", "--layer", "LAYER"],
    ["--layer", "nosuchlayer"],
]


def layer_name(fixture):
    base = os.path.basename(fixture)
    dot = base.rfind(".")
    return base[:dot] if dot > 0 else base


def run_one(fixture, op):
    outs = {}
    for label, exe in (("a", ORACLE), ("b", MINE)):
        p = subprocess.run([exe, "vector", "info"] + op + [fixture],
                           capture_output=True)
        outs[label] = (p.returncode, p.stdout, p.stderr)
    a, b = outs["a"], outs["b"]
    if a[0] != b[0]:
        return "rc %d vs %d" % (a[0], b[0])
    if a[1] != b[1]:
        for i, (x, y) in enumerate(zip(a[1].splitlines(), b[1].splitlines())):
            if x != y:
                return "stdout diff line %d: %r vs %r" % (i + 1, x[:150],
                                                          y[:150])
        return "stdout diff len %d vs %d" % (len(a[1]), len(b[1]))
    if a[2] != b[2]:
        return "stderr diff: %r vs %r" % (a[2][:200], b[2][:200])
    return None


def main():
    pats = sys.argv[1:] or ["/tmp/vj/*"]
    fixtures = sorted(sum((glob.glob(p) for p in pats), []))
    fixtures = [f for f in fixtures if
                (os.path.isdir(f) or os.path.isfile(f)) and
                not f.endswith((".aux.xml", ".shx", ".prj"))]
    fails = 0
    total = 0
    for fx in fixtures:
        ops = [list(op) for op in OPTS]
        ln = layer_name(fx)
        for op in LAYER_OPTS:
            ops.append([ln if x == "LAYER" else x for x in op])
        for op in ops:
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
