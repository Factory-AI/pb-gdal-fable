#!/usr/bin/env python3
"""Differential harness for commands that produce files.

Each case runs in twin scratch dirs (oracle vs candidate); compares rc,
stdout, stderr, and the full resulting directory tree byte-for-byte.
"""
import os, shutil, subprocess, sys, tempfile

ORACLE = '/home/agent/oracle/executable'
MINE = '/workspace/executable'

CASES = [
    ['raster', 'create', '--size', '3,2', 'out.tif'],
    ['raster', 'create', '--size', '1,1', 'out.tif'],
    ['raster', 'create', '--size', '256,257', '--datatype', 'Float64', 'out.tif'],
    ['raster', 'create', '--size', '300,300', '--band-count', '3', '--datatype', 'UInt16',
     '--nodata', '7', '--crs', 'EPSG:32631', '--bbox', '1,2,3,4', '--metadata', 'FOO=BAR',
     '--burn', '1', '--burn', '2', '--burn', '3', 'out.tif'],
    ['raster', 'create', '--size', '9,7', '--band-count', '6', '--datatype', 'Float64',
     '--nodata', '1e-300', '--metadata', 'A=B', '--crs', 'EPSG:32631', '--bbox', '1,2,3,4', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--band-count', '3', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--band-count', '4', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--band-count', '2', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--band-count', '5', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--band-count', '4', '--datatype', 'UInt16', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--band-count', '3', '--co', 'INTERLEAVE=BAND', 'out.tif'],
    ['raster', 'create', '--size', '20,20', '--band-count', '2', '--co', 'INTERLEAVE=BAND',
     '--burn', '1', '--burn', '2', 'out.tif'],
    ['raster', 'create', '--size', '100,100', '--co', 'TILED=YES', 'out.tif'],
    ['raster', 'create', '--size', '520,300', '--co', 'TILED=YES', '--burn', '9', 'out.tif'],
    ['raster', 'create', '--size', '300,300', '--burn', '9', '--co', 'TILED=YES', 'out.tif'],
    ['raster', 'create', '--size', '40,48', '--co', 'TILED=YES', '--co', 'BLOCKXSIZE=32',
     '--co', 'BLOCKYSIZE=16', '--burn', '3', 'out.tif'],
    ['raster', 'create', '--size', '64,64', '--burn', '7', '--co', 'COMPRESS=DEFLATE', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--burn', '5', '--co', 'COMPRESS=DEFLATE', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--burn', '5', '--co', 'COMPRESS=DEFLATE',
     '--co', 'ZLEVEL=1', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--burn', '5', '--co', 'COMPRESS=DEFLATE',
     '--co', 'ZLEVEL=9', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--burn', '5', '--co', 'COMPRESS=DEFLATE',
     '--co', 'ZLEVEL=12', 'out.tif'],
    ['raster', 'create', '--size', '16,4', '--burn', '3', '--co', 'COMPRESS=LZW', 'out.tif'],
    ['raster', 'create', '--size', '16,4', '--burn', '3', '--co', 'COMPRESS=PACKBITS', 'out.tif'],
    ['raster', 'create', '--size', '400,200', '--burn', '250', '--co', 'COMPRESS=LZW',
     '--datatype', 'UInt16', 'out.tif'],
    ['raster', 'create', '--size', '400,200', '--burn', '77', '--co', 'COMPRESS=PACKBITS', 'out.tif'],
    ['raster', 'create', '--size', '16,16', '--burn', '1', '--co', 'COMPRESS=LZW',
     '--co', 'PREDICTOR=2', '--datatype', 'UInt16', 'out.tif'],
    ['raster', 'create', '--size', '33,17', '--burn', '1.5', '--co', 'COMPRESS=DEFLATE',
     '--co', 'PREDICTOR=3', '--datatype', 'Float32', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float64', '--nodata', '0.1', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float32', '--nodata', '0.1', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float32', '--nodata', 'nan', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float64', '--nodata', 'inf', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float64', '--nodata', '-inf', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Int16', '--nodata', '-32768', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'CInt16', '--nodata', '5', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'CFloat64', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float16', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Int8', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'UInt8', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'UInt64', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Int64', '--nodata', '-1', 'out.tif'],
    ['raster', 'create', '--size', '2,1', '--burn', '300', 'out.tif'],
    ['raster', 'create', '--size', '2,1', '--burn', '-5', 'out.tif'],
    ['raster', 'create', '--size', '2,1', '--burn', '1.7', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4326', '--bbox', '1,2,3,4', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4326', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4979', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4985', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4901', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:27561', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:2255', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:26591', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:3857', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4267', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4322', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:32760', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:3413', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:2154', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--metadata', 'Z=1', '--metadata', 'A=<&>"', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--metadata', 'A=B c', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--co', 'PROFILE=BASELINE', '--nodata', '5',
     '--metadata', 'A=B', '--crs', 'EPSG:4326', '--bbox', '1,2,3,4', 'out.tif'],
    ['raster', 'create', '--size', '4,4', '--co', 'PROFILE=GeoTIFF', '--crs', 'EPSG:4326',
     '--bbox', '1,2,3,4', '--metadata', 'A=B', '--nodata', '5', 'out.tif'],
    ['raster', 'create', '--size', '4,4', '--co', 'PROFILE=BASELINE', '--crs', 'EPSG:32631',
     '--bbox', '1,2,3,4', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--co', 'SPARSE_OK=YES', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'SPARSE_OK=YES', 'out.tif'],
    ['raster', 'create', '--size', '8,100', '--co', 'BLOCKYSIZE=7', 'out.tif'],
    ['raster', 'create', '--size', '10000,2', '--datatype', 'Float64', 'out.tif'],
    # errors
    ['raster', 'create', '--size', '0,0', 'out.tif'],
    ['raster', 'create', 'out.tif'],
    ['raster', 'create', '--size', '8', 'out.tif'],
    ['raster', 'create', '--bbox', '1,2,3', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--crs', 'banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--nodata', 'banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--nodata', '300', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--nodata', '-1.5', '--datatype', 'Int16', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--nodata', 'nan', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--burn', '1', '--burn', '2', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--band-count', '3', '--burn', '1', '--burn', '2', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--bbox', '3,2,1,4', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--bbox', '1,4,3,2', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'BANANA=1', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'COMPRESS=banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'PROFILE=banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'TILED=banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'ZLEVEL=0', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'PREDICTOR=2', 'out.tif'],
    ['raster', 'create', '--size', '40,40', '--co', 'BLOCKXSIZE=32', 'out.tif'],
    ['raster', 'create', '--size', '40,40', '--co', 'TILED=YES', '--co', 'BLOCKXSIZE=100', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'INTERLEAVE=TILE', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--band-count', '0', 'out.tif'],
    ['raster', 'create', '--size', '-2,2', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'unknown', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '-f', 'banana', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '-f', 'PNG', 'out.tif'],
    ['raster', 'create', '--size', '2,2', 'out.png'],
    ['raster', 'create', '--size', '2,2', 'noext'],
    ['raster', 'create', '--size', '2,2', '--co', 'blockysize=7', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--co', 'compress=lzw', 'out.tif'],
    # overwrite / append (PRE: marks a setup command run with the same tool)
    ('PRE', ['raster', 'create', '--size', '3,2', 'out.tif'],
     ['raster', 'create', '--size', '2,2', 'out.tif']),
    ('PRE', ['raster', 'create', '--size', '3,2', 'out.tif'],
     ['raster', 'create', '--size', '2,2', '--overwrite', 'out.tif']),
    ('PRE', ['raster', 'create', '--size', '3,2', 'out.tif'],
     ['raster', 'create', '--size', '5,4', '--append', '--metadata', 'A=B', 'out.tif']),
    ('PRE', ['raster', 'create', '--size', '1,1', 'out.tif'],
     ['raster', 'create', '--size', '1,1', '--append', 'out.tif']),
    ('PRE', ['raster', 'create', '--size', '3,2', '--crs', 'EPSG:32631', '--bbox', '0,0,3,2', 'out.tif'],
     ['raster', 'create', '--like', 'out.tif', 'out2.tif']),
    ['raster', 'create', '--size', '64,300', '--co', 'COMPRESS=DEFLATE', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'COMPRESS=LZW', '--burn', '3', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'COMPRESS=PACKBITS', '--burn', '3', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--nodata', '5', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--datatype', 'Int16', '--nodata', '-3',
     '--co', 'COMPRESS=DEFLATE', '--co', 'PREDICTOR=2', 'out.tif'],
    ['raster', 'create', '--size', '8,100', '--co', 'BLOCKYSIZE=7', '--nodata', '5', 'out.tif'],
    ['raster', 'create', '--size', '8,100', '--co', 'BLOCKYSIZE=7', '--burn', '0', 'out.tif'],
    ['raster', 'create', '--size', '8,100', '--co', 'BLOCKYSIZE=7', '--burn', '1', 'out.tif'],
    ['raster', 'create', '--size', '20,30', '--band-count', '2', '--co', 'INTERLEAVE=BAND',
     '--co', 'BLOCKYSIZE=8', '--burn', '1', '--burn', '2', 'out.tif'],
    ['raster', 'create', '--size', '20,30', '--band-count', '2', '--co', 'INTERLEAVE=BAND',
     '--co', 'BLOCKYSIZE=8', '--nodata', '9', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'CInt16', '--nodata', '5', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Int64', '--nodata', '-1', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'UInt16', '--nodata', '7', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float32', '--nodata', '0.1', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--datatype', 'Float64', '--nodata', 'nan', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:26591', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4901', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:4267', 'out.tif'],
    ['raster', 'create', '--size', '2,2', '--crs', 'EPSG:27561', 'out.tif'],
    ['raster', 'create', '--size', '8', 'out.tif'],
    ['raster', 'create', '--size', '8,8,8', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--bbox', '1,2,3', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--co', 'banana', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--metadata', 'nokv', 'out.tif'],
    ['raster', 'create', '--size', '8,8', '--co', '=X', 'out.tif'],
    ['raster', 'create', '--size', '-2,2', 'out.tif'],
    ['raster', 'create', '--size', '30,40', '--co', 'TILED=YES', '--co', 'BLOCKXSIZE=16',
     '--co', 'BLOCKYSIZE=32', '--nodata', '3', 'out.tif'],
    ['raster', 'create', '--size', '600,5', '--co', 'COMPRESS=DEFLATE', '--co', 'ZLEVEL=4',
     '--nodata', '250', '--datatype', 'Byte', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'SPARSE_OK=YES', '--nodata', '5', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'SPARSE_OK=YES', '--burn', '0', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'SPARSE_OK=YES', '--nodata', '5', '--burn', '5', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--co', 'SPARSE_OK=YES', '--nodata', '5', '--burn', '6', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--nodata', '5', '--co', 'COMPRESS=DEFLATE', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--nodata', '5', '--co', 'COMPRESS=LZW', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--nodata', '5', '--co', 'COMPRESS=PACKBITS', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--datatype', 'Int16', '--nodata', '-3', 'out.tif'],
    ['raster', 'create', '--size', '64,300', '--datatype', 'byte', '--burn', '2', 'out.tif'],
    ['raster', 'create', '--size', '4,4', '--datatype', 'Byte', 'out.tif'],
    ['raster', 'create', '--size', '30,70', '--band-count', '3', '--nodata', '8',
     '--co', 'COMPRESS=DEFLATE', '--co', 'INTERLEAVE=BAND', '--co', 'BLOCKYSIZE=32', 'out.tif'],
    ['raster', 'create', '--size', '6552,3', '--co', 'COMPRESS=DEFLATE', '--co', 'BLOCKYSIZE=1', 'out.tif'],
    ['raster', 'create', '--size', '6553,3', '--co', 'COMPRESS=DEFLATE', '--co', 'BLOCKYSIZE=1', 'out.tif'],
    ['raster', 'create', '--size', '6552,3', '--co', 'COMPRESS=LZW', '--co', 'BLOCKYSIZE=1', '--burn', '4', 'out.tif'],
    ['raster', 'create', '--size', '200,3', '--co', 'COMPRESS=PACKBITS', '--co', 'BLOCKYSIZE=1', 'out.tif'],
    ['raster', 'create', '--size', '200,100', '--co', 'TILED=YES', '--co', 'COMPRESS=DEFLATE',
     '--co', 'BLOCKXSIZE=64', '--co', 'BLOCKYSIZE=32', '--nodata', '2', 'out.tif'],
    ['raster', 'create', '--size', '64,1', '--co', 'COMPRESS=DEFLATE', 'out.tif'],
    ['raster', 'create', '--size', '70000,3', 'out.tif'],
    ['raster', 'create', '--size', '40,40', '--co', 'TILED=YES', '--co', 'BLOCKXSIZE=16',
     '--co', 'BLOCKYSIZE=16', '--co', 'COMPRESS=LZW', 'out.tif'],
]


def run_case(exe, case, d):
    if isinstance(case, tuple):
        pre, cmd = case[1], case[2]
        subprocess.run([exe] + pre, cwd=d, capture_output=True)
    else:
        cmd = case
    return subprocess.run([exe] + cmd, cwd=d, capture_output=True, text=True)


def tree(d):
    out = {}
    for root, _, files in os.walk(d):
        for f in files:
            p = os.path.join(root, f)
            with open(p, 'rb') as fh:
                out[os.path.relpath(p, d)] = fh.read()
    return out


def main():
    failed = 0
    for i, case in enumerate(CASES):
        da = tempfile.mkdtemp(prefix='cre_a_')
        db = tempfile.mkdtemp(prefix='cre_b_')
        ra = run_case(ORACLE, case, da)
        rb = run_case(MINE, case, db)
        label = ' '.join(case[2]) if isinstance(case, tuple) else ' '.join(case)
        problems = []
        if ra.returncode != rb.returncode:
            problems.append('rc %d vs %d' % (ra.returncode, rb.returncode))
        if ra.stdout != rb.stdout:
            problems.append('stdout %r vs %r' % (ra.stdout[:200], rb.stdout[:200]))
        if ra.stderr != rb.stderr:
            problems.append('stderr %r vs %r' % (ra.stderr[:300], rb.stderr[:300]))
        ta, tb = tree(da), tree(db)
        if set(ta) != set(tb):
            problems.append('files %s vs %s' % (sorted(ta), sorted(tb)))
        else:
            for f in ta:
                if ta[f] != tb[f]:
                    la, lb = len(ta[f]), len(tb[f])
                    diff = next((j for j in range(min(la, lb)) if ta[f][j] != tb[f][j]), min(la, lb))
                    problems.append('file %s differs (len %d vs %d, first at %d)' % (f, la, lb, diff))
        if problems:
            failed += 1
            print('FAIL [%d] %s' % (i, label))
            for p in problems:
                print('   ', p)
        shutil.rmtree(da)
        shutil.rmtree(db)
    print('%d/%d passed' % (len(CASES) - failed, len(CASES)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
