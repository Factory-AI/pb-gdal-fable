#!/usr/bin/env python3
"""Cleanroom simulator of qhull 2020.2 'd Qbb Qc Qz Qt' (3-D lifted hull of
2-D Delaunay input), reconstructed from oracle probes. Goal: reproduce the
vertex insertion order, merged-facet structure, Qt fan order, and the final
facet list as GDAL's delaunay.c sees it."""
import math

EPS = 2.220446049250313e-16
REALMAX = 1.7976931348623157e308


class P:  # knobs
    scalelast_form = "mul_add"      # z*scale + (-lo*scale)  vs "sub_mul"
    nearzero_factor = 80.0          # NEARzero = f * MAXsumcoord * eps
    distoutside_mode = "minoutside"  # partition threshold
    partitionall_ge = True          # dist >= distoutside (vs >)
    centrum_radius_plus = True      # centrum_radius = premerge_centrum + DISTround
    mergeset_lifo = True            # facet_mergeset processed via setdellast
    flipped_new_thresh = "distround"  # new-facet flipped: dist > -DISTround
    maxpoints_minfirst = True       # qh_maxmin append order per dim
    msx_within_ge = False           # qh_maxsimplex maxpoints loop: det >= vs >
    msx_fallback_ge = False         # qh_maxsimplex all-points loop: det >= vs >
    partitionall_pointmajor = False  # per-point findbest vs facet-major scan
    initfacets_reverse = False      # initial simplex facets: omit asc vs desc
    bestnew_upper = "noexit"        # upper facets in findbestnew: skip|noexit|eligible
    bestnew_thresh = "minout"       # early-exit threshold: minout|maxout|twice|never
    cone_ridge_reverse = True       # iterate visible facet's neighbors reversed
    cone_visible_reverse = False    # iterate visible list reversed


class Vertex:
    __slots__ = ("id", "pid", "newlist")

    def __init__(self, vid, pid):
        self.id = vid
        self.pid = pid


class Ridge:
    __slots__ = ("vertices", "top", "bottom")

    def __init__(self, vertices, top, bottom):
        self.vertices = vertices  # list of Vertex, desc id
        self.top = top
        self.bottom = bottom

    def otherfacet(self, f):
        return self.bottom if self.top is f else self.top


class Facet:
    __slots__ = ("id", "vertices", "neighbors", "ridges", "normal", "offset",
                 "toporient", "upperdelaunay", "simplicial", "visible",
                 "newfacet", "mergehorizon", "coplanarhorizon", "samecycle",
                 "flipped", "outsideset", "furthestdist", "coplanarset",
                 "visitid", "f_replace", "center", "seen", "tested",
                 "degenerate", "prev", "next")

    def __init__(self, fid):
        self.id = fid
        self.vertices = []      # Vertex list desc id
        self.neighbors = []     # Facet list; simplicial: slot k opp vertex k
        self.ridges = None      # list of Ridge or None (simplicial implicit)
        self.normal = None
        self.offset = 0.0
        self.toporient = True
        self.upperdelaunay = False
        self.simplicial = True
        self.visible = False
        self.newfacet = False
        self.mergehorizon = False
        self.coplanarhorizon = False
        self.samecycle = None
        self.flipped = False
        self.outsideset = []
        self.furthestdist = -REALMAX
        self.coplanarset = []
        self.visitid = 0
        self.f_replace = None
        self.center = None
        self.seen = False
        self.tested = False
        self.degenerate = False
        self.prev = None
        self.next = None


def vsort(vs):
    return sorted(vs, key=lambda v: -v.id)


class FacetList:
    """Doubly-linked facet list mirroring qhull's facet_list/facet_tail."""

    def __init__(self):
        self.head = None
        self.tail_sentinel = Facet(-1)  # facet_tail

    def append(self, f):
        # insert before tail sentinel == end of list
        t = self.tail_sentinel
        p = t.prev
        f.prev = p
        f.next = t
        t.prev = f
        if p is not None:
            p.next = f
        else:
            self.head = f

    def remove(self, f):
        p, n = f.prev, f.next
        if p is not None:
            p.next = n
        else:
            self.head = n if n is not self.tail_sentinel else None
        if n is not None:
            n.prev = p
        if self.head is self.tail_sentinel:
            self.head = None
        f.prev = f.next = None

    def __iter__(self):
        f = self.head
        while f is not None and f is not self.tail_sentinel:
            nxt = f.next
            yield f
            f = nxt

    def iter_from(self, f):
        while f is not None and f is not self.tail_sentinel:
            nxt = f.next
            yield f
            f = nxt


class QhullError(Exception):
    pass


class Qhull:
    def __init__(self, pts2d, trace=False):
        self.trace = trace
        self.log = []
        self.build_input(pts2d)
        self.detconstants()
        self.visit_id = 0
        self.vertex_id = 1
        self.facet_id = 1
        self.facets = FacetList()
        self.facet_next = None
        self.max_outside = 0.0
        self.insertion_order = []   # point ids in vertex-creation order
        self.findbestnew = False
        self.run()

    # ---------------- input transform ----------------
    def build_input(self, pts2d):
        n = len(pts2d)
        pts = []
        sx = 0.0
        sy = 0.0
        maxb = 0.0
        for (x, y) in pts2d:
            parab = x * x + y * y
            pts.append([x, y, parab])
            sx += x
            sy += y
            if parab > maxb:
                maxb = parab
        pts.append([sx / n, sy / n, maxb * 1.1])
        self.numpoints = n + 1
        self.ninput = n
        # Qbb: scale last coordinate to [0, maxabs-of-other-coords]
        newhigh = 0.0
        for p in pts:
            for k in (0, 1):
                a = abs(p[k])
                if a > newhigh:
                    newhigh = a
        lo = min(p[2] for p in pts)
        hi = max(p[2] for p in pts)
        if hi - lo == 0.0:
            raise QhullError("degenerate lastcoord")
        scale = newhigh / (hi - lo)
        shift = -lo * scale
        for p in pts:
            if P.scalelast_form == "mul_add":
                p[2] = p[2] * scale + shift
            else:
                p[2] = (p[2] - lo) * scale
        self.pts = pts

    def detconstants(self):
        pts = self.pts
        dim = 3
        maxpoints = []
        maxabs = 0.0
        maxwidth = -REALMAX
        maxsum = 0.0
        for k in range(dim):
            mn = mx = 0
            for i in range(1, self.numpoints):
                if pts[mx][k] < pts[i][k]:
                    mx = i
                elif pts[mn][k] > pts[i][k]:
                    mn = i
            if k == dim - 1:
                maxcoord = maxabs  # SCALElast quirk: assume scaled to MAXabs
            else:
                maxcoord = max(pts[mx][k], -pts[mn][k])
                w = pts[mx][k] - pts[mn][k]
                if w > maxwidth:
                    maxwidth = w
            if maxcoord > maxabs:
                maxabs = maxcoord
            maxsum += maxcoord
            if P.maxpoints_minfirst:
                maxpoints.append(mn)
                maxpoints.append(mx)
            else:
                maxpoints.append(mx)
                maxpoints.append(mn)
        self.maxpoints = maxpoints
        self.MAXabs = maxabs
        self.MAXwidth = maxwidth
        self.MAXsum = maxsum
        maxdistsum = math.sqrt(3.0) * maxabs
        if maxsum < maxdistsum:
            maxdistsum = maxsum
        self.DISTround = EPS * (dim * maxdistsum * 1.01 + maxabs)
        self.ANGLEround = 1.01 * dim * EPS
        self.premerge_centrum = 2 * self.DISTround
        self.MINvisible = self.premerge_centrum
        self.MAXcoplanar = self.MINvisible
        self.MINoutside = 2 * self.MINvisible
        self.WIDEfacet = max(self.MINoutside, 6 * self.MAXcoplanar)
        self.NEARzero = P.nearzero_factor * maxsum * EPS
        self.MINdenom = EPS * maxabs
        self.centrum_radius = self.premerge_centrum + \
            (self.DISTround if P.centrum_radius_plus else 0.0)

    # ---------------- geometry ----------------
    def distplane(self, pid, facet):
        p = self.pts[pid]
        n = facet.normal
        return facet.offset + p[0] * n[0] + p[1] * n[1] + p[2] * n[2]

    def setfacetplane(self, facet):
        rows = [self.pts[v.pid] for v in facet.vertices[:3]]
        dx10 = rows[1][0] - rows[0][0]
        dy10 = rows[1][1] - rows[0][1]
        dz10 = rows[1][2] - rows[0][2]
        dx20 = rows[2][0] - rows[0][0]
        dy20 = rows[2][1] - rows[0][1]
        dz20 = rows[2][2] - rows[0][2]
        n0 = dy20 * dz10 - dz20 * dy10
        n1 = dx10 * dz20 - dz10 * dx20
        n2 = dx20 * dy10 - dy20 * dx10
        norm = math.sqrt(n0 * n0 + n1 * n1 + n2 * n2)
        if norm > self.MINdenom:
            if not facet.toporient:
                norm = -norm
            n0 /= norm
            n1 /= norm
            n2 /= norm
        else:
            # degenerate: qh_normalize2 fallback (max component -> +-1)
            mx = max(abs(n0), abs(n1), abs(n2))
            if mx == 0.0:
                n0, n1, n2 = 0.0, 0.0, (1.0 if facet.toporient else -1.0)
            else:
                s = (1.0 if facet.toporient else -1.0)
                n0 = s * (n0 / mx if abs(n0) == mx else
                          (0.0 if abs(n0) != mx else 0.0))
                # simplified: set largest to +-1, zero others
                vals = [n0, n1, n2]
                out = [0.0, 0.0, 0.0]
                for k in range(3):
                    if abs(vals[k]) == mx:
                        out[k] = s if vals[k] > 0 else -s
                        break
                n0, n1, n2 = out
        facet.normal = (n0, n1, n2)
        p0 = rows[0]
        facet.offset = -(p0[0] * n0 + p0[1] * n1 + p0[2] * n2)
        facet.upperdelaunay = (n2 > -self.ANGLEround * 2.0)
        if self.interior_pt is not None:
            d = facet.offset + self.interior_pt[0] * n0 + \
                self.interior_pt[1] * n1 + self.interior_pt[2] * n2
            facet.flipped = (d > -self.DISTround)

    def dist_interior(self, facet):
        n = facet.normal
        return facet.offset + self.interior_pt[0] * n[0] + \
            self.interior_pt[1] * n[1] + self.interior_pt[2] * n[2]

    # ---------------- maxsimplex / initial hull ----------------
    def detsimplex(self, cand, simplex, k):
        c = self.pts[cand]
        rows = [[self.pts[simplex[i]][j] - c[j] for j in range(k)]
                for i in range(k)]
        if k == 2:
            det = rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0]
            nz = abs(det) < 10 * self.NEARzero
        else:
            det = (rows[0][0] * (rows[1][1] * rows[2][2] -
                                 rows[1][2] * rows[2][1])
                   - rows[0][1] * (rows[1][0] * rows[2][2] -
                                   rows[1][2] * rows[2][0])
                   + rows[0][2] * (rows[1][0] * rows[2][1] -
                                   rows[1][1] * rows[2][0]))
            nz = abs(det) < 10 * self.NEARzero
        return det, nz

    def maxsimplex(self):
        maxx = minx = None
        maxc, minc = -REALMAX, REALMAX
        for i in self.maxpoints:
            x = self.pts[i][0]
            if maxc < x:
                maxc = x
                maxx = i
            if minc > x:
                minc = x
                minx = i
        simplex = [minx]
        if maxx not in simplex:
            simplex.append(maxx)
        for k in range(len(simplex), 4):
            best, bestdet, bestnz = None, -REALMAX, False
            for i in self.maxpoints:
                if i in simplex:
                    continue
                d, nz = self.detsimplex(i, simplex, k)
                d = abs(d)
                if (d >= bestdet) if P.msx_within_ge else (d > bestdet):
                    bestdet, best, bestnz = d, i, nz
            if best is None or bestnz:
                for i in range(self.numpoints):
                    if i == best or i in simplex:
                        continue
                    d, nz = self.detsimplex(i, simplex, k)
                    d = abs(d)
                    if (d >= bestdet) if P.msx_fallback_ge else \
                            (d > bestdet):
                        bestdet, best, bestnz = d, i, nz
            if best is None:
                raise QhullError("maxsimplex failed")
            simplex.append(best)
        return simplex

    def initialhull(self):
        simplex = self.maxsimplex()
        verts = []
        for pid in simplex:
            v = Vertex(self.vertex_id, pid)
            self.vertex_id += 1
            self.insertion_order.append(pid)
            verts.append(v)
        desc = vsort(verts)
        # interior point = mean of simplex vertices (desc order sum)
        self.interior_pt = [
            sum(self.pts[v.pid][k] for v in desc) / 4.0 for k in range(3)]
        order = list(range(4))
        if P.initfacets_reverse:
            order.reverse()
        by_omit = {}
        facets = []
        toporient = True
        for k in order:
            f = Facet(self.facet_id)
            self.facet_id += 1
            f.vertices = desc[:k] + desc[k + 1:]
            f.toporient = toporient
            toporient = not toporient
            self.facets.append(f)
            by_omit[k] = f
            facets.append(f)
        for k in range(4):
            by_omit[k].neighbors = [by_omit[j] for j in range(4) if j != k]
        first = facets[0]
        self.interior_pt_tmp = None
        # first facet plane; global flip if flipped
        self.setfacetplane(first)
        if first.flipped:
            first.flipped = False
            for f in facets:
                f.toporient = not f.toporient
            self.setfacetplane(first)
        for f in facets:
            if f is not first:
                self.setfacetplane(f)
            if f.flipped:
                raise QhullError("flat")
        self.facet_next = self.facets.head

    # ---------------- partitioning ----------------
    def distoutside(self):
        return self.MINoutside

    def partitionall(self):
        vertpids = set(self.insertion_order)
        if P.partitionall_pointmajor:
            for pid in range(self.numpoints):
                if pid not in vertpids:
                    self.partitionpoint(pid, self.facets.head)
            return
        pointset = [i if i not in vertpids else None
                    for i in range(self.numpoints)]
        for facet in self.facets:
            best = None
            bestdist = -REALMAX
            for i, pid in enumerate(pointset):
                if pid is None:
                    continue
                dist = self.distplane(pid, facet)
                th = self.distoutside()
                out = dist >= th if P.partitionall_ge else dist > th
                if out:
                    if best is None:
                        best, bestdist = pid, dist
                    elif dist > bestdist:
                        facet.outsideset.append(best)
                        best, bestdist = pid, dist
                    else:
                        facet.outsideset.append(pid)
                    pointset[i] = None
            if best is not None:
                facet.outsideset.append(best)
                facet.furthestdist = bestdist
        # remaining points: coplanar candidates (Qc); qhull calls
        # qh_partitioncoplanar per point with bestfacet search
        for pid in pointset:
            if pid is None:
                continue
            self.partitioncoplanar(pid, self.facets.head, None)

    def findbesthorizon(self, pid, startfacet, bestdist):
        searchdist = (self.max_outside + 2 * self.DISTround) + \
            max(self.MINvisible, self.MAXcoplanar)
        self.visit_id += 1
        vid = self.visit_id
        startfacet.visitid = vid
        best = startfacet
        queue = [startfacet]
        qi = 0
        while qi < len(queue):
            f = queue[qi]
            qi += 1
            for nb in self.neighbors_of(f):
                if nb.visitid == vid:
                    continue
                nb.visitid = vid
                if nb.visible or nb.flipped:
                    continue
                d = self.distplane(pid, nb)
                if d > bestdist and not nb.upperdelaunay:
                    bestdist = d
                    best = nb
                    queue.append(nb)
                elif d > bestdist - searchdist:
                    queue.append(nb)
        return best, bestdist

    def neighbors_of(self, f):
        if f.ridges is not None:
            return [r.otherfacet(f) for r in f.ridges]
        return f.neighbors

    def findbest(self, pid, startfacet, bestoutside, isnewfacets, noupper):
        """hill-climb; returns (facet, dist, isoutside)"""
        self.visit_id += 1
        vid = self.visit_id
        facet = startfacet
        while facet.visible:
            facet = facet.f_replace or facet.next
        facet.visitid = vid
        dist = self.distplane(pid, facet)
        bestdist = dist
        bestfacet = facet
        if not bestoutside and dist >= self.MINoutside and \
                not (facet.upperdelaunay and noupper):
            return facet, dist, True
        improved = True
        while improved:
            improved = False
            for nb in self.neighbors_of(bestfacet):
                if nb.visitid == vid:
                    continue
                nb.visitid = vid
                if nb.visible or nb.flipped:
                    continue
                d = self.distplane(pid, nb)
                if nb.upperdelaunay and noupper and d < self.MINoutside:
                    continue
                if not bestoutside and d >= self.MINoutside and \
                        not (nb.upperdelaunay and noupper):
                    return nb, d, True
                if d > bestdist:
                    bestdist = d
                    bestfacet = nb
                    improved = True
                    break
        bestfacet, bestdist = self.findbesthorizon(pid, bestfacet, bestdist)
        return bestfacet, bestdist, bestdist >= self.MINoutside

    def bestnew_exit_thresh(self):
        if P.bestnew_thresh == "minout":
            return self.MINoutside
        if P.bestnew_thresh == "maxout":
            return max(self.max_outside + 2 * self.DISTround,
                       self.MINoutside)
        if P.bestnew_thresh == "never":
            return REALMAX
        return 2 * (self.max_outside + self.MINoutside)

    def findbestnew_search(self, pid, startfacet):
        """scan new facets in list order from startfacet; early exit at the
        first clearly-outside facet."""
        thresh = self.bestnew_exit_thresh()
        bestdist = -REALMAX
        bestfacet = None
        isout = False
        f = startfacet
        while f is not None and f is not self.facets.tail_sentinel:
            if not f.flipped and not f.visible and f.normal is not None:
                if not (f.upperdelaunay and P.bestnew_upper == "skip"):
                    d = self.distplane(pid, f)
                    if d > bestdist and (not f.upperdelaunay or
                                         d >= self.MINoutside or
                                         P.bestnew_upper == "eligible"):
                        bestdist = d
                        bestfacet = f
                        if d >= thresh and (not f.upperdelaunay or
                                            P.bestnew_upper == "eligible"):
                            isout = True
                            break
            f = f.next
        if bestfacet is None:
            for f in self.iter_new():
                if f.visible or f.normal is None:
                    continue
                d = self.distplane(pid, f)
                if d > bestdist:
                    bestdist = d
                    bestfacet = f
        if bestfacet is None:
            bestfacet = self.facets.head
            bestdist = self.distplane(pid, bestfacet)
        if not isout:
            bestfacet, bestdist = self.findbesthorizon(pid, bestfacet,
                                                       bestdist)
            isout = bestdist >= self.MINoutside
        return bestfacet, bestdist, isout

    def iter_new(self):
        return iter(self.newfacets)

    def partitionpoint(self, pid, startfacet):
        if self.findbestnew:
            bestfacet, bestdist, isout = self.findbestnew_search(pid,
                                                                 startfacet)
        else:
            bestfacet, bestdist, isout = self.findbest(
                pid, startfacet, False, True, False)
        if isout:
            if not bestfacet.outsideset:
                bestfacet.outsideset.append(pid)
                bestfacet.furthestdist = bestdist
                if not bestfacet.newfacet:
                    # keep facet after qh.facet_next: move to end of list
                    self.remove_facet_keepnext(bestfacet)
                    self.facets.append(bestfacet)
            else:
                if bestdist > bestfacet.furthestdist:
                    bestfacet.outsideset.append(pid)
                    bestfacet.furthestdist = bestdist
                else:
                    bestfacet.outsideset.insert(len(bestfacet.outsideset) - 1,
                                                pid)
        else:
            # Delaunay: keep coplanar (Qc)
            self.partitioncoplanar(pid, bestfacet, bestdist)

    def partitioncoplanar(self, pid, facet, dist):
        if dist is None:
            bestfacet, bestdist, isout = self.findbest(
                pid, facet, True, False, True)
        else:
            bestfacet, bestdist = facet, dist
        if bestdist > self.max_outside:
            self.max_outside = bestdist
        bestfacet.coplanarset.append(pid)

    def remove_facet_keepnext(self, f):
        if self.facet_next is f:
            self.facet_next = f.next
        if self.newfacet_first is f:
            self.newfacet_first = f.next
        self.facets.remove(f)

    # ---------------- cone building ----------------
    def nextfurthest(self):
        f = self.facet_next
        while f is not None and f is not self.facets.tail_sentinel:
            if not f.outsideset:
                self.facet_next = f.next
                f = f.next
                continue
            return f, f.outsideset[-1]
        return None, None

    def findhorizon(self, pid, facet):
        # visible facets moved to end of facet_list, in BFS order
        self.remove_facet_keepnext(facet)
        self.facets.append(facet)
        facet.visible = True
        facet.f_replace = None
        self.visible = [facet]
        self.visit_id += 1
        vid = self.visit_id
        facet.visitid = vid
        i = 0
        while i < len(self.visible):
            vis = self.visible[i]
            i += 1
            for nb in self.neighbors_of(vis):
                if nb.visitid == vid:
                    continue
                nb.visitid = vid
                dist = self.distplane(pid, nb)
                if dist > self.MINvisible:
                    self.remove_facet_keepnext(nb)
                    self.facets.append(nb)
                    nb.visible = True
                    nb.f_replace = None
                    self.visible.append(nb)
                else:
                    nb.coplanarhorizon = (dist >= -self.MAXcoplanar)

    def facetintersect(self, fA, fB):
        """shared vertices (desc) and skip index in each (simplicial)"""
        inter = [v for v in fA.vertices if v in fB.vertices]
        skipA = next(i for i, v in enumerate(fA.vertices)
                     if v not in fB.vertices)
        skipB = next(i for i, v in enumerate(fB.vertices)
                     if v not in fA.vertices)
        return inter, skipA, skipB

    def makenewfacet(self, apex, ridgeverts, toporient, horizon):
        f = Facet(self.facet_id)
        self.facet_id += 1
        f.vertices = [apex] + list(ridgeverts)
        f.toporient = toporient
        f.newfacet = True
        f.neighbors = [horizon, None, None]
        self.facets.append(f)
        if self.newfacet_first is None:
            self.newfacet_first = f
        return f

    def makenewfacets(self, pid):
        apex = Vertex(self.vertex_id, pid)
        self.vertex_id += 1
        self.insertion_order.append(pid)
        self.newfacet_first = None
        newfacets = []
        horizon_cycles = {}  # horizon facet id -> samecycle head
        vislist = list(self.visible)
        if P.cone_visible_reverse:
            vislist.reverse()
        for vis in vislist:
            if vis.simplicial:
                slots = list(enumerate(vis.neighbors))
                if P.cone_ridge_reverse:
                    slots.reverse()
                for k, nb in slots:
                    if nb.visible:
                        continue
                    ridge = vis.vertices[:k] + vis.vertices[k + 1:]
                    # toporient from horizon's slot of vis
                    hk = nb.neighbors.index(vis) if nb.simplicial else None
                    if nb.simplicial:
                        t = nb.toporient if (hk & 1) else not nb.toporient
                    else:
                        # horizon nonsimplicial: use its ridge to vis
                        r = next(r for r in nb.ridges
                                 if r.otherfacet(nb) is vis)
                        t = (r.top is vis)
                    nf = self.makenewfacet(apex, ridge, t, nb)
                    newfacets.append(nf)
                    if nb.coplanarhorizon:
                        nf.mergehorizon = True
                        key = nb.id
                        if key in horizon_cycles:
                            horizon_cycles[key].append(nf)
                        else:
                            horizon_cycles[key] = [nf]
                            nf.samecycle = horizon_cycles[key]
                        nf.samecycle = horizon_cycles[key]
            else:
                rlist = list(vis.ridges)
                if P.cone_ridge_reverse:
                    rlist.reverse()
                for r in rlist:
                    nb = r.otherfacet(vis)
                    if nb.visible:
                        continue
                    t = (r.top is vis)
                    nf = self.makenewfacet(apex, r.vertices, t, nb)
                    newfacets.append(nf)
                    if nb.coplanarhorizon:
                        nf.mergehorizon = True
                        key = nb.id
                        if key in horizon_cycles:
                            horizon_cycles[key].append(nf)
                        else:
                            horizon_cycles[key] = [nf]
                        nf.samecycle = horizon_cycles[key]
        self.newfacets = newfacets
        self.apex = apex
        # attach: replace visible with newfacet in horizon neighbor/ridges
        for nf in newfacets:
            hz = nf.neighbors[0]
            vis = None
            if hz.simplicial:
                for k, nb in enumerate(hz.neighbors):
                    if nb.visible:
                        # ensure the shared ridge matches nf's ridge verts
                        rid = hz.vertices[:k] + hz.vertices[k + 1:]
                        if all(v in nf.vertices for v in rid):
                            hz.neighbors[k] = nf
                            break
            else:
                for r in hz.ridges:
                    of = r.otherfacet(hz)
                    if of.visible and \
                            all(v in nf.vertices for v in r.vertices):
                        if r.top is hz:
                            r.bottom = nf
                        else:
                            r.top = nf
                        nf.ridges = nf.ridges or None
                        # nf stays simplicial; ridge belongs to horizon side
                        break
                # horizon's neighbor list update
                for k, nb in enumerate(hz.neighbors):
                    if nb.visible:
                        rid_ok = True
                        # find matching entry by ridge
                        pass
                hz.neighbors = [nf if (n.visible and self._shares_ridge(
                    hz, n, nf)) else n for n in hz.neighbors]
        # match new facets to each other: slot k>0 opposite vertices[k]
        edgemap = {}
        for nf in newfacets:
            for k in (1, 2):
                edge = tuple(sorted((nf.vertices[0].id,
                                     nf.vertices[3 - k].id)))
                # neighbor opposite vertices[k] shares apex+vertices[3-k]
                if edge in edgemap:
                    other, ok = edgemap.pop(edge)
                    nf.neighbors[k] = other
                    other.neighbors[ok] = nf
                else:
                    edgemap[edge] = (nf, k)
        if edgemap:
            raise QhullError("unmatched new facets (dupridge?)")

    def _shares_ridge(self, hz, vis, nf):
        return True  # simplification for neighbor replacement

    def makenewplanes(self):
        for nf in self.newfacets:
            if not nf.mergehorizon:
                self.setfacetplane(nf)

    # ---------------- merging ----------------
    def checkzero(self):
        for nf in self.newfacets:
            hz = nf.neighbors[0]
            if not hz.simplicial:
                return False
            if nf.flipped or nf.mergehorizon:
                return False
        for nf in self.newfacets:
            hz = nf.neighbors[0]
            hzverts = set(v.id for v in hz.vertices)
            for v in nf.vertices:
                if v.id not in hzverts:
                    d = self.distplane(v.pid, hz)
                    if d >= -self.MINvisible:
                        return False
            for k in (1, 2):
                nb = nf.neighbors[k]
                d = self.distplane(nf.vertices[k].pid, nb)
                if d >= -self.MINvisible:
                    return False
        return True

    def makeridges(self, f):
        if f.ridges is not None:
            return
        f.ridges = []
        for k, nb in enumerate(f.neighbors):
            rv = f.vertices[:k] + f.vertices[k + 1:]
            sense = f.toporient ^ (k & 1 == 1)
            # check if neighbor already has this ridge
            existing = None
            if nb.ridges is not None:
                for r in nb.ridges:
                    if r.otherfacet(nb) is f and \
                            set(v.id for v in r.vertices) == \
                            set(v.id for v in rv):
                        existing = r
                        break
            if existing is not None:
                f.ridges.append(existing)
            else:
                if sense:
                    r = Ridge(rv, f, nb)
                else:
                    r = Ridge(rv, nb, f)
                f.ridges.append(r)
                if nb.ridges is not None:
                    nb.ridges.append(r)
        f.simplicial = False

    def replace_neighbor(self, f, old, new):
        for k, nb in enumerate(f.neighbors):
            if nb is old:
                f.neighbors[k] = new
                return

    def willdelete(self, f, replace):
        f.visible = True
        f.f_replace = replace
        self.remove_facet_keepnext(f)
        self.facets.append(f)
        self.visible.append(f)

    def mergefacet(self, f1, f2):
        """merge f1 into f2 (f2 keeps id, plane, position)"""
        self.makeridges(f1)
        self.makeridges(f2)
        shared = [r for r in f1.ridges if r.otherfacet(f1) is f2]
        for r in shared:
            f1.ridges.remove(r)
            f2.ridges.remove(r)
        for r in f1.ridges:
            if r.top is f1:
                r.top = f2
            else:
                r.bottom = f2
            f2.ridges.append(r)
        # vertices union desc
        have = set(v.id for v in f2.vertices)
        f2.vertices = vsort(f2.vertices +
                            [v for v in f1.vertices if v.id not in have])
        # neighbors union
        for nb in f1.neighbors:
            if nb is f2 or nb in f2.neighbors:
                continue
            f2.neighbors.append(nb)
        for nb in f1.neighbors:
            if nb is not f2:
                self.replace_neighbor(nb, f1, f2)
        f2.neighbors = [n for n in f2.neighbors if n is not f1]
        f2.simplicial = False
        f2.center = None
        # move points
        f2.outsideset += f1.outsideset
        f1.outsideset = []
        f2.coplanarset += f1.coplanarset
        f1.coplanarset = []
        if f1.newfacet and not f2.newfacet:
            f2.newfacet = f2.newfacet  # horizon merge: f2 stays old
        self.willdelete(f1, f2)

    def mergecycle_all(self):
        merged = False
        for nf in list(self.newfacets):
            if nf.visible or not nf.mergehorizon:
                continue
            cycle = nf.samecycle or [nf]
            horizon = nf.neighbors[0]
            for cf in cycle:
                if cf.visible:
                    continue
                self.mergefacet(cf, horizon)
                merged = True
        return merged

    def getcentrum(self, f):
        if f.center is None:
            n = len(f.vertices)
            f.center = [sum(self.pts[v.pid][k] for v in f.vertices) / n
                        for k in range(3)]
        return f.center

    def dist_centrum(self, c, facet):
        n = facet.normal
        return facet.offset + c[0] * n[0] + c[1] * n[1] + c[2] * n[2]

    def test_appendmerge(self, f, nb):
        """C-0 zero-centrum style convexity test; returns merge or None"""
        c = self.getcentrum(f)
        d = self.dist_centrum(c, nb)
        if d >= -self.centrum_radius:
            return (f, nb, d)
        return None

    def premerge(self):
        if self.checkzero():
            return
        # flipped merges
        for nf in list(self.newfacets):
            if nf.flipped and not nf.visible and not nf.mergehorizon:
                best, bestdist = None, REALMAX
                for nb in self.neighbors_of(nf):
                    if nb.visible or nb.normal is None:
                        continue
                    dmax = -REALMAX
                    for v in nf.vertices:
                        d = self.distplane(v.pid, nb)
                        if d > dmax:
                            dmax = d
                    if dmax < bestdist:
                        bestdist = dmax
                        best = nb
                if best is not None:
                    self.mergefacet(nf, best)
        # horizon merges
        self.mergecycle_all()
        # centrum merges among new facets & neighbors
        mergeset = []
        for nf in self.newfacets:
            if nf.visible or nf.normal is None:
                continue
            for nb in self.neighbors_of(nf):
                if nb.visible or nb.normal is None:
                    continue
                if nb.newfacet and nb.id < nf.id:
                    continue  # test each new-new pair once
                m = self.test_appendmerge(nf, nb)
                if m is not None:
                    mergeset.append(m)
        while mergeset:
            f, nb, d = mergeset.pop() if P.mergeset_lifo else mergeset.pop(0)
            if f.visible or nb.visible:
                continue
            self.mergefacet(f, nb)
            # retest merged facet's neighbors
            for nb2 in self.neighbors_of(nb):
                if nb2.visible or nb2.normal is None or nb is nb2:
                    continue
                m = self.test_appendmerge(nb, nb2)
                if m is not None and m not in mergeset:
                    mergeset.append(m)

    # ---------------- cone driver ----------------
    def addpoint(self, pid, facet):
        facet.outsideset.pop()
        self.findhorizon(pid, facet)
        self.makenewfacets(pid)
        self.makenewplanes()
        self.premerge()
        self.findbestnew = True
        # partitionvisible
        for vis in self.visible:
            if not vis.outsideset and not vis.coplanarset:
                continue
            start = self.newfacet_first
            if start is None or start is self.facets.tail_sentinel:
                start = self.facets.head
            for p in vis.outsideset:
                self.partitionpoint(p, start)
            vis.outsideset = []
            for p in vis.coplanarset:
                if self.findbestnew:
                    bf, bd, isout = self.findbestnew_search(p, start)
                else:
                    bf, bd, isout = self.findbest(p, start, True, True, False)
                if isout:
                    # promote to outside set
                    if not bf.outsideset:
                        bf.outsideset.append(p)
                        bf.furthestdist = bd
                        if not bf.newfacet:
                            self.remove_facet_keepnext(bf)
                            self.facets.append(bf)
                    elif bd > bf.furthestdist:
                        bf.outsideset.append(p)
                        bf.furthestdist = bd
                    else:
                        bf.outsideset.insert(len(bf.outsideset) - 1, p)
                else:
                    if bd > self.max_outside:
                        self.max_outside = bd
                    bf.coplanarset.append(p)
            vis.coplanarset = []
        self.findbestnew = False
        # deletevisible
        for vis in self.visible:
            self.facets.remove(vis)
        self.visible = []
        # resetlists
        for nf in self.newfacets:
            nf.newfacet = False
            nf.mergehorizon = False
        for f in self.facets:
            f.coplanarhorizon = False
        self.newfacets = []
        self.newfacet_first = None

    def run(self):
        self.newfacet_first = None
        self.newfacets = []
        self.visible = []
        self.interior_pt = None
        self.initialhull()
        self.partitionall()
        while True:
            facet, pid = self.nextfurthest()
            if facet is None:
                break
            self.addpoint(pid, facet)
        self.triangulate()

    # ---------------- Qt ----------------
    def triangulate(self):
        for facet in list(self.facets):
            if facet.simplicial or facet.visible:
                continue
            apex = facet.vertices[0]
            fans = []
            for r in facet.ridges:
                if apex in r.vertices:
                    continue
                nf = Facet(self.facet_id)
                self.facet_id += 1
                nf.vertices = [apex] + list(r.vertices)
                nf.upperdelaunay = facet.upperdelaunay
                nf.normal = facet.normal
                nf.offset = facet.offset
                nf.simplicial = True
                self.facets.append(nf)
                # repoint ridge to fan facet for adjacent merged facets
                other = r.otherfacet(facet)
                if r.top is facet:
                    r.top = nf
                else:
                    r.bottom = nf
                fans.append(nf)
            self.facets.remove(facet)

    def result(self):
        """list of (pid triple in facet vertex order) for lower facets"""
        tris = []
        for f in self.facets:
            if f.upperdelaunay:
                continue
            pids = [v.pid for v in f.vertices]
            if len(pids) != 3:
                raise QhullError("non-simplicial in result")
            if any(p >= self.ninput for p in pids):
                continue  # infinity point facet (shouldn't be lower)
            tris.append(tuple(pids))
        return tris
