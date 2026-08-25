import sys

sys.path.insert(0, '/workspace/tests')
from mk_tail_fixtures import write_tif

G4326 = None  # epsg keyword path
UTM33 = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32633]


def make():
    write_tif('g1.tif', 'Float64', 3, 1, [['2', '3', '7']])
    write_tif('g2.tif', 'Float64', 3, 1, [['5', '-1', '0.25']])
    write_tif('ndA.tif', 'Float64', 3, 1, [['-999', '4', '-999']],
              nodata='-999')
    write_tif('ndB.tif', 'Float64', 3, 1, [['-999', '7', '2']],
              nodata='-999')
    write_tif('gb.tif', 'Byte', 3, 1, [['1', '2', '7']])
    write_tif('bnd.tif', 'Byte', 3, 1, [['1', '255', '3']], nodata='255')
    write_tif('f32.tif', 'Float32', 3, 1, [['1.5'] * 3])
    write_tif('mb2.tif', 'Byte', 4, 3, [['1'] * 12, ['2'] * 12],
              gt=(0, 1, 0, 3, 0, -1), epsg=4326)
    write_tif('mb3.tif', 'Byte', 4, 3,
              [['1'] * 12, ['2'] * 12, ['3'] * 12])
    write_tif('nb2.tif', 'Byte', 4, 3, [['1'] * 12, ['2'] * 12])
    write_tif('hi.tif', 'Byte', 6, 2, [['9'] * 12],
              gt=(0, 4.0 / 6.0, 0, 3, 0, -1.5), epsg=4326)
    write_tif('utm.tif', 'Byte', 4, 3, [['5'] * 12],
              gt=(0, 1, 0, 3, 0, -1), geokeys=UTM33)
    write_tif('shift.tif', 'Byte', 4, 3, [['5'] * 12],
              gt=(1, 1, 0, 3, 0, -1), epsg=4326)
    with open('map1.txt', 'w') as f:
        f.write('2=5\n# comment\n3=6\n7=8\n')
    with open('map2.txt', 'w') as f:
        f.write('2=5\n3=6;7=8\n')
    with open('map3.txt', 'w') as f:
        f.write('2=5\n# c\n3=6\n7=8\n')
    with open('map4.txt', 'w') as f:
        f.write('1=2;\n')
    with open('map5.txt', 'w') as f:
        f.write('2=5\n\n3=6\n7=8\n')


if __name__ == '__main__':
    make()
