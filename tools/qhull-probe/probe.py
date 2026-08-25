import os
#!/usr/bin/env python3
"""Probe rig: generate lattice point sets, run oracle `vector grid linear`,
read the Float64 raster, decode each quad's Delaunay diagonal."""
import json, os, struct, subprocess, sys, random

ORACLE = os.environ.get("ORACLE", "/home/agent/oracle/executable")
WORK = os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "work")


def read_tif(path):
    """Minimal TIFF reader: single IFD, uncompressed strips, Float64."""
    d = open(path, "rb").read()
    bo = "<" if d[:2] == b"II" else ">"
    off = struct.unpack(bo + "I", d[4:8])[0]
    n = struct.unpack(bo + "H", d[off:off + 2])[0]
    tags = {}
    for i in range(n):
        e = off + 2 + 12 * i
        tag, typ, cnt = struct.unpack(bo + "HHI", d[e:e + 8])
        val = d[e + 8:e + 12]
        tags[tag] = (typ, cnt, val)

    def tagvals(tag):
        typ, cnt, raw = tags[tag]
        size = {1: 1, 2: 1, 3: 2, 4: 4, 11: 4, 12: 8}[typ]
        fmt = {1: "B", 3: "H", 4: "I", 11: "f", 12: "d"}[typ]
        total = size * cnt
        if total <= 4:
            buf = raw[:total]
        else:
            p = struct.unpack(bo + "I", raw)[0]
            buf = d[p:p + total]
        return list(struct.unpack(bo + fmt * cnt, buf))

    w = tagvals(256)[0]
    h = tagvals(257)[0]
    bps = tagvals(258)[0]
    assert bps == 64, bps
    assert tagvals(259)[0] == 1
    offs = tagvals(273)
    cnts = tagvals(279)
    rps = tagvals(278)[0] if 278 in tags else h
    data = b"".join(d[o:o + c] for o, c in zip(offs, cnts))
    vals = struct.unpack(bo + "d" * (w * h), data[:8 * w * h])
    # rows top-down (row 0 = ymax side)
    return w, h, vals


def make_lattice(w, h, ox=0.0, oy=0.0, step=1.0, seed=1, order=None,
                 zvals=None):
    """Points at (ox+i*step, oy+j*step). order: permutation of range(w*h)
    mapping feature position -> lattice index (j*w+i). Returns (pts, feats)
    where pts[k] = (x, y, z) in FEATURE order."""
    random.seed(seed)
    zs = zvals if zvals is not None else [
        round(random.uniform(0, 10), 3) for _ in range(w * h)
    ]
    idxs = order if order is not None else list(range(w * h))
    pts = []
    feats = []
    for li in idxs:
        j, i = divmod(li, w)
        x, y, z = ox + i * step, oy + j * step, zs[li]
        pts.append((x, y, z))
        feats.append({
            "type": "Feature", "properties": {"z": z},
            "geometry": {"type": "Point", "coordinates": [x, y, z]}
        })
    return pts, {"type": "FeatureCollection", "name": "lat",
                 "features": feats}


def run_grid(fc, w, h, sizex, sizey, extra=None, tag="p"):
    os.makedirs(WORK, exist_ok=True)
    jf = f"{WORK}/{tag}.json"
    tf = f"{WORK}/{tag}.tif"
    if os.path.exists(tf):
        os.unlink(tf)
    with open(jf, "w") as f:
        json.dump(fc, f)
    cmd = [ORACLE, "vector", "grid", "linear", jf, tf,
           "--size", f"{sizex},{sizey}"] + (extra or [])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r
    return read_tif(tf), r


def plane_eval(p1, p2, p3, x, y):
    (x1, y1, z1), (x2, y2, z2), (x3, y3, z3) = p1, p2, p3
    den = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    l1 = ((y2 - y3) * (x - x3) + (x3 - x2) * (y - y3)) / den
    l2 = ((y3 - y1) * (x - x3) + (x1 - x3) * (y - y3)) / den
    l3 = 1.0 - l1 - l2
    return l1 * z1 + l2 * z2 + l3 * z3


def decode_diagonals(w, h, ox, oy, step, zs, raster):
    """raster from run_grid with size 3(w-1) x 3(h-1). Returns dict
    (i,j)->'/' or '\\' or '?' for each quad (i,j)=lower-left lattice idx."""
    rw, rh, vals = raster
    assert rw == 3 * (w - 1) and rh == 3 * (h - 1)
    dx = (w - 1) * step / rw
    dy = (h - 1) * step / rh
    xmin, ymax = ox, oy + (h - 1) * step

    def sample(px, py):
        return vals[py * rw + px]

    out = {}
    for j in range(h - 1):
        for i in range(w - 1):
            c00 = (ox + i * step, oy + j * step, zs[j * w + i])
            c10 = (ox + (i + 1) * step, oy + j * step, zs[j * w + i + 1])
            c01 = (ox + i * step, oy + (j + 1) * step, zs[(j + 1) * w + i])
            c11 = (ox + (i + 1) * step, oy + (j + 1) * step,
                   zs[(j + 1) * w + i + 1])
            errA = 0.0  # '/' diag c00-c11: lower {c00,c10,c11}, upper {c00,c11,c01}
            errB = 0.0  # '\' diag c10-c01: lower {c00,c10,c01}, upper {c10,c11,c01}
            for su in range(3):
                for sv in range(3):
                    if su == 1 and sv == 1:
                        continue
                    px = 3 * i + su
                    py = rh - 1 - (3 * j + sv)
                    x = xmin + dx * (px + 0.5)
                    y = ymax - dy * (py + 0.5)
                    u = (x - c00[0]) / step
                    v = (y - c00[1]) / step
                    got = sample(px, py)
                    pa = plane_eval(c00, c10, c11, x, y) if v < u else \
                        plane_eval(c00, c11, c01, x, y)
                    pb = plane_eval(c00, c10, c01, x, y) if v < 1 - u else \
                        plane_eval(c10, c11, c01, x, y)
                    errA = max(errA, abs(got - pa))
                    errB = max(errB, abs(got - pb))
            if errA < 1e-8 and errB > 1e-6:
                out[(i, j)] = "/"
            elif errB < 1e-8 and errA > 1e-6:
                out[(i, j)] = "\\"
            elif errA < 1e-8 and errB < 1e-8:
                out[(i, j)] = "="  # planar quad, ambiguous
            else:
                out[(i, j)] = "?"
    return out


def probe_lattice(w, h, ox=0.0, oy=0.0, step=1.0, seed=1, order=None,
                  zvals=None, tag="p"):
    pts, fc = make_lattice(w, h, ox, oy, step, seed, order, zvals)
    zs = zvals
    if zs is None:
        random.seed(seed)
        zs = [round(random.uniform(0, 10), 3) for _ in range(w * h)]
    raster, r = run_grid(fc, w, h, 3 * (w - 1), 3 * (h - 1), tag=tag)
    if raster is None:
        print("FAILED:", r.stderr)
        return None
    return decode_diagonals(w, h, ox, oy, step, zs, raster)


def show(diags, w, h):
    for j in range(h - 2, -1, -1):
        print("".join(diags[(i, j)] for i in range(w - 1)))


if __name__ == "__main__":
    w, h = int(sys.argv[1]), int(sys.argv[2])
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    d = probe_lattice(w, h, seed=seed)
    if d:
        show(d, w, h)
