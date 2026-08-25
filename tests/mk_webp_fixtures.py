#!/usr/bin/env python3
"""Fixtures for cases_webp.txt: procedural uncompressed sources plus
pre-baked WEBP-in-TIFF fixtures (verified byte-identical to reference
convert/overview output at bake time; used here only as read inputs).
wpl.tif is a band-separate WEBP whose non-first planes carry the
reference's uninitialized tail bytes; wcor.tif has a corrupted chunk."""
import base64
import struct
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else '.'


def mktif(path, w, h, spp, phot, bps=8, extras=None):
    n = w * h * spp
    if bps == 8:
        data = bytes(((i * 37) % 251) for i in range(n))
    else:
        data = b''.join(struct.pack('<H', (i * 373) % 60001)
                        for i in range(n))
    entries = [(256, 3, 1, struct.pack('<HH', w, 0)),
               (257, 3, 1, struct.pack('<HH', h, 0)),
               (258, 3, spp,
                None if spp > 1 else struct.pack('<HH', bps, 0)),
               (259, 3, 1, struct.pack('<HH', 1, 0)),
               (262, 3, 1, struct.pack('<HH', phot, 0)),
               (273, 4, 1, struct.pack('<I', 8)),
               (277, 3, 1, struct.pack('<HH', spp, 0)),
               (278, 3, 1, struct.pack('<HH', h, 0)),
               (279, 4, 1, struct.pack('<I', len(data)))]
    if extras:
        entries.append((338, 3, len(extras),
                        struct.pack('<HH', extras[0], 0)
                        if len(extras) == 1 else None))
    entries.append((284, 3, 1, struct.pack('<HH', 1, 0)))
    entries.append((339, 3, spp,
                    None if spp > 1 else struct.pack('<HH', 1, 0)))
    entries.sort()
    ifd_off = 8 + len(data)
    nn = len(entries)
    tail_off = ifd_off + 2 + 12 * nn + 4
    body = b''
    tail = b''
    for t, ty, c, v in entries:
        if v is None:
            if t == 258:
                arr = struct.pack('<%dH' % c, *([bps] * c))
            elif t == 338:
                arr = struct.pack('<%dH' % c, *extras)
            else:
                arr = struct.pack('<%dH' % c, *([1] * c))
            body += struct.pack('<HHI', t, ty, c)
            if len(arr) <= 4:
                body += arr + b'\0' * (4 - len(arr))
            else:
                body += struct.pack('<I', tail_off + len(tail))
                tail += arr
        else:
            body += struct.pack('<HHI', t, ty, c) + v
    out = (struct.pack('<2sHI', b'II', 42, ifd_off) + data +
           struct.pack('<H', nn) + body + struct.pack('<I', 0) + tail)
    with open(path, 'wb') as fp:
        fp.write(out)


mktif(OUT + '/src_gray.tif', 37, 29, 1, 1)
mktif(OUT + '/src_rgb.tif', 37, 29, 3, 2)
mktif(OUT + '/src_rgba.tif', 37, 29, 4, 2, extras=[2])
mktif(OUT + '/src_2b.tif', 8, 8, 2, 1, extras=[0])
mktif(OUT + '/src_5b.tif', 8, 8, 5, 1, extras=[0, 0, 0, 0])
mktif(OUT + '/src_u16.tif', 8, 8, 3, 2, bps=16)
mktif(OUT + '/src_big.tif', 260, 24, 3, 2)

BLOBS = {
    'srcn7.tif':
    'SUkqAAgAAAALAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwADAAAAkgAAAAMBAwAB'
    'AAAAAQAAAAYBAwABAAAAAgAAABEBBAABAAAAngAAABUBAwABAAAAAwAAABYBAwABAAAA'
    'HQAAABcBBAABAAAAGgsAABwBAwABAAAAAQAAAFMBAwADAAAAmAAAAAAAAAAHAAcABwAB'
    'AAEAAQAAllb///+IW0u////INbX////mHuL////0EXV///waiby///4thV9f//8m4u//'
    '//+bgZf////RyNv///DK6HX///jmdj7///yzvB////56Ho///4FNL4f//8Sup+P//+Rb'
    'ALf////mX2P////0MbX///wimdz///4xjW9///8o5vf///+cg5v////SSd3///DrKPb/'
    '//j2ln9///y7zD////5+Jp///4JPM4///8Uvqef//+Sb3P////NP8n////oo+z+A//wq'
    'qf0///41lX+f//8q6v////+dhZ///+ASyt////ELaXf///kGtr////zD3F////6CLq//'
    '/4NRN5f//8Wwq+v//+TcXf////NwMv////o5G3///hldDr///xzOx9///4As7wf///+e'
    'h6P//+BTS+H///Erqfj///kW1v////zL7H////6GNr///4RTO5///8Yxre///+Uc3v//'
    '//OQc3////pJO7///h1lHt///x7Sz+///5d5h////8/E0///8EnmAOP///FL6nn///km'
    '9z////zT/J////6KPs///4VVP6f//8ayr/P//+VdX/////Ows////AJZW////iFtLv//'
    '/yDW1////5h7i////9BF1f//8Gom8v//+LYVfX///JuLv/+A//zcDL////6ORt///4ZX'
    'Q6///8czsff//+Wd4P////PQ9H///AppfD///iV1Px///yLa3////5l9j////9DG1///'
    '8Ipnc///+MY1vf///KOb3////nIOb////0knd///w4BZR7f//8e0s/v//+XeYf////Px'
    'NP///BJ5nH///il9Tz///yTe5////5p/k////9FH2f//8Kqn9P//+NZV/n///Kur////'
    '/nYWf///gEsrf///xC2l3///5Bra////8w9xAP////QRdX///BqJvL///i2FX1///ybi'
    '7////5uBl////9HI2///8Mrodf//+OZ2Pv///LO8H////noej///gU0vh///xK6n4///'
    '5Ftb////8y+x////+hja///+EUzuf/+A/jGNb3///yjm9////5yDm////9JJ3f//8Oso'
    '9v//+PaWf3///LvMP////n4mn///gk8zj///xS+p5///5Jvc////80/yf///+ij7P//+'
    'FVT+n///Gsq/z///lXV/////zoCFn///4BLK3///8Qtpd///+Qa2v////MPcX////oIu'
    'r///g1E3l///xbCr6///5Nxd////83Ay////+jkbf//+GV0Ov///HM7H3///lneD////'
    'z0PR///wKaXw///4ldT8AP//+RbW/////Mvsf////oY2v///hFM7n///xjGt7///5Rze'
    '////85Bzf///+kk7v//+HWUe3///HtLP7///l3mH////z8TT///wSeZx///4pfU8///8'
    'k3uf///+af5P//+A/oo+z///hVU/p///xrKv8///5V1f////87Cz///8Allb///+IW0u'
    '////INbX////mHuL////0EXV///waiby///4thV9f//8m4u////+bgZf////RyNv///D'
    'K6HX///jmYCx9///5Z3g////89D0f//8Cml8P//+JXU/H///Itrf////mX2P////0MbX'
    '///wimdz///4xjW9///8o5vf///+cg5v////SSd3///DrKPb///j2ln9///y7zD////5'
    '+Jp/gP/8Enmcf//+KX1PP///JN7n////mn+T////0UfZ///wqqf0///41lX+f//8q6v/'
    '///+dhZ///+ASyt////ELaXf///kGtr////zD3F////6CLq///4NRN5f//8Wwq+v//+A'
    'JuLv////m4GX////0cjb///wyuh1///45nY+///8s7wf///+eh6P//+BTS+H///Erqfj'
    '///kW1v////zL7H////6GNr///4RTO5///8Yxre///+Uc3v////OQc3////pJIDd///w'
    '6yj2///49pZ/f//8u8w////+fiaf//+CTzOP///FL6nn///km9z////zT/J////6KPs/'
    '//4VVP6f//8ayr/P//+VdX/////Ows////AJZW////iFtLv///yDW1//gP/8w9xf///+'
    'gi6v//+DUTeX///FsKvr///k3F3////zcDL////6ORt///4ZXQ6///8czsff//+Wd4P/'
    '///PQ9H///AppfD///iV1Px///yLa3////5l9j////9DG1///8IAUzuf///GMa3v///l'
    'HN7////zkHN////6STu///4dZR7f//8e0s/v//+XeYf////PxNP///BJ5nH///il9Tz/'
    '//yTe5////5p/k////9FH2f//8Kqn9P//+NZV/n///Kur4D////zsLP///wCWVv///4h'
    'bS7///8g1tf///+Ye4v////QRdX///BqJvL///i2FX1///ybi7////5uBl////9HI2//'
    '/8Mrodf//+OZ2Pv///LO8H////noej///gU0vh//gP4ldT8f//8i2t////+ZfY/////Q'
    'xtf///CKZ3P///jGNb3///yjm9////5yDm////9JJ3f//8Oso9v//+PaWf3///LvMP//'
    '//n4mn///gk8zj///xS+p5///5Jvc////80Af5P////RR9n///Cqp/T///jWVf5///yr'
    'q/////52Fn///4BLK3///8Qtpd///+Qa2v////MPcX////oIur///g1E3l///xbCr6//'
    '/5Nxd////83Ay////+jkbf//+GV0OoD///jmdj7///yzvB////56Ho///4FNL4f//8Su'
    'p+P//+RbW/////Mvsf////oY2v///hFM7n///xjGt7///5Rze////85Bzf///+kk7v//'
    '+HWUe3///HtLP7///l3mH///gP5+Jp///4JPM4///8Uvqef//+Sb3P////NP8n////oo'
    '+z///hVU/p///xrKv8///5V1f////87Cz///8Allb///+IW0u////INbX////mHuL///'
    '/0EXV///waiby///4tgAq+v//+TcXf////NwMv////o5G3///hldDr///xzOx9///5Z3'
    'g////89D0f//8Cml8P//+JXU/H///Itrf////mX2P////0MbX///wimdz///4xjW9///'
    '8o5vf///+cg5v4D///pJO7///h1lHt///x7Sz+///5d5h////8/E0///8Enmcf//+KX1'
    'PP///JN7n////mn+T////0UfZ///wqqf0///41lX+f//8q6v////+dhZ///+ASyt////'
    'ELaXf///gCDW1////5h7i////9BF1f//8Gom8v//+LYVfX///JuLv////m4GX////0cj'
    'b///wyuh1///45nY+///8s7wf///+eh6P//+BTS+H///Erqfj///kW1v////zL7H////'
    '6GMA1///8Ipnc///+MY1vf///KOb3////nIOb////0knd///w6yj2///49pZ/f//8u8w'
    '////+fiaf//+CTzOP///FL6nn///km9z////zT/J////6KPs///4VVP6f//8ayr/P4D/'
    '/Kur/////nYWf///gEsrf///xC2l3///5Bra////8w9xf///+gi6v//+DUTeX///FsKv'
    'r///k3F3////zcDL////6ORt///4ZXQ6///8czsff//+Wd4P////PQ9H///AgE0vh///'
    'xK6n4///5Ftb////8y+x////+hja///+EUzuf///GMa3v///lHN7////zkHN////6STu'
    '///4dZR7f//8e0s/v//+XeYf////PxNP///BJ5nH///il9Tz///yTe4A',
    'w75.tif':
    'SUkqAAgAAAAMAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwADAAAAngAAAAMBAwAB'
    'AAAAUcMAAAYBAwABAAAAAgAAABEBBAABAAAABwEAABUBAwABAAAAAwAAABYBAwABAAAA'
    'HQAAABcBBAABAAAAvAMAABwBAwABAAAAAQAAAFMBAwADAAAApAAAAICkAgBdAAAAqgAA'
    'AAAAAAAIAAgACAABAAEAAQA8R0RBTE1ldGFkYXRhPgogIDxJdGVtIG5hbWU9IldFQlBf'
    'TEVWRUwiIGRvbWFpbj0iSU1BR0VfU1RSVUNUVVJFIj43NTwvSXRlbT4KPC9HREFMTWV0'
    'YWRhdGE+CgBSSUZGtAMAAFdFQlBWUDggqAMAAJASAJ0BKiUAHQA+gTKTR6UjIaE1SACg'
    'EAlsAJ0yhBAXr2OHsCQI/zv/AewDbAeYD9a/0l95f0AbwBz3n7kfAV+33pXVgJkFxhZA'
    'nSfYEGBR7X7J/kvEG+Gf2nzNeXl/If1j9x/6T7CPmb/qf3T4DP5n/TfQa9QH6AewP+lp'
    'BhTTvZDa5yvOfRu1pCxqKJ4as+zmi6aJTLyiS7ZGUQAA/YOqRKu/yUjVUxlR2Uwl0srh'
    'fPsQN9e6U19EESJ2Tm0raFsvuaY/v15y+dr30I8Br+IHIfOPII0O76OfFUROAxwXgw9I'
    'OqfCa1umJT8Mo6pm3KGMZJhJLNQCd1J+5wZ8LaE5f38eRLXMQYm8c5F3OAxEyspsEZsq'
    'GceuI+g+AvEMS5skg9CD4RnbZBHNJLfdXVpXWhWG/HGq+mUE1oo3j9GXZQ1G/W0qN7Ho'
    'mnH5McosioF1MISHwLpYKAyc8TkTRL0TrwhQDE/fuIHd7imYsUpLUB3X7nr2yEjM+nHf'
    'aEJ/WRE+g5Ivusn3UdTGZQBMX3XYNwFnL9TUG4qv47TFs6USorpv18oIo2O1CWpsEBEa'
    'rjiEHvnfiygQmXpBqnL3/GL2ePApf+gtFLDbb8r8Az5vD8TDXN+c94d6+to/Kf9Dcb2p'
    '8LOsyBd9xq6m1KT11f2119C+UQOUQX05iwOZSm0l1L8cgj3eXpnRD1AtAhXl5RNebbc8'
    'YrCKg2rk7C7NIqigmaYT+w+chskmv9E0wObtVRnnsY2lCIwOC+5dDWC13RlclOpnVYEg'
    'Ou8RlgPrK/joyo7KYS4O2EsQOw7l/VdclTkFLGvcQtnt9fO0Wi+AYqTSYacjWY/Jk9Zt'
    'ObQQ9CB9U4DP+Wz0RJBL4jhxO3B5DR97aV5iZM9MN/6EuN5WRf70wr3QGcQ4f8RzX/wr'
    'REhp5qgraLEAiX0V91xPFPgloTW8zns2f+4cKDwJZcB2fPX9P+fSkAJHC5AAHUabDYMs'
    'tCwvDZLgMLlfP5eDtQ08lncmbGJm2n8ZgXYsM/b69/Wgl9sshNn7hi/dj85tS0XMyWpx'
    'c0n+RqZnPJxJIufMsVsbLfNBrF9eZ8NyddlebUHZ/p5dwbmM7FVUTKXTtyr/7ac/vaZ0'
    'yLwt2oaWtbKH+86N1Oc6usiLHnUu4xW8Klpr4UX/jIGaARvs//nwv/9oOJgiB0lHnK/I'
    'MrilnqDBUEY/7bGtxyhA8n2/+CHnO4ZmH61qsL6YJZ5aZn0Zgex1xczvQAAAAA==',
    'w20.tif':
    'SUkqAAgAAAAMAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwADAAAAngAAAAMBAwAB'
    'AAAAUcMAAAYBAwABAAAAAgAAABEBBAABAAAABwEAABUBAwABAAAAAwAAABYBAwABAAAA'
    'HQAAABcBBAABAAAATAIAABwBAwABAAAAAQAAAFMBAwADAAAApAAAAICkAgBdAAAAqgAA'
    'AAAAAAAIAAgACAABAAEAAQA8R0RBTE1ldGFkYXRhPgogIDxJdGVtIG5hbWU9IldFQlBf'
    'TEVWRUwiIGRvbWFpbj0iSU1BR0VfU1RSVUNUVVJFIj4yMDwvSXRlbT4KPC9HREFMTWV0'
    'YWRhdGE+CgBSSUZGRAIAAFdFQlBWUDggOAIAALAMAJ0BKiUAHQA/JXixU64nJKK1SAHA'
    'JIlsAJ07/8+f2mq+ov84gWn9G9QGYk+vx6AP//kX3qtT46x+tjtHbdqjkzAv72dkyBDM'
    'v+t/a/MLQNepz9XfRAHUQQtZ4YAAfHdrY+AfYp9A6TXiMhMAAOgo/WvRMZSwd/5WIxrI'
    'oBfNkcqiifs++RMTV/lG+D9wu6OhtZa4QyWZnJQkDL8CyyZqZwDvUDgJIqn/rTFsbnQX'
    'XTjvTSybwOMrsIrcy8fYvOBA5auDv+Ut9wzbj91I+6seAkbpu8qyXnSs2N+hC7zJNrt/'
    '3whoW88bemVBEzipwGW5o3l0/zALAEy8RwhLWf6/opTb60u4fN8zMYaxnPDpSH7exLxs'
    'i3vjBLc0pjMHzzvWWJx8CDqvPUqmqyqBGKFm8XXMs5/+Y42GEf3H6YVbBTht3lyFkFXS'
    'p/SPP04DwGVf/2WyB0sqfIrfFGrQVIT8NZJEOBNZCW1EUC/4i0H8dyEnur9uhBQ4CzRE'
    'X92E7q997V5HrvgvgmtF+hW60RpntDdGRrOoozlcgr/Kha9TNrlt6VyPlCT4lTTC58Gw'
    'rVbm1o8s2XorkVv3iZOR87OxPQB96LbnQThIhIT4n6YBOyrb+w5m1Giot6M91vLV6gv1'
    'l2DF/hATJFopTPYiFQfvk7IwkLAO3aNUXytjwE8kBtvI1Nu9bGhhrYs//oZ/rANKPBQ3'
    'ceSIYT04y5nnvi3AIgG38G/MMPOOvPoTu7Dn8OW5MzzTIAA=',
    'wll.tif':
    'SUkqAAgAAAAMAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwADAAAAngAAAAMBAwAB'
    'AAAAUcMAAAYBAwABAAAAAgAAABEBBAABAAAAHAEAABUBAwABAAAAAwAAABYBAwABAAAA'
    'HQAAABcBBAABAAAA7AAAABwBAwABAAAAAQAAAFMBAwADAAAApAAAAICkAgByAAAAqgAA'
    'AAAAAAAIAAgACAABAAEAAQA8R0RBTE1ldGFkYXRhPgogIDxJdGVtIG5hbWU9IkNPTVBS'
    'RVNTSU9OX1JFVkVSU0lCSUxJVFkiIGRvbWFpbj0iSU1BR0VfU1RSVUNUVVJFIj5MT1NT'
    'TEVTUzwvSXRlbT4KPC9HREFMTWV0YWRhdGE+CgBSSUZG5AAAAFdFQlBWUDhM2AAAAC8k'
    'AAcAzSgEEAAFTUT/4zcR0f+Aq0iynUoNqvbnOCB65oVVQdo5KCDStimZf4GXckKkbVMy'
    'Kfcv8CQUuW3bIMd0CIJeMWwj240ZtfnIxvHSA8CozSOA2MvYzWUuAD5zTATYy0jGXAFu'
    'A1nLE+tlfPGqEfRtE1glY2+9fbtabnpv366KuIGsdVR5uqpE5ZBV5A1jrYvKIasClcer'
    'UkP5cw5MedYnoPJYm0QKbeTQLUf0lOMcqD4FKM+sOQdeTl1L+VMOrxRqxCuFmoIVinPo'
    'liP1lBuaA9RnAA==',
    'wrgba.tif':
    'SUkqAAgAAAANAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwAEAAAAqgAAAAMBAwAB'
    'AAAAUcMAAAYBAwABAAAAAgAAABEBBAABAAAAFwEAABUBAwABAAAABAAAABYBAwABAAAA'
    'HQAAABcBBAABAAAA2gMAABwBAwABAAAAAQAAAFIBAwABAAAAAgAAAFMBAwAEAAAAsgAA'
    'AICkAgBdAAAAugAAAAAAAAAIAAgACAAIAAEAAQABAAEAPEdEQUxNZXRhZGF0YT4KICA8'
    'SXRlbSBuYW1lPSJXRUJQX0xFVkVMIiBkb21haW49IklNQUdFX1NUUlVDVFVSRSI+NzU8'
    'L0l0ZW0+CjwvR0RBTE1ldGFkYXRhPgoAUklGRtIDAABXRUJQVlA4WAoAAAAQAAAAJAAA'
    'HAAAQUxQSEAAAAABmQpE9D+gqJEkRfKaORTFmnjxGzEBCty2UcZwkHfAzFni9dyNZiqm'
    'RYuHczggBRqvTQUar00FGq9NBRqvzsUEVlA4IGwDAABQEgCdASolAB0APoEylEelIyIh'
    'NUgAoBAJbACdMoSylUBhB7Aj93oA2xviAf27+Ae/F6AP+vvgHPtf6r/AfB//hfNsrCPI'
    '+AFyOftf4S+YNzJ2BMIHMBxDfxDwBvhv8n/WP8QPoX/sf+89LXMn+Qf3D/s+4b/I/5//'
    'n+uf6OX7EDCjONcDu5EPAww8KvnegOZ87JvzR2AvMt5JQADdneBjTT6FxN1gy9J1D7mB'
    'LGqpNjCr2+5l5WtHRaG7ibRqqW+72jKQHVZ9syxcL95zj5WA41ZtgKz+nGcBk2ENS/3j'
    'WEa5/O8wCdADCuPUWSxTOWYyUIEYrkZPr2pH3z4MGHzljpNeG9VeEMp6uqoGZDMzMoj4'
    'G6MoMuTytyNN53aaB1jnbzDdE9JbnWysGULE6RC/VIWC8wEfZpy0cwoS/hmX1mcWrwDN'
    'i5ZK5nLMSkesL9Yd3QItHAhr0GHtSe9Nc5LzfpOR32bx1OY7WmrIPV0VdAV31o8Ejcsd'
    'RmfzZfpLUng3ZDH9FRKSzJebNcftUeSfBEq7aniwodNIJOYFavpzhbeH1mueDVqV763Y'
    'f8YkJx/DuRwxY4s/X07tJmIRNA36hmxo9Mf/n/98v2hymsdXuJ1g6iit3ByCouP7psvn'
    'l7f7tO/+LQk/7ASSzLWqN47uclb3Rz6I1oR87Z2J/8zugJfdYnkKb/3xK1GPuJ/NBJ47'
    'Oakvq1ER4oOu3DLp6GBvdV+iAH+AB+IW1qUYf6mfHfKfnOPaN8QFBeJRWk2SBiehWw/6'
    'BURwmEBQGMYr2ODByidcxJChjtGbd7p6/F2+QxVc/+48VWLNGafuJ23XsRX7+NqfeZTb'
    'TG9fUNc8QEp5gdPRiWyB/f+vGOK3XiApMSdHmVyNza1zhaIlGWbu/1sW/DRh6ZyU8lSG'
    '75uThVSu1G+gFc3381hyzqkfJrMBY6USMbwy+Xz2xU1DASGrt24wVlU78I8GXvfTJODr'
    'KHt3ba4bK0vzY/30GrrV8yX3tDk2Qj6NwGUoq4nnD1AmQWhnxyymtZTyXneSww2Cv3cB'
    'Xe+as77oLhpNjP12D50I+qvwDbKhn1WoM8wsqDP8ao0H/xFPKedi+m/+H5AmeQOSfVee'
    'Ek6fJ7H/ejY1vM0aYmpb/zaROXJfGJNqoZiwcp3AFge8Hg8Ku5EeAAA=',
    'wtl.tif':
    'SUkqAAgAAAANAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwADAAAAqgAAAAMBAwAB'
    'AAAAUcMAAAYBAwABAAAAAgAAABUBAwABAAAAAwAAABwBAwABAAAAAQAAAEIBAwABAAAA'
    'AAEAAEMBAwABAAAAAAEAAEQBBAABAAAAEwEAAEUBBAABAAAA2AMAAFMBAwADAAAAsAAA'
    'AICkAgBdAAAAtgAAAAAAAAAIAAgACAABAAEAAQA8R0RBTE1ldGFkYXRhPgogIDxJdGVt'
    'IG5hbWU9IldFQlBfTEVWRUwiIGRvbWFpbj0iSU1BR0VfU1RSVUNUVVJFIj43NTwvSXRl'
    'bT4KPC9HREFMTWV0YWRhdGE+CgBSSUZG0AMAAFdFQlBWUDggxAMAAJAdAJ0BKgABAAE+'
    'kUihTKWkIyIg2qgAsBIJaW7hdfi51lV2J6ANsBz0/oA3gDn2PYF/dChTMsCgUAX9D7Ag'
    'wLtzzHf3zwBvh39y8zvqc/qv/A/w35R+wv599gD9XPRp9Rv6bewP+oQAj1ha9zBK88jn'
    'D6MLkXSyWlD8pknUQVCNcV9N4wRd6ehjXE9zIlgHupvo4qXtqHOMoB7qb6NqHOMoB7qb'
    '6NqHOMoB7qb6NqHOMoB7qb6NqHOMoB7qb6NqHOMoB7qb6NqHOMoB7qb6NqHOMoB7qb6N'
    'qHOMoB7qb6NqHOMoB7qb6NqHOMoB7qb6NqHOMncAAPtJi5BkQ1CB9545s1y+GdieqODG'
    'v/GCrefn+zAdW0NXo1jqCupTonX8TUIlH98BQD5j1JDPI06mq2dTO+SWqAayrOQcPQ9O'
    'kYIui89OGAWfY6lx1TsoZCxCpnuZ+zu7Kn86wETJsA3bDgEnAUwa/smP7JRMIBs6AHRT'
    'Rg2WLY58h4txmIpxOv5uHx2tZRdGQ+30BcfMQ0Gp/51Qjc37SuyJMb7N10J8XEi031ew'
    'zeilb/6LxERnckcd9AeoPhTQNvNQF2CeDSZOfmB804nG+MJhN9XUueTljlIUcCO3Y91W'
    '02U90GoY2nfCp3PKt+R2shVegkQk0FOHnx7NzXlz86pXavL7wuN5NAVri4cC2UJKRAta'
    '73JW6tLlkDZjf52p6OzKtliHyMNmhn8Zq1RVEYeEsGRgdRy6LIzvdYjM8kLvrXfCXtAY'
    'lkOp9oAmxUwt5CmL2aWVThwhE6AUhRCcf0RYRiHjCM8OXEwfz0wfz0wfz0wfz0wfz2ff'
    'Geefrv6zOKWDgsDTPWCd7POO6A9XseB4/DvceHlc20ZZdqtsRu/Fngry+Z1LVaFo5IeU'
    'uOiRqH2dTHZ/JrU1P7JtoIGnii8gpplJ+NH17aB0x3DkjUpXIYyR1PgttyHa2X8mWXxT'
    'q9tyXoKuHOF9rUZ9Hi5NNKkZcgTwaAIfXJQXQf8VN8vD7Tr3AkTN85a0ezLO87CPwMnx'
    '9sMSvyNWq8r54RNXblEK9P0RdB5KJDcecZFE0yDAqesvZD7pCAtYTBPGQy4e/nEHMApZ'
    '5GnXwG5/QVrJLe1zU0e3AI8LJVEcqT7kiovD/c4uXjgHuWEiTqyYLLIUqZJQv/wkR2bV'
    'Un/9GL48L257R+SzB30UP+Ua4jwBeyr4uo5xzw3JwyWEpsVBQvTKaLPA+poytztKpubX'
    'mDq6nPDtbZL3mf0SiLQAAAhfb8qDHYY5YAAAAAAAAAAAAAA=',
    'wbig.tif':
    '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',
    'wov.tif':
    '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',
    'wpl.tif':
    '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'
    'WDp8X/TiZwAdpp1l3bI88Dri1l/U8+21B3M6yXY2G5HbBS6gCzh9IfTIjVA5bpp3YFEG'
    '0tOLNnOieSWueCJrIgq0080uVXP1gp2Js2CLnI9Yr/yXm+7PkJUTZJ85obZZDUK2rqpy'
    'oAzbVkP/Y6U/+NgkhCpGZf/aXy6pyP7Jb3f9LcVawkOOv04FA+/uyn4mZI671YRs23d1'
    'S+FO1ReOfPDxbnXhsyCrdDf8/5ZpbzhnZvd8X2d5vOScKhV1sCNI/DLfh9QgQ5c6uKyD'
    'T3q2MRzP9IctU9TX7mJvcK+YtXGf+uvd6V5HbCDcFNVfpl5ZzbV0ytxzPVuIIrCmGw0C'
    'F/0cJDupVFajtTDbf+/ZuTIhm/vp4XQnD6oQier/Nuv8R9MhYs1BYyel2lZeGMcnMalw'
    'gnroDS19KFoNWw78QDhIG4tdoEUNCWnJ3GJeV8+HFDfeCyBz6tPvKaFm6UZQ/16SrSwa'
    '/RbE2ZiN1F2xMRDDelYNwoomnLnrCSgydeOLMInyjanC5nisMLKBZMu4LDquTQsUnsl0'
    'Ige+0wIXQ84R8obz5rnWDN/lXqsyyQCF+wySGnVTgWHYxx95b+j6uRIP1v++4PQ08/7P'
    '91Zy3cKrIS5ZDIk7+XSt7ZGrQzdmUabRnxtQWYDR0NbXJdTpf5B/oRP6hCt6WnH6g4AX'
    '4Fsg//oDamPA1Y9XvdeuiTu/+O+y8dwI5Hn7t1RqOCeI7m2CbwuPtlWjbwMKVHgpxeFw'
    '+uJI4tVnGht44DGqbMZ/2A6/r0uXbmB15pha8viSu+w2xv8uDUF3tvPatreR8jpv8iv0'
    'AAAAUklGRg4EAABXRUJQVlA4IAIEAACQFQCdASolAB0APoEyk0elIqGhNUgAoBAJbACd'
    'MoQQEaFeB1jPUBtgPMB+l/+O/s3vPegDeAP2q9gD9APSy/c74Ff7t/r/2i9n27O8R3J7'
    '4H9lbou/XfAH3B/u+Qv74n+J76T939ANJl679RD+if6r+depd/cf3LzQfMv/Y/ufwFfy'
    'z+mf8T83fg59Y364eyh+uZRg2loSXEBHGU4zex1FB1odH2POGfKeLq1SkAG7DKx4Wfgg'
    'AM4aMaG1Ecm1UFakCcx9c/oAbqgZPWbEhNNoeSQQ/54MQweUe+4Yru36+u7Pg8+R9KcV'
    '/MIz89wAFgyOvgSFXccjnuafRvZCg7VHu0LPosfnZ3XLkFtODhTZmaWCy+qdBbAdl325'
    'xI4P5gENDkNE/S1rMlCkZYxkpoZlFpp31HbIqbsv7HmGVmfCSgutAnbYanNo6CSRweM9'
    '9dxtUN63guPjiH5ilixQffsLEOIWuFo0Lo3c4+t34O/nhjzOeWgBPODrxcilTYtB0NvG'
    'VGVyN5nI0Np7y7Iqyd/MmJVrUfNhJ1exppt6DC9Cj1dv8thS6SX/5/tw/Yqg6MD209dV'
    'inFFvoS+Xmr3BQxtZWnNes5UtsH1i/c8g533DjP8pIpVPCHTLthNCgHyRhJ71Cp4NZKI'
    '8SRE3Us6fFCHQB/lUHWhcAhXKmTHbql97fk/Btst7DddS/dAyKmKg5nQVFMgYqlz/CHX'
    '+LcwikXeYf2Valn3bH8TMQxx/6xv+1PbJ68bi7hOyrxsJWHL+VlWyKqtw/mvCCEJYe/v'
    'Q/qdrDUf/FHMZQBBn4QP8I3GSXyd3Z4qN3Bo0Dp5WbiK07Yk6p7G3Sl+x1Uf51Ug88il'
    'oM4bBnCzC6t+dihzU3txDTITGcDD2jDQ9NHQpd6zuDz2JXLiuTOx/v69EB9gYjbQWYNe'
    'Oxm32/JEUCybGcj5YI9wTApxnbg9c4Ul+PMkCkvDVPWxOKDjLDRQu+khg0bZ8GQgYuQI'
    'BdVKBm2qE421IfdeY0E8UJW0TtVPav8irn75BO+nFpp2BmIFFKYmJo0HQh1hynitIo11'
    'pjz1/OW6yy5/uAubMJXh/+B3+pn6tAT8HwLsj8m8GAI8RKG1sEpErWr4tdx5OC5q6g5W'
    '5/+GmqBZoPGJ0ebnvspbyJloN/ONif2IE0epxLzJ0PeyGJ39ZgWSlRmofdZoTh0kSYO9'
    'qpl5f0PE6oJ27BfM20IDyksVLv1JfOHVSIFoPf6uadwf/+JruDX6wygsMX/48CrxMw0I'
    '++RqrdRxAYya3NZVROcEwpwpYF9KEQ0g/C4gK5VfDLNy7Jd2RVpL0F+/z3pQwa/6zjn0'
    'd/SDz+uMN9XW/lwNSDvPdQf6sh373LpMnegAAAA=',
    'wcor.tif':
    'SUkqAAgAAAAMAAABAwABAAAAJQAAAAEBAwABAAAAHQAAAAIBAwADAAAAngAAAAMBAwAB'
    'AAAAUcMAAAYBAwABAAAAAgAAABEBBAABAAAABwEAABUBAwABAAAAAwAAABYBAwABAAAA'
    'HQAAABcBBAABAAAAvAMAABwBAwABAAAAAQAAAFMBAwADAAAApAAAAICkAgBdAAAAqgAA'
    'AAAAAAAIAAgACAABAAEAAQA8R0RBTE1ldGFkYXRhPgogIDxJdGVtIG5hbWU9IldFQlBf'
    'TEVWRUwiIGRvbWFpbj0iSU1BR0VfU1RSVUNUVVJFIj43NTwvSXRlbT4KPC9HREFMTWV0'
    'YWRhdGE+CgBSSUZGtAMAAFdFQlBWUDggqAMAAJASAJ0BKiUAHQA+gc2TR6UjIaE1SACg'
    'EAlsAJ0yhBAXr2OHsCQI/zv/AewDbAeYD9a/0l95f0AbwBz3n7kfAV+33pXVgJkFxhZA'
    'nSfYEGBR7X7J/kvEG+Gf2nzNeXl/If1j9x/6T7CPmb/qf3T4DP5n/TfQa9QH6AewP+lp'
    'BhTTvZDa5yvOfRu1pCxqKJ4as+zmi6aJTLyiS7ZGUQAA/YOqRKu/yUjVUxlR2Uwl0srh'
    'fPsQN9e6U19EESJ2Tm0raFsvuaY/v15y+dr30I8Br+IHIfOPII0O76OfFUROAxwXgw9I'
    'OqfCa1umJT8Mo6pm3KGMZJhJLNQCd1J+5wZ8LaE5f38eRLXMQYm8c5F3OAxEyspsEZsq'
    'GceuI+g+AvEMS5skg9CD4RnbZBHNJLfdXVpXWhWG/HGq+mUE1oo3j9GXZQ1G/W0qN7Ho'
    'mnH5McosioF1MISHwLpYKAyc8TkTRL0TrwhQDE/fuIHd7imYsUpLUB3X7nr2yEjM+nHf'
    'aEJ/WRE+g5Ivusn3UdTGZQBMX3XYNwFnL9TUG4qv47TFs6USorpv18oIo2O1CWpsEBEa'
    'rjiEHvnfiygQmXpBqnL3/GL2ePApf+gtFLDbb8r8Az5vD8TDXN+c94d6+to/Kf9Dcb2p'
    '8LOsyBd9xq6m1KT11f2119C+UQOUQX05iwOZSm0l1L8cgj3eXpnRD1AtAhXl5RNebbc8'
    'YrCKg2rk7C7NIqigmaYT+w+chskmv9E0wObtVRnnsY2lCIwOC+5dDWC13RlclOpnVYEg'
    'Ou8RlgPrK/joyo7KYS4O2EsQOw7l/VdclTkFLGvcQtnt9fO0Wi+AYqTSYacjWY/Jk9Zt'
    'ObQQ9CB9U4DP+Wz0RJBL4jhxO3B5DR97aV5iZM9MN/6EuN5WRf70wr3QGcQ4f8RzX/wr'
    'REhp5qgraLEAiX0V91xPFPgloTW8zns2f+4cKDwJZcB2fPX9P+fSkAJHC5AAHUabDYMs'
    'tCwvDZLgMLlfP5eDtQ08lncmbGJm2n8ZgXYsM/b69/Wgl9sshNn7hi/dj85tS0XMyWpx'
    'c0n+RqZnPJxJIufMsVsbLfNBrF9eZ8NyddlebUHZ/p5dwbmM7FVUTKXTtyr/7ac/vaZ0'
    'yLwt2oaWtbKH+86N1Oc6usiLHnUu4xW8Klpr4UX/jIGaARvs//nwv/9oOJgiB0lHnK/I'
    'MrilnqDBUEY/7bGtxyhA8n2/+CHnO4ZmH61qsL6YJZ5aZn0Zgex1xczvQAAAAA==',
}


for name, b64 in BLOBS.items():
    with open(OUT + '/' + name, 'wb') as fp:
        fp.write(base64.b64decode(b64))
