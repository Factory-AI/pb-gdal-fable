import os
#!/usr/bin/env python3
"""Score structural knob combos against the r23 precedence edges plus the
2x2/3x2 cached lattice fits."""
import itertools, json
import sim, fit
import r23 as R

edges = [tuple(e) for e in json.load(open(os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache", "r23edges.json")))]
feats = R.gen_r23()['features']
pts = [(f['geometry']['coordinates'][0], f['geometry']['coordinates'][1])
       for f in feats]


def score():
    try:
        q = sim.Qhull(pts)
    except Exception as e:
        return None, f"crash {e}"
    vid = {p: i for i, p in enumerate(q.insertion_order)}
    viol = [e for e in edges if vid[e[0]] <= vid[e[1]]]
    ok2 = sum(fit.compare(2, 2, perm, verbose=False)
              for perm in itertools.permutations(range(4)))
    return len(viol), f"viol={len(viol)}/{len(edges)} 2x2={ok2}/24"


for pm, ir in itertools.product([False, True], repeat=2):
    sim.P.partitionall_pointmajor = pm
    sim.P.initfacets_reverse = ir
    v, msg = score()
    print(f"pointmajor={pm} initrev={ir}: {msg}")
