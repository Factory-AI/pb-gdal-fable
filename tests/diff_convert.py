#!/usr/bin/env python3
"""Differential harness for `gdal raster convert`.

Each case runs in twin scratch dirs (oracle vs candidate); compares rc,
stdout, stderr, and the full resulting directory tree byte-for-byte.

Case forms:
  [args...]                         plain invocation, cwd = scratch dir
  ('SETUP', [ops...], [args...])    ops run first in the scratch dir:
      ('cp', src_abs, rel)   copy a fixture in
      ('touch', rel)         create empty file
      ('mkdir', rel)         create directory
      ('run', [args...])     run the same executable first
"""
import glob, os, shutil, subprocess, sys, tempfile

ORACLE = '/home/agent/oracle/executable'
MINE = '/workspace/executable'

DS = sorted(glob.glob('/tmp/ds/*.tif'))
DZ = sorted(glob.glob('/tmp/dz/*.tif'))

SKIP = set()

CASES = []

# fixture sweep: GTiff and VRT outputs
for f in DS + DZ:
    if os.path.basename(f) in SKIP:
        continue
    CASES.append(['raster', 'convert', f, 'out.tif'])
    CASES.append(['raster', 'convert', f, 'out.vrt'])

# other output formats
for f in ['/tmp/ds/utm.tif', '/tmp/ds/f.tif', '/tmp/dz/mw8.tif']:
    CASES.append(['raster', 'convert', '--of', 'MEM', f, 'x'])
    CASES.append(['raster', 'convert', '--of', 'stream', f, 'x'])
    CASES.append(['raster', 'convert', '--of', 'GDALG', f, 'out.gdalg.json'])
    CASES.append(['raster', 'convert', f, 'out.gdalg.json'])

# quiet
CASES += [
    ['raster', 'convert', '-q', '/tmp/ds/utm.tif', 'out.tif'],
    ['raster', 'convert', '--quiet', '/tmp/dz/us1.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/us1.tif', 'out.tif'],
    ['raster', 'convert', '-q', '/tmp/dz/us1.tif', 'out.vrt'],
]

# creation options on GTiff output
SRC = '/tmp/ds/rnd.tif'
for co in [
    ['TILED=YES'],
    ['TILED=YES', 'BLOCKXSIZE=32', 'BLOCKYSIZE=16'],
    ['TILED=YES', 'BLOCKXSIZE=100'],
    ['BLOCKYSIZE=7'],
    ['BLOCKXSIZE=32'],
    ['COMPRESS=DEFLATE'],
    ['COMPRESS=DEFLATE', 'ZLEVEL=1'],
    ['COMPRESS=DEFLATE', 'ZLEVEL=9'],
    ['COMPRESS=DEFLATE', 'PREDICTOR=2'],
    ['COMPRESS=LZW'],
    ['COMPRESS=LZW', 'PREDICTOR=2'],
    ['COMPRESS=PACKBITS'],
    ['PREDICTOR=2'],
    ['INTERLEAVE=BAND'],
    ['INTERLEAVE=PIXEL'],
    ['SPARSE_OK=YES'],
    ['PROFILE=BASELINE'],
    ['PROFILE=GeoTIFF'],
    ['BANANA=1'],
    ['banana'],
    ['COMPRESS=banana'],
    ['TILED=banana'],
    ['compress=lzw'],
]:
    c = ['raster', 'convert', SRC, 'out.tif']
    for kv in co:
        c[2:2] = ['--co', kv]
    CASES.append(c)

for co in [['TILED=YES'], ['COMPRESS=DEFLATE'], ['INTERLEAVE=PIXEL']]:
    c = ['raster', 'convert', '/tmp/ds/utm.tif', 'out.tif']
    for kv in co:
        c[2:2] = ['--co', kv]
    CASES.append(c)

# --co on non-GTiff outputs
CASES += [
    ['raster', 'convert', '--of', 'MEM', '--co', 'TILED=YES', '/tmp/ds/utm.tif', 'x'],
    ['raster', 'convert', '--of', 'MEM', '--co', 'A=B', '--co', 'C=D', '/tmp/ds/utm.tif', 'x'],
    ['raster', 'convert', '--co', 'TILED=YES', '/tmp/ds/utm.tif', 'out.vrt'],
    ['raster', 'convert', '--of', 'GDALG', '--co', 'A=B', '/tmp/ds/utm.tif', 'out.gdalg.json'],
]

# GDALG command_line serialization variants
CASES += [
    ['raster', 'convert', '-q', '/tmp/ds/utm.tif', 'out.gdalg.json'],
    ['raster', 'convert', '--if', 'GTiff', '/tmp/ds/utm.tif', 'out.gdalg.json'],
    ['raster', 'convert', '--if', 'GTiff', '--if', 'VRT', '/tmp/ds/utm.tif', 'out.gdalg.json'],
    ['raster', 'convert', '--oo', 'COLOR_TABLE_MULTIPLIER=1', '/tmp/dz/pal.tif', 'out.gdalg.json'],
    ['raster', 'convert', '--oo', 'GEOREF_SOURCES=PAM', '/tmp/dz/pw.tif', 'out.gdalg.json'],
]

# open options / input format on tif output
CASES += [
    ['raster', 'convert', '--oo', 'COLOR_TABLE_MULTIPLIER=1', '/tmp/dz/pal.tif', 'out.tif'],
    ['raster', 'convert', '--oo', 'COLOR_TABLE_MULTIPLIER=257', '/tmp/dz/pal.tif', 'out.tif'],
    ['raster', 'convert', '--oo', 'GEOREF_SOURCES=PAM', '/tmp/dz/pw.tif', 'out.tif'],
    ['raster', 'convert', '--oo', 'GEOREF_SOURCES=WORLDFILE', '/tmp/dz/pw.tif', 'out.tif'],
    ['raster', 'convert', '--oo', 'GEOREF_SOURCES=NONE', '/tmp/dz/pw.tif', 'out.tif'],
    ['raster', 'convert', '--if', 'GTiff', '/tmp/ds/utm.tif', 'out.tif'],
    ['raster', 'convert', '--if', 'VRT', '/tmp/ds/utm.tif', 'out.tif'],
    ['raster', 'convert', '--if', 'BANANA', '/tmp/ds/utm.tif', 'out.tif'],
]

# subdataset inputs
CASES += [
    ['raster', 'convert', 'GTIFF_DIR:1:/tmp/dz/sub.tif', 'out.tif'],
    ['raster', 'convert', 'GTIFF_DIR:2:/tmp/dz/sub.tif', 'out.tif'],
    ['raster', 'convert', 'GTIFF_DIR:2:/tmp/dz/sub.tif', 'out.vrt'],
    ['raster', 'convert', 'GTIFF_DIR:5:/tmp/dz/sub.tif', 'out.tif'],
]

# error paths
CASES += [
    ['raster', 'convert', 'missing.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/ds/area.txt', 'out.tif'],
    ['raster', 'convert', '--of', 'BANANA', '/tmp/ds/utm.tif', 'out.tif'],
    ['raster', 'convert', '--of', 'PNG', '/tmp/ds/utm.tif', 'out.png'],
    ['raster', 'convert', '/tmp/ds/utm.tif', 'out.xyz'],
    ['raster', 'convert', 'missing.tif', 'out.xyz'],
    ['raster', 'convert', '/tmp/ds/utm.tif'],
    ['raster', 'convert'],
]

# exists / overwrite / append
CP_UTM = ('cp', '/tmp/ds/utm.tif', 'utm.tif')
CASES += [
    ('SETUP', [('touch', 'out.tif')],
     ['raster', 'convert', '/tmp/ds/utm.tif', 'out.tif']),
    ('SETUP', [('touch', 'out.xyz')],
     ['raster', 'convert', '/tmp/ds/utm.tif', 'out.xyz']),
    ('SETUP', [('mkdir', 'out.tif')],
     ['raster', 'convert', '/tmp/ds/utm.tif', 'out.tif']),
    ('SETUP', [('cp', '/tmp/ds/nocrs.tif', 'out.tif')],
     ['raster', 'convert', '/tmp/ds/utm.tif', 'out.tif']),
    ('SETUP', [('cp', '/tmp/ds/nocrs.tif', 'out.tif')],
     ['raster', 'convert', '--overwrite', '/tmp/ds/utm.tif', 'out.tif']),
    ('SETUP', [('cp', '/tmp/ds/nocrs.tif', 'out.tif')],
     ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.tif']),
    ('SETUP', [('touch', 'out.tif')],
     ['raster', 'convert', '--overwrite', '/tmp/ds/utm.tif', 'out.tif']),
    ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.tif'],
    ['raster', 'convert', '--overwrite', '/tmp/ds/utm.tif', 'out.tif'],
    ('SETUP', [('run', ['raster', 'convert', '/tmp/ds/nocrs.tif', 'out.tif'])],
     ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.tif']),
    # existing output with stale .aux.xml
    ('SETUP', [('cp', '/tmp/ds/nocrs.tif', 'out.tif'),
               ('cp', '/tmp/ds/p2.tif.aux.xml', 'out.tif.aux.xml')],
     ['raster', 'convert', '--overwrite', '/tmp/ds/utm.tif', 'out.tif']),
    ('SETUP', [('cp', '/tmp/ds/nocrs.tif', 'out.tif'),
               ('cp', '/tmp/ds/p2.tif.aux.xml', 'out.tif.aux.xml')],
     ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.tif']),
    # MEM / VRT / GDALG exists variants
    ('SETUP', [('touch', 'x')],
     ['raster', 'convert', '--of', 'MEM', '/tmp/ds/utm.tif', 'x']),
    ('SETUP', [('touch', 'x')],
     ['raster', 'convert', '--of', 'MEM', '--overwrite', '/tmp/ds/utm.tif', 'x']),
    ['raster', 'convert', '--of', 'MEM', '--append', '/tmp/ds/utm.tif', 'x'],
    ('SETUP', [('touch', 'x')],
     ['raster', 'convert', '--of', 'MEM', '--append', '/tmp/ds/utm.tif', 'x']),
    ('SETUP', [('run', ['raster', 'convert', '/tmp/ds/nocrs.tif', 'out.vrt'])],
     ['raster', 'convert', '/tmp/ds/utm.tif', 'out.vrt']),
    ('SETUP', [('run', ['raster', 'convert', '/tmp/ds/nocrs.tif', 'out.vrt'])],
     ['raster', 'convert', '--overwrite', '/tmp/ds/utm.tif', 'out.vrt']),
    ('SETUP', [('run', ['raster', 'convert', '/tmp/ds/nocrs.tif', 'out.vrt'])],
     ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.vrt']),
    ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.vrt'],
    ('SETUP', [('touch', 'out.gdalg.json')],
     ['raster', 'convert', '/tmp/ds/utm.tif', 'out.gdalg.json']),
    ('SETUP', [('touch', 'out.gdalg.json')],
     ['raster', 'convert', '--overwrite', '/tmp/ds/utm.tif', 'out.gdalg.json']),
    ('SETUP', [('touch', 'out.gdalg.json')],
     ['raster', 'convert', '--append', '/tmp/ds/utm.tif', 'out.gdalg.json']),
]

# relative path variants (VRT SourceFilename rules)
CASES += [
    ('SETUP', [CP_UTM], ['raster', 'convert', 'utm.tif', 'out.vrt']),
    ('SETUP', [CP_UTM], ['raster', 'convert', 'utm.tif', 'out.tif']),
    ('SETUP', [('mkdir', 'sub'), ('cp', '/tmp/ds/utm.tif', 'sub/utm.tif')],
     ['raster', 'convert', 'sub/utm.tif', 'out.vrt']),
    ('SETUP', [('mkdir', 'sub'), ('cp', '/tmp/ds/utm.tif', 'sub/utm.tif')],
     ['raster', 'convert', 'sub/utm.tif', 'sub/out.vrt']),
    ('SETUP', [('mkdir', 'sub'), ('mkdir', 'sub2'),
               ('cp', '/tmp/ds/utm.tif', 'sub/utm.tif')],
     ['raster', 'convert', 'sub/utm.tif', 'sub2/out.vrt']),
    ('SETUP', [CP_UTM, ('mkdir', 'sub2')],
     ['raster', 'convert', 'utm.tif', 'sub2/out.vrt']),
    ('SETUP', [CP_UTM], ['raster', 'convert', './utm.tif', 'out.vrt']),
]

# worldfile / PAM sidecar inputs
CASES += [
    ['raster', 'convert', '/tmp/dz/wf.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/wf.tif', 'out.vrt'],
    ['raster', 'convert', '/tmp/dz/wfa.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/pw.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/kw.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/ka.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/kk.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/md2.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/cd1b.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/dz/p2gen.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/ds/p2.tif', 'out.tif'],
    ['raster', 'convert', '/tmp/ds/p2.tif', 'out.vrt'],
]

# vsi inputs
CASES += [
    ['raster', 'convert', '/vsizip//tmp/dz/a1/z.zip/utm.tif', 'out.tif']
    if os.path.exists('/tmp/dz/a1/z.zip') else
    ['raster', 'convert', '/tmp/ds/utm.tif', 'out.tif'],
]

# top-level `gdal convert` alias
CASES += [
    ['convert', '/tmp/ds/utm.tif', 'out.tif'],
    ['convert', '/tmp/ds/utm.tif', 'out.vrt'],
    ['convert', '/tmp/ds/utm.tif', 'out.gdalg.json'],
    ['convert', '--of', 'BANANA', '/tmp/ds/utm.tif', 'out.tif'],
    ['convert', 'missing.tif', 'out.tif'],
    ('SETUP', [('touch', 'out.tif')],
     ['convert', '/tmp/ds/utm.tif', 'out.tif']),
]


def run_case(exe, case, d):
    if isinstance(case, tuple):
        ops, cmd = case[1], case[2]
        for op in ops:
            if op[0] == 'cp':
                shutil.copy(op[1], os.path.join(d, op[2]))
            elif op[0] == 'touch':
                open(os.path.join(d, op[1]), 'w').close()
            elif op[0] == 'mkdir':
                os.makedirs(os.path.join(d, op[1]), exist_ok=True)
            elif op[0] == 'run':
                subprocess.run([exe] + op[1], cwd=d, capture_output=True)
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
    only = [int(a) for a in sys.argv[1:]]
    failed = 0
    for i, case in enumerate(CASES):
        if only and i not in only:
            continue
        da = tempfile.mkdtemp(prefix='cnv_a_')
        db = tempfile.mkdtemp(prefix='cnv_b_')
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
