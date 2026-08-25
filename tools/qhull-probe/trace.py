import os
#!/usr/bin/env python3
"""Trace sim pops and stop at first oracle-constraint violation, dumping
full facet state."""
import json, sys
import sim
import r23 as R

sim.P.bestnew_upper = sys.argv[1] if len(sys.argv) > 1 else "noexit"
sim.P.bestnew_thresh = "minout"

edges = [tuple(e) for e in json.load(open(os.path.join(os.environ.get("QHPROBE_DIR", os.path.dirname(os.path.abspath(__file__))), "cache", "r23edges.json")))]
feats = R.gen_r23()['features']
pts = [(f['geometry']['coordinates'][0], f['geometry']['coordinates'][1])
       for f in feats]
after = {}
for a, b in edges:
    after.setdefault(a, set()).add(b)

state = {"inserted": None, "stop": False}
orig_next = sim.Qhull.nextfurthest


def patched(self):
    if state["inserted"] is None:
        state["inserted"] = set(self.insertion_order)
        print("simplex:", self.insertion_order)
    f, pid = orig_next(self)
    if f is None or state["stop"]:
        return f, pid
    need = after.get(pid, set()) - state["inserted"]
    pos = 0
    for g in self.facets:
        if g is f:
            break
        pos += 1
    print(f"pop: p{pid} from facet{f.id} verts={[v.pid for v in f.vertices]}"
          f" (listpos {pos}) oset={f.outsideset}"
          + (f"  VIOL needs {sorted(need)}" if need else ""))
    if need:
        state["stop"] = True
        print("=== full state ===")
        i = 0
        for g in self.facets:
            mark = " <-- next" if g is self.facet_next else ""
            print(f"[{i}] facet{g.id} verts={[v.pid for v in g.vertices]} "
                  f"upper={int(g.upperdelaunay)} oset={g.outsideset} "
                  f"cop={g.coplanarset}{mark}")
            i += 1
        for x in sorted(need | {pid}):
            print(f"p{x} xy={pts[x] if x < len(pts) else 'INF'}")
            for g in self.facets:
                if g.normal is None:
                    continue
                d = self.distplane(x, g)
                if d > -1e-7:
                    print(f"   dist facet{g.id} "
                          f"{[v.pid for v in g.vertices]}: {d!r}")
        raise SystemExit
    state["inserted"].add(pid)
    return f, pid


sim.Qhull.nextfurthest = patched
q = sim.Qhull(pts)
print("finished with no violation")
