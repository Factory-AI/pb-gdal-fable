#!/usr/bin/env python3
"""H1: diagonal of each cocircular quad = fan from the corner with the
highest qhull vertex id. Check whether a global order on lattice points
exists such that for every quad, argmax(corner order) lies on the
reported diagonal. Peel largest-first with backtracking."""
import sys
from probe import probe_lattice

sys.setrecursionlimit(10000)


def check(w, h, diags):
    quads = {}
    for (i, j), d in diags.items():
        corners = [(i, j), (i + 1, j), (i, j + 1), (i + 1, j + 1)]
        if d == "/":
            diag = {(i, j), (i + 1, j + 1)}
        elif d == "\\":
            diag = {(i + 1, j), (i, j + 1)}
        else:
            diag = set(corners)  # ambiguous: no constraint
        quads[(i, j)] = (corners, diag)

    pts = frozenset((i, j) for i in range(w) for j in range(h))
    seen = set()

    def peel(alive, order):
        if len(alive) <= 3:
            return order
        if alive in seen:
            return None
        cands = []
        for p in alive:
            ok = True
            for (qi, qj) in ((p[0] - 1, p[1] - 1), (p[0], p[1] - 1),
                             (p[0] - 1, p[1]), (p[0], p[1])):
                if (qi, qj) not in quads:
                    continue
                corners, diag = quads[(qi, qj)]
                if all(c in alive for c in corners) and p not in diag:
                    ok = False
                    break
            if ok:
                cands.append(p)
        for p in cands:
            r = peel(alive - {p}, order + [p])
            if r is not None:
                return r
        seen.add(alive)
        return None

    return peel(pts, [])


if __name__ == "__main__":
    for (w, h, seed) in [(3, 3, 1), (4, 3, 1), (5, 5, 1), (7, 5, 1),
                         (6, 6, 1), (8, 4, 1)]:
        d = probe_lattice(w, h, seed=seed, tag=f"h1_{w}x{h}")
        r = check(w, h, d)
        print(f"{w}x{h}: {'CONSISTENT' if r is not None else 'INCONSISTENT'}")
        if r is not None:
            print("  peel order (largest first):", r[:12], "...")
