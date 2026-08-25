#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "util.h"
#include "warp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

std::string vsFmtG(double v)
{
    return strPrintf("%g", v);
}

struct VsParams
{
    bool hasPos = false;
    std::vector<double> pos;
    bool hasHeight = false;
    double height = 0;
    bool hasSd = false;
    std::string sd;
    bool hasTarget = false;
    double target = 0;
    bool hasMode = false;
    std::string mode = "normal";
    bool hasMax = false;
    double maxDist = 0;
    bool hasMin = false;
    double minDist = 0;
    bool hasStart = false;
    double startA = 0;
    bool hasEnd = false;
    double endA = 0;
    bool hasHigh = false;
    double high = 0;
    bool hasLow = false;
    double low = 0;
    bool hasCurv = false;
    double curv = 0;
    bool hasBand = false;
    int band = 1;
    bool hasVis = false;
    double vis = 255;
    bool hasInvis = false;
    double invis = 0;
    bool hasMaybe = false;
    double maybe = 2;
    bool hasOor = false;
    double oor = 0;
    bool hasDstNodata = false;
    double dstNodata = 0;
    bool hasSpacing = false;
    int spacing = 10;
    bool hasThreads = false;
    std::string threads = "3";
};

using VsGetter =
    std::function<const std::vector<std::string> *(const std::string &)>;

VsParams vsFillParams(const VsGetter &get)
{
    VsParams p;
    auto first = [&](const std::string &n) -> const std::string * {
        const auto *v = get(n);
        return v && !v->empty() ? &(*v)[0] : nullptr;
    };
    if (const auto *v = get("position"))
    {
        p.hasPos = true;
        for (const std::string &s : *v)
            p.pos.push_back(atof(s.c_str()));
    }
    if (const std::string *v = first("height"))
    {
        p.hasHeight = true;
        p.height = atof(v->c_str());
    }
    if (const std::string *v = first("sd-filename"))
    {
        p.hasSd = true;
        p.sd = *v;
    }
    if (const std::string *v = first("target-height"))
    {
        p.hasTarget = true;
        p.target = atof(v->c_str());
    }
    if (const std::string *v = first("mode"))
    {
        p.hasMode = true;
        static const char *const kModes[] = {"normal", "DEM", "ground",
                                             "cumulative"};
        for (const char *m : kModes)
            if (strEqualNoCase(*v, m))
                p.mode = m;
    }
    if (const std::string *v = first("max-distance"))
    {
        p.hasMax = true;
        p.maxDist = atof(v->c_str());
    }
    if (const std::string *v = first("min-distance"))
    {
        p.hasMin = true;
        p.minDist = atof(v->c_str());
    }
    if (const std::string *v = first("start-angle"))
    {
        p.hasStart = true;
        p.startA = atof(v->c_str());
    }
    if (const std::string *v = first("end-angle"))
    {
        p.hasEnd = true;
        p.endA = atof(v->c_str());
    }
    if (const std::string *v = first("high-pitch"))
    {
        p.hasHigh = true;
        p.high = atof(v->c_str());
    }
    if (const std::string *v = first("low-pitch"))
    {
        p.hasLow = true;
        p.low = atof(v->c_str());
    }
    if (const std::string *v = first("curvature-coefficient"))
    {
        p.hasCurv = true;
        p.curv = atof(v->c_str());
    }
    if (const std::string *v = first("band"))
    {
        p.hasBand = true;
        p.band = atoi(v->c_str());
    }
    if (const std::string *v = first("visible-value"))
    {
        p.hasVis = true;
        p.vis = atof(v->c_str());
    }
    if (const std::string *v = first("invisible-value"))
    {
        p.hasInvis = true;
        p.invis = atof(v->c_str());
    }
    if (const std::string *v = first("maybe-visible-value"))
    {
        p.hasMaybe = true;
        p.maybe = atof(v->c_str());
    }
    if (const std::string *v = first("out-of-range-value"))
    {
        p.hasOor = true;
        p.oor = atof(v->c_str());
    }
    if (const std::string *v = first("dst-nodata"))
    {
        p.hasDstNodata = true;
        p.dstNodata = atof(v->c_str());
    }
    if (const std::string *v = first("observer-spacing"))
    {
        p.hasSpacing = true;
        p.spacing = atoi(v->c_str());
    }
    if (const std::string *v = first("num-threads"))
    {
        p.hasThreads = true;
        p.threads = *v;
    }
    return p;
}

std::string vsArgsEcho(const VsParams &p)
{
    std::string e;
    if (p.hasPos)
    {
        e += " --position ";
        for (size_t i = 0; i < p.pos.size(); ++i)
            e += (i ? "," : "") + vsFmtG(p.pos[i]);
    }
    if (p.hasHeight)
        e += " --height " + vsFmtG(p.height);
    if (p.hasSd)
        e += " --sd-filename " + p.sd;
    if (p.hasTarget)
        e += " --target-height " + vsFmtG(p.target);
    if (p.hasMode)
        e += " --mode " + p.mode;
    if (p.hasMax)
        e += " --max-distance " + vsFmtG(p.maxDist);
    if (p.hasMin)
        e += " --min-distance " + vsFmtG(p.minDist);
    if (p.hasStart)
        e += " --start-angle " + vsFmtG(p.startA);
    if (p.hasEnd)
        e += " --end-angle " + vsFmtG(p.endA);
    if (p.hasHigh)
        e += " --high-pitch " + vsFmtG(p.high);
    if (p.hasLow)
        e += " --low-pitch " + vsFmtG(p.low);
    if (p.hasCurv)
        e += " --curvature-coefficient " + vsFmtG(p.curv);
    if (p.hasBand)
        e += strPrintf(" --band %d", p.band);
    if (p.hasVis)
        e += " --visible-value " + vsFmtG(p.vis);
    if (p.hasInvis)
        e += " --invisible-value " + vsFmtG(p.invis);
    if (p.hasMaybe)
        e += " --maybe-visible-value " + vsFmtG(p.maybe);
    if (p.hasOor)
        e += " --out-of-range-value " + vsFmtG(p.oor);
    if (p.hasDstNodata)
        e += " --dst-nodata " + vsFmtG(p.dstNodata);
    if (p.hasSpacing)
        e += strPrintf(" --observer-spacing %d", p.spacing);
    if (p.hasThreads)
        e += " --num-threads " + p.threads;
    return e;
}

constexpr double kInf = std::numeric_limits<double>::infinity();

// per-row angular mask: open-interval exclusions in fractional grid-x
struct ArcMask
{
    bool active = false;
    double s = 0, e = 0;  // e >= s, e - s <= 360
    double gx = 0, gy = 0;
    int ox = 0;
    int H = 0;  // raster height, for the boundary-ray exit rule
    int W = 0;
    // window column bounds; boundary-ray drifts reaching them decide
    // between dropping and widening the masked span
    int wx0 = 0, wx1 = 0;

    bool inArc(double a) const
    {
        double aa = a;
        if (aa < s)
            aa += 360.0;
        return aa <= e;
    }

    // in-range x-intervals for the row centered at grid y
    void rowInIntervals(double y,
                        std::vector<std::pair<double, double>> &in) const
    {
        in.clear();
        // the wedge geometry lives in cell-index space; pixel
        // resolution is ignored entirely
        double f = gy - y;  // >0 north of observer
        bool north = f > 0;
        // northward bearings live in [-90,90] mod 360, southward in
        // [90,270]; scan the arc [s,e] against the halves
        double lo0 = north ? -90.0 : 90.0;
        for (int k = -2; k <= 2; ++k)
        {
            double a1 = lo0 + k * 360.0, a2 = a1 + 180.0;
            double b1 = std::max(a1, s), b2 = std::min(a2, e);
            if (b1 > b2)
                continue;
            auto xOf = [&](double a) -> double {
                if (a <= a1)
                    return north ? -kInf : kInf;
                if (a >= a2)
                    return north ? kInf : -kInf;
                return gx + std::tan(a * M_PI / 180.0) * f;
            };
            double xa = xOf(b1), xb = xOf(b2);
            in.emplace_back(std::min(xa, xb), std::max(xa, xb));
        }
        std::sort(in.begin(), in.end());
        size_t w = 0;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (w && in[i].first <= in[w - 1].second)
                in[w - 1].second =
                    std::max(in[w - 1].second, in[i].second);
            else
                in[w++] = in[i];
        }
        in.resize(w);
    }

    // Each out-of-arc gap (lo,hi) masks the cells excluded by both of
    // its edges.  An edge falling outside the observer column cuts at
    // its own cell; an edge inside the observer column is resolved
    // from where its ray exits the raster vertically (offset D from
    // the observer center): an edge of an unbounded gap whose exit has
    // drifted a full cell away masks nothing, otherwise the cut sits
    // on the near or far side of the observer column depending on the
    // ray direction.
    void rowExcluded(double y, int cx0, int cx1,
                     std::vector<bool> &excl) const
    {
        std::vector<std::pair<double, double>> in;
        rowInIntervals(y, in);
        excl.assign((size_t)(cx1 - cx0 + 1), false);
        bool north = y < gy;
        double j = std::fabs(gy - y);
        double dyExit = north ? gy + 0.5 : (double)H + 0.5 - gy;
        // lowest column index still masked by the gap's low edge;
        // an observer-column edge is resolved from the column its
        // ray has drifted to at the vertical raster exit: reaching
        // the window edge on the gap side drops the mask, drifting a
        // full cell past the opposite window edge masks everything
        auto loCut = [&](double lo, double leftLow,
                         double gapHi) -> double {
            if (lo <= -kInf)
                return -kInf;
            if (std::floor(lo) != (double)ox)
                return std::floor(lo) + 1;
            double D = (lo - gx) * dyExit / j;
            double cd = std::floor(gx + D);
            if (D >= 1.0 && cd >= (double)wx1)
                return kInf;
            if (leftLow > -kInf)
            {
                // finite interval crossing the west window edge:
                // a ray still drifting a full cell over its
                // remaining run drags the cut along with it
                double run = (gx - lo) * (dyExit - j) / j;
                if (leftLow < (double)wx0 - 0.5 && run >= 1.0)
                    return std::floor(lo + D);
            }
            else if (j == 1.0 && gx + D < gx / 2.0 &&
                     gapHi > (double)wx1 + 0.5)
                return -kInf;
            return D >= 0.0 ? ox + 1 : ox;
        };
        auto hiCut = [&](double hi, double rightHigh,
                         double gapLo) -> double {
            if (hi >= kInf)
                return kInf;
            if (std::floor(hi) != (double)ox)
                return std::floor(hi) - 1;
            double D = (hi - gx) * dyExit / j;
            double cd = std::floor(gx + D);
            if (D <= -1.0 &&
                (cd <= (double)wx0 || (north && gapLo > -kInf)))
                return -kInf;
            if (rightHigh < kInf)
            {
                double run = (hi - gx) * (dyExit - j) / j;
                if (rightHigh > (double)wx1 + 0.5 && run >= 1.0 &&
                    (north || cd > (double)wx1))
                    return std::ceil(hi + D);
            }
            else if (north ? D >= 1.0 : gx + D > (double)W - 0.5)
                return kInf;
            return D <= 0.0 ? ox - 1 : ox;
        };
        auto markGap = [&](double lo, double hi, double leftLow,
                           double rightHigh) {
            double c0 = loCut(lo, leftLow, hi);
            double c1 = hiCut(hi, rightHigh, lo);
            if (lo > -kInf && hi < kInf &&
                std::floor(lo) == std::floor(hi) &&
                std::floor(lo) != (double)ox)
            {
                // wedge contained in a single off-column cell masks
                // everything on its far side of the observer
                if (std::floor(lo) > (double)ox)
                    c1 = kInf;
                else
                    c0 = -kInf;
            }
            for (int c = cx0; c <= cx1; ++c)
                if (c >= c0 && c <= c1)
                    excl[(size_t)(c - cx0)] = true;
        };
        double prev = -kInf, prevLow = -kInf;
        for (size_t idx = 0; idx < in.size(); ++idx)
        {
            markGap(prev, in[idx].first, prevLow, in[idx].second);
            prev = std::max(prev, in[idx].second);
            prevLow = in[idx].first;
        }
        markGap(prev, kInf, prevLow, kInf);
    }
};

struct VsCompute
{
    // window in source cells
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<double> out;  // (y1-y0+1) x (x1-x0+1)
};

struct VsCtx
{
    const std::vector<double> *dem;
    int W, H;
    int ox, oy;       // observer cell
    double gx, gy;    // observer point in fractional grid coords
    double xres, yres;
    double kadj;      // curv / earthDiameter
    double zobs;
    const VsParams *p;
    bool useArc = false;
    ArcMask arc;
};

// LOS over a window; mode: 0 normal, 1 DEM, 2 ground, 3 count(visible=1)
void vsRunLos(const VsCtx &cx, int x0, int y0, int x1, int y1, int mode,
              std::vector<double> &res, std::vector<uint32_t> *counts)
{
    const VsParams &p = *cx.p;
    const std::vector<double> &dem = *cx.dem;
    const int W = cx.W;
    const int ww = x1 - x0 + 1;
    double tanHigh = 0, tanLow = 0;
    bool useHigh = p.hasHigh, useLow = p.hasLow;
    if (useHigh)
        tanHigh = std::tan(p.high * M_PI / 180.0);
    if (useLow)
        tanLow = std::tan(p.low * M_PI / 180.0);

    std::vector<double> lastRow((size_t)W, 0.0), thisRow((size_t)W, 0.0);
    std::vector<double> obsRowProp;
    std::vector<bool> excl;

    auto rowAngleMask = [&](int r, std::vector<bool> &ex) {
        if (!cx.useArc)
        {
            ex.assign((size_t)ww, false);
            return;
        }
        if (r == cx.oy)
        {
            ex.assign((size_t)ww, false);
            bool westIn = cx.arc.inArc(270.0);
            bool eastIn = cx.arc.inArc(90.0);
            for (int c = x0; c <= x1; ++c)
            {
                if (c == cx.ox)
                    continue;
                if (c < cx.ox ? !westIn : !eastIn)
                    ex[(size_t)(c - x0)] = true;
            }
            return;
        }
        cx.arc.rowExcluded(r + 0.5, x0, x1, ex);
    };

    auto processRow = [&](int r, const std::vector<double> *last,
                          std::vector<double> &cur) {
        int j = std::abs(r - cx.oy);
        rowAngleMask(r, excl);
        std::vector<int> order;
        if (cx.ox >= x0 && cx.ox <= x1)
            order.push_back(cx.ox);
        for (int c = std::max(cx.ox + 1, x0); c <= x1; ++c)
            order.push_back(c);
        for (int c = std::min(cx.ox - 1, x1); c >= x0; --c)
            order.push_back(c);
        for (int c : order)
        {
            int i = std::abs(c - cx.ox);
            int step = c > cx.ox ? 1 : -1;
            double dfz;
            bool special = false;
            if (j == 0)
            {
                if (i <= 1)
                    special = true, dfz = 0;
                else
                    dfz = cur[(size_t)(c - step)] * i / (i - 1);
            }
            else if (i == 0)
            {
                if (j == 1)
                    special = true, dfz = 0;
                else
                    dfz = (*last)[(size_t)c] * j / (j - 1);
            }
            else if (i == j)
            {
                if (i == 1)
                    special = true, dfz = 0;
                else
                    dfz = (*last)[(size_t)(c - step)] * i / (i - 1);
            }
            else if (i > j)
            {
                dfz = (cur[(size_t)(c - step)] * (i - j) +
                       (*last)[(size_t)(c - step)] * j) /
                      (i - 1);
            }
            else
            {
                dfz = ((*last)[(size_t)c] * (j - i) +
                       (*last)[(size_t)(c - step)] * i) /
                      (j - 1);
            }
            double dgx = (c - cx.ox) * cx.xres;
            double dgy = (r - cx.oy) * cx.yres;
            double d2 = dgx * dgx + dgy * dgy;
            double d = std::sqrt(d2);
            double cellv =
                dem[(size_t)r * W + c] - (cx.kadj * d2 + cx.zobs);
            bool oor = false;
            bool minMasked = false;
            if (p.hasMax && d > p.maxDist)
                oor = true;
            if (p.hasMin && d < p.minDist)
            {
                oor = true;
                minMasked = true;
            }
            if (excl[(size_t)(c - x0)])
                oor = true;
            double dfzEff = special ? cellv : dfz;
            if (useLow)
                dfzEff = std::max(dfzEff, d * tanLow);
            if (useHigh && cellv > d * tanHigh)
                oor = true;
            double pv = std::max(cellv, dfzEff);
            // masked observer-column cells propagate their own height,
            // not the accumulated line of sight
            if ((minMasked || excl[(size_t)(c - x0)]) && i == 0 && j != 0)
                pv = cellv;
            cur[(size_t)c] = pv;

            double v;
            if (mode == 0 || mode == 3)
            {
                if (oor)
                    v = mode == 3 ? 0.0 : p.oor;
                else if (cellv + p.target >= dfzEff)
                    v = mode == 3 ? 1.0 : p.vis;
                else
                    v = mode == 3 ? 0.0 : p.invis;
                if (mode == 3 && v != 0.0)
                    ++(*counts)[(size_t)r * W + c];
            }
            else if (mode == 1)
            {
                v = oor ? p.oor
                        : dem[(size_t)r * W + c] +
                              std::max(0.0, dfzEff - cellv);
            }
            else
            {
                v = oor ? p.oor : std::max(0.0, dfzEff - cellv);
            }
            if (mode != 3)
                res[(size_t)(r - y0) * ww + (c - x0)] = v;
        }
    };

    int obsR = cx.oy;
    if (obsR >= y0 && obsR <= y1)
    {
        processRow(obsR, nullptr, thisRow);
        obsRowProp = thisRow;
    }
    else
    {
        obsRowProp.assign((size_t)W, 0.0);
    }
    lastRow = obsRowProp;
    for (int r = std::min(cx.oy - 1, y1); r >= y0; --r)
    {
        thisRow.assign((size_t)W, 0.0);
        processRow(r, &lastRow, thisRow);
        lastRow = thisRow;
    }
    lastRow = obsRowProp;
    for (int r = std::max(cx.oy + 1, y0); r <= y1; ++r)
    {
        thisRow.assign((size_t)W, 0.0);
        processRow(r, &lastRow, thisRow);
        lastRow = thisRow;
    }
}

class ViewshedDataset final : public RasterDatasetBase
{
  public:
    std::vector<double> vals;

    ViewshedDataset(RasterDatasetBase &src, int wx0, int wy0, int w,
                    int h, DType t, const VsParams &p)
    {
        width = w;
        height = h;
        hasGT = src.hasGT;
        demWriteDefaultGt = !src.hasGT;
        memcpy(gt, src.gt, sizeof gt);
        gt[0] = src.gt[0] + wx0 * src.gt[1] + wy0 * src.gt[2];
        gt[3] = src.gt[3] + wx0 * src.gt[4] + wy0 * src.gt[5];
        srs = src.srs;
        hasSrs = src.hasSrs;
        srsSynthetic = src.srsSynthetic;
        inMemoryVrtCopy = true;
        deferredWarnings = src.deferredWarnings;
        src.deferredWarnings.clear();
        Band b;
        b.index = 1;
        b.type = t;
        b.blockX = w;
        b.blockY = 1;
        b.colorInterp = "Undefined";
        if (p.hasDstNodata)
        {
            b.hasNodata = true;
            b.nodata = p.dstNodata;
        }
        bands.push_back(std::move(b));
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        (void)band;
        out.resize(vals.size());
        for (size_t i = 0; i < vals.size(); ++i)
            out[i] = rasterFinishReal(vals[i], bands[0].type);
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        (void)band;
        DType t = bands[0].type;
        size_t sz = (size_t)dtypeSizeBytes(t);
        out.assign(vals.size() * sz, 0);
        for (size_t i = 0; i < vals.size(); ++i)
            rasterEncodeReal(t, out.data() + i * sz,
                             rasterFinishReal(vals[i], t), 0);
        return true;
    }
};

int vsWrap(const VsParams &p, std::unique_ptr<RasterDatasetBase> &d)
{
    bool cumulative = p.mode == "cumulative";
    if (p.hasPos && p.pos.size() == 3 && p.hasHeight)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "viewshed: Height can't be specified in both "
                    "'position' and 'height' arguments");
        return 1;
    }
    if (!cumulative)
    {
        if (p.hasThreads)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "viewshed: Option 'num-threads' can't be used "
                        "in standard mode.");
            return 1;
        }
        if (!p.hasPos)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "viewshed: Option 'position' must be specified "
                        "in standard mode.");
            return 1;
        }
    }
    else
    {
        struct
        {
            bool set;
            const char *name;
        } kForbidden[] = {
            {p.hasMax, "max-distance"},   {p.hasMin, "min-distance"},
            {p.hasStart, "start-angle"},  {p.hasEnd, "end-angle"},
            {p.hasLow, "low-pitch"},      {p.hasHigh, "high-pitch"},
            {p.hasPos, "position"},
        };
        for (const auto &f : kForbidden)
            if (f.set)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            std::string("viewshed: Option '") + f.name +
                                "' can't be used in cumulative mode.");
                return 1;
            }
    }
    if (p.hasHigh && p.hasLow && p.high <= p.low)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Invalid pitch. highPitch must be > lowPitch");
        return 1;
    }

    const int W = d->width, H = d->height;
    std::vector<double> dem;
    if (!d->readBand(p.band, dem))
        return 1;

    VsCtx cx;
    cx.dem = &dem;
    cx.W = W;
    cx.H = H;
    cx.xres = std::fabs(d->gt[1]);
    cx.yres = std::fabs(d->gt[5]);
    cx.p = &p;

    double earthDiam = 0;
    if (d->hasSrs && d->srs.valid())
    {
        double a = d->srs.semiMajor();
        if (a > 0)
            earthDiam = 2 * a;
    }
    double curv = p.hasCurv ? p.curv : 0.85714;
    cx.kadj = earthDiam > 0 ? curv / earthDiam : 0.0;

    int x0 = 0, x1 = W - 1, y0 = 0, y1 = H - 1;

    if (!cumulative)
    {
        // observer cell from georeferenced position
        double det = d->gt[1] * d->gt[5] - d->gt[2] * d->gt[4];
        double px = 0, py = 0;
        if (det != 0)
        {
            double ox2 = p.pos[0] - d->gt[0], oy2 = p.pos[1] - d->gt[3];
            px = (d->gt[5] * ox2 - d->gt[2] * oy2) / det;
            py = (d->gt[1] * oy2 - d->gt[4] * ox2) / det;
        }
        cx.ox = (int)std::floor(px);
        cx.oy = (int)std::floor(py);
        if (cx.ox < 0 || cx.ox >= W || cx.oy < 0 || cx.oy >= H)
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "NOTE: The observer location falls outside of "
                        "the DEM area");
            cx.ox = std::max(0, std::min(cx.ox, W - 1));
            cx.oy = std::max(0, std::min(cx.oy, H - 1));
        }
        // the wedge/window geometry hangs off the observer cell center
        cx.gx = cx.ox + 0.5;
        cx.gy = cx.oy + 0.5;
        // -z/--height is ignored in standard mode (GDAL quirk); the
        // observer height is the third position value, else 2
        double oz = p.pos.size() == 3 ? p.pos[2] : 2.0;
        cx.zobs = dem[(size_t)cx.oy * W + cx.ox] + oz;

        if (p.hasMax)
        {
            int rx = (int)std::ceil(p.maxDist / cx.xres);
            int ry = (int)std::ceil(p.maxDist / cx.yres);
            x0 = std::max(0, cx.ox - rx);
            x1 = std::min(W - 1, cx.ox + rx);
            y0 = std::max(0, cx.oy - ry);
            y1 = std::min(H - 1, cx.oy + ry);
        }
        bool useArc = p.hasStart && p.hasEnd && p.startA != p.endA;
        if ((p.hasStart || p.hasEnd) &&
            (p.hasStart ? p.startA : 0.0) != (p.hasEnd ? p.endA : 0.0))
            useArc = true;
        if (useArc)
        {
            cx.useArc = true;
            cx.arc.active = true;
            cx.arc.s = p.hasStart ? p.startA : 0.0;
            cx.arc.e = p.hasEnd ? p.endA : 0.0;
            if (cx.arc.e < cx.arc.s)
                cx.arc.e += 360.0;
            cx.arc.gx = cx.gx;
            cx.arc.gy = cx.gy;
            cx.arc.ox = cx.ox;
            cx.arc.H = H;
            cx.arc.W = W;

            // window bbox from wedge candidates; the ray exits are
            // taken against y=0 (top) but overshot half a cell past
            // the other three edges; the max-distance window is
            // intersected independently afterwards
            double minx = cx.gx, maxx = cx.gx, miny = cx.gy,
                   maxy = cx.gy;
            auto addPt = [&](double x, double y) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
                miny = std::min(miny, y);
                maxy = std::max(maxy, y);
            };
            // arcs containing both horizontal cardinals overshoot
            // the west/east exits by half a cell; other arcs stop at
            // the outer column centers
            bool wide = cx.arc.inArc(90.0) && cx.arc.inArc(270.0);
            double wx = wide ? -0.5 : 0.5;
            double ex = W + 0.5;
            auto rayExit = [&](double aDeg) {
                double a = aDeg * M_PI / 180.0;
                // index-space direction, resolution ignored
                double dxg = std::sin(a);
                double dyg = -std::cos(a);
                double t = kInf;
                if (dxg > 0)
                    t = std::min(t, (ex - cx.gx) / dxg);
                if (dyg > 0)
                    t = std::min(t, (H + 0.5 - cx.gy) / dyg);
                else if (dyg < 0)
                    t = std::min(t, (0.5 - cx.gy) / dyg);
                if (dxg < 0)
                {
                    // west crossing wins only if reached before the
                    // vertical exit; wide arcs overshoot only for
                    // north-going rays, retrying the inner edge
                    double wxr = (wide && dyg < 0) ? wx : 0.5;
                    double t1 = (wxr - cx.gx) / dxg;
                    if (t1 <= t)
                        t = t1;
                    else if (wide && dyg < 0)
                    {
                        double t2 = (0.5 - cx.gx) / dxg;
                        if (t2 <= t)
                            t = t2;
                    }
                }
                if (t == kInf)
                    t = 0;
                addPt(cx.gx + dxg * t, cx.gy + dyg * t);
            };
            rayExit(cx.arc.s);
            rayExit(cx.arc.e >= 360.0 ? cx.arc.e - 360.0 : cx.arc.e);
            for (double card : {0.0, 90.0, 180.0, 270.0})
                if (cx.arc.inArc(card))
                    rayExit(card);
            double ci = wide ? 0.0 : 0.5;
            const double corners[4][2] = {
                {ci, ci},
                {W - ci, ci},
                {ci, H - ci},
                {W - ci, H - ci}};
            for (const auto &cn : corners)
            {
                double dxe = cn[0] - cx.gx;
                double dyn = cx.gy - cn[1];
                double a = std::atan2(dxe, dyn) * 180.0 / M_PI;
                if (a < 0)
                    a += 360.0;
                if (cx.arc.inArc(a))
                    addPt(cn[0], cn[1]);
            }
            x0 = std::max(x0, (int)std::floor(minx));
            x1 = std::min(x1, (int)std::floor(maxx));
            y0 = std::max(y0, (int)std::floor(miny));
            y1 = std::min(y1, (int)std::floor(maxy));
            x0 = std::max(0, std::min(x0, W - 1));
            x1 = std::max(0, std::min(x1, W - 1));
            y0 = std::max(0, std::min(y0, H - 1));
            y1 = std::max(0, std::min(y1, H - 1));
            cx.arc.wx0 = x0;
            cx.arc.wx1 = x1;
        }
    }
    else
    {
        cx.zobs = 0;  // per-observer below
    }

    // nodata presence warning, scanned over the window; cells beyond
    // the max distance are never evaluated and do not count
    const Band &sb = d->bands[(size_t)p.band - 1];
    if (sb.hasNodata)
    {
        bool found = false;
        for (int r = y0; r <= y1 && !found; ++r)
            for (int c = x0; c <= x1; ++c)
            {
                if (p.hasMax)
                {
                    double dgx = (c - cx.ox) * cx.xres;
                    double dgy = (r - cx.oy) * cx.yres;
                    if (std::sqrt(dgx * dgx + dgy * dgy) > p.maxDist)
                        continue;
                }
                if (dem[(size_t)r * W + c] == sb.nodata)
                {
                    found = true;
                    break;
                }
            }
        if (found)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Nodata value found in input DEM. Output will "
                        "be likely incorrect");
    }

    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    DType t =
        (p.mode == "DEM" || p.mode == "ground") ? DType::Float64
                                                : DType::Byte;
    auto vd = std::make_unique<ViewshedDataset>(*d, x0, y0, w, h, t, p);

    if (!cumulative)
    {
        int mode = p.mode == "DEM" ? 1 : p.mode == "ground" ? 2 : 0;
        vd->vals.assign((size_t)w * h, 0.0);
        vsRunLos(cx, x0, y0, x1, y1, mode, vd->vals, nullptr);
    }
    else
    {
        std::vector<uint32_t> counts((size_t)W * H, 0);
        std::vector<double> scratch;
        for (int obY = 0; obY < H; obY += p.spacing)
            for (int obX = 0; obX < W; obX += p.spacing)
            {
                VsCtx c2 = cx;
                c2.kadj = 0.0;  // cumulative applies no curvature
                c2.ox = obX;
                c2.oy = obY;
                c2.gx = obX + 0.5;
                c2.gy = obY + 0.5;
                c2.zobs = dem[(size_t)obY * W + obX] +
                          (p.hasHeight ? p.height : 0.0);
                vsRunLos(c2, 0, 0, W - 1, H - 1, 3, scratch, &counts);
            }
        uint32_t maxc = 0;
        for (uint32_t v : counts)
            maxc = std::max(maxc, v);
        vd->vals.assign((size_t)W * H, 0.0);
        if (maxc)
        {
            double scale = 255.0 / maxc;
            for (size_t i = 0; i < counts.size(); ++i)
                vd->vals[i] = (double)(int)(counts[i] * scale);
        }
    }

    d = std::move(vd);
    return 0;
}

struct PrefixScope
{
    bool active;
    explicit PrefixScope(const char *name)
    {
        active = g_pipelineStepPrefix.empty();
        if (active)
            g_pipelineStepPrefix = name;
    }
    ~PrefixScope()
    {
        if (active)
            g_pipelineStepPrefix.clear();
    }
};

int viewshedLeafHandler(ParseResult &r)
{
    PrefixScope prefix("viewshed");
    std::string format = r.str("output-format");
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");
    std::string drv;
    {
        std::string issue = rasterOutFormatIssue(format, drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    std::string err;
    auto ds = openRaster(input, err);
    if (!ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    VsParams p = vsFillParams(
        [&](const std::string &n) -> const std::vector<std::string> * {
            const ArgValue *v = r.get(n);
            return v ? &v->values : nullptr;
        });
    std::string extra = vsArgsEcho(p);
    auto materialize =
        [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        return vsWrap(p, d);
    };
    int nb = (int)ds->bands.size();
    auto preValidate = [&](const std::string &, bool &failed) {
        if (p.band > nb)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("viewshed: Value of 'band' should be "
                                  "greater or equal than 1 and less or "
                                  "equal than %d.",
                                  nb));
            failed = true;
        }
    };
    return rasterConvertWriteOutput(ds, r, input, output, quiet,
                                    overwrite, append, drv, extra,
                                    materialize, preValidate);
}

int viewshedPreValidator(const CmdSpec &, ParseResult &r)
{
    if (r.get("band") && atoi(r.str("band").c_str()) < 1)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Value of 'band' should greater or equal to 1.");
        handlerPrintUsage();
        return 1;
    }
    return 0;
}

}  // namespace

void registerRasterViewshedHandler()
{
    registerHandler("raster_viewshed", [](const CmdSpec &, ParseResult &r) {
        return viewshedLeafHandler(r);
    });
    registerPreValidator("raster_viewshed", viewshedPreValidator);
    registerArgValueCheck(
        "raster_viewshed",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName == "num-threads" && !warpNumThreadsValid(value))
                return "\x05Invalid value for 'num-threads' argument";
            return "";
        });
}
