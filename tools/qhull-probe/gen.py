import json, sys, random

def gen(nx, ny, ox=0.0, oy=0.0, sx=1.0, sy=1.0, zf="xy", order="row", seed=0, path=None):
    pts = []
    for j in range(ny):
        for i in range(nx):
            x = ox + i*sx; y = oy + j*sy
            if zf == "xy": z = x*y
            elif zf == "x2y": z = x*x + y
            elif zf == "rand":
                z = None  # fill later
            pts.append([x, y, z])
    if zf == "rand":
        rr = random.Random(seed+1000)
        for p in pts: p[2] = rr.uniform(0, 100)
    if order == "shuffle":
        rr = random.Random(seed)
        rr.shuffle(pts)
    elif order == "col":
        pts.sort(key=lambda p: (p[0], p[1]))
    elif order == "rev":
        pts.reverse()
    feats = [{"type":"Feature","properties":{"v":p[2]},
              "geometry":{"type":"Point","coordinates":[p[0],p[1]]}} for p in pts]
    fc = {"type":"FeatureCollection","features":feats}
    with open(path,"w") as f: json.dump(fc,f)
    return pts

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("path"); ap.add_argument("--nx",type=int,default=10); ap.add_argument("--ny",type=int,default=6)
    ap.add_argument("--ox",type=float,default=0.0); ap.add_argument("--oy",type=float,default=0.0)
    ap.add_argument("--sx",type=float,default=1.0); ap.add_argument("--sy",type=float,default=1.0)
    ap.add_argument("--zf",default="xy"); ap.add_argument("--order",default="row"); ap.add_argument("--seed",type=int,default=0)
    a = ap.parse_args()
    gen(a.nx,a.ny,a.ox,a.oy,a.sx,a.sy,a.zf,a.order,a.seed,a.path)
