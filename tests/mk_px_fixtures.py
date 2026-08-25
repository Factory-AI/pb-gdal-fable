import json
import sys

sys.path.insert(0, '/workspace/tests')
from mk_tail_fixtures import write_tif


def write_inputs():
    write_tif('t.tif', 'Byte', 4, 3, [[str(i * 20) for i in range(12)]],
              gt=(10, 1, 0, 30, 0, -1), epsg=4326)
    write_tif('f.tif', 'Float64', 3, 2,
              [['1.5', '-2.25', 'nan', '4', '5', '6']],
              gt=(0, 1, 0, 2, 0, -1), epsg=32631, nodata='4')
    write_tif('m3.tif', 'Int16', 2, 2,
              [['1', '2', '3', '4'], ['5', '6', '7', '8'],
               ['9', '10', '11', '12']],
              gt=(0, 1, 0, 2, 0, -1), epsg=4326)
    write_tif('g.tif', 'Float64', 6, 6, [[str(v) for v in range(36)]],
              gt=(0, 1, 0, 6, 0, -1), epsg=4326)
    write_tif('sq.tif', 'Float64', 8, 8,
              [[str(c * c * 3.0 + r * r * 7.0 + r * c * 2.0)
                for r in range(8) for c in range(8)]],
              gt=(0, 1, 0, 8, 0, -1), epsg=4326)
    write_tif('nog.tif', 'Byte', 2, 2, [['1', '2', '3', '4']])
    write_tif('i2.tif', 'Int16', 2, 2, [['0', '10', '-3', '-8']],
              gt=(0, 1, 0, 2, 0, -1), epsg=4326)
    write_tif('sc.tif', 'Int16', 2, 2, [['1', '2', '3', '4']],
              gt=(0, 1, 0, 2, 0, -1), epsg=4326,
              gmd='<GDALMetadata>'
                  '<Item name="SCALE" sample="0" role="scale">2.5</Item>'
                  '<Item name="OFFSET" sample="0" role="offset">10</Item>'
                  '</GDALMetadata>')
    write_tif('f32.tif', 'Float32', 2, 1, [['0.1', '1.5']],
              gt=(0, 1, 0, 1, 0, -1), epsg=4326)
    write_tif('n4.tif', 'Float64', 4, 4,
              [[('99' if (r, c) == (3, 3) else str(r * 4 + c))
                for r in range(4) for c in range(4)]],
              gt=(0, 1, 0, 4, 0, -1), epsg=4326, nodata='99')
    write_tif('n5.tif', 'Float64', 4, 4,
              [[('99' if (r, c) == (0, 0) else str(r * 4 + c))
                for r in range(4) for c in range(4)]],
              gt=(0, 1, 0, 4, 0, -1), epsg=4326, nodata='99')
    write_tif('id.tif', 'Byte', 20, 20,
              [[str((r + c) % 250) for r in range(20) for c in range(20)]],
              gt=(0, 1, 0, 0, 0, 1), epsg=4326)
    write_tif('fmt.tif', 'Float64', 6, 1,
              [['43.320000000000014', '0.10000000149011612',
                '97.29129999999998', '-1.999999999999999',
                '2.0000000000000004', '1234567.9999999999']],
              gt=(0, 1, 0, 1, 0, -1), epsg=4326)
    write_tif('fmt2.tif', 'Float64', 6, 1,
              [['-95.488160999999721', '-68.773050000007487',
                '1.0000001999999998', '0.0001999999999999999',
                '43.999999999999993', '1e300']],
              gt=(0, 1, 0, 1, 0, -1), epsg=4326)
    write_tif('w83.tif', 'Float64', 8, 3,
              [[str(c * c * 3.0 + r * r * 7.0 + r * c * 2.0)
                for r in range(3) for c in range(8)]],
              gt=(0, 1, 0, 3, 0, -1), epsg=4326)
    write_tif('w18.tif', 'Float64', 1, 8,
              [[str(r * 3.0) for r in range(8)]],
              gt=(0, 1, 0, 8, 0, -1), epsg=4326)
    open('ex.json', 'w').write('x')
    fc = {'type': 'FeatureCollection', 'features': [
        {'type': 'Feature', 'properties': {'nm': 'a', 'k': 7},
         'geometry': {'type': 'Point', 'coordinates': [1.0, 2.0]}},
        {'type': 'Feature', 'properties': {'nm': 'b', 'k': 8},
         'geometry': {'type': 'Point', 'coordinates': [3.5, 4.5]}}]}
    open('pos.geojson', 'w').write(json.dumps(fc))
    fc2 = {'type': 'FeatureCollection',
           'crs': {'type': 'name', 'properties':
                   {'name': 'urn:ogc:def:crs:EPSG::32631'}},
           'features': [
               {'type': 'Feature', 'properties': {'lbl': 'p1'},
                'geometry': {'type': 'Point',
                             'coordinates': [11.2, 28.4]}}]}
    open('posutm.geojson', 'w').write(json.dumps(fc2))
    fc3 = {'type': 'FeatureCollection', 'features': [
        {'type': 'Feature',
         'properties': {'s': 'hi', 'n': 5, 'r': 1.5, 'b': True,
                        'w': 9007199254740993},
         'geometry': {'type': 'Point', 'coordinates': [1.0, 2.0]}},
        {'type': 'Feature',
         'properties': {'s': None, 'n': None, 'r': None, 'b': None,
                        'w': None},
         'geometry': {'type': 'Point', 'coordinates': [2.5, 3.5]}},
        {'type': 'Feature', 'properties': {},
         'geometry': {'type': 'Point', 'coordinates': [3.0, 1.0]}}]}
    open('posnull.geojson', 'w').write(json.dumps(fc3))
    fc4 = {'type': 'FeatureCollection', 'features': [
        {'type': 'Feature', 'properties': {'d': '2021-05-06'},
         'geometry': {'type': 'Point', 'coordinates': [1.0, 2.0]}}]}
    open('posdate.geojson', 'w').write(json.dumps(fc4))
    fc5 = {'type': 'FeatureCollection', 'features': [
        {'type': 'Feature',
         'properties': {'il': [1, 2], 'j': {'k': 1}},
         'geometry': {'type': 'Point', 'coordinates': [1.0, 2.0]}},
        {'type': 'Feature', 'properties': {'il': None, 'j': {'z': 2}},
         'geometry': {'type': 'Point', 'coordinates': [2.5, 3.5]}}]}
    open('poslist.geojson', 'w').write(json.dumps(fc5))
    fc6 = {'type': 'FeatureCollection', 'features': [
        {'type': 'Feature',
         'properties': {'il': [1, 2], 'j': {'k': 1}, 'sl': ['a', 'b']},
         'geometry': {'type': 'Point', 'coordinates': [1.0, 2.0]}}]}
    open('poslistok.geojson', 'w').write(json.dumps(fc6))


if __name__ == '__main__':
    write_inputs()
