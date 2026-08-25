#include "engine.h"
#include "cpl.h"
#include "dataset.h"
#include "gtiff_write.h"
#include "ogr.h"
#include "ogrsql.h"
#include "progress.h"
#include "srs.h"
#include "util.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr double kCoincEps = 0.0000000000001;

struct GridCfg
{
    std::string alg;  // leaf name (invdist, average, ...)
    double power = 2.0, smoothing = 0.0;
    double radius1 = 0.0, radius2 = 0.0, angle = 0.0, nodata = 0.0;
    bool radiusGroupSet = false;
    int minPoints = 0;
    int maxPoints = 0;
    bool maxPointsSet = false;
    int minPPQ = 0;
    int maxPPQ = 2147483647;
    double linearRadius = std::numeric_limits<double>::infinity();

    bool quadrant() const { return minPPQ > 0 || maxPPQ != 2147483647; }
};

struct GridPoints
{
    std::vector<double> x, y, z;
    std::vector<float> fx, fy, fz;
    size_t size() const { return x.size(); }
    mutable std::vector<int> qtOrder;
    mutable bool qtOrderBuilt = false;
};

// ------------------------------------------------------------------
// CPLQuadTree emulation: the reference routes invdistnn (and the
// per-quadrant variants) through a quadtree whose DFS traversal order
// decides which of exactly-tied-distance points survives the
// max-points cut (multimap keeps the first-inserted of equal keys).
// Probed: bucket capacity 8 (split on the 9th insert), split ratio
// 0.55 with overlapping halves (longer axis first, ties split Y),
// subnode order low-half-low, low-half-high, high-half-low,
// high-half-high, features redistributed on split, first-containing
// subnode wins, leaf order = insertion order.
// ------------------------------------------------------------------
struct QtRect
{
    double minx, miny, maxx, maxy;
};

void qtSplit(const QtRect &in, QtRect &o1, QtRect &o2)
{
    constexpr double ratio = 0.55;
    o1 = in;
    o2 = in;
    if (in.maxx - in.minx > in.maxy - in.miny)
    {
        o1.maxx = in.minx + (in.maxx - in.minx) * ratio;
        o2.minx = in.maxx - (in.maxx - in.minx) * ratio;
    }
    else
    {
        o1.maxy = in.miny + (in.maxy - in.miny) * ratio;
        o2.miny = in.maxy - (in.maxy - in.miny) * ratio;
    }
}

struct QtNode
{
    QtRect rect;
    std::vector<int> feats;
    std::unique_ptr<QtNode> sub[4];
    bool hasSub = false;
};

void qtAdd(QtNode *node, int idx, const GridPoints &pts, int depth)
{
    if (!node->hasSub)
    {
        // depth guard against co-located clusters that can never split
        // apart
        if ((int)node->feats.size() >= 8 && depth < 64)
        {
            QtRect h1, h2, q[4];
            qtSplit(node->rect, h1, h2);
            qtSplit(h1, q[0], q[1]);
            qtSplit(h2, q[2], q[3]);
            node->hasSub = true;
            for (int s = 0; s < 4; ++s)
            {
                node->sub[s].reset(new QtNode());
                node->sub[s]->rect = q[s];
            }
            std::vector<int> old = std::move(node->feats);
            node->feats.clear();
            for (int f : old)
                qtAdd(node, f, pts, depth);
            qtAdd(node, idx, pts, depth);
            return;
        }
        node->feats.push_back(idx);
        return;
    }
    const double x = pts.x[(size_t)idx], y = pts.y[(size_t)idx];
    for (int s = 0; s < 4; ++s)
    {
        const QtRect &r = node->sub[s]->rect;
        if (x >= r.minx && x <= r.maxx && y >= r.miny && y <= r.maxy)
        {
            qtAdd(node->sub[s].get(), idx, pts, depth + 1);
            return;
        }
    }
    node->feats.push_back(idx);
}

void qtCollect(const QtNode *node, std::vector<int> &out)
{
    for (int f : node->feats)
        out.push_back(f);
    if (node->hasSub)
        for (int s = 0; s < 4; ++s)
            qtCollect(node->sub[s].get(), out);
}

const std::vector<int> &gridQtOrder(const GridPoints &pts)
{
    if (!pts.qtOrderBuilt)
    {
        pts.qtOrderBuilt = true;
        const size_t n = pts.size();
        pts.qtOrder.reserve(n);
        if (n)
        {
            QtNode root;
            root.rect = {pts.x[0], pts.y[0], pts.x[0], pts.y[0]};
            for (size_t i = 1; i < n; ++i)
            {
                root.rect.minx = std::min(root.rect.minx, pts.x[i]);
                root.rect.miny = std::min(root.rect.miny, pts.y[i]);
                root.rect.maxx = std::max(root.rect.maxx, pts.x[i]);
                root.rect.maxy = std::max(root.rect.maxy, pts.y[i]);
            }
            for (size_t i = 0; i < n; ++i)
                qtAdd(&root, (int)i, pts, 0);
            qtCollect(&root, pts.qtOrder);
        }
    }
    return pts.qtOrder;
}

// ------------------------------------------------------------------
// search-ellipse helper (GDAL gdalgrid.cpp semantics: rotation replaces
// the deltas, the membership test never divides)
// ------------------------------------------------------------------
struct Ellipse
{
    bool active = false;
    bool rotated = false;
    double c1 = 1.0, c2 = 0.0;
    double r1sq = 0, r2sq = 0, r12sq = 0;

    void init(const GridCfg &c)
    {
        active = c.radius1 != 0.0 || c.radius2 != 0.0;
        r1sq = c.radius1 * c.radius1;
        r2sq = c.radius2 * c.radius2;
        r12sq = r1sq * r2sq;
        rotated = c.angle != 0.0;
        if (rotated)
        {
            const double a = c.angle * (M_PI / 180.0);
            c1 = cos(a);
            c2 = sin(a);
        }
    }
    // rx/ry are replaced by their rotated values when an angle is set
    bool contains(double &rx, double &ry) const
    {
        if (rotated)
        {
            const double rxr = rx * c1 + ry * c2;
            const double ryr = ry * c1 - rx * c2;
            rx = rxr;
            ry = ryr;
        }
        if (!active)
            return true;
        return r2sq * rx * rx + r1sq * ry * ry <= r12sq;
    }
};

// ------------------------------------------------------------------
// exact float32 replica of the reference's AVX inverse-distance path
// (16-wide single accumulator, vrcpps weights, sequential hsum, exact
// scalar remainder)
// ------------------------------------------------------------------
__attribute__((target("avx"))) double
invdistAvx(const float *px, const float *py, const float *pz, size_t n,
           float fx, float fy)
{
    const __m256 small = _mm256_set1_ps(0.0000000000001f);
    const __m256 vx = _mm256_set1_ps(fx), vy = _mm256_set1_ps(fy);
    __m256 nomA = _mm256_setzero_ps(), denA = _mm256_setzero_ps();
    int mask = 0;
    size_t i = 0;
    const size_t nRound = (n / 16) * 16;
    for (; i < nRound; i += 16)
    {
        const __m256 rxA = _mm256_sub_ps(_mm256_loadu_ps(px + i), vx);
        const __m256 ryA = _mm256_sub_ps(_mm256_loadu_ps(py + i), vy);
        const __m256 rxB = _mm256_sub_ps(_mm256_loadu_ps(px + i + 8), vx);
        const __m256 ryB = _mm256_sub_ps(_mm256_loadu_ps(py + i + 8), vy);
        const __m256 r2A =
            _mm256_add_ps(_mm256_mul_ps(rxA, rxA), _mm256_mul_ps(ryA, ryA));
        const __m256 r2B =
            _mm256_add_ps(_mm256_mul_ps(rxB, rxB), _mm256_mul_ps(ryB, ryB));
        mask = _mm256_movemask_ps(_mm256_cmp_ps(r2A, small, _CMP_LT_OS)) |
               (_mm256_movemask_ps(_mm256_cmp_ps(r2B, small, _CMP_LT_OS))
                << 8);
        if (mask)
            break;
        const __m256 invA = _mm256_rcp_ps(r2A), invB = _mm256_rcp_ps(r2B);
        nomA = _mm256_add_ps(nomA,
                             _mm256_mul_ps(invA, _mm256_loadu_ps(pz + i)));
        denA = _mm256_add_ps(denA, invA);
        nomA = _mm256_add_ps(
            nomA, _mm256_mul_ps(invB, _mm256_loadu_ps(pz + i + 8)));
        denA = _mm256_add_ps(denA, invB);
    }
    if (mask)
    {
        for (size_t k = i; k < n; ++k)
        {
            const float rx = px[k] - fx, ry = py[k] - fy;
            if (rx * rx + ry * ry < 0.0000000000001f)
                return pz[k];
        }
        return pz[i];  // unreachable: SIMD and scalar r2 agree exactly
    }
    float cn[8], cd[8];
    _mm256_storeu_ps(cn, nomA);
    _mm256_storeu_ps(cd, denA);
    float fNom = cn[0] + cn[1] + cn[2] + cn[3] + cn[4] + cn[5] + cn[6] +
                 cn[7];
    float fDen = cd[0] + cd[1] + cd[2] + cd[3] + cd[4] + cd[5] + cd[6] +
                 cd[7];
    for (; i < n; ++i)
    {
        const float rx = px[i] - fx, ry = py[i] - fy;
        const float r2 = rx * rx + ry * ry;
        if (r2 < 0.0000000000001f)
            return pz[i];
        const float inv = 1.0f / r2;
        fNom += pz[i] * inv;
        fDen += inv;
    }
    return (double)(fNom / fDen);
}

double invdistFloatScalar(const float *px, const float *py, const float *pz,
                          size_t n, float fx, float fy)
{
    float fNom = 0.0f, fDen = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        const float rx = px[i] - fx, ry = py[i] - fy;
        const float r2 = rx * rx + ry * ry;
        if (r2 < 0.0000000000001f)
            return pz[i];
        const float inv = 1.0f / r2;
        fNom += pz[i] * inv;
        fDen += inv;
    }
    return (double)(fNom / fDen);
}

bool cpuHasAvx()
{
    static const bool has = __builtin_cpu_supports("avx");
    return has;
}

// ------------------------------------------------------------------
// nearest-first selection shared by the per-quadrant variants and
// invdistnn: ascending distance with quadrant-index insertion order on
// ties (SW, SE, NW, NE), per-quadrant cap keeps the nearest
// ------------------------------------------------------------------
struct SelPoint
{
    double r2;
    double z;
};

// returns false -> nodata
bool selectNearest(const GridPoints &pts, double xp, double yp,
                   double radiusSq, bool useQuadrant, int minPPQ,
                   int maxPPQ, bool capTotal, int maxTotal, int minPoints,
                   std::vector<SelPoint> &out)
{
    out.clear();
    const std::vector<int> &ord = gridQtOrder(pts);
    if (useQuadrant)
    {
        std::vector<SelPoint> quad[4];
        const size_t n = pts.size();
        for (size_t k = 0; k < n; ++k)
        {
            const size_t i = (size_t)ord[k];
            const double rx = pts.x[i] - xp;
            const double ry = pts.y[i] - yp;
            const double r2 = rx * rx + ry * ry;
            if (r2 > radiusSq)
                continue;
            const int q = (rx >= 0 ? 1 : 0) + (ry >= 0 ? 2 : 0);
            quad[q].push_back({r2, pts.z[i]});
        }
        size_t maxRank = 0;
        for (int q = 0; q < 4; ++q)
        {
            std::stable_sort(quad[q].begin(), quad[q].end(),
                             [](const SelPoint &a, const SelPoint &b) {
                                 return a.r2 < b.r2;
                             });
            if ((size_t)maxPPQ < quad[q].size())
                quad[q].resize(maxPPQ);
            maxRank = std::max(maxRank, quad[q].size());
        }
        if (minPPQ > 0)
            for (int q = 0; q < 4; ++q)
                if ((int)quad[q].size() < minPPQ)
                    return false;
        // round-robin across quadrants by rank matches the reference
        // accumulation order
        for (size_t rank = 0; rank < maxRank; ++rank)
            for (int q = 0; q < 4; ++q)
                if (rank < quad[q].size())
                    out.push_back(quad[q][rank]);
    }
    else
    {
        const size_t n = pts.size();
        for (size_t k = 0; k < n; ++k)
        {
            const size_t i = (size_t)ord[k];
            const double rx = pts.x[i] - xp;
            const double ry = pts.y[i] - yp;
            const double r2 = rx * rx + ry * ry;
            if (r2 > radiusSq)
                continue;
            out.push_back({r2, pts.z[i]});
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const SelPoint &a, const SelPoint &b) {
                             return a.r2 < b.r2;
                         });
    }
    if (capTotal && (size_t)maxTotal < out.size())
        out.resize(maxTotal);
    if ((int)out.size() < minPoints || out.empty())
        return false;
    return true;
}

// ------------------------------------------------------------------
// per-node evaluation for the double-precision paths
// ------------------------------------------------------------------
// GDALGridInverseDistanceToAPower semantics: r2 (with smoothing) from the
// unrotated deltas, the rotation only feeds the ellipse test
double evalInvdistDouble(const GridCfg &c, const Ellipse &el,
                         const GridPoints &pts, double xp, double yp)
{
    const double s2 = c.smoothing * c.smoothing;
    const double powerDiv2 = c.power / 2;
    double nom = 0.0, den = 0.0;
    int count = 0;
    const size_t np = pts.size();
    for (size_t i = 0; i < np; ++i)
    {
        double rx = pts.x[i] - xp;
        double ry = pts.y[i] - yp;
        const double r2 = rx * rx + ry * ry + s2;
        if (!el.contains(rx, ry))
            continue;
        if (r2 < kCoincEps)
            return pts.z[i];
        const double w = 1.0 / pow(r2, powerDiv2);
        nom += w * pts.z[i];
        den += w;
        ++count;
        if (el.active && c.maxPointsSet && count > c.maxPoints)
            break;
    }
    if (el.active && (count < c.minPoints || den == 0.0))
        return c.nodata;
    if (den == 0.0)
        return c.nodata;
    return nom / den;
}

double evalInvdistNN(const GridCfg &c, const GridPoints &pts, double xp,
                     double yp)
{
    const double radiusSq = c.radius1 * c.radius1;
    std::vector<SelPoint> sel;
    if (!selectNearest(pts, xp, yp, radiusSq, c.quadrant(), c.minPPQ,
                       c.maxPPQ, true, c.maxPoints, c.minPoints, sel))
        return c.nodata;
    const double s2 = c.smoothing * c.smoothing;
    const double powerDiv2 = c.power / 2;
    double nom = 0.0, den = 0.0;
    for (const SelPoint &p : sel)
    {
        const double r2 = p.r2 + s2;
        if (r2 < kCoincEps)
            return p.z;
        const double w = 1.0 / pow(r2, powerDiv2);
        nom += w * p.z;
        den += w;
    }
    if (den == 0.0)
        return c.nodata;
    return nom / den;
}

enum class Metric
{
    Average,
    Minimum,
    Maximum,
    Range,
    Count,
    AvgDist,
    AvgDistPts
};

double evalMetricQuadrant(Metric m, const GridCfg &c, const GridPoints &pts,
                          double xp, double yp)
{
    const double radiusSq = c.radius1 * c.radius1;
    std::vector<SelPoint> sel;
    const bool capTotal = c.maxPointsSet;
    if (!selectNearest(pts, xp, yp, radiusSq, true, c.minPPQ, c.maxPPQ,
                       capTotal, c.maxPoints, c.minPoints, sel))
        return c.nodata;
    switch (m)
    {
        case Metric::Average:
        {
            double acc = 0.0;
            for (const SelPoint &p : sel)
                acc += p.z;
            return acc / (double)sel.size();
        }
        case Metric::Minimum:
        {
            double v = sel[0].z;
            for (const SelPoint &p : sel)
                if (p.z < v)
                    v = p.z;
            return v;
        }
        case Metric::Maximum:
        {
            double v = sel[0].z;
            for (const SelPoint &p : sel)
                if (p.z > v)
                    v = p.z;
            return v;
        }
        case Metric::Range:
        {
            double lo = sel[0].z, hi = sel[0].z;
            for (const SelPoint &p : sel)
            {
                if (p.z < lo)
                    lo = p.z;
                if (p.z > hi)
                    hi = p.z;
            }
            return hi - lo;
        }
        case Metric::Count:
            return (double)sel.size();
        case Metric::AvgDist:
        {
            double acc = 0.0;
            for (const SelPoint &p : sel)
                acc += sqrt(p.r2);
            return acc / (double)sel.size();
        }
        case Metric::AvgDistPts:
        {
            if (sel.size() < 2)
                return c.nodata;
            // pairwise distances need the coordinates; the selector only
            // keeps r2/z, so this path recollects inside the caller
            return c.nodata;
        }
    }
    return c.nodata;
}

double evalMetric(Metric m, const GridCfg &c, const Ellipse &el,
                  const GridPoints &pts, double xp, double yp)
{
    const size_t np = pts.size();
    switch (m)
    {
        case Metric::Average:
        {
            double acc = 0.0;
            long long n = 0;
            for (size_t i = 0; i < np; ++i)
            {
                double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
                if (!el.contains(rx, ry))
                    continue;
                acc += pts.z[i];
                ++n;
            }
            if (n < c.minPoints || n == 0)
                return c.nodata;
            return acc / (double)n;
        }
        case Metric::Minimum:
        case Metric::Maximum:
        case Metric::Range:
        {
            double lo = 0, hi = 0;
            long long n = 0;
            for (size_t i = 0; i < np; ++i)
            {
                double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
                if (!el.contains(rx, ry))
                    continue;
                if (n == 0)
                    lo = hi = pts.z[i];
                else
                {
                    if (pts.z[i] < lo)
                        lo = pts.z[i];
                    if (pts.z[i] > hi)
                        hi = pts.z[i];
                }
                ++n;
            }
            if (n < c.minPoints || n == 0)
                return c.nodata;
            if (m == Metric::Minimum)
                return lo;
            if (m == Metric::Maximum)
                return hi;
            return hi - lo;
        }
        case Metric::Count:
        {
            long long n = 0;
            for (size_t i = 0; i < np; ++i)
            {
                double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
                if (!el.contains(rx, ry))
                    continue;
                ++n;
            }
            if (n < c.minPoints)
                return c.nodata;
            return (double)n;
        }
        case Metric::AvgDist:
        {
            double acc = 0.0;
            long long n = 0;
            for (size_t i = 0; i < np; ++i)
            {
                double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
                if (!el.contains(rx, ry))
                    continue;
                acc += sqrt(rx * rx + ry * ry);
                ++n;
            }
            if (n < c.minPoints || n == 0)
                return c.nodata;
            return acc / (double)n;
        }
        case Metric::AvgDistPts:
        {
            std::vector<size_t> in;
            for (size_t i = 0; i < np; ++i)
            {
                double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
                if (!el.contains(rx, ry))
                    continue;
                in.push_back(i);
            }
            if ((long long)in.size() < c.minPoints || in.size() < 2)
                return c.nodata;
            double acc = 0.0;
            long long n = 0;
            for (size_t a = 0; a < in.size(); ++a)
                for (size_t b = a + 1; b < in.size(); ++b)
                {
                    const double dx = pts.x[in[a]] - pts.x[in[b]];
                    const double dy = pts.y[in[a]] - pts.y[in[b]];
                    acc += sqrt(dx * dx + dy * dy);
                    ++n;
                }
            return acc / (double)n;
        }
    }
    return c.nodata;
}

double evalNearest(const GridCfg &c, const Ellipse &el,
                   const GridPoints &pts, double xp, double yp)
{
    const size_t np = pts.size();
    double best = std::numeric_limits<double>::max();
    double val = c.nodata;
    bool found = false;
    for (size_t i = 0; i < np; ++i)
    {
        double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
        if (!el.contains(rx, ry))
            continue;
        const double r2 = rx * rx + ry * ry;
        if (r2 <= best)
        {
            best = r2;
            val = pts.z[i];
            found = true;
        }
    }
    return found ? val : c.nodata;
}

// ------------------------------------------------------------------
// linear: Delaunay (Bowyer-Watson) + barycentric interpolation with
// nearest-neighbor fallback outside the hull
// ------------------------------------------------------------------
struct Tri
{
    int a, b, c;
};

struct Delaunay
{
    std::vector<Tri> tris;
    bool valid = false;

    void build(const GridPoints &pts)
    {
        const size_t n = pts.size();
        valid = false;
        tris.clear();
        if (n < 3)
            return;
        double xmin = pts.x[0], xmax = pts.x[0];
        double ymin = pts.y[0], ymax = pts.y[0];
        for (size_t i = 1; i < n; ++i)
        {
            xmin = std::min(xmin, pts.x[i]);
            xmax = std::max(xmax, pts.x[i]);
            ymin = std::min(ymin, pts.y[i]);
            ymax = std::max(ymax, pts.y[i]);
        }
        const double w = std::max(xmax - xmin, ymax - ymin);
        const double cx = (xmin + xmax) / 2, cy = (ymin + ymax) / 2;
        const double m = std::max(w, 1.0) * 20;
        std::vector<double> px(pts.x.begin(), pts.x.end());
        std::vector<double> py(pts.y.begin(), pts.y.end());
        px.push_back(cx - m);
        py.push_back(cy - m);
        px.push_back(cx + m);
        py.push_back(cy - m);
        px.push_back(cx);
        py.push_back(cy + m);
        const int s0 = (int)n, s1 = (int)n + 1, s2 = (int)n + 2;
        std::vector<Tri> work{{s0, s1, s2}};
        for (int ip = 0; ip < (int)n; ++ip)
        {
            std::vector<std::pair<int, int>> edges;
            std::vector<Tri> keep;
            for (const Tri &t : work)
            {
                if (inCircum(px, py, t, px[ip], py[ip]))
                {
                    edges.push_back({t.a, t.b});
                    edges.push_back({t.b, t.c});
                    edges.push_back({t.c, t.a});
                }
                else
                    keep.push_back(t);
            }
            // boundary edges appear exactly once (up to orientation)
            for (size_t e = 0; e < edges.size(); ++e)
            {
                bool dup = false;
                for (size_t f = 0; f < edges.size(); ++f)
                {
                    if (e == f)
                        continue;
                    if ((edges[e].first == edges[f].second &&
                         edges[e].second == edges[f].first) ||
                        (edges[e].first == edges[f].first &&
                         edges[e].second == edges[f].second))
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                    keep.push_back({edges[e].first, edges[e].second, ip});
            }
            work = std::move(keep);
        }
        for (const Tri &t : work)
            if (t.a < (int)n && t.b < (int)n && t.c < (int)n)
                tris.push_back(t);
        valid = !tris.empty();
    }

    static bool inCircum(const std::vector<double> &px,
                         const std::vector<double> &py, const Tri &t,
                         double x, double y)
    {
        const double ax = px[t.a] - x, ay = py[t.a] - y;
        const double bx = px[t.b] - x, by = py[t.b] - y;
        const double cx = px[t.c] - x, cy = py[t.c] - y;
        const double det =
            (ax * ax + ay * ay) * (bx * cy - cx * by) -
            (bx * bx + by * by) * (ax * cy - cx * ay) +
            (cx * cx + cy * cy) * (ax * by - bx * ay);
        const double orient =
            (px[t.b] - px[t.a]) * (py[t.c] - py[t.a]) -
            (px[t.c] - px[t.a]) * (py[t.b] - py[t.a]);
        return orient > 0 ? det > 0 : det < 0;
    }
};

double evalLinear(const GridCfg &c, const Delaunay &dt,
                  const GridPoints &pts, double xp, double yp)
{
    if (dt.valid)
    {
        for (const Tri &t : dt.tris)
        {
            // coefficient-form barycentric evaluation, vertices in
            // descending point-index order (closest observed match to
            // the reference facet vertex ordering)
            int i1 = t.a, i2 = t.b, i3 = t.c;
            if (i1 < i2)
                std::swap(i1, i2);
            if (i2 < i3)
                std::swap(i2, i3);
            if (i1 < i2)
                std::swap(i1, i2);
            const double x1 = pts.x[i1], y1 = pts.y[i1];
            const double x2 = pts.x[i2], y2 = pts.y[i2];
            const double x3 = pts.x[i3], y3 = pts.y[i3];
            const double den =
                (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3);
            if (den == 0.0)
                continue;
            const double m1x = (y2 - y3) / den;
            const double m1y = (x3 - x2) / den;
            const double m2x = (y3 - y1) / den;
            const double m2y = (x1 - x3) / den;
            const double l1 = m1x * (xp - x3) + m1y * (yp - y3);
            const double l2 = m2x * (xp - x3) + m2y * (yp - y3);
            const double l3 = 1.0 - l1 - l2;
            const double eps = -1e-11;
            if (l1 >= eps && l2 >= eps && l3 >= eps)
                return l1 * pts.z[i1] + l2 * pts.z[i2] +
                       l3 * pts.z[i3];
        }
    }
    // outside the hull (or no triangulation): nearest neighbor within
    // the linear radius; radius 0 means nodata
    if (c.linearRadius == 0.0)
        return c.nodata;
    double best = std::numeric_limits<double>::max();
    double val = c.nodata;
    bool found = false;
    const bool inf = std::isinf(c.linearRadius);
    const double radiusSq = inf ? 0.0 : c.linearRadius * c.linearRadius;
    const size_t np = pts.size();
    for (size_t i = 0; i < np; ++i)
    {
        const double rx = pts.x[i] - xp, ry = pts.y[i] - yp;
        const double r2 = rx * rx + ry * ry;
        if (!inf && r2 > radiusSq)
            continue;
        if (r2 <= best)
        {
            best = r2;
            val = pts.z[i];
            found = true;
        }
    }
    return found ? val : c.nodata;
}

// ------------------------------------------------------------------
// option-string rendering (reference reproduces gdal_grid's open-quote
// oddity on most algorithms)
// ------------------------------------------------------------------
std::string quadrantSuffix(const GridCfg &c)
{
    std::string s;
    if (c.minPPQ > 0)
        s += strPrintf(":min_points_per_quadrant=%d", c.minPPQ);
    if (c.maxPPQ != 2147483647)
        s += strPrintf(":max_points_per_quadrant=%d", c.maxPPQ);
    return s;
}

std::string optionsLine(const GridCfg &c)
{
    if (c.alg == "invdist" && !c.quadrant())
        return strPrintf(
            "Options are "
            "\"power=%f:smoothing=%f:radius1=%f:radius2=%f:angle=%f:"
            "max_points=%d:min_points=%d:nodata=%f\"",
            c.power, c.smoothing, c.radius1, c.radius2, c.angle,
            c.maxPointsSet ? c.maxPoints : 0, c.minPoints, c.nodata);
    if (c.alg == "invdist" || c.alg == "invdistnn")
        return strPrintf(
                   "Options are: "
                   "\"power=%f:smoothing=%f:radius=%f:max_points=%d:"
                   "min_points=%d:nodata=%f",
                   c.power, c.smoothing, c.radius1,
                   c.maxPointsSet ? c.maxPoints : 12, c.minPoints,
                   c.nodata) +
               quadrantSuffix(c);
    if (c.alg == "nearest")
        return strPrintf(
            "Options are \"radius1=%f:radius2=%f:angle=%f:nodata=%f\"",
            c.radius1, c.radius2, c.angle, c.nodata);
    if (c.alg == "linear")
        return strPrintf("Options are \"radius=%f:nodata=%f\"",
                         c.linearRadius, c.nodata);
    std::string s = strPrintf(
        "Options are: "
        "\"radius1=%f:radius2=%f:angle=%f:min_points=%d:nodata=%f",
        c.radius1, c.radius2, c.angle, c.minPoints, c.nodata);
    s += quadrantSuffix(c);
    if (c.maxPointsSet)
        s += strPrintf(":max_points=%d", c.maxPoints);
    return s;
}

// ------------------------------------------------------------------
// point collection
// ------------------------------------------------------------------
void geomEnvelope(const OgrGeometry &g, bool &any, double env[4])
{
    for (size_t i = 0; i + 2 < g.coords.size() + 2; i += 3)
    {
        if (i + 1 >= g.coords.size())
            break;
        const double x = g.coords[i], y = g.coords[i + 1];
        if (!any)
        {
            env[0] = env[2] = x;
            env[1] = env[3] = y;
            any = true;
        }
        else
        {
            env[0] = std::min(env[0], x);
            env[2] = std::max(env[2], x);
            env[1] = std::min(env[1], y);
            env[3] = std::max(env[3], y);
        }
    }
    for (const OgrGeometry &p : g.parts)
        geomEnvelope(p, any, env);
}

void collectCoords(const OgrGeometry &g, double zoffset, double zmultiply,
                   bool useField, double fieldZ, GridPoints &pts,
                   bool &sawNoZ)
{
    if (!g.hasZ && (!g.coords.empty() || !g.parts.empty()))
        sawNoZ = true;
    for (size_t i = 0; i + 1 < g.coords.size(); i += 3)
    {
        const double x = g.coords[i], y = g.coords[i + 1];
        double z = useField ? fieldZ
                            : (i + 2 < g.coords.size() ? g.coords[i + 2]
                                                       : 0.0);
        if (!useField && !g.hasZ)
            z = 0.0;
        z = (z + zoffset) * zmultiply;
        pts.x.push_back(x);
        pts.y.push_back(y);
        pts.z.push_back(z);
    }
    for (const OgrGeometry &p : g.parts)
        collectCoords(p, zoffset, zmultiply, useField, fieldZ, pts, sawNoZ);
}

double fieldAsDouble(const OgrFeature &f, size_t idx)
{
    if (idx >= f.values.size() || !f.values[idx].set)
        return 0.0;
    const JVal &v = f.values[idx].v;
    switch (v.type)
    {
        case JVal::INT:
            return (double)v.i;
        case JVal::DOUBLE:
            return v.d;
        case JVal::BOOL:
            return v.b ? 1.0 : 0.0;
        case JVal::STRING:
            return atof(v.s.c_str());
        default:
            return 0.0;
    }
}

// ------------------------------------------------------------------
// handler
// ------------------------------------------------------------------
double encodeGridValue(DType type, double v)
{
    if (type == DType::Float32 || type == DType::Float64)
        return v;
    double lo = 0, hi = 0;
    switch (type)
    {
        case DType::Byte: lo = 0; hi = 255; break;
        case DType::Int8: lo = -128; hi = 127; break;
        case DType::UInt16: lo = 0; hi = 65535; break;
        case DType::Int16: lo = -32768; hi = 32767; break;
        case DType::UInt32: lo = 0; hi = 4294967295.0; break;
        case DType::Int32: lo = -2147483648.0; hi = 2147483647.0; break;
        case DType::UInt64: lo = 0; hi = 18446744073709551615.0; break;
        case DType::Int64:
            lo = -9223372036854775808.0;
            hi = 9223372036854775807.0;
            break;
        default: break;
    }
    if (v > hi)
        return hi;
    if (v < lo)
        return lo;
    return std::floor(v + 0.5);
}

// scale+tiepoint only representable for north-up negative y step; the
// reference stores the full matrix otherwise
void applyGtTags(GTiffCreateParams &p, const double gt[6])
{
    memcpy(p.gt, gt, 6 * sizeof(double));
    if (!(gt[5] < 0))
    {
        p.hasXform = true;
        const double xf[16] = {gt[1], 0, 0, gt[0],
                               0, gt[5], 0, gt[3],
                               0, 0, 0, 0,
                               0, 0, 0, 1};
        memcpy(p.xform, xf, sizeof(xf));
    }
    else
        p.hasXform = false;
}

int gridHandler(const CmdSpec &spec, ParseResult &r)
{
    const std::string alg = spec.id.substr(strlen("vector_grid_"));
    const std::vector<std::string> inputs = r.list("input");
    const std::string output = r.str("output");
    const bool quiet = r.flag("quiet");
    std::string of = r.str("output-format");

    GridCfg cfg;
    cfg.alg = alg;
    if (const ArgValue *v = r.get("power"))
        cfg.power = atof(v->str().c_str());
    if (const ArgValue *v = r.get("smoothing"))
        cfg.smoothing = atof(v->str().c_str());
    if (const ArgValue *v = r.get("radius"))
    {
        if (alg == "linear")
            cfg.linearRadius = atof(v->str().c_str());
        else
        {
            cfg.radius1 = cfg.radius2 = atof(v->str().c_str());
            cfg.radiusGroupSet = true;
        }
    }
    if (const ArgValue *v = r.get("radius1"))
    {
        cfg.radius1 = atof(v->str().c_str());
        cfg.radiusGroupSet = true;
    }
    if (const ArgValue *v = r.get("radius2"))
    {
        cfg.radius2 = atof(v->str().c_str());
        cfg.radiusGroupSet = true;
    }
    if (const ArgValue *v = r.get("angle"))
        cfg.angle = atof(v->str().c_str());
    if (const ArgValue *v = r.get("nodata"))
        cfg.nodata = atof(v->str().c_str());
    if (const ArgValue *v = r.get("min-points"))
        cfg.minPoints = atoi(v->str().c_str());
    if (const ArgValue *v = r.get("max-points"))
    {
        cfg.maxPoints = atoi(v->str().c_str());
        cfg.maxPointsSet = true;
    }
    else if (alg == "invdistnn")
    {
        cfg.maxPoints = 12;
        cfg.maxPointsSet = true;
    }
    if (const ArgValue *v = r.get("min-points-per-quadrant"))
        cfg.minPPQ = atoi(v->str().c_str());
    if (const ArgValue *v = r.get("max-points-per-quadrant"))
        cfg.maxPPQ = atoi(v->str().c_str());
    if (alg == "invdist" && cfg.maxPointsSet &&
        cfg.maxPoints == 2147483647)
        cfg.maxPointsSet = false;
    if (alg == "invdist" && cfg.quadrant() && !cfg.maxPointsSet)
    {
        // invdist with quadrant options runs the invdistnn machinery,
        // inheriting its default point cap
        cfg.maxPoints = 12;
        cfg.maxPointsSet = true;
    }

    // runtime algorithm-parameter validation (before any output work)
    if (cfg.quadrant())
    {
        if (alg == "average-distance-points")
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "Algorithm average_distance_pts not supported "
                        "when per quadrant parameters are specified");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to process algorithm name and "
                        "parameters");
            return 1;
        }
        if (alg != "linear" && alg != "nearest")
        {
            if (cfg.radiusGroupSet && cfg.radius1 != cfg.radius2)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "radius1 != radius2 not supported when "
                            "min_points_per_quadrant and/or "
                            "max_points_per_quadrant is specified");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to process algorithm name and "
                            "parameters");
                return 1;
            }
            if (cfg.radius1 <= 0.0)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "Radius value should be strictly positive "
                            "when per quadrant parameters are specified");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to process algorithm name and "
                            "parameters");
                return 1;
            }
        }
    }
    if (alg == "invdistnn" && cfg.radius1 <= 0.0)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "Radius value should be strictly positive");
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to process algorithm name and parameters");
        return 1;
    }

    // output driver resolution: --of validated at parse time; extension
    // guesses fail silently
    bool isMem = false;
    if (!of.empty())
    {
        if (strEqualNoCase(of, "MEM"))
            isMem = true;
        else if (!strEqualNoCase(of, "GTiff") && !strEqualNoCase(of, "COG"))
            return 1;  // GDALG already refused during validation
    }
    else
    {
        const std::string lo = strToLower(output);
        if (!strEndsWith(lo, ".tif") && !strEndsWith(lo, ".tiff"))
            return 1;
    }

    // grid geometry from arguments
    bool haveExtent = false;
    double ext[4] = {0, 0, 0, 0};
    if (const ArgValue *v = r.get("extent"))
    {
        for (int i = 0; i < 4; ++i)
            ext[i] = atof(v->values[i].c_str());
        haveExtent = true;
    }
    int width = 256, height = 256;
    bool haveSize = false;
    if (const ArgValue *v = r.get("size"))
    {
        width = atoi(v->values[0].c_str());
        height = atoi(v->values[1].c_str());
        haveSize = true;
    }
    double resX = 0, resY = 0;
    bool haveRes = false;
    if (const ArgValue *v = r.get("resolution"))
    {
        resX = atof(v->values[0].c_str());
        resY = atof(v->values[1].c_str());
        haveRes = true;
    }
    if (haveRes && haveExtent)
    {
        width = (int)std::round((ext[2] - ext[0]) / resX);
        height = (int)std::round((ext[3] - ext[1]) / resY);
        haveSize = true;
    }
    (void)haveSize;

    DType type = DType::Float64;
    if (const ArgValue *v = r.get("output-data-type"))
    {
        std::string s = v->str();
        type = s == "UInt8" ? DType::Byte : dtypeFromName(s);
    }

    Srs crsOverride;
    bool haveCrsOverride = false;
    if (const ArgValue *v = r.get("crs"))
    {
        bool ok = false;
        crsOverride = Srs::fromCliInput(v->str(), ok, true);
        haveCrsOverride = ok && crsOverride.valid();
        if (!haveCrsOverride)
            return 1;  // parse-time check already reported
    }

    // open the (first) input
    std::string err;
    auto ds = openVectorDataset(inputs.empty() ? "" : inputs[0], err,
                                r.list("input-format"),
                                r.list("open-option"));
    if (!ds)
        return 1;

    // band plan: --sql yields one band, -l one per entry, otherwise one
    // per layer
    const std::vector<std::string> layerSel = r.list("input-layer");
    std::string sql = r.str("sql");
    if (!sql.empty() && sql[0] == '@')
    {
        std::string content;
        readFileToString(sql.substr(1), content);
        sql = content;
    }
    int bands;
    if (!sql.empty())
        bands = 1;
    else if (!layerSel.empty())
        bands = (int)layerSel.size();
    else
        bands = (int)ds->layers.size();
    if (bands <= 0)
        bands = 1;

    const int pxSize = dtypeSizeBytes(type);
    std::vector<std::vector<uint8_t>> pixels(
        (size_t)bands,
        std::vector<uint8_t>((size_t)width * height * pxSize, 0));
    {
        // bands start out nodata-filled; skipped layers keep that fill
        uint8_t cell[16] = {0};
        rasterEncodeReal(type, cell, encodeGridValue(type, cfg.nodata),
                         0.0);
        for (auto &b : pixels)
            for (size_t i = 0; i < (size_t)width * height; ++i)
                memcpy(b.data() + i * pxSize, cell, pxSize);
    }

    GTiffCreateParams p;
    p.width = width;
    p.height = height;
    p.bands = bands;
    p.type = type;
    p.hasNodata = true;
    p.nodata = cfg.nodata;

    std::vector<std::pair<std::string, std::string>> cos;
    for (const auto &c : r.list("creation-option"))
    {
        size_t eq = c.find('=');
        cos.push_back({c.substr(0, eq),
                       eq == std::string::npos ? "" : c.substr(eq + 1)});
    }
    CreationOptions o = parseCreationOptions(cos, output, alg);
    if (o.fatal)
        return 1;
    if (!finalizeCreationOptions(o, output, p.bands, p.type))
        return 1;
    if (o.tiled)
    {
        p.tiled = true;
        p.blockX = o.blockXFinal;
        p.blockY = o.blockYFinal;
    }
    else
        p.blockY = o.blockYFinal;
    p.predictor = o.predictorFinal;
    p.compression = o.compression;
    p.zlevel = o.zlevel;
    p.zstdLevel = o.zstdLevel;
    p.bandInterleave = o.bandInterleave;
    p.sparse = o.sparse;
    p.profile = o.profile;

    // resolve layers up front (the reference creates the output before
    // this check, so a miss leaves a zero grid on disk)
    std::vector<OgrLayer *> layers;
    std::unique_ptr<OgrLayer> sqlLayer;
    bool layerFail = false;
    std::string layerFailName;
    if (!sql.empty())
    {
        sqlLayer = ogrExecuteSql(*ds, sql);
        if (!sqlLayer)
            layerFail = true;
        else
            layers.push_back(sqlLayer.get());
    }
    else if (!layerSel.empty())
    {
        for (const std::string &want : layerSel)
        {
            OgrLayer *hit = nullptr;
            for (auto &l : ds->layers)
                if (l.name == want)
                {
                    hit = &l;
                    break;
                }
            if (!hit)
                for (auto &l : ds->layers)
                    if (strEqualNoCase(l.name, want))
                    {
                        hit = &l;
                        break;
                    }
            if (!hit)
            {
                layerFail = true;
                layerFailName = want;
                break;
            }
            layers.push_back(hit);
        }
    }
    else
    {
        for (auto &l : ds->layers)
            layers.push_back(&l);
    }

    bool gridComputedAny = false;
    auto writeResult = [&](bool gtReady, const Srs *srs) -> bool {
        if (isMem)
            return true;
        if (!gtReady && haveExtent)
        {
            // nothing computed: the create-time transform survives,
            // raw extent order, bottom-up y
            const double gt[6] = {ext[0], (ext[2] - ext[0]) / width, 0,
                                  ext[1], 0, (ext[3] - ext[1]) / height};
            applyGtTags(p, gt);
            gtReady = true;
        }
        p.pixels = &pixels;
        p.hasGT = gtReady;
        p.gridOrphanIfd = gridComputedAny;
        p.srs = srs && srs->valid() ? srs : nullptr;
        std::string werr;
        if (!gtiffWrite(output, p, werr))
        {
            if (werr != "reported")
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            alg + ": " + werr);
            return false;
        }
        return true;
    };

    if (layerFail)
    {
        if (!layerFailName.empty())
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unable to find layer \"" + layerFailName + "\".");
        writeResult(false, nullptr);
        return 1;
    }

    const ArgValue *bboxArg = r.get("bbox");
    double bbox[4] = {0, 0, 0, 0};
    if (bboxArg)
        for (int i = 0; i < 4; ++i)
            bbox[i] = atof(bboxArg->values[i].c_str());

    const std::string zfield = r.str("zfield");
    double zoffset = 0.0, zmultiply = 1.0;
    if (const ArgValue *v = r.get("zoffset"))
        zoffset = atof(v->str().c_str());
    if (const ArgValue *v = r.get("zmultiply"))
        zmultiply = atof(v->str().c_str());

    Ellipse el;
    el.init(cfg);
    const bool floatPath = alg == "invdist" && cfg.power == 2.0 &&
                           cfg.smoothing == 0.0 && !el.active &&
                           !cfg.quadrant();

    bool gtReady = false;
    Srs layerSrs;
    bool haveLayerSrs = false;
    double xmin = 0, ymin = 0, xmax = 0, ymax = 0, dxCell = 0, dyCell = 0;
    if (haveExtent)
    {
        // x kept raw (reversed extents yield negative x step), y sorted
        xmin = ext[0];
        xmax = ext[2];
        ymin = std::min(ext[1], ext[3]);
        ymax = std::max(ext[1], ext[3]);
    }

    bool aborted = false;
    for (size_t li = 0; li < layers.size(); ++li)
    {
        OgrLayer &lyr = *layers[li];
        // sql result layers carry no srs in the reference output
        if (!haveLayerSrs && lyr.hasSrs && sql.empty())
        {
            layerSrs = lyr.srs;
            haveLayerSrs = true;
        }

        int zIdx = -1;
        if (!zfield.empty())
        {
            for (size_t f = 0; f < lyr.fields.size(); ++f)
                if (lyr.fields[f].name == zfield)
                {
                    zIdx = (int)f;
                    break;
                }
            if (zIdx < 0)
                for (size_t f = 0; f < lyr.fields.size(); ++f)
                    if (strEqualNoCase(lyr.fields[f].name, zfield))
                    {
                        zIdx = (int)f;
                        break;
                    }
            if (zIdx < 0)
            {
                printf("Failed to find field %s on layer %s, skipping.\n",
                       zfield.c_str(), lyr.name.c_str());
                fflush(stdout);
                aborted = true;
                continue;
            }
        }

        GridPoints pts;
        bool sawNoZ = false;
        for (const OgrFeature &feat : lyr.features)
        {
            if (!feat.hasGeom || feat.geom.empty)
                continue;
            if (bboxArg)
            {
                bool any = false;
                double env[4] = {0, 0, 0, 0};
                geomEnvelope(feat.geom, any, env);
                if (!any)
                    continue;
                if (env[2] < bbox[0] || env[0] > bbox[2] ||
                    env[3] < bbox[1] || env[1] > bbox[3])
                    continue;
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "GEOS support not enabled.");
                continue;
            }
            const double fz =
                zIdx >= 0 ? fieldAsDouble(feat, (size_t)zIdx) : 0.0;
            collectCoords(feat.geom, zoffset, zmultiply, zIdx >= 0, fz,
                          pts, sawNoZ);
        }

        if (pts.size() == 0)
        {
            printf("No point geometry found on layer %s, skipping.\n",
                   lyr.name.c_str());
            fflush(stdout);
            continue;
        }

        if (zIdx < 0 && sawNoZ)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        alg + ": At least one geometry of layer '" +
                            lyr.name +
                            "' lacks a Z component. You may need to set "
                            "the 'zfield' argument");

        if (!gtReady)
        {
            if (!haveExtent)
            {
                xmin = xmax = pts.x[0];
                ymin = ymax = pts.y[0];
                for (size_t i = 1; i < pts.size(); ++i)
                {
                    xmin = std::min(xmin, pts.x[i]);
                    xmax = std::max(xmax, pts.x[i]);
                    ymin = std::min(ymin, pts.y[i]);
                    ymax = std::max(ymax, pts.y[i]);
                }
            }
            dxCell = (xmax - xmin) / width;
            dyCell = (ymax - ymin) / height;
            const double gt[6] = {xmin, dxCell, 0, ymax, 0,
                                  (ymin - ymax) / height};
            applyGtTags(p, gt);
            gtReady = true;
        }

        if (!quiet)
        {
            printf("Grid data type is \"%s\"\n", dtypeName(type));
            printf("Grid size = (%d %d).\n", width, height);
            printf("Corner coordinates = (%f %f)-(%f %f).\n", xmin, ymax,
                   xmax, ymin);
            printf("Grid cell size = (%f %f).\n", dxCell, p.gt[5]);
            printf("Source point count = %llu.\n",
                   (unsigned long long)pts.size());
            const char *algPrint = alg == "average-distance"
                                       ? "average_distance"
                                   : alg == "average-distance-points"
                                       ? "average_distance_pts"
                                   : (alg == "invdist" && cfg.quadrant())
                                       ? "invdistnn"
                                       : alg.c_str();
            printf("Algorithm name: \"%s\".\n", algPrint);
            printf("%s\n", optionsLine(cfg).c_str());
            printf("\n");
            fflush(stdout);
        }

        if (floatPath)
        {
            pts.fx.resize(pts.size());
            pts.fy.resize(pts.size());
            pts.fz.resize(pts.size());
            for (size_t i = 0; i < pts.size(); ++i)
            {
                pts.fx[i] = (float)pts.x[i];
                pts.fy[i] = (float)pts.y[i];
                pts.fz[i] = (float)pts.z[i];
            }
        }

        Metric metric = Metric::Average;
        bool isMetric = false;
        if (alg == "average")
        {
            metric = Metric::Average;
            isMetric = true;
        }
        else if (alg == "minimum")
        {
            metric = Metric::Minimum;
            isMetric = true;
        }
        else if (alg == "maximum")
        {
            metric = Metric::Maximum;
            isMetric = true;
        }
        else if (alg == "range")
        {
            metric = Metric::Range;
            isMetric = true;
        }
        else if (alg == "count")
        {
            metric = Metric::Count;
            isMetric = true;
        }
        else if (alg == "average-distance")
        {
            metric = Metric::AvgDist;
            isMetric = true;
        }
        else if (alg == "average-distance-points")
        {
            metric = Metric::AvgDistPts;
            isMetric = true;
        }

        Delaunay dt;
        if (alg == "linear")
        {
            dt.build(pts);
            const bool tooFew = pts.size() < 3;
            if (tooFew || !dt.valid)
            {
                if (tooFew)
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        strPrintf("qhull input error: not enough "
                                  "points(%d) to construct initial "
                                  "simplex (need 4)\n",
                                  (int)pts.size()));
                else
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        "Qhull precision error: Initial simplex is "
                        "flat (facet 1 is coplanar with the interior "
                        "point)\n");
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "\nWhile executing:  | qhull d Qbb Qc Qz "
                            "Qt\n");
                if (tooFew)
                {
                    // qhull seeds its rng with time(NULL); same-second
                    // runs reproduce the reference id
                    const int runId =
                        (int)((16807LL * (long long)time(nullptr)) %
                              2147483647LL);
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        strPrintf(
                            "Options selected for Qhull 2020.2.r "
                            "2020/08/31:\n  run-id %d  delaunay  "
                            "Qbbound-last  Qcoplanar-keep  "
                            "Qz-infinity-point\n  Qtriangulate  "
                            "_pre-merge  _zero-centrum  "
                            "Qinterior-keep  _maxoutside  0\n",
                            runId));
                }
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Delaunay triangulation failed");
                aborted = true;
                break;
            }
        }

        uint8_t *band = pixels[li].data();
        TermProgress prog;
        for (int row = 0; row < height; ++row)
        {
            const double yp = ymax - dyCell * (row + 0.5);
            for (int col = 0; col < width; ++col)
            {
                const double xp = xmin + dxCell * (col + 0.5);
                double v;
                if (alg == "invdist")
                {
                    if (cfg.quadrant())
                        v = evalInvdistNN(cfg, pts, xp, yp);
                    else if (floatPath)
                    {
                        const size_t n = pts.size();
                        if (n >= 16 && cpuHasAvx())
                            v = invdistAvx(pts.fx.data(), pts.fy.data(),
                                           pts.fz.data(), n, (float)xp,
                                           (float)yp);
                        else
                            v = invdistFloatScalar(
                                pts.fx.data(), pts.fy.data(),
                                pts.fz.data(), n, (float)xp, (float)yp);
                    }
                    else
                        v = evalInvdistDouble(cfg, el, pts, xp, yp);
                }
                else if (alg == "invdistnn")
                    v = evalInvdistNN(cfg, pts, xp, yp);
                else if (alg == "nearest")
                    v = evalNearest(cfg, el, pts, xp, yp);
                else if (alg == "linear")
                    v = evalLinear(cfg, dt, pts, xp, yp);
                else if (isMetric)
                {
                    if (cfg.quadrant())
                        v = evalMetricQuadrant(metric, cfg, pts, xp, yp);
                    else
                        v = evalMetric(metric, cfg, el, pts, xp, yp);
                }
                else
                    v = cfg.nodata;
                rasterEncodeReal(
                    type,
                    band + ((size_t)row * width + col) * pxSize,
                    encodeGridValue(type, v), 0.0);
            }
            if (!quiet)
                prog.update((row + 1) / (double)height);
        }
        gridComputedAny = true;
    }

    // --crs only takes effect once a grid was actually computed; skip
    // paths keep the layer SRS set at create time
    const Srs *outSrs = nullptr;
    if (haveCrsOverride && gridComputedAny)
        outSrs = &crsOverride;
    else if (haveLayerSrs)
        outSrs = &layerSrs;

    if (!writeResult(gtReady, outSrs))
        return 1;
    return aborted ? 1 : 0;
}

const char *const kGridAlgs[] = {
    "average",     "average-distance", "average-distance-points",
    "count",       "invdist",          "invdistnn",
    "linear",      "maximum",          "minimum",
    "nearest",     "range"};

}  // namespace

void registerVectorGridHandlers()
{
    for (const char *a : kGridAlgs)
    {
        const std::string id = std::string("vector_grid_") + a;
        const std::string alg = a;
        registerHandler(id, gridHandler);
        registerArgValueCheck(
            id,
            [alg](const std::string &argName,
                  const std::string &value) -> std::string {
                if (argName == "output-format")
                {
                    if (strEqualNoCase(value, "GDALG"))
                        return "\x07GDALG output is not supported.";
                    if (!strEqualNoCase(value, "GTiff") &&
                        !strEqualNoCase(value, "COG") &&
                        !strEqualNoCase(value, "MEM"))
                        return "Invalid value for argument "
                               "'output-format'. Driver '" +
                               value + "' does not exist.";
                }
                if (argName == "crs")
                {
                    bool ok = false;
                    Srs s = Srs::fromCliInput(value, ok, true);
                    if (!ok || !s.valid())
                        return "Invalid value for 'crs' argument";
                }
                return "";
            });
        registerPostValidator(
            id,
            [alg](const CmdSpec &, ParseResult &r, bool) -> bool {
                bool bad = false;
                const std::string of = r.str("output-format");
                const std::string out = r.str("output");
                if (of.empty() &&
                    strEndsWith(strToLower(out), ".gdalg.json"))
                {
                    cplErrorStr(CE_Failure, CPLE_NotSupported,
                                alg + ": GDALG output is not supported");
                    bad = true;
                }
                if (r.get("resolution") && !r.get("extent"))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                alg + ": 'resolution' should be defined "
                                      "when 'extent' is.");
                    bad = true;
                }
                if (r.get("radius1") && !r.get("radius2"))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                alg + ": 'radius2' should be defined "
                                      "when 'radius1' is.");
                    bad = true;
                }
                if (r.get("radius2") && !r.get("radius1"))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                alg + ": 'radius1' should be defined "
                                      "when 'radius2' is.");
                    bad = true;
                }
                const bool radiusGiven = r.get("radius") ||
                                         r.get("radius1") ||
                                         r.get("radius2");
                if (alg == "invdist")
                {
                    const ArgValue *mn = r.get("min-points");
                    if (mn && atoi(mn->str().c_str()) > 0 && !radiusGiven)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            alg + ": 'radius' or 'radius1' and 'radius2' "
                                  "should be defined when 'min-points' "
                                  "is.");
                        bad = true;
                    }
                    const ArgValue *mx = r.get("max-points");
                    if (mx && atoi(mx->str().c_str()) != 2147483647 &&
                        !radiusGiven)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            alg + ": 'radius' or 'radius1' and 'radius2' "
                                  "should be defined when 'max-points' "
                                  "is.");
                        bad = true;
                    }
                }
                if (alg == "average")
                {
                    const ArgValue *mx = r.get("max-points");
                    if (mx && atoi(mx->str().c_str()) != 2147483647 &&
                        !r.get("min-points-per-quadrant") &&
                        !r.get("max-points-per-quadrant"))
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            alg + ": 'min-points-per-quadrant' and/or "
                                  "'max-points-per-quadrant' should be "
                                  "defined when 'max-points' is.");
                        bad = true;
                    }
                }
                return bad;
            });
    }
}
