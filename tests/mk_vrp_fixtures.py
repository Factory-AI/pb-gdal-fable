#!/usr/bin/env python3
"""Vector reproject fixtures: tiny GeoJSON datasets exercising geometry
types, missing/degenerate SRS, and partial transform failures, plus the
g1.tif raster from the warp set for generic-dispatch cases."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_tail_fixtures import write_tif

OUT = sys.argv[1] if len(sys.argv) > 1 else "."


def w(name, text):
    with open(os.path.join(OUT, name), "w") as f:
        f.write(text)


w("vp.geojson",
  '{"type":"FeatureCollection","features":[{"type":"Feature",'
  '"properties":{"n":1},"geometry":{"type":"Point",'
  '"coordinates":[2.5,45.5]}}]}')

w("vl.geojson",
  '{"type":"FeatureCollection","features":['
  '{"type":"Feature","properties":{"n":1},"geometry":{"type":"LineString",'
  '"coordinates":[[2.5,45.5],[2.6,45.6],[2.7,45.55]]}},'
  '{"type":"Feature","properties":{"n":2},"geometry":{"type":"Polygon",'
  '"coordinates":[[[2.1,45.1],[2.2,45.1],[2.2,45.2],[2.1,45.1]]]}},'
  '{"type":"Feature","properties":{"n":3},"geometry":{"type":"Point",'
  '"coordinates":[2.55,45.65,120.5]}}]}')

w("vmp.geojson",
  '{"type":"FeatureCollection","features":[{"type":"Feature",'
  '"properties":{"n":1},"geometry":{"type":"MultiPoint",'
  '"coordinates":[[2.5,45.5],[0,95]]}}]}')

w("vnull.geojson",
  '{"type":"FeatureCollection","features":[{"type":"Feature",'
  '"properties":{"n":1},"geometry":null}]}')

w("vnosrs.geojson",
  '{"type":"FeatureCollection","features":[{"type":"Feature",'
  '"properties":{},"geometry":{"type":"Point",'
  '"coordinates":[2.5,45.5]}}]}')

feats = []
for i in range(30):
    lat = 45.0 + i * 0.01 if i % 5 else 95.0 + i
    feats.append('{"type":"Feature","properties":{"n":%d},'
                 '"geometry":{"type":"Point","coordinates":[%g,%g]}}'
                 % (i, 2.0 + i * 0.01, lat))
w("vmany.geojson",
  '{"type":"FeatureCollection","features":[' + ",".join(feats) + ']}')

gw, gh = 10, 8
vals = [(r * gw + c) % 256 for r in range(gh) for c in range(gw)]
write_tif(os.path.join(OUT, "g1.tif"), "Byte", gw, gh, [vals],
          gt=[2.0, 0.1, 0.0, 46.0, 0.0, -0.1], epsg=4326)
