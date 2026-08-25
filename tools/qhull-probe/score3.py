import os
#!/usr/bin/env python3
import itertools, json, random
import sim, fit
import r23 as R

edges = [tuple(e) for e in json.load(open(os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache", "r23edges.json")))]
feats = R.gen_r23()['features']
pts = [(f['geometry']['coordinates'][0], f['geometry']['coordinates'][1])
       for f in feats]


def score():
    try:
        q = sim.Qhull(pts)
        vid = {p: i for i, p in enumerate(q.insertion_order)}
        viol = sum(1 for a, b in edges if vid[a] <= vid[b])
    except Exception as e:
        return f"crash({type(e).__name__}: {e})"
    ok2 = sum(fit.compare(2, 2, p, verbose=False)
              for p in itertools.permutations(range(4)))
    perms = list(itertools.permutations(range(6)))
    random.seed(42)
    random.shuffle(perms)
    ok3 = sum(fit.compare(3, 2, p, verbose=False) for p in perms[:40])
    return f"viol={viol}/{len(edges)} 2x2={ok2}/24 3x2={ok3}/40"


for up in ("noexit", "skip", "eligible"):
    for rr, vr in itertools.product([False, True], repeat=2):
        sim.P.bestnew_upper = up
        sim.P.cone_ridge_reverse = rr
        sim.P.cone_visible_reverse = vr
        print(f"upper={up:8s} ridge_rev={int(rr)} vis_rev={int(vr)}: "
              f"{score()}")
