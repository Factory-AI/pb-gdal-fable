import math
import struct
import sys

sys.path.insert(0, '/workspace/tests')
from mk_tail_fixtures import write_tif


def _rgb(w, h, fn):
    r, g, b = [], [], []
    for y in range(h):
        for x in range(w):
            pr, pg, pb = fn(x, y)
            r.append(str(pr))
            g.append(str(pg))
            b.append(str(pb))
    return [r, g, b]


def clamp(v):
    return max(0, min(255, int(v)))


def write_pal_tif(out, w, h, idx, ct):
    data = bytes(idx)
    ifd_off = 8 + len(data)
    ents = []

    def add(t, ty, c, v):
        ents.append(struct.pack('<HHII', t, ty, c, v))

    nent = 9
    extra_off = ifd_off + 2 + 12 * nent + 4
    full = list(ct) + [(0, 0, 0)] * (256 - len(ct))
    ctv = b''
    for comp in range(3):
        for c in full:
            ctv += struct.pack('<H', c[comp] * 257)
    add(256, 3, 1, w)
    add(257, 3, 1, h)
    add(258, 3, 1, 8)
    add(259, 3, 1, 1)
    add(262, 3, 1, 3)
    add(273, 4, 1, 8)
    add(277, 3, 1, 1)
    add(279, 4, 1, len(data))
    add(320, 3, 256 * 3, extra_off)
    ifd = struct.pack('<H', nent) + b''.join(ents) + struct.pack('<I', 0)
    with open(out, 'wb') as f:
        f.write(struct.pack('<2sHI', b'II', 42, ifd_off) + data + ifd + ctv)


def make():
    write_tif('radial.tif', 'Byte', 48, 32, _rgb(48, 32, lambda x, y: (
        clamp(255 - 4 * math.hypot(x - 24, y - 16)),) * 3))
    write_tif('blobs.tif', 'Byte', 48, 32, _rgb(48, 32, lambda x, y: (
        clamp(255 - 5 * math.hypot(x - 12, y - 10)),
        clamp(255 - 5 * math.hypot(x - 36, y - 14)),
        clamp(255 - 5 * math.hypot(x - 22, y - 26)))))
    write_tif('check.tif', 'Byte', 40, 24, _rgb(40, 24, lambda x, y: (
        (255, 0, 0) if (x // 4 + y // 4) % 2 else (0, 0, 255))))
    write_tif('cgray.tif', 'Byte', 64, 1, _rgb(64, 1,
                                               lambda x, y: (x * 4,) * 3))
    write_tif('cflat.tif', 'Byte', 16, 2, _rgb(16, 2,
                                               lambda x, y: (128, 128, 128)))
    fewc = [(10, 200, 30), (200, 10, 30), (10, 30, 200)]
    write_tif('cfew.tif', 'Byte', 8, 1, _rgb(8, 1, lambda x, y: fewc[x % 3]))
    write_tif('cmany.tif', 'Byte', 20, 5, _rgb(20, 5, lambda x, y: (
        (x * 13) % 256, (y * 53 + 11) % 256, (x * 7 + y * 31) % 256)))
    write_tif('crgb2.tif', 'Byte', 6, 2,
              [[str((i * 40) % 256) for i in range(12)],
               [str((i * 21 + 5) % 256) for i in range(12)]])
    write_tif('cf32.tif', 'Float32', 4, 2,
              [[str(v) for v in range(8)]] * 3)

    write_tif('rgbnd.tif', 'Byte', 12, 8, _rgb(12, 8, lambda x, y: (
        (7, 7, 7) if (x + y) % 5 == 0 else
        (clamp(20 * x), clamp(30 * y), clamp(10 * (x + y))))), nodata='7')
    write_tif('graynd.tif', 'Byte', 10, 2,
              [[str((i * 9) % 256 if i % 4 else 33) for i in range(20)]],
              nodata='33')
    write_tif('rgbplain.tif', 'Byte', 6, 4, _rgb(6, 4, lambda x, y: (
        x * 40, y * 60, (x + y) * 20)))

    write_pal_tif('pal4.tif', 8, 3,
                  [(x + y) % 4 for y in range(3) for x in range(8)],
                  [(0, 0, 0), (255, 0, 0), (0, 255, 0), (10, 20, 30)])

    with open('bwmap.txt', 'w') as f:
        f.write('0 0 0 0 255\n1 255 255 255 255\n')
    with open('map4.txt', 'w') as f:
        f.write('0 10 20 30 255\n1 200 100 50 255\n'
                '2 5 250 125 255\n3 33 66 99 255\n')
    with open('rampmap.txt', 'w') as f:
        for i in range(16):
            f.write('%d %d %d %d 255\n' % (i * 17, i * 16, 255 - i * 16,
                                           (i * 40) % 256))
    with open('badmap.txt', 'w') as f:
        f.write('hello world\n')
