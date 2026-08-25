import json, subprocess, sys, itertools
sys.path.insert(0,"/tmp/qh")
from readtif import read_tif

def probe(coords, tag=""):
    # coords: list of (x,y); z = x*y
    feats=[{"type":"Feature","properties":{"v":x*y},"geometry":{"type":"Point","coordinates":[x,y]}} for x,y in coords]
    json.dump({"type":"FeatureCollection","features":feats}, open("pp.json","w"))
    xs=[c[0] for c in coords]; ys=[c[1] for c in coords]
    x0,x1=min(xs),max(xs); y0,y1=min(ys),max(ys)
    subprocess.run(["/home/agent/oracle/executable","vector","grid","linear","pp.json","pp.tif",
                    "--size","4,4","--zfield","v","-q","--overwrite"],check=True,capture_output=True)
    w,h,rows=read_tif("pp.tif")
    # sample u=0.125,v=0.375 of the single quad => px=0, py = h-1- (int) ... quad local
    val = rows[2][0]  # py: v=0.375 -> y=y0+0.375*(y1-y0); row = (y1-y)/dy-0.5; dy=(y1-y0)/4 -> row=2. px=0 (u=0.125)
    u=0.125; v=0.375
    X0,Y0,X1,Y1=x0,y0,x1,y1
    zsl=(1-v)*(X0*Y0)+u*(X1*Y1)+(v-u)*(X0*Y1)
    zbk=(1-u-v)*(X0*Y0)+u*(X1*Y0)+v*(X0*Y1)
    return "/" if abs(val-zsl)<abs(val-zbk) else "\\"

base=[(0.0,0.0),(1.0,0.0),(0.0,1.0),(1.0,1.0)]  # BL BR TL TR
for lbl, sq in [("unit",base), ("off3",[(x+3,y+3) for x,y in base]), ("rect2",[(2*x,y) for x,y in base])]:
    out=[]
    for perm in itertools.permutations(range(4)):
        coords=[sq[i] for i in perm]
        d=probe(coords)
        out.append(("".join("BLBRTLTR"[2*i:2*i+2] for i in perm), d))
    print(lbl, " ".join(f"{o}:{d}" for o,d in out))
