#!/usr/bin/env python3
"""Predict 2x2 diagonal: initial simplex via qh_maxmin/qh_maxsimplex; the
left-out corner is inserted last (max vertex id); Qt fans from it."""
import itertools, sys

ox, oy = 0.0, 0.0


def predict(perm):
    # points p0..p3 = lattice corners perm[i]; p4 = infinity (mean)
    lat = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
    pts = []
    for li in perm:
        x, y = lat[li][0] + ox, lat[li][1] + oy
        pts.append([x, y, x * x + y * y])
    mx = sum(p[0] for p in pts) / 4.0
    my = sum(p[1] for p in pts) / 4.0
    maxb = max(p[2] for p in pts)
    pts.append([mx, my, maxb * 1.1])
    # Qbb scale last coord to [0, maxabs-of-others]
    newhigh = max(max(abs(p[0]), abs(p[1])) for p in pts)
    lo = min(p[2] for p in pts)
    hi = max(p[2] for p in pts)
    scale = newhigh / (hi - lo)
    shift = -lo * scale
    for p in pts:
        p[2] = p[2] * scale + shift

    # qh_maxmin: per dim, first-max / first-min (else-if rule)
    maxpoints = []
    for k in range(3):
        mn = mxp = 0
        for i in range(1, 5):
            if pts[mxp][k] < pts[i][k]:
                mxp = i
            elif pts[mn][k] > pts[i][k]:
                mn = i
        maxpoints += [mxp, mn]

    # qh_maxsimplex: init with minx/maxx of maxpoints (first wins ties)
    maxx = minx = None
    maxc, minc = -1e308, 1e308
    for i in maxpoints:
        if maxc < pts[i][0]:
            maxc = pts[i][0]
            maxx = i
        if minc > pts[i][0]:
            minc = pts[i][0]
            minx = i
    simplex = [minx]
    if maxx not in simplex:
        simplex.append(maxx)

    def detsimplex(cand, k):
        rows = [[pts[simplex[i]][c] - pts[cand][c] for c in range(k)]
                for i in range(k)]
        if k == 2:
            return rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0]
        return (rows[0][0] * (rows[1][1] * rows[2][2] - rows[1][2] * rows[2][1])
                - rows[0][1] * (rows[1][0] * rows[2][2] - rows[1][2] * rows[2][0])
                + rows[0][2] * (rows[1][0] * rows[2][1] - rows[1][1] * rows[2][0]))

    eps = 2.220446049250313e-16
    maxabs = max(max(abs(c) for c in p) for p in pts)
    maxsum = sum(max(abs(p[k]) for p in pts) for k in range(3))
    nearzero_th = 80 * maxsum * eps  # qh NEARzero guess

    for k in range(2, 4):
        best, bestdet, bestnz = None, -1e308, False
        for i in maxpoints:
            if i in simplex:
                continue
            d = abs(detsimplex(i, k))
            if d > bestdet:
                bestdet = d
                best = i
                bestnz = d < 10 * nearzero_th
        if best is None or bestnz:
            for i in range(5):
                if i in simplex:
                    continue
                d = abs(detsimplex(i, k))
                if d > bestdet:
                    bestdet = d
                    best = i
                    bestnz = d < 10 * nearzero_th
        simplex.append(best)

    left = [i for i in range(5) if i not in simplex]
    assert len(left) == 1 and left[0] != 4, (perm, simplex)
    d_lat = perm[left[0]]
    return "/" if d_lat in (0, 3) else "\\"


got = {}
for line in open(sys.argv[1]):
    a, b = line.split()
    got[tuple(int(c) for c in a)] = b
bad = 0
for perm in itertools.permutations(range(4)):
    p = predict(perm)
    g = got[perm]
    m = "ok" if p == g else "MISMATCH"
    if p != g:
        bad += 1
        print("".join(map(str, perm)), "pred", p, "got", g, m)
print("mismatches:", bad, "/ 24")
