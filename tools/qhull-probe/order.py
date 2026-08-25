import os
#!/usr/bin/env python3
"""Recover the oracle's insertion order from facet vertex orders decoded
via dense-grid pixel values."""
import itertools, json, os, struct, subprocess, sys
import r23 as R
import sim

SIZE = 100


def run_oracle_size(size):
    jf = os.path.join(R.WORK, "r23.json")
    json.dump(R.gen_r23(), open(jf, 'w'))
    out = os.path.join(R.WORK, "outb.tif")
    if os.path.exists(out):
        os.unlink(out)
    cmd = [R.ORACLE, "vector", "grid", "linear", jf, out,
           "--size", f"{size},{size}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=R.WORK)
    assert r.returncode == 0, r.stderr
    return R.read_tif_f64(out)


def main():
    feats = R.gen_r23()['features']
    xs = [f['geometry']['coordinates'][0] for f in feats]
    ys = [f['geometry']['coordinates'][1] for f in feats]
    zs = [f['geometry']['coordinates'][2] for f in feats]
    pts = list(zip(xs, ys))
    tri = R.GDALTri(pts)
    w, h, opx = run_oracle_size(SIZE)
    xmin, xmax = min(xs), max(xs)
    ymin, ymax = min(ys), max(ys)
    dx = (xmax - xmin) / SIZE
    dy = (ymax - ymin) / SIZE

    def val(order, xp, yp):
        i1, i2, i3 = order
        x1, y1 = pts[i1]
        x2, y2 = pts[i2]
        x3, y3 = pts[i3]
        den = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
        m1x = (y2 - y3) / den
        m1y = (x3 - x2) / den
        m2x = (y3 - y1) / den
        m2y = (x1 - x3) / den
        l1 = m1x * (xp - x3) + m1y * (yp - y3)
        l2 = m2x * (xp - x3) + m2y * (yp - y3)
        l3 = 1.0 - l1 - l2
        return l1 * zs[i1] + l2 * zs[i2] + l3 * zs[i3]

    cand = {i: set(itertools.permutations(f))
            for i, f in enumerate(tri.facets)}
    hint = -1
    for row in range(SIZE):
        yp = ymax - dy * (row + 0.5)
        for col in range(SIZE):
            xp = xmin + dx * (col + 0.5)
            ok, idx = tri.find(hint, xp, yp, pts)
            hint = idx
            if not ok:
                continue
            x3, y3, m1x, m1y, m2x, m2y = tri.coefs(idx, pts)
            l1 = m1x * (xp - x3) + m1y * (yp - y3)
            l2 = m2x * (xp - x3) + m2y * (yp - y3)
            l3 = 1.0 - l1 - l2
            if min(l1, l2, l3) < 1e-7:
                continue  # near an edge: oracle may use the neighbor
            o = struct.pack('<d', opx[row * SIZE + col])
            keep = {p for p in cand[idx]
                    if struct.pack('<d', val(p, xp, yp)) == o}
            if keep:
                cand[idx] = keep
            else:
                print(f"  facet{idx}: NO perm matches interior pixel "
                      f"({col},{row})!")

    # build precedence constraints assuming desc-vid facet order
    edges = set()
    amb = 0
    for i, s in cand.items():
        if len(s) == 1:
            a, b, c = next(iter(s))
            edges.add((a, b))
            edges.add((b, c))
        else:
            amb += 1
    print(f"facets: {len(cand)}, unique-order {len(cand)-amb}, amb {amb}")
    simvid0 = {p: i for i, p in enumerate(tri.q.insertion_order)}
    json.dump({str(i): [list(p) for p in s] for i, s in cand.items()},
              open(os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache", "r23orders.json"), "w"))
    print("--- oracle facet orders (unique) with sim vids ---")
    for i, s in sorted(cand.items()):
        if len(s) != 1:
            continue
        o = next(iter(s))
        print(f"facet{i}: order {o} simvids {[simvid0[p] for p in o]}")
    for i, s in sorted(cand.items()):
        if len(s) > 1:
            print(f"  facet{i} {sorted(s)}")
    # my sim order for comparison
    simvid = {p: i for i, p in enumerate(tri.q.insertion_order)}
    viol = [(a, b) for (a, b) in edges if simvid[a] <= simvid[b]]
    print("sim insertion:", tri.q.insertion_order)
    print(f"constraint edges: {len(edges)}, violated by sim: {len(viol)}")
    for v in sorted(viol):
        print("  violated: %d must be AFTER %d (sim vids %d,%d)" %
              (v[0], v[1], simvid[v[0]], simvid[v[1]]))
    # topological order (Kahn, stable by smallest sim vid) for reference
    import collections
    nodes = set(range(len(pts)))
    succ = collections.defaultdict(set)
    pred = collections.defaultdict(set)
    for a, b in edges:  # a inserted AFTER b => edge b->a
        succ[b].add(a)
        pred[a].add(b)
    avail = sorted(n for n in nodes if not pred[n])
    topo = []
    pred2 = {n: set(pred[n]) for n in nodes}
    while avail:
        n = avail.pop(0)
        topo.append(n)
        for m in sorted(succ[n]):
            pred2[m].discard(n)
            if not pred2[m] and m not in topo and m not in avail:
                avail.append(m)
    print("a topo-compatible oracle order:", topo)
    json.dump(sorted(edges), open(os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache", "r23edges.json"), "w"))


if __name__ == "__main__":
    main()
