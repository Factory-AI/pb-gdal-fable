import sys, subprocess, os, json
sys.path.insert(0, "/tmp/qh")
from readtif import read_tif

# diag map for z=xy lattice run at size 4*(nx-1),4*(ny-1)
def diagmap(path, nx, ny, ox=0.0, oy=0.0, sx=1.0, sy=1.0):
    w,h,rows = read_tif(path)
    assert w == 4*(nx-1) and h == 4*(ny-1), (w,h)
    out = []
    for j in range(ny-1):
        line = ""
        for i in range(nx-1):
            px = 4*i          # u=0.125
            py = 4*(ny-1-j)-2 # v=0.375
            val = rows[py][px]
            x = ox+ (i+0.125)*sx; y = oy + (j+0.375)*sy
            zslash = x*y  # not used; compute uv-part instead
            X0=ox+i*sx; Y0=oy+j*sy; X1=ox+(i+1)*sx; Y1=oy+(j+1)*sy
            u=0.125; v=0.375
            zsl = (1-v)*(X0*Y0) + u*(X1*Y1) + (v-u)*(X0*Y1)
            zbk = (1-u-v)*(X0*Y0) + u*(X1*Y0) + v*(X0*Y1)
            ds = abs(val-zsl); db = abs(val-zbk)
            line += "/" if ds<db else "\\"
        out.append(line)
    return out[::-1]  # top row = max j

if __name__=="__main__":
    path=sys.argv[1]; nx=int(sys.argv[2]); ny=int(sys.argv[3])
    ox=float(sys.argv[4]) if len(sys.argv)>4 else 0.0
    oy=float(sys.argv[5]) if len(sys.argv)>5 else 0.0
    sx=float(sys.argv[6]) if len(sys.argv)>6 else 1.0
    sy=float(sys.argv[7]) if len(sys.argv)>7 else 1.0
    for l in diagmap(path,nx,ny,ox,oy,sx,sy): print(l)
