#!/usr/bin/env python3
"""Vector fixtures for the rasterize pipeline-transition suite."""
import json
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def fc(features):
    return {"type": "FeatureCollection", "features": features}


def feat(geom, props=None):
    return {"type": "Feature", "properties": props or {},
            "geometry": geom}


def poly(coords):
    return {"type": "Polygon", "coordinates": [coords]}


def write(name, obj):
    with open(os.path.join(OUT, name), "w") as f:
        json.dump(obj, f, separators=(",", ":"))


write("poly.geojson", fc([
    feat(poly([[0, 0], [2, 0], [2, 2], [0, 0]])),
]))

write("attr.geojson", fc([
    feat(poly([[0, 0], [1, 0], [1, 1], [0, 1], [0, 0]]), {"A": 1}),
    feat(poly([[1, 1], [2, 1], [2, 2], [1, 2], [1, 1]]), {"A": 3}),
]))

write("z.geojson", fc([
    feat({"type": "Polygon",
          "coordinates": [[[0, 0, 5], [2, 0, 10], [2, 2, 20],
                           [0, 2, 15], [0, 0, 5]]]}),
]))

write("pt.geojson", fc([
    feat({"type": "Point", "coordinates": [0.5, 0.5]}),
    feat({"type": "Point", "coordinates": [1.5, 1.7]}),
]))

write("lines.geojson", fc([
    feat({"type": "LineString",
          "coordinates": [[0, 0.3], [2, 1.9]]}),
]))

with open(os.path.join(OUT, "plain.txt"), "w") as f:
    f.write("hello\n")
