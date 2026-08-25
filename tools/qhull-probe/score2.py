import os
#!/usr/bin/env python3
import itertools, json, random
import sim, fit
import r23 as R

edges = [tuple(e) for e in json.load(open(os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache", "r23edges.json")))]
feats = R.gen_r23()['features']
pts = [(f['geometry']['coordinates'][0], f['geometry']['coordinates'][1])
       for f in feats]


def score(quick=False):
    try:
        q = sim.Qhull(pts)
        vid = {p: i for i, p in enumerate(q.insertion_order)}
        viol = sum(1 for a, b in edges if vid[a] <= vid[b])
    except Exception as e:
        viol = f"crash({type(e).__name__})"
    ok2 = sum(fit.compare(2, 2, p, verbose=False)
              for p in itertools.permutations(range(4)))
    perms = list(itertools.permutations(range(6)))
    random.seed(42)
    random.shuffle(perms)
    ok3 = sum(fit.compare(3, 2, p, verbose=False) for p in perms[:40])
    return f"viol={viol}/{len(edges)} 2x2={ok2}/24 3x2={ok3}/40"


for up, th in itertools.product(["skip", "noexit", "eligible"],
                                ["minout", "maxout", "twice"]):
    sim.P.bestnew_upper = up
    sim.P.bestnew_thresh = th
    print(f"upper={up:8s} thresh={th:7s}: {score()}")
