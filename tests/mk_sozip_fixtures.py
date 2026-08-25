"""Input files and foreign-zip fixtures for the vsi sozip cases."""

import os
import random
import struct
import zipfile


def write_inputs():
    random.seed(42)
    open('a.txt', 'w').write('hello world\n')
    open('b.txt', 'w').write('second\n')
    open('empty.bin', 'wb').write(b'')
    open('big.bin', 'wb').write(
        bytes(random.getrandbits(8) for _ in range(100000)) * 15)
    open('mb.bin', 'wb').write(b'\xab' * 1048576)
    open('mb2.bin', 'wb').write(b'\xab' * 1048577)
    os.makedirs('sub/deep', exist_ok=True)
    open('sub/f1.txt', 'w').write('alpha\n')
    open('sub/deep/f2.txt', 'w').write('beta\n')
    os.makedirs('nest/n2', exist_ok=True)
    open('nest/n2/deep.bin', 'wb').write(
        bytes(random.getrandbits(8) for _ in range(100000)) * 15)
    open('garbage.zip', 'wb').write(b'not a zip at all')
    open('bare.zip', 'wb').write(b'PK\x05\x06' + b'\x00' * 18)
    for f, t in [('a.txt', 1609556645), ('b.txt', 1609556645),
                 ('empty.bin', 1609556645), ('big.bin', 1646370367),
                 ('mb.bin', 1609556645), ('mb2.bin', 1609556645),
                 ('sub/f1.txt', 1609556645),
                 ('sub/deep/f2.txt', 1609556645),
                 ('nest/n2/deep.bin', 1646370367),
                 ('garbage.zip', 1623053350), ('bare.zip', 1623053350)]:
        os.utime(f, (t, t))


def write_foreign():
    z = zipfile.ZipFile('py.zip', 'w')
    zi = zipfile.ZipInfo('dir/', (2020, 5, 6, 7, 8, 10))
    zi.external_attr = 0o40755 << 16
    z.writestr(zi, '')
    zi = zipfile.ZipInfo('dir/stored.txt', (2020, 5, 6, 7, 8, 10))
    z.writestr(zi, 'stored content', compress_type=zipfile.ZIP_STORED)
    zi = zipfile.ZipInfo('zz_first.txt', (2019, 1, 1, 0, 0, 0))
    z.writestr(zi, 'x' * 100, compress_type=zipfile.ZIP_DEFLATED)
    zi = zipfile.ZipInfo('averyveryverylongfilename_exceeding_column.txt',
                         (2019, 1, 1, 0, 0, 0))
    z.writestr(zi, 'y', compress_type=zipfile.ZIP_DEFLATED)
    z.close()
    z = zipfile.ZipFile('self.zip', 'w')
    z.writestr(zipfile.ZipInfo('self.zip', (2019, 1, 1, 0, 0, 0)), 'inner')
    z.close()
    z = zipfile.ZipFile('old.zip', 'w')
    z.writestr(zipfile.ZipInfo('t.txt', (1980, 1, 1, 0, 0, 0)), 'tiny')
    z.close()
    for f in ['py.zip', 'self.zip', 'old.zip']:
        os.utime(f, (1623053350, 1623053350))


def corrupt(src):
    d = bytearray(open(src, 'rb').read())
    d[100] ^= 0xff
    open('corrupt1.zip', 'wb').write(bytes(d))
    d = bytearray(open(src, 'rb').read())
    j = bytes(d).find(b'.big.bin.sozip.idx')
    lh = bytes(d).rfind(b'PK\x03\x04', 0, j)
    nl, el = struct.unpack('<HH', bytes(d[lh + 26:lh + 30]))
    off = lh + 30 + nl + el
    d[off + 32] ^= 0x01
    open('corrupt2.zip', 'wb').write(bytes(d))
    os.utime('corrupt1.zip', (1623053350, 1623053350))
    os.utime('corrupt2.zip', (1623053350, 1623053350))
