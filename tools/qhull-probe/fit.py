import os
#!/usr/bin/env python3
"""Fit harness: compare sim.py diagonal predictions with oracle maps."""
import itertools, json, os, random, sys
import probe
import sim
from importlib import reload

CACHE = os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache")
os.makedirs(CACHE, exist_ok=True)


def oracle_map(w, h, order, ox=0.0, oy=0.0, step=1.0, seed=5):
    key = f"{w}x{h}_{ox}_{oy}_{step}_{seed}_" + "-".join(map(str, order))
    fp = os.path.join(CACHE, key + ".json")
    if os.path.exists(fp):
        d = json.load(open(fp))
        return {tuple(map(int, k.split(","))): v for k, v in d.items()}
    m = probe.probe_lattice(w, h, ox=ox, oy=oy, step=step, seed=seed,
                            order=list(order), tag="fit")
    json.dump({f"{i},{j}": v for (i, j), v in m.items()}, open(fp, "w"))
    return m


def sim_map(w, h, order, ox=0.0, oy=0.0, step=1.0):
    # feature k = lattice order[k]; qhull point id = feature index
    lat2pid = {}
    pts = []
    for pid, li in enumerate(order):
        j, i = divmod(li, w)
        pts.append((ox + i * step, oy + j * step))
        lat2pid[li] = pid
    q = sim.Qhull(pts)
    tris = q.result()
    out = {}
    for j in range(h - 1):
        for i in range(w - 1):
            ll = lat2pid[j * w + i]
            lr = lat2pid[j * w + i + 1]
            ul = lat2pid[(j + 1) * w + i]
            ur = lat2pid[(j + 1) * w + i + 1]
            diag = None
            for t in tris:
                s = set(t)
                if ll in s and ur in s and (lr in s or ul in s):
                    diag = "/"
                    break
                if lr in s and ul in s and (ll in s or ur in s):
                    diag = "\\"
                    break
            out[(i, j)] = diag or "?"
    return out


def compare(w, h, order, ox=0.0, oy=0.0, step=1.0, seed=5, verbose=True):
    om = oracle_map(w, h, order, ox, oy, step, seed)
    try:
        sm = sim_map(w, h, order, ox, oy, step)
    except sim.QhullError as e:
        if verbose:
            print(f"  sim error: {e}")
        return False
    bad = [k for k in om if om[k] != sm.get(k)]
    if bad and verbose:
        print(f"  order={order} mismatches={len(bad)}/{len(om)}")
        for j in range(h - 2, -1, -1):
            row_o = "".join(om[(i, j)] for i in range(w - 1))
            row_s = "".join(sm.get((i, j), "?") for i in range(w - 1))
            print(f"    oracle {row_o}   sim {row_s}")
    return not bad


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "2x2"
    if mode == "2x2":
        ok = bad = 0
        for perm in itertools.permutations(range(4)):
            if compare(2, 2, perm, verbose=False):
                ok += 1
            else:
                bad += 1
                print("2x2 FAIL:", perm)
                compare(2, 2, perm, verbose=True)
        print(f"2x2: {ok} ok, {bad} bad")
    elif mode == "3x2":
        ok = bad = 0
        perms = list(itertools.permutations(range(6)))
        lim = int(sys.argv[2]) if len(sys.argv) > 2 else 120
        random.seed(42)
        random.shuffle(perms)
        for perm in perms[:lim]:
            if compare(3, 2, perm, verbose=False):
                ok += 1
            else:
                bad += 1
                if bad <= 5:
                    print("3x2 FAIL:", perm)
                    compare(3, 2, perm, verbose=True)
        print(f"3x2: {ok} ok, {bad} bad")
    elif mode == "big":
        for (w, h) in [(3, 3), (4, 3), (5, 5), (7, 5), (6, 6), (8, 4)]:
            order = list(range(w * h))
            r = compare(w, h, order, seed=1)
            print(f"{w}x{h} identity: {'OK' if r else 'FAIL'}")
    elif mode == "shuf":
        for (w, h) in [(4, 4), (5, 5), (6, 6)]:
            for s in range(3):
                order = list(range(w * h))
                random.seed(100 + s)
                random.shuffle(order)
                r = compare(w, h, order, seed=1)
                print(f"{w}x{h} shuffle{s}: {'OK' if r else 'FAIL'}")


if __name__ == "__main__":
    main()
