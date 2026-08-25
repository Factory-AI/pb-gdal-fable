#!/usr/bin/env python3
"""Fixtures for the BigTIFF suite: classic sources of varying strip
geometry (single strip, multi strip, wide strips)."""
import random
import struct
import sys


def write_tif(path, w, h, vals):
    raw = bytes(vals)
    entries = [
        (256, 3, 1, struct.pack('<HH', w, 0)),
        (257, 3, 1, struct.pack('<HH', h, 0)),
        (258, 3, 1, struct.pack('<HH', 8, 0)),
        (259, 3, 1, struct.pack('<HH', 1, 0)),
        (262, 3, 1, struct.pack('<HH', 1, 0)),
        (273, 4, 1, struct.pack('<I', 8)),
        (277, 3, 1, struct.pack('<HH', 1, 0)),
        (278, 3, 1, struct.pack('<HH', h, 0)),
        (279, 4, 1, struct.pack('<I', len(raw))),
    ]
    body = b''.join(struct.pack('<HHI4s', t, ty, c, v)
                    for t, ty, c, v in entries)
    ifd = struct.pack('<H', len(entries)) + body + struct.pack('<I', 0)
    with open(path, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, 8 + len(raw)) + raw + ifd)


def main(outdir):
    random.seed(9)
    for name, w, h in (('s1.tif', 64, 48), ('s2.tif', 100, 300),
                       ('s3.tif', 3000, 300)):
        write_tif(outdir + '/' + name, w, h,
                  [random.randrange(256) for _ in range(w * h)])


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
