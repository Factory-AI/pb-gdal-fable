#!/usr/bin/env python3
"""Fixture set for the pipeline/leaf edit calibration suite.

usage: mk_pipeedit_fixtures.py [outdir]

Reuses the standalone GTiff writer from mk_tail_fixtures. pe2.tif mirrors
the probing fixture: 2-band Int16, UTM 32611, nodata 7, band-2 scale 2.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif, gmd_scale


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(d, exist_ok=True)
    j = os.path.join
    utm = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
    gt = [0.0, 10.0, 0.0, 20.0, 0.0, -10.0]
    write_tif(j(d, 'pe2.tif'), 'Int16', 3, 2,
              [[-300, 5, 7, 32767, -32768, 0],
               [1, 2, 3, 4, 5, 6]],
              gt=gt, geokeys=utm, nodata='7',
              gmd=gmd_scale([(1, '0', '2')]))
    write_tif(j(d, 'pe3.tif'), 'Byte', 3, 2,
              [[0, 1, 5, 254, 255, 128],
               [9, 8, 7, 6, 5, 4],
               [1, 1, 2, 2, 3, 3]],
              gt=gt, geokeys=utm)
    write_tif(j(d, 'pe1.tif'), 'Byte', 4, 2,
              [[0, 1, 5, 254, 255, 128, 64, 32]], gt=gt, geokeys=utm)
    with open(j(d, 'gcps.txt'), 'w') as f:
        f.write('1,2,3,4\n5,6,7,8,9\n')
    with open(j(d, 'gcps_bad.txt'), 'w') as f:
        f.write('1,2,3\n')


if __name__ == '__main__':
    main()
