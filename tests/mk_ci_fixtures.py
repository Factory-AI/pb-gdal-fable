"""Color-interpretation edit fixtures: a plain 3-band tif plus hand-built
minisblack files whose ExtraSamples tag is missing or too short."""
import struct


def _custom(name, extravals):
    w = h = 4
    spp = 3
    pix = bytes((x * 5 + y * 11 + b) % 256
                for y in range(h) for x in range(w) for b in range(spp))
    tags = [(256, 3, 1, w), (257, 3, 1, h), (258, 3, 3, None),
            (259, 3, 1, 1), (262, 3, 1, 1), (273, 4, 1, None),
            (277, 3, 1, spp), (278, 3, 1, h), (279, 4, 1, len(pix)),
            (284, 3, 1, 1)]
    if extravals is not None:
        tags.append((338, 3, len(extravals), None))
    nt = len(tags)
    data_off = 8 + 2 + 12 * nt + 4
    bps_off = data_off
    strip_off = bps_off + 6
    out = bytearray(b'II*\x00' + struct.pack('<I', 8))
    out += struct.pack('<H', nt)
    for t, typ, cnt, val in tags:
        if t == 258:
            val = bps_off
        elif t == 273:
            val = strip_off
        elif t == 338:
            val = 0
            for i, v in enumerate(extravals):
                val |= v << (16 * i)
        out += struct.pack('<HHII', t, typ, cnt, val)
    out += struct.pack('<I', 0)
    out += struct.pack('<HHH', 8, 8, 8)
    out += pix
    open(name, 'wb').write(bytes(out))


def make():
    import sys
    sys.path.insert(0, '/workspace/tests')
    from mk_tail_fixtures import write_tif
    vals = [[str(i) for i in range(256)],
            [str((i * 3) % 256) for i in range(256)],
            [str((i * 7) % 256) for i in range(256)]]
    write_tif('rgb.tif', 'Byte', 16, 16, vals, gt=(0, 1, 0, 0, 0, -1))
    _custom('no338.tif', None)
    _custom('w1.tif', [0])
    _custom('w2.tif', [2])
