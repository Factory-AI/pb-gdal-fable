import sys

sys.path.insert(0, '/workspace/tests')
from mk_tail_fixtures import write_tif

UTM = [1, 1, 0, 3, 1024, 0, 1, 1, 1025, 0, 1, 1, 3072, 0, 1, 32611]
UTM_EGM96 = [1, 1, 1, 4, 1024, 0, 1, 1, 1025, 0, 1, 1,
             3072, 0, 1, 32611, 4096, 0, 1, 5773]
GT = (500000, 10, 0, 3800000, 0, -10)


def make():
    vals = [str((i * 3) % 256) for i in range(64)]
    write_tif('b1.tif', 'Byte', 8, 8, [vals], gt=GT, geokeys=UTM)
    write_tif('vsrc.tif', 'Byte', 8, 8, [vals], gt=GT, geokeys=UTM_EGM96)
    write_tif('nd1.tif', 'Byte', 8, 8, [vals], gt=GT, geokeys=UTM,
              nodata='6')
    write_tif('i16.tif', 'Int16', 8, 8,
              [[str(i * 7 - 30) for i in range(64)]], gt=GT, geokeys=UTM)
    write_tif('f32.tif', 'Float32', 8, 8,
              [[str(i * 0.25) for i in range(64)]], gt=GT, geokeys=UTM)
    write_tif('z64.tif', 'Float64', 8, 8, [['0'] * 64], gt=GT, geokeys=UTM)
    rgb = [[str((i * k + 11 * k) % 256) for i in range(64)]
           for k in (1, 2, 3)]
    write_tif('rgbu.tif', 'Byte', 8, 8, rgb, gt=GT, geokeys=UTM)
    write_tif('rgbv.tif', 'Byte', 8, 8, rgb, gt=GT, geokeys=UTM_EGM96)


if __name__ == '__main__':
    make()
