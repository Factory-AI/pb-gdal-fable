import os
#!/usr/bin/env python3
"""Full pixel pipeline for the r23 --size 8,8 residual cases: oracle vs
sim-based prediction, bit-exact comparison."""
import json, math, os, random, struct, subprocess, sys
import sim

ORACLE = os.environ.get("ORACLE", "/home/agent/oracle/executable")
WORK = os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "r23work")
os.makedirs(WORK, exist_ok=True)


def gen_r23():
    random.seed(3)
    f = [{'type': 'Feature', 'properties': {'z': round(random.uniform(0, 10), 2)},
          'geometry': {'type': 'Point',
                       'coordinates': [round(random.uniform(0, 10), 2),
                                       round(random.uniform(0, 10), 2),
                                       round(random.uniform(0, 10), 2)]}}
         for i in range(23)]
    return {'type': 'FeatureCollection', 'name': 'r23', 'features': f}


def read_tif_f64(path):
    data = open(path, 'rb').read()
    bo = '<' if data[:2] == b'II' else '>'
    off = struct.unpack(bo + 'I', data[4:8])[0]
    tags = {}
    n = struct.unpack(bo + 'H', data[off:off + 2])[0]
    for i in range(n):
        e = off + 2 + i * 12
        tag, typ, cnt = struct.unpack(bo + 'HHI', data[e:e + 8])
        val = struct.unpack(bo + 'I', data[e + 8:e + 12])[0]
        tags[tag] = (typ, cnt, val)
    w = tags[256][2]
    h = tags[257][2]
    sof = tags[273]
    sbc = tags[279]
    def readvals(entry):
        typ, cnt, val = entry
        size = {3: 2, 4: 4}[typ]
        fmt = {3: 'H', 4: 'I'}[typ]
        if cnt * size <= 4:
            packed = struct.pack(bo + 'I', val)
            return struct.unpack(bo + fmt * cnt, packed[:cnt * size])
        return struct.unpack(bo + fmt * cnt, data[val:val + size * cnt])

    offs = readvals(sof)
    cnts = readvals(sbc)
    raw = b''.join(data[o:o + c] for o, c in zip(offs, cnts))
    px = struct.unpack(bo + 'd' * (w * h), raw)
    return w, h, px


def run_oracle(extra):
    jf = os.path.join(WORK, "r23.json")
    json.dump(gen_r23(), open(jf, 'w'))
    out = os.path.join(WORK, "out.tif")
    if os.path.exists(out):
        os.unlink(out)
    cmd = [ORACLE, "vector", "grid", "linear", jf, out,
           "--size", "8,8"] + extra
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=WORK)
    if r.returncode != 0:
        print(r.stderr)
        raise SystemExit("oracle failed")
    return read_tif_f64(out)


class GDALTri:
    def __init__(self, pts2d):
        self.q = sim.Qhull(pts2d)
        self.facets = []      # (pid triple in facet vertex order)
        self.neigh = []
        flist = [f for f in self.q.facets if not f.upperdelaunay]
        fidx = {id(f): i for i, f in enumerate(flist)}
        for f in flist:
            self.facets.append(tuple(v.pid for v in f.vertices))
            nb = []
            for k in range(3):
                n = f.neighbors[k] if f.neighbors and len(f.neighbors) == 3 \
                    else None
                nb.append(fidx.get(id(n), -1) if n is not None else -1)
            self.neigh.append(nb)

    def coefs(self, i, pts):
        i1, i2, i3 = self.facets[i]
        x1, y1 = pts[i1]
        x2, y2 = pts[i2]
        x3, y3 = pts[i3]
        den = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
        m1x = (y2 - y3) / den
        m1y = (x3 - x2) / den
        m2x = (y3 - y1) / den
        m2y = (x1 - x3) / den
        return x3, y3, m1x, m1y, m2x, m2y

    def find(self, hint, x, y, pts):
        idx = hint if hint >= 0 else 0
        for _ in range(len(self.facets) + 1):
            x3, y3, m1x, m1y, m2x, m2y = self.coefs(idx, pts)
            l1 = m1x * (x - x3) + m1y * (y - y3)
            l2 = m2x * (x - x3) + m2y * (y - y3)
            l3 = 1.0 - l1 - l2
            if l1 < -1e-11:
                nb = self.neigh[idx][0]
            elif l2 < -1e-11:
                nb = self.neigh[idx][1]
            elif l3 < -1e-11:
                nb = self.neigh[idx][2]
            else:
                return True, idx
            if nb < 0:
                return False, idx
            idx = nb
        # loop guard: brute-force
        for i in range(len(self.facets)):
            x3, y3, m1x, m1y, m2x, m2y = self.coefs(i, pts)
            l1 = m1x * (x - x3) + m1y * (y - y3)
            l2 = m2x * (x - x3) + m2y * (y - y3)
            l3 = 1.0 - l1 - l2
            if l1 >= -1e-11 and l2 >= -1e-11 and l3 >= -1e-11:
                return True, i
        return False, idx


def predict(radius):
    feats = gen_r23()['features']
    xs = [f['geometry']['coordinates'][0] for f in feats]
    ys = [f['geometry']['coordinates'][1] for f in feats]
    zs = [(f['geometry']['coordinates'][2] + 0.0) * 1.0 for f in feats]
    tri = GDALTri(list(zip(xs, ys)))
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    w = h = 8
    dx = (xmax - xmin) / w
    dy = (ymax - ymin) / h
    pts = list(zip(xs, ys))
    out = []
    hint = -1
    nodata = 0.0
    for row in range(h):
        yp = ymax - dy * (row + 0.5)
        for col in range(w):
            xp = xmin + dx * (col + 0.5)
            ok, idx = tri.find(hint, xp, yp, pts)
            hint = idx
            if ok:
                i1, i2, i3 = tri.facets[idx]
                x3, y3, m1x, m1y, m2x, m2y = tri.coefs(idx, pts)
                l1 = m1x * (xp - x3) + m1y * (yp - y3)
                l2 = m2x * (xp - x3) + m2y * (yp - y3)
                l3 = 1.0 - l1 - l2
                v = l1 * zs[i1] + l2 * zs[i2] + l3 * zs[i3]
            else:
                if radius == 0.0:
                    v = nodata
                else:
                    best = float('inf')
                    v = nodata
                    found = False
                    inf = math.isinf(radius)
                    r2max = 0.0 if inf else radius * radius
                    for i in range(len(xs)):
                        rx = xs[i] - xp
                        ry = ys[i] - yp
                        r2 = rx * rx + ry * ry
                        if not inf and r2 > r2max:
                            continue
                        if r2 <= best:
                            best = r2
                            v = zs[i]
                            found = True
                    if not found:
                        v = nodata
            out.append(v)
    return out


def main():
    for extra, radius in ((["--radius", "0"], 0.0),
                          (["--radius", "2"], 2.0),
                          ([], float('inf'))):
        w, h, opx = run_oracle(extra)
        mine = predict(radius)
        bad = [(i, opx[i], mine[i]) for i in range(64)
               if struct.pack('<d', opx[i]) != struct.pack('<d', mine[i])]
        tag = extra or ["default"]
        print(f"{' '.join(tag)}: {64 - len(bad)}/64 exact")
        for i, o, m in bad[:8]:
            print(f"  px({i%8},{i//8}) oracle={o!r} mine={m!r}")


if __name__ == "__main__":
    main()
