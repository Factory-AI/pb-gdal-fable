import struct, sys

def read_tif(path):
    d = open(path,"rb").read()
    bo = "<" if d[:2]==b"II" else ">"
    magic, = struct.unpack(bo+"H", d[2:4])
    assert magic == 42
    off, = struct.unpack(bo+"I", d[4:8])
    tags = {}
    while off:
        n, = struct.unpack(bo+"H", d[off:off+2])
        for k in range(n):
            e = off+2+12*k
            tag,typ,cnt = struct.unpack(bo+"HHI", d[e:e+8])
            if typ==3: sz=2; fmt="H"
            elif typ==4: sz=4; fmt="I"
            elif typ==12: sz=8; fmt="d"
            elif typ==11: sz=4; fmt="f"
            else: sz=1; fmt="B"
            total = sz*cnt
            if total<=4: raw = d[e+8:e+8+total]
            else:
                p, = struct.unpack(bo+"I", d[e+8:e+12]); raw = d[p:p+total]
            tags[tag] = struct.unpack(bo+str(cnt)+fmt, raw)
        noff, = struct.unpack(bo+"I", d[off+2+12*n:off+6+12*n])
        # take the LAST ifd (grid writes orphan first)
        if tags.get(256) and tags.get(273):
            pass
        off = noff
        if off and tags.get(273,(0,))[0]==0:
            tags = {}
            continue
        if off: tags = {}
    # re-walk: collect all IFDs, use last one with nonzero strip offsets
    off, = struct.unpack(bo+"I", d[4:8])
    best=None
    while off:
        n, = struct.unpack(bo+"H", d[off:off+2])
        t={}
        for k in range(n):
            e = off+2+12*k
            tag,typ,cnt = struct.unpack(bo+"HHI", d[e:e+8])
            if typ==3: sz=2; fmt="H"
            elif typ==4: sz=4; fmt="I"
            elif typ==12: sz=8; fmt="d"
            elif typ==11: sz=4; fmt="f"
            elif typ==2: sz=1; fmt="s"
            else: sz=1; fmt="B"
            total = sz*cnt
            if total<=4: raw = d[e+8:e+8+total]
            else:
                p, = struct.unpack(bo+"I", d[e+8:e+12]); raw = d[p:p+total]
            if fmt!="s": t[tag]=struct.unpack(bo+str(cnt)+fmt, raw)
        off2, = struct.unpack(bo+"I", d[off+2+12*n:off+6+12*n])
        if t.get(273,(0,))[0]!=0: best=t
        off=off2
    t=best
    w=t[256][0]; h=t[257][0]
    offs=t[273]; cnts=t[279]; rps=t.get(278,(h,))[0]
    bps=t[258][0]; assert bps==64, bps
    vals=[]
    for si,(o,c) in enumerate(zip(offs,cnts)):
        vals += list(struct.unpack(bo+str(c//8)+"d", d[o:o+c]))
    rows=[vals[y*w:(y+1)*w] for y in range(h)]
    return w,h,rows

if __name__=="__main__":
    w,h,rows=read_tif(sys.argv[1])
    print(w,h)
