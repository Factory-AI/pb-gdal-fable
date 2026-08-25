#!/usr/bin/env python3
import sim

order = (4, 5, 1, 2, 0, 3)
w, h = 3, 2
pts = []
for pid, li in enumerate(order):
    j, i = divmod(li, w)
    pts.append((float(i), float(j)))

q = sim.Qhull(pts)
print("insertion order (pids):", q.insertion_order)
print("lattice of pids:", [order[p] if p < len(order) else "INF"
                           for p in q.insertion_order])
print("tris:", q.result())

# recompute the initial facets and the two distances by hand
q2 = sim.Qhull.__new__(sim.Qhull)
q2.trace = False
q2.log = []
q2.build_input(pts)
q2.detconstants()
q2.visit_id = 0
q2.vertex_id = 1
q2.facet_id = 1
q2.facets = sim.FacetList()
q2.facet_next = None
q2.max_outside = 0.0
q2.insertion_order = []
q2.findbestnew = False
q2.newfacet_first = None
q2.newfacets = []
q2.visible = []
q2.interior_pt = None
q2.initialhull()
print("simplex pids:", q2.insertion_order)
f1 = q2.facets.head
print("f1 verts:", [v.pid for v in f1.vertices], "normal", f1.normal,
      "offset", f1.offset)
for pid in range(q2.numpoints):
    if pid in q2.insertion_order:
        continue
    for fi, f in enumerate(q2.facets):
        d = q2.distplane(pid, f)
        if d >= q2.MINoutside:
            print(f"p{pid} outside f{fi+1}: dist {d!r}")
print("scaled z:", [p[2] for p in q2.pts])
