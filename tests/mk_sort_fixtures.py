#!/usr/bin/env python3
"""Fixtures for vector sort: point/polygon GeoJSON sets exercising the
hilbert key (envelope centers, envelope-union extent, ties, nulls)."""
import json
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def fc(feats):
    return {"type": "FeatureCollection", "features": feats}


def feat(n, geom):
    return {"type": "Feature", "properties": {"n": n}, "geometry": geom}


def pt(x, y):
    return {"type": "Point", "coordinates": [x, y]}


def write(name, doc):
    with open(os.path.join(OUT, name), "w") as f:
        json.dump(doc, f)


write("pts.geojson", fc([
    feat(0, pt(9, 9)), feat(1, pt(0, 0)), feat(2, pt(9, 0)),
    feat(3, pt(0, 9)), feat(4, pt(5, 5)), feat(5, pt(2, 7)),
    feat(6, None), feat(7, pt(7, 2)),
]))

write("mixed.geojson", fc([
    feat(0, {"type": "Polygon",
             "coordinates": [[[0, 0], [100, 0], [100, 100], [0, 100],
                              [0, 0]]]}),
    feat(1, pt(60, 40)),
    feat(2, pt(10, 10)),
    feat(3, {"type": "LineString", "coordinates": [[80, 5], [90, 25]]}),
    feat(4, {"type": "Polygon", "coordinates": []}),
    feat(5, pt(50, 50)),
]))

write("ties.geojson", fc([
    feat(0, pt(5.000001, 5.000001)), feat(1, pt(5.0, 5.0)),
    feat(2, pt(0, 0)), feat(3, pt(10, 10)),
    feat(4, pt(5.0000002, 5.0)),
]))

write("flat.geojson", fc([
    feat(0, pt(5, 3)), feat(1, pt(1, 3)), feat(2, pt(9, 3)),
    feat(3, pt(3, 3)),
]))

write("grid.geojson", fc([
    feat(i, pt((i * 37) % 12 + 0.5, (i * 53) % 9 + 0.25))
    for i in range(40)
]))

write("neg.geojson", fc([
    feat(0, pt(-5, -5)), feat(1, pt(-20, 15)), feat(2, pt(30, -8)),
    feat(3, pt(0, 0)), feat(4, pt(-20, -20)), feat(5, pt(30, 15)),
]))

write("one.geojson", fc([feat(0, pt(3, 4))]))
write("empty.geojson", fc([]))
write("nulls.geojson", fc([feat(0, None), feat(1, None)]))

dense = [feat(j, pt(j * 0.125 / 65535, 0.0)) for j in range(400)]
dense.append(feat(400, pt(1.0, 1.0)))
write("dense.geojson", fc(dense))

big = []
pi = 0
for i in range(30):
    if i % 7 == 3:
        big.append(feat(i, None))
    else:
        big.append(feat(i, pt(float(pi * 3000 % 65534),
                              float((pi * 7000) % 65534))))
        pi += 1
write("bignull.geojson", fc(big))

write("half.geojson", fc([
    feat(0, pt(0.0, 0.0)), feat(1, pt(65534.0, 65534.0)),
    feat(2, pt(49150.5, 3.0)), feat(3, pt(49151.0, 3.0)),
    feat(4, pt(32768.0, 17.0)), feat(5, pt(32767.0, 17.0)),
    feat(6, pt(40000.0, 0.0)), feat(7, pt(40001.0, 0.0)),
]))
