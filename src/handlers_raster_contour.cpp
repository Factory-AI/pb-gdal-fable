// gdal raster contour: marching-squares contour lines / polygons over one
// band, written through the shared vector_convert delegate.
#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "ogr.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <cstring>

namespace
{

void setArg(const CmdSpec &spec, ParseResult &r, const std::string &longName,
            const std::vector<std::string> &vals)
{
    const ArgSpec *a = spec.findLong(longName);
    if (!a)
        return;
    ArgValue &v = r.byName[a->name];
    v.spec = a;
    v.set = true;
    v.values = vals;
    r.order.push_back(a->name);
}

void initResult(const CmdSpec &spec, ParseResult &r)
{
    for (const auto &a : spec.args)
        r.byName[a.name].spec = &a;
}

bool fileExistsCt(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

int runConvertDelegateCt(std::unique_ptr<OgrDataset> srcDs,
                         const std::string &inputPath,
                         const std::string &output,
                         const std::string &format,
                         const std::vector<std::string> &co,
                         const std::vector<std::string> &lco,
                         const char *prefix, const ParseResult &vr)
{
    if (g_pipelineTransCapture)
    {
        g_pipelineTransCaptured = std::move(srcDs);
        return 0;
    }
    const Spec &spec = Spec::instance();
    const CmdSpec *cs = spec.findById("vector_convert");
    Handler h = findHandler("vector_convert");
    if (!cs || !h)
        return 1;
    ParseResult cr;
    initResult(*cs, cr);
    setArg(*cs, cr, "input", {inputPath});
    setArg(*cs, cr, "output", {output});
    if (!format.empty())
        setArg(*cs, cr, "of", {format});
    if (!co.empty())
        setArg(*cs, cr, "co", co);
    if (!lco.empty())
        setArg(*cs, cr, "lco", lco);
    for (const char *fl : {"overwrite", "append", "update",
                           "overwrite-layer", "upsert", "skip-errors"})
        if (vr.flag(fl))
            setArg(*cs, cr, fl, {"true"});
    setArg(*cs, cr, "quiet", {"true"});
    g_convertSourceOverride = std::move(srcDs);
    g_pipelineStepPrefix = prefix;
    int rc = h(*cs, cr);
    g_pipelineStepPrefix.clear();
    g_convertSourceOverride.reset();
    return rc;
}

// ------------------------------------------------------------- numerics

const double kNan = std::numeric_limits<double>::quiet_NaN();

struct Pt
{
    double x, y;
    bool operator==(const Pt &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Pt &o) const { return !(*this == o); }
};

struct PV
{
    double x, y, v;
};

inline double fudge(double level, double v)
{
    return std::fabs(v - level) < 1e-6 ? v + 1e-6 : v;
}

inline bool straddle(double level, double a, double b)
{
    double fa = fudge(level, a);
    double fb = fudge(level, b);
    return (fa > level) != (fb > level);
}

Pt interp1(double level, const PV &p0, const PV &p1)
{
    double f0 = fudge(level, p0.v);
    double f1 = fudge(level, p1.v);
    double ratio = (level - f0) / (f1 - f0);
    Pt p;
    p.x = p0.x == p1.x ? p0.x : p0.x * (1 - ratio) + p1.x * ratio;
    p.y = p0.y == p1.y ? p0.y : p0.y * (1 - ratio) + p1.y * ratio;
    return p;
}

PV midPoint(const PV &p0, const PV &p1)
{
    PV m;
    m.x = (p0.x + p1.x) / 2;
    m.y = (p0.y + p1.y) / 2;
    if (std::isnan(p0.v))
        m.v = p1.v;
    else if (std::isnan(p1.v))
        m.v = p0.v;
    else
        m.v = (p0.v + p1.v) / 2;
    return m;
}

Pt interp2(double level, const PV &p0, const PV &p1)
{
    PV m = midPoint(p0, p1);
    if (straddle(level, p0.v, m.v))
        return interp1(level, p0, m);
    return interp1(level, m, p1);
}

// border crossings blend in px space with canonical operand order:
// horizontal west-first, vertical south-first
Pt crossCanon(double level, const PV &a, const PV &b)
{
    const PV *p0 = &a;
    const PV *p1 = &b;
    if (a.y == b.y)
    {
        if (a.x > b.x)
            std::swap(p0, p1);
    }
    else if (a.y < b.y)
        std::swap(p0, p1);
    return interp1(level, *p0, *p1);
}

// ------------------------------------------------------- level generators

struct LevelGen
{
    // interval/exp levels are clamped to the band-nodata-masked data range
    // (dmin exclusive, dmax inclusive); fixed levels are not
    double gdmin = -std::numeric_limits<double>::infinity();
    double gdmax = std::numeric_limits<double>::infinity();
    bool inRange(double v) const { return v > gdmin && v <= gdmax; }
    virtual ~LevelGen() = default;
    // ids and values of the levels a square spanning [mn, mx] must probe
    virtual void range(double mn, double mx,
                       std::vector<std::pair<long long, double>> &out)
        const = 0;
    // display value of level id (line elevation attribute)
    virtual double levelValue(long long li) const = 0;
    // polygon-mode bucket of a border value / bound of bucket k
    virtual long long bucketOf(double) const { return 0; }
    virtual double levelAt(long long) const { return 0; }
    // polygon feature bounds; false = skip the bucket
    virtual bool bucketBounds(long long k, bool hasData, double dmin,
                              double dmax, double &mi, double &ma) const
    {
        (void)k;
        (void)hasData;
        (void)dmin;
        (void)dmax;
        (void)mi;
        (void)ma;
        return false;
    }
};

struct IntervalGen : LevelGen
{
    double interval, offset;
    IntervalGen(double i, double o) : interval(i), offset(o) {}

    void range(double mn, double mx,
               std::vector<std::pair<long long, double>> &out) const override
    {
        long long i1 = (long long)std::ceil((mn - offset) / interval);
        long long i2 = (long long)std::floor((mx - offset) / interval) + 1;
        for (long long k = i1; k < i2; ++k)
        {
            double v = offset + k * interval;
            if (inRange(v))
                out.emplace_back(k, v);
        }
    }
    double levelValue(long long li) const override
    {
        return offset + li * interval;
    }
};

struct PolyIntervalGen : LevelGen
{
    double interval, offset, dmax;
    long long extraIdx;
    bool extraRegular;

    PolyIntervalGen(double i, double o, double dm)
        : interval(i), offset(o), dmax(dm)
    {
        extraIdx = (long long)std::ceil((dmax - offset) / interval);
        extraRegular = offset + extraIdx * interval == dmax;
    }

    void range(double mn, double mx,
               std::vector<std::pair<long long, double>> &out) const override
    {
        long long i1 = (long long)std::ceil((mn - offset) / interval);
        long long i2 = (long long)std::floor((mx - offset) / interval) + 1;
        for (long long k = i1; k < i2; ++k)
        {
            double v = offset + k * interval;
            if (inRange(v))
                out.emplace_back(k, v);
        }
        if (!extraRegular && mx >= dmax && dmax > mn)
            out.emplace_back(extraIdx, dmax);
    }
    double levelValue(long long li) const override
    {
        return offset + li * interval;
    }
    long long bucketOf(double v) const override
    {
        if (!extraRegular && v == dmax)
            return extraIdx + 1;
        long long k = (long long)std::ceil((v - offset) / interval);
        if (v == offset + k * interval)
            ++k;
        return k;
    }
    double levelAt(long long k) const override
    {
        if (!extraRegular && k == extraIdx)
            return dmax;
        return offset + k * interval;
    }
    bool bucketBounds(long long k, bool hasData, double dmin, double dmaxD,
                      double &mi, double &ma) const override
    {
        if (!hasData)
            return false;
        mi = std::max(levelAt(k - 1), dmin);
        ma = std::min(levelAt(k), dmaxD);
        return mi < ma;
    }
};

struct FixedGen : LevelGen
{
    std::vector<double> levels;
    bool poly;
    explicit FixedGen(std::vector<double> l, bool p)
        : levels(std::move(l)), poly(p)
    {
    }

    void range(double mn, double mx,
               std::vector<std::pair<long long, double>> &out) const override
    {
        for (size_t i = 0; i < levels.size(); ++i)
        {
            double l = levels[i];
            if ((mn <= l && l <= mx) || straddle(l, mn, mx) ||
                straddle(l, mn, l) || straddle(l, l, mx))
                out.emplace_back((long long)i, l);
        }
    }
    double levelValue(long long li) const override
    {
        return levels[(size_t)li];
    }
    long long bucketOf(double v) const override
    {
        for (size_t i = 0; i < levels.size(); ++i)
        {
            if (levels[i] == v)
                return (long long)i + 1;
            if (levels[i] > v)
                return (long long)i;
        }
        return (long long)levels.size();
    }
    double levelAt(long long k) const override
    {
        if (k < 0)
            return -std::numeric_limits<double>::infinity();
        if (k >= (long long)levels.size())
            return std::numeric_limits<double>::infinity();
        return levels[(size_t)k];
    }
    bool bucketBounds(long long k, bool hasData, double, double, double &mi,
                      double &ma) const override
    {
        if (!hasData || k <= 0 || k >= (long long)levels.size())
            return false;
        mi = levels[(size_t)(k - 1)];
        ma = levels[(size_t)k];
        return mi < ma;
    }
};

struct ExpGen : LevelGen
{
    double base;
    explicit ExpGen(double b) : base(b) {}

    void range(double mn, double mx,
               std::vector<std::pair<long long, double>> &out) const override
    {
        long long k = 0;
        while (std::pow(base, (double)k) < mn)
            ++k;
        while (std::pow(base, (double)k) <= mx)
        {
            double v = std::pow(base, (double)k);
            if (inRange(v))
                out.emplace_back(k, v);
            ++k;
        }
    }
    double levelValue(long long li) const override
    {
        return std::pow(base, (double)li);
    }
};

struct PolyExpGen : LevelGen
{
    std::vector<double> powers;  // base^k <= dmax, k >= 0
    double dmax;
    bool extraRegular;

    PolyExpGen(double base, double dm) : dmax(dm)
    {
        long long k = 0;
        while (std::pow(base, (double)k) <= dmax)
        {
            powers.push_back(std::pow(base, (double)k));
            ++k;
        }
        extraRegular = !powers.empty() && powers.back() == dmax;
    }
    long long extraIdx() const { return (long long)powers.size(); }

    void range(double mn, double mx,
               std::vector<std::pair<long long, double>> &out) const override
    {
        for (size_t i = 0; i < powers.size(); ++i)
            if (mn <= powers[i] && powers[i] <= mx && inRange(powers[i]))
                out.emplace_back((long long)i, powers[i]);
        if (!extraRegular && mx >= dmax && dmax > mn)
            out.emplace_back(extraIdx(), dmax);
    }
    double levelValue(long long li) const override
    {
        return powers[(size_t)li];
    }
    long long bucketOf(double v) const override
    {
        if (!extraRegular && v == dmax)
            return extraIdx() + 1;
        for (size_t i = 0; i < powers.size(); ++i)
        {
            if (powers[i] == v)
                return (long long)i + 1;
            if (powers[i] > v)
                return (long long)i;
        }
        return extraIdx();
    }
    double levelAt(long long k) const override
    {
        if (k < 0)
            return -std::numeric_limits<double>::infinity();
        if (k < (long long)powers.size())
            return powers[(size_t)k];
        if (!extraRegular && k == extraIdx())
            return dmax;
        return std::numeric_limits<double>::infinity();
    }
    bool bucketBounds(long long k, bool hasData, double dmin, double dmaxD,
                      double &mi, double &ma) const override
    {
        if (!hasData)
            return false;
        mi = std::max(levelAt(k - 1), dmin);
        ma = std::min(levelAt(k), dmaxD);
        return mi < ma;
    }
};

// --------------------------------------------------------------- merger

struct Merger
{
    struct Entry
    {
        std::vector<Pt> pts;
        bool touched = false;
        bool closed = false;
        int kind = -1;
        long long seq = 0;
    };

    bool polyMode = false;
    int curKind = -1;
    long long seqCtr = 0;
    std::map<long long, std::list<Entry>> lines;
    struct Emitted
    {
        long long li;
        std::vector<Pt> pts;
        int kind;
        long long seq;
    };
    std::vector<Emitted> emitted;

    void addSegment(long long li, const Pt &p0, const Pt &p1)
    {
        if (p0 == p1)
            return;
        auto &lst = lines[li];
        auto mergedIt = lst.end();
        for (auto it = lst.begin(); it != lst.end(); ++it)
        {
            if (it->closed)
                continue;
            auto &pts = it->pts;
            bool att = false;
            if (polyMode)
            {
                const char *ord = "1302";
                for (int oi = 0; !att && oi < 4; ++oi)
                {
                    switch (ord[oi])
                    {
                        case '0': // F0
                            if (pts.front() == p0)
                            {
                                pts.insert(pts.begin(), p1);
                                att = true;
                            }
                            break;
                        case '1': // F1
                            if (pts.front() == p1)
                            {
                                pts.insert(pts.begin(), p0);
                                att = true;
                            }
                            break;
                        case '2': // B0
                            if (pts.back() == p0)
                            {
                                pts.push_back(p1);
                                att = true;
                            }
                            break;
                        default: // B1
                            if (pts.back() == p1)
                            {
                                pts.push_back(p0);
                                att = true;
                            }
                            break;
                    }
                }
            }
            else
            {
                // attach order F1 F0 B0 B1
                if (pts.front() == p1)
                {
                    pts.insert(pts.begin(), p0);
                    att = true;
                }
                else if (pts.front() == p0)
                {
                    pts.insert(pts.begin(), p1);
                    att = true;
                }
                else if (pts.back() == p0)
                {
                    pts.push_back(p1);
                    att = true;
                }
                else if (pts.back() == p1)
                {
                    pts.push_back(p0);
                    att = true;
                }
            }
            if (!att)
                continue;
            it->touched = true;
            mergedIt = it;
            break;
        }
        if (mergedIt == lst.end())
        {
            Entry e;
            e.pts = {p0, p1};
            e.touched = true;
            e.kind = curKind;
            e.seq = seqCtr++;
            lst.push_back(std::move(e));
            return;
        }
        if (closeCheck(li, lst, mergedIt))
            return;
        bool again = true;
        while (again)
        {
            again = false;
            for (auto it = lst.begin(); it != lst.end(); ++it)
            {
                if (it == mergedIt || it->closed)
                    continue;
                auto &a = mergedIt->pts;
                auto &b = it->pts;
                if (a.back() == b.front())
                {
                    a.insert(a.end(), b.begin() + 1, b.end());
                    if (it->seq < mergedIt->seq)
                    {
                        mergedIt->seq = it->seq;
                        mergedIt->kind = it->kind;
                    }
                    lst.erase(it);
                }
                else if (a.back() == b.back())
                {
                    a.insert(a.end(), b.rbegin() + 1, b.rend());
                    if (it->seq < mergedIt->seq)
                    {
                        mergedIt->seq = it->seq;
                        mergedIt->kind = it->kind;
                    }
                    lst.erase(it);
                }
                else if (a.front() == b.back())
                {
                    b.insert(b.end(), a.begin() + 1, a.end());
                    it->touched = true;
                    if (mergedIt->seq < it->seq)
                    {
                        it->seq = mergedIt->seq;
                        it->kind = mergedIt->kind;
                    }
                    lst.erase(mergedIt);
                    mergedIt = it;
                }
                else if (a.front() == b.front())
                {
                    std::reverse(b.begin(), b.end());
                    b.insert(b.end(), a.begin() + 1, a.end());
                    it->touched = true;
                    if (mergedIt->seq < it->seq)
                    {
                        it->seq = mergedIt->seq;
                        it->kind = mergedIt->kind;
                    }
                    lst.erase(mergedIt);
                    mergedIt = it;
                }
                else
                    continue;
                if (closeCheck(li, lst, mergedIt))
                    return;
                again = true;
                break;
            }
        }
    }

    bool closeCheck(long long li, std::list<Entry> &lst,
                    std::list<Entry>::iterator mergedIt)
    {
        if (mergedIt->pts.front() != mergedIt->pts.back())
            return false;
        mergedIt->closed = true;
        if (polyMode)
        {
            emitted.push_back(
                {li, std::move(mergedIt->pts), mergedIt->kind, mergedIt->seq});
            lst.erase(mergedIt);
        }
        return true;
    }

    void beginRow()
    {
        for (auto &kv : lines)
            for (auto &e : kv.second)
                e.touched = false;
    }

    void endRow()
    {
        for (auto &kv : lines)
        {
            for (auto it = kv.second.begin(); it != kv.second.end();)
            {
                if (!it->touched)
                {
                    emitted.push_back(
                        {kv.first, std::move(it->pts), it->kind, it->seq});
                    it = kv.second.erase(it);
                }
                else
                    ++it;
            }
        }
    }

    void finish()
    {
        for (auto &kv : lines)
            for (auto &e : kv.second)
                emitted.push_back({kv.first, std::move(e.pts), e.kind, e.seq});
        lines.clear();
    }
};

// --------------------------------------------------------------- squares

// segment side tables per marching-squares case; 0 = none
struct CaseSegs
{
    int n;
    char s[2][2];
};

const CaseSegs *caseSegs(int c)
{
    static const std::map<int, CaseSegs> k = {
        {1, {1, {{'U', 'W'}, {0, 0}}}},   {2, {1, {{'E', 'U'}, {0, 0}}}},
        {4, {1, {{'W', 'L'}, {0, 0}}}},   {8, {1, {{'L', 'E'}, {0, 0}}}},
        {3, {1, {{'E', 'W'}, {0, 0}}}},   {12, {1, {{'W', 'E'}, {0, 0}}}},
        {5, {1, {{'U', 'L'}, {0, 0}}}},   {10, {1, {{'L', 'U'}, {0, 0}}}},
        {7, {1, {{'E', 'L'}, {0, 0}}}},   {11, {1, {{'L', 'W'}, {0, 0}}}},
        {13, {1, {{'U', 'E'}, {0, 0}}}},  {14, {1, {{'W', 'U'}, {0, 0}}}},
        {9, {2, {{'U', 'E'}, {'L', 'W'}}}},
        {6, {2, {{'E', 'U'}, {'W', 'L'}}}},
    };
    auto it = k.find(c);
    return it == k.end() ? nullptr : &it->second;
}

struct HullSide
{
    PV a, b;
    bool preAll = false;
};

inline HullSide hullMk(int, const PV &a, const PV &b, bool preAll)
{
    return {a, b, preAll};
}

struct Sq
{
    PV ul, ur, ll, lr;
    bool split = false;
    std::vector<HullSide> hull;

    PV center() const
    {
        const PV *cs[4] = {&ul, &ur, &ll, &lr};
        double s = 0;
        int n = 0;
        for (const PV *p : cs)
            if (!std::isnan(p->v))
            {
                s += p->v;
                ++n;
            }
        PV c;
        c.x = (ul.x + lr.x) / 2;
        c.y = (ul.y + lr.y) / 2;
        c.v = n ? s / n : kNan;
        return c;
    }

    Pt sidePoint(char side, double level) const
    {
        const PV *a;
        const PV *b;
        switch (side)
        {
            case 'U':
                a = &ul;
                b = &ur;
                break;
            case 'L':
                a = &ll;
                b = &lr;
                break;
            case 'W':
                a = &ll;
                b = &ul;
                break;
            default:
                a = &lr;
                b = &ur;
                break;
        }
        return split ? interp1(level, *a, *b) : interp2(level, *a, *b);
    }
};

struct HullPiece
{
    long long li;
    Pt p, q;
    int kind;
};

// walk the border edge from oa to ob (valid interior on the left);
// crossings split the edge and each piece lands in its own bucket.
// Pieces keep the walk direction but are listed in enumeration order:
// horizontals enumerate along the walk, verticals against it.
void hullPieces(const LevelGen &gen, const PV &oa, const PV &ob,
                std::vector<HullPiece> &out)
{
    const PV *a = &oa;
    const PV *b = &ob;
    bool rev = false;
    if (a->v > b->v)
    {
        std::swap(a, b);
        rev = true;
    }
    std::vector<std::pair<long long, double>> lv;
    gen.range(a->v, b->v, lv);
    std::vector<HullPiece> pieces;
    Pt prev = {a->x, a->y};
    for (const auto &pr : lv)
    {
        if (straddle(pr.second, a->v, b->v))
        {
            Pt pt = crossCanon(pr.second, oa, ob);
            pieces.push_back({pr.first, prev, pt, 2});
            prev = pt;
        }
    }
    pieces.push_back({gen.bucketOf(b->v), prev, {b->x, b->y}, 3});
    if (rev)
    {
        std::reverse(pieces.begin(), pieces.end());
        for (auto &pc : pieces)
            std::swap(pc.p, pc.q);
    }
    if (oa.x == ob.x)
        std::reverse(pieces.begin(), pieces.end());
    out.insert(out.end(), pieces.begin(), pieces.end());
}

void processSquare(const Sq &sq, const LevelGen &gen, Merger &merger,
                   bool poly, double xhi, double yhi)
{
    const PV *cs[4] = {&sq.ul, &sq.ur, &sq.ll, &sq.lr};
    if (!sq.split && (std::isnan(sq.ul.v) || std::isnan(sq.ur.v) ||
                      std::isnan(sq.ll.v) || std::isnan(sq.lr.v)))
    {
        PV c = sq.center();
        PV uc = midPoint(sq.ul, sq.ur);
        PV lc = midPoint(sq.ll, sq.lr);
        PV lec = midPoint(sq.ul, sq.ll);
        PV ric = midPoint(sq.ur, sq.lr);
        bool ulv = !std::isnan(sq.ul.v);
        bool urv = !std::isnan(sq.ur.v);
        bool llv = !std::isnan(sq.ll.v);
        bool lrv = !std::isnan(sq.lr.v);
        auto offRaster = [&](const PV &p) {
            return p.x < 0.5 || p.x > xhi || p.y < 0.5 || p.y > yhi;
        };
        if (ulv)
        {
            Sq s{sq.ul, uc, lec, c, true, {}};
            if (poly)
            {
                if (!urv)
                    s.hull.push_back(hullMk(0, c, uc, offRaster(sq.ur)));
                if (!llv)
                    s.hull.push_back(hullMk(1, lec, c, false));
            }
            processSquare(s, gen, merger, poly, xhi, yhi);
        }
        if (urv)
        {
            Sq s{uc, sq.ur, c, ric, true, {}};
            if (poly)
            {
                if (!ulv)
                    s.hull.push_back(hullMk(2, uc, c, true));
                if (!lrv)
                    s.hull.push_back(hullMk(3, c, ric, false));
            }
            processSquare(s, gen, merger, poly, xhi, yhi);
        }
        if (llv)
        {
            Sq s{lec, c, sq.ll, lc, true, {}};
            if (poly)
            {
                if (!ulv)
                    s.hull.push_back(hullMk(4, c, lec, true));
                if (!lrv)
                    s.hull.push_back(hullMk(5, lc, c, offRaster(sq.lr)));
            }
            processSquare(s, gen, merger, poly, xhi, yhi);
        }
        if (lrv)
        {
            Sq s{c, ric, lc, sq.lr, true, {}};
            if (poly)
            {
                if (!urv)
                    s.hull.push_back(hullMk(6, ric, c, true));
                if (!llv)
                    s.hull.push_back(hullMk(7, c, lc, true));
            }
            processSquare(s, gen, merger, poly, xhi, yhi);
        }
        return;
    }
    double vals[4] = {cs[0]->v, cs[1]->v, cs[2]->v, cs[3]->v};
    double mn = std::min(std::min(vals[0], vals[1]),
                         std::min(vals[2], vals[3]));
    double mx = std::max(std::max(vals[0], vals[1]),
                         std::max(vals[2], vals[3]));
    std::vector<std::pair<long long, double>> lv;
    gen.range(mn, mx, lv);
    // border pieces already below a segment-bearing level go out before the
    // level loop (through the last covered piece per edge), the rest after
    std::vector<std::vector<HullPiece>> edges;
    std::vector<size_t> cut;
    if (!sq.hull.empty())
    {
        bool anySeg = false;
        long long maxSeg = 0;
        for (const auto &pr : lv)
        {
            double level = pr.second;
            int cse = (fudge(level, vals[0]) > level ? 1 : 0) |
                      (fudge(level, vals[1]) > level ? 2 : 0) |
                      (fudge(level, vals[2]) > level ? 4 : 0) |
                      (fudge(level, vals[3]) > level ? 8 : 0);
            if (caseSegs(cse))
            {
                anySeg = true;
                maxSeg = pr.first;
            }
        }
        for (const auto &h : sq.hull)
        {
            edges.emplace_back();
            hullPieces(gen, h.a, h.b, edges.back());
        }
        for (size_t k = 0; k < edges.size(); ++k)
        {
            const auto &e = edges[k];
            size_t j = 0;
            if (sq.hull[k].preAll)
                j = e.size();
            else
                for (size_t i = 0; i < e.size(); ++i)
                    if (anySeg && e[i].li <= maxSeg)
                        j = i + 1;
            cut.push_back(j);
        }
        for (size_t k = 0; k < edges.size(); ++k)
            for (size_t i = 0; i < cut[k]; ++i)
            {
                merger.curKind = edges[k][i].kind;
                merger.addSegment(edges[k][i].li, edges[k][i].p,
                                  edges[k][i].q);
            }
    }
    for (const auto &pr : lv)
    {
        double level = pr.second;
        int cse = (fudge(level, vals[0]) > level ? 1 : 0) |
                  (fudge(level, vals[1]) > level ? 2 : 0) |
                  (fudge(level, vals[2]) > level ? 4 : 0) |
                  (fudge(level, vals[3]) > level ? 8 : 0);
        const CaseSegs *segs = caseSegs(cse);
        if (!segs)
            continue;
        for (int s = 0; s < segs->n; ++s)
        {
            Pt p0 = sq.sidePoint(segs->s[s][0], level);
            Pt p1 = sq.sidePoint(segs->s[s][1], level);
            merger.curKind = 0;
            merger.addSegment(pr.first, p0, p1);
            if (poly)
            {
                merger.curKind = 1;
                merger.addSegment(pr.first + 1, p0, p1);
            }
        }
    }
    for (size_t k = 0; k < edges.size(); ++k)
        for (size_t i = cut[k]; i < edges[k].size(); ++i)
        {
            merger.curKind = edges[k][i].kind;
            merger.addSegment(edges[k][i].li, edges[k][i].p, edges[k][i].q);
        }
}

struct Grid
{
    const std::vector<double> *vals;
    int w, h;
    bool hasNd;
    double nd;

    double at(int i, int j) const
    {
        if (i < 1 || i > w || j < 1 || j > h)
            return kNan;
        double v = (*vals)[(size_t)(j - 1) * w + (i - 1)];
        if (hasNd && v == nd)
            return kNan;
        return v;
    }
};

void runGrid(const Grid &g, const LevelGen &gen, Merger &merger, bool poly)
{
    for (int j = 0; j <= g.h; ++j)
    {
        merger.beginRow();
        for (int i = 0; i <= g.w; ++i)
        {
            Sq sq;
            sq.ul = {i - 0.5, j - 0.5, g.at(i, j)};
            sq.ur = {i + 0.5, j - 0.5, g.at(i + 1, j)};
            sq.ll = {i - 0.5, j + 0.5, g.at(i, j + 1)};
            sq.lr = {i + 0.5, j + 0.5, g.at(i + 1, j + 1)};
            processSquare(sq, gen, merger, poly, g.w - 0.5, g.h - 0.5);
        }
        merger.endRow();
    }
    merger.finish();
}

// ------------------------------------------------- polygon ring nesting

bool pnpoly(const Pt &pt, const std::vector<Pt> &ring)
{
    double x = pt.x, y = pt.y;
    bool inside = false;
    size_t n = ring.size();
    size_t j = n - 1;
    for (size_t i = 0; i < n; ++i)
    {
        double xi = ring[i].x, yi = ring[i].y;
        double xj = ring[j].x, yj = ring[j].y;
        if ((yi > y) != (yj > y))
        {
            double xint = (xj - xi) * (y - yi) / (yj - yi) + xi;
            if (x < xint)
                inside = !inside;
        }
        j = i;
    }
    return inside;
}

struct FNode
{
    std::vector<Pt> ring;
    std::vector<FNode> children;
};

// incremental containment forest: descend into the containing node, else
// swallow the nodes the new ring contains and append at the back
void forestInsert(std::vector<FNode> &nodes, FNode nr)
{
    for (auto &n : nodes)
    {
        if (pnpoly(nr.ring.front(), n.ring))
        {
            forestInsert(n.children, std::move(nr));
            return;
        }
    }
    size_t i = 0;
    while (i < nodes.size())
    {
        if (pnpoly(nodes[i].ring.front(), nr.ring))
        {
            nr.children.push_back(std::move(nodes[i]));
            nodes.erase(nodes.begin() + i);
        }
        else
            ++i;
    }
    nodes.push_back(std::move(nr));
}

// DFS write order: shell, its holes, then polygons nested inside the holes
void forestEmit(std::vector<FNode> &nodes,
                std::vector<std::vector<std::vector<Pt>>> &polys)
{
    for (auto &n : nodes)
    {
        polys.emplace_back();
        size_t pi = polys.size() - 1;
        polys[pi].push_back(std::move(n.ring));
        for (auto &c : n.children)
            polys[pi].push_back(std::move(c.ring));
        for (auto &c : n.children)
            forestEmit(c.children, polys);
    }
}

// ------------------------------------------------------------ assembly

void appendRingCoords(OgrGeometry &ring, const std::vector<Pt> &pts,
                      bool z3d, double zval)
{
    ring.type = 2;
    ring.hasZ = z3d;
    ring.coords.reserve(pts.size() * 3);
    for (const auto &p : pts)
    {
        ring.coords.push_back(p.x);
        ring.coords.push_back(p.y);
        ring.coords.push_back(z3d ? zval : 0.0);
    }
}

OgrFieldValue jvInt(long long v)
{
    OgrFieldValue fv;
    fv.set = true;
    fv.v.type = JVal::INT;
    fv.v.i = v;
    return fv;
}

OgrFieldValue jvDouble(double v)
{
    OgrFieldValue fv;
    fv.set = true;
    fv.v.type = JVal::DOUBLE;
    fv.v.d = v;
    return fv;
}

// classic gdal_contour usage block printed on a negative -e
const char kContourUsage[] =
    "Usage: gdal_contour [--help] [--long-usage] [--help-general]\n"
    "                    [-b <name>] [-a <name>] [-amin <name>] [-amax "
    "<name>] [-3d]\n"
    "                    [-inodata] [-snodata <value>]\n"
    "                    [[-i <interval>]|[-e <base>]]\n"
    "                    [-fl <level>] [-off <offset>] [-nln <name>] [-p]\n"
    "                    [-gt <n>|unlimited] [--quiet] [-oo "
    "<NAME>=<VALUE>]...\n"
    "                    [-dsco <NAME>=<VALUE>]... [-of <output_format>]\n"
    "                    [-co <NAME>=<VALUE>]... [-lco <NAME>=<VALUE>]...\n"
    "                    src_filename dst_filename\n"
    "\n"
    "One of -i, -fl or -e must be specified.\n";

int rasterContourHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool poly = r.flag("polygonize");
    bool want3d = r.flag("3d");
    long long band = strtoll(r.str("band", "1").c_str(), nullptr, 10);

    bool hasInterval = r.get("interval") != nullptr;
    double interval = strtod(r.str("interval").c_str(), nullptr);
    double offset = strtod(r.str("offset", "0").c_str(), nullptr);
    std::vector<std::string> levelTokens = r.list("levels");
    long long expBase =
        r.get("exp-base") ? strtoll(r.str("exp-base").c_str(), nullptr, 10)
                          : 0;
    bool hasExp = expBase != 0;

    if (!hasInterval && levelTokens.empty() && !hasExp)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "contour: One of 'interval', 'levels', 'exp-base' must "
                    "be specified.");
        return 1;
    }
    if (hasExp && expBase < 0)
    {
        fputs(kContourUsage, stderr);
        return 1;
    }

    std::string format = r.str("output-format");
    std::string driver;
    if (!format.empty())
    {
        if (!vectorOutputDriverResolve(format, driver).empty())
            return 1;  // parse-time check already rejected
    }
    else
    {
        std::string low = strToLower(output);
        if (!strEndsWith(low, ".gdalg.json") &&
            (strEndsWith(low, ".json") || strEndsWith(low, ".geojson")))
            driver = "GeoJSON";
        else if (strEndsWith(low, ".geojsonl") ||
                 strEndsWith(low, ".geojsons"))
            driver = "GeoJSONSeq";
        else if (strEndsWith(low, ".shp") || strEndsWith(low, ".dbf"))
            driver = "ESRI Shapefile";
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot guess driver for " + output);
            return 1;
        }
    }
    if (driver == "GDALG" || driver == "stream")
    {
        fprintf(stderr, "Unable to find format driver named %s.\n",
                driver.c_str());
        return 1;
    }

    if (poly && r.get("elevation-name"))
        cplErrorStr(CE_Warning, CPLE_NotSupported,
                    "-a is ignored in polygonal contouring mode. Use -amin "
                    "and/or -amax instead");
    if (!poly && (r.get("min-name") || r.get("max-name")))
        cplErrorStr(CE_Warning, CPLE_NotSupported,
                    "-amin and/or -amax are ignored in line contouring "
                    "mode. Use -a instead");

    std::string err;
    auto ds = openRaster(input, err);
    if (!ds)
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed, err);
        return 1;
    }
    if (g_pipelineTailMaterialize && g_pipelineTailMaterialize(ds))
        return 1;
    if (band < 1 || band > (long long)ds->bands.size())
        return 1;

    std::vector<double> vals;
    if (!readBandValues(*ds, (int)band, vals))
        return 1;

    Grid g;
    g.vals = &vals;
    g.w = ds->width;
    g.h = ds->height;
    g.hasNd = false;
    g.nd = 0;
    if (r.get("src-nodata"))
    {
        g.hasNd = true;
        g.nd = strtod(r.str("src-nodata").c_str(), nullptr);
    }
    else if (ds->bands[(size_t)band - 1].hasNodata)
    {
        g.hasNd = true;
        g.nd = ds->bands[(size_t)band - 1].nodata;
    }

    // the level range always masks by the band-declared nodata; an
    // explicit --src-nodata only swaps the marching absence value
    bool rangeNd = ds->bands[(size_t)band - 1].hasNodata;
    double rangeNdVal = rangeNd ? ds->bands[(size_t)band - 1].nodata : 0;
    bool hasData = false;
    double dmin = 0, dmax = 0;
    for (int j = 1; j <= g.h; ++j)
        for (int i = 1; i <= g.w; ++i)
        {
            double v = (*g.vals)[(size_t)(j - 1) * g.w + (i - 1)];
            if (std::isnan(v) || (rangeNd && v == rangeNdVal))
                continue;
            if (!hasData)
            {
                dmin = dmax = v;
                hasData = true;
            }
            else
            {
                if (v < dmin)
                    dmin = v;
                if (v > dmax)
                    dmax = v;
            }
        }

    std::vector<double> fixedLevels;
    if (!levelTokens.empty())
    {
        for (const auto &t : levelTokens)
        {
            if (strEqualNoCase(t, "MIN"))
                fixedLevels.push_back(dmin);
            else if (strEqualNoCase(t, "MAX"))
                fixedLevels.push_back(dmax);
            else
                fixedLevels.push_back(strtod(t.c_str(), nullptr));
        }
        for (size_t i = 1; i < fixedLevels.size(); ++i)
            if (fixedLevels[i] <= fixedLevels[i - 1])
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "FIXED_LEVELS should be strictly increasing");
                return 1;
            }
    }
    if (hasData &&
        ((hasInterval && (dmax - dmin) / interval > 100000) ||
         (hasExp && expBase == 1)))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Input values and/or interval settings would lead to "
                    "too many levels");
        return 1;
    }

    std::unique_ptr<LevelGen> gen;
    if (!levelTokens.empty())
        gen.reset(new FixedGen(fixedLevels, poly));
    else if (hasInterval)
    {
        if (poly && hasData)
            gen.reset(new PolyIntervalGen(interval, offset, dmax));
        else
            gen.reset(new IntervalGen(interval, offset));
    }
    else
    {
        if (poly && hasData)
            gen.reset(new PolyExpGen((double)expBase, dmax));
        else
            gen.reset(new ExpGen((double)expBase));
    }

    if (hasData && levelTokens.empty())
    {
        gen->gdmin = dmin;
        gen->gdmax = dmax;
    }

    Merger merger;
    merger.polyMode = poly;
    if (hasData)
        runGrid(g, *gen, merger, poly);

    const double *gt = ds->gt;
    auto geoX = [&](const Pt &p) { return gt[0] + p.x * gt[1] + p.y * gt[2]; };
    auto geoY = [&](const Pt &p) { return gt[3] + p.x * gt[4] + p.y * gt[5]; };

    auto uds = std::unique_ptr<OgrDataset>(new OgrDataset());
    uds->path = output;
    uds->driverShort = "MEM";
    OgrLayer ul;
    std::string nln = r.str("output-layer");
    ul.name = nln.empty() ? "contour" : nln;
    ul.geomType = poly ? 6 : 2;
    ul.geomHasZ = want3d;
    ul.hasSrs = ds->hasSrs;
    if (ds->hasSrs)
        ul.srs = ds->srs;

    OgrFieldDefn idf;
    idf.name = "ID";
    idf.type = OFTInteger;
    idf.width = 8;
    ul.fields.push_back(idf);
    bool withElev = !poly && r.get("elevation-name") != nullptr;
    bool withMin = poly && r.get("min-name") != nullptr;
    bool withMax = poly && r.get("max-name") != nullptr;
    auto realField = [&](const std::string &name)
    {
        OgrFieldDefn f;
        f.name = name;
        f.type = OFTReal;
        f.width = 24;
        f.precision = 15;
        ul.fields.push_back(f);
    };
    if (withElev)
        realField(r.str("elevation-name"));
    if (withMin)
        realField(r.str("min-name"));
    if (withMax)
        realField(r.str("max-name"));

    if (!poly)
    {
        long long id = 0;
        for (auto &em : merger.emitted)
        {
            double level = gen->levelValue(em.li);
            OgrFeature f;
            f.values.push_back(jvInt(id));
            if (withElev)
                f.values.push_back(jvDouble(level));
            f.hasGeom = true;
            f.geom.type = 2;
            f.geom.hasZ = want3d;
            f.geom.coords.reserve(em.pts.size() * 3);
            for (const auto &p : em.pts)
            {
                f.geom.coords.push_back(geoX(p));
                f.geom.coords.push_back(geoY(p));
                f.geom.coords.push_back(want3d ? level : 0.0);
            }
            ul.features.push_back(std::move(f));
            ++id;
        }
    }
    else
    {
        std::map<long long, std::vector<std::vector<Pt>>> buckets;
        for (auto &em : merger.emitted)
        {
            std::vector<Pt> geo;
            geo.reserve(em.pts.size());
            for (const auto &p : em.pts)
                geo.push_back({geoX(p), geoY(p)});
            buckets[em.li].push_back(std::move(geo));
        }
        long long id = 0;
        for (auto &kv : buckets)
        {
            double mi = 0, ma = 0;
            if (!gen->bucketBounds(kv.first, hasData, dmin, dmax, mi, ma))
                continue;
            std::vector<FNode> forest;
            for (auto &ring : kv.second)
            {
                FNode n;
                n.ring = std::move(ring);
                forestInsert(forest, std::move(n));
            }
            std::vector<std::vector<std::vector<Pt>>> polys;
            forestEmit(forest, polys);
            OgrFeature f;
            f.values.push_back(jvInt(id));
            if (withMin)
                f.values.push_back(jvDouble(mi));
            if (withMax)
                f.values.push_back(jvDouble(ma));
            f.hasGeom = true;
            f.geom.type = 6;
            f.geom.hasZ = want3d;
            for (auto &pl : polys)
            {
                OgrGeometry pg;
                pg.type = 3;
                pg.hasZ = want3d;
                for (auto &ring : pl)
                {
                    OgrGeometry rg;
                    appendRingCoords(rg, ring, want3d, ma);
                    pg.parts.push_back(std::move(rg));
                }
                f.geom.parts.push_back(std::move(pg));
            }
            ul.features.push_back(std::move(f));
            ++id;
        }
    }
    vectorLayerRecomputeExtent(ul);
    uds->layers.push_back(std::move(ul));

    if (!quiet)
        printProgress();

    return runConvertDelegateCt(std::move(uds), input, output, driver,
                                r.list("creation-option"),
                                r.list("layer-creation-option"), "contour",
                                r);
}

}  // namespace

void registerRasterContourHandler()
{
    registerHandler("raster_contour", rasterContourHandler);
    registerArgValueCheck(
        "raster_contour",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName != "output-format")
                return "";
            std::string driver;
            return vectorOutputDriverResolve(value, driver);
        });
    registerPreValidator(
        "raster_contour",
        [](const CmdSpec &, ParseResult &r) -> int {
            if (r.get("band"))
            {
                long long b = strtoll(r.str("band").c_str(), nullptr, 10);
                if (b < 1)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Value of 'band' should greater or equal "
                                "to 1.");
                    handlerPrintUsage();
                    return 1;
                }
            }
            std::vector<std::string> toks = r.list("levels");
            for (size_t i = 0; i < toks.size(); ++i)
                for (size_t j = i + 1; j < toks.size(); ++j)
                {
                    bool dup;
                    if (strEqualNoCase(toks[i], "MIN") ||
                        strEqualNoCase(toks[i], "MAX") ||
                        strEqualNoCase(toks[j], "MIN") ||
                        strEqualNoCase(toks[j], "MAX"))
                        dup = strEqualNoCase(toks[i], toks[j]);
                    else
                        dup = strtod(toks[i].c_str(), nullptr) ==
                              strtod(toks[j].c_str(), nullptr);
                    if (dup)
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "'levels' must be a list of unique "
                                    "values.");
                        handlerPrintUsage();
                        return 1;
                    }
                }
            return 0;
        });
    registerPostValidator(
        "raster_contour",
        [](const CmdSpec &, ParseResult &r, bool inputFailed) -> bool {
            bool bad = false;
            std::string output = r.str("output");
            std::string of = r.str("output-format");
            bool memLike = strEqualNoCase(of, "MEM") ||
                           strEqualNoCase(of, "Memory") ||
                           strEqualNoCase(of, "stream");
            if (!output.empty() && !memLike && fileExistsCt(output))
            {
                if (!r.flag("overwrite"))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "contour: " + outputExistsKind(output) +
                                    " '" + output +
                                    "' already exists. You may specify the "
                                    "--overwrite option.");
                    bad = true;
                }
                else
                    overwriteDeleteFileset(output);
            }
            if (!inputFailed)
            {
                long long b = strtoll(r.str("band", "1").c_str(), nullptr,
                                      10);
                if (b >= 1)
                {
                    std::string err;
                    cplPushQuietHandler();
                    auto ds = openRaster(r.str("input"), err);
                    cplPopHandler();
                    if (ds && b > (long long)ds->bands.size())
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            strPrintf("contour: Value of 'band' should be "
                                      "greater or equal than 1 and less or "
                                      "equal than %d.",
                                      (int)ds->bands.size()));
                        bad = true;
                    }
                }
            }
            return bad;
        });
}
