#include "engine.h"
#include "cpl.h"
#include "dataset.h"
#include "ogr.h"
#include "rasterpolyfoot.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <sys/stat.h>

std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver);
bool rpfFileExists(const std::string &path);

namespace
{

void zsForceArg(ParseResult &r, const std::string &name,
                const std::string &val)
{
    auto it = r.byName.find(name);
    if (it == r.byName.end())
        return;
    it->second.values.assign(1, val);
    it->second.set = true;
}

void zsClearArg(ParseResult &r, const std::string &name)
{
    auto it = r.byName.find(name);
    if (it == r.byName.end())
        return;
    it->second.values.clear();
    it->second.set = false;
}

bool zsDTypeIntegral(DType t)
{
    switch (t)
    {
        case DType::Byte:
        case DType::Int8:
        case DType::UInt16:
        case DType::Int16:
        case DType::UInt32:
        case DType::Int32:
        case DType::Int64:
        case DType::UInt64:
            return true;
        default:
            return false;
    }
}

// nodata comparisons happen in the band's storage domain, so float
// bands quantize the declared value before matching
double zsQuantNodata(DType t, double nd)
{
    if (t == DType::Float32 || t == DType::CFloat32)
        return (double)(float)nd;
    if (t == DType::Float16)
        return tailHalfToFloat(tailFloatToHalf((float)nd));
    return nd;
}

bool zsBandToDoubles(RasterDatasetBase &ds, int band,
                     std::vector<double> &out, std::vector<bool> &valid)
{
    const Band &b = ds.bands[band - 1];
    std::vector<uint8_t> raw;
    if (!ds.readBandRaw(band, raw))
        return false;
    size_t n = (size_t)ds.width * ds.height;
    int esz = dtypeSizeBytes(b.type);
    out.resize(n);
    for (size_t i = 0; i < n; i++)
    {
        const uint8_t *p = &raw[i * esz];
        double v = 0;
        switch (b.type)
        {
            case DType::Byte:
                v = *p;
                break;
            case DType::Int8:
                v = *(const int8_t *)p;
                break;
            case DType::UInt16:
                v = *(const uint16_t *)p;
                break;
            case DType::Int16:
            case DType::CInt16:
                v = *(const int16_t *)p;
                break;
            case DType::UInt32:
                v = *(const uint32_t *)p;
                break;
            case DType::Int32:
            case DType::CInt32:
                v = *(const int32_t *)p;
                break;
            case DType::Int64:
                v = (double)*(const int64_t *)p;
                break;
            case DType::UInt64:
                v = (double)*(const uint64_t *)p;
                break;
            case DType::Float16:
                v = tailHalfToFloat(*(const uint16_t *)p);
                break;
            case DType::Float32:
            case DType::CFloat32:
                v = *(const float *)p;
                break;
            case DType::Float64:
            case DType::CFloat64:
                v = *(const double *)p;
                break;
            default:
                break;
        }
        out[i] = v;
    }
    valid.assign(n, true);
    if (b.hasNodata)
    {
        double nd = zsQuantNodata(b.type, b.nodata);
        for (size_t i = 0; i < n; i++)
            if (out[i] == nd || (std::isnan(nd) && std::isnan(out[i])))
                valid[i] = false;
    }
    return true;
}

// resample a source band onto a window of the destination grid,
// mimicking the reference RasterIO average path: the source window is
// the floor/ceil snap of the destination window extent; when its span
// divides evenly into the destination size the per-cell windows are the
// snapped integer blocks (equal weights, dtype-domain accumulation,
// with the 2x2 Float32 kernel pairing columns four cells at a time),
// otherwise exact fractional windows with area weights accumulated in
// double are used; cells with no valid source coverage stay invalid
void zsResampleAverage(int sw, int sh, const double *sgt,
                       const std::vector<double> &sv,
                       const std::vector<bool> &svalid, int dw, int dh,
                       const double *dgt, std::vector<double> &dv,
                       std::vector<bool> &dvalid, DType dt,
                       bool srcHasNodata, int fc0 = -1, int fc1 = -1,
                       int fr0 = -1, int fr1 = -1)
{
    dv.assign((size_t)dw * dh, 0.0);
    dvalid.assign((size_t)dw * dh, false);
    if (fc0 < 0)
    {
        fc0 = 0;
        fc1 = dw;
        fr0 = 0;
        fr1 = dh;
    }
    // source extent in destination cell space: restrict to cells with
    // positive overlap
    double sX0 = sgt[0], sX1 = sgt[0] + sw * sgt[1];
    if (sX0 > sX1)
        std::swap(sX0, sX1);
    double sY0 = sgt[3], sY1 = sgt[3] + sh * sgt[5];
    if (sY0 > sY1)
        std::swap(sY0, sY1);
    auto cellRange = [](double o, double px, double lo, double hi,
                        int &a, int &b)
    {
        double c0 = (lo - o) / px;
        double c1 = (hi - o) / px;
        if (c0 > c1)
            std::swap(c0, c1);
        int ia = (int)std::floor(c0 + 1e-9);
        int ib = (int)std::ceil(c1 - 1e-9);
        a = std::max(a, ia);
        b = std::min(b, ib);
    };
    int dc0 = fc0, dc1 = fc1, dr0 = fr0, dr1 = fr1;
    cellRange(dgt[0], dgt[1], sX0, sX1, dc0, dc1);
    cellRange(dgt[3], dgt[5], sY0, sY1, dr0, dr1);
    if (dc0 >= dc1 || dr0 >= dr1)
        return;
    // window extent in source pixel space
    auto toSrcX = [&](double X) { return (X - sgt[0]) / sgt[1]; };
    auto toSrcY = [&](double Y) { return (Y - sgt[3]) / sgt[5]; };
    double ex0 = toSrcX(dgt[0] + dc0 * dgt[1]);
    double ex1 = toSrcX(dgt[0] + dc1 * dgt[1]);
    if (ex0 > ex1)
        std::swap(ex0, ex1);
    double ey0 = toSrcY(dgt[3] + dr0 * dgt[5]);
    double ey1 = toSrcY(dgt[3] + dr1 * dgt[5]);
    if (ey0 > ey1)
        std::swap(ey0, ey1);
    long long wx0 = (long long)std::floor(ex0 + 1e-9);
    long long wx1 = (long long)std::ceil(ex1 - 1e-9);
    long long wy0 = (long long)std::floor(ey0 + 1e-9);
    long long wy1 = (long long)std::ceil(ey1 - 1e-9);
    long long spanx = wx1 - wx0, spany = wy1 - wy0;
    int nx = dc1 - dc0, ny = dr1 - dr0;
    bool snapx = spanx > 0 && spanx % nx == 0;
    bool snapy = spany > 0 && spany % ny == 0;
    // reference working types: Byte/UInt16 average in the integer
    // domain, Int8/Int16 ride the float path, wider types use double
    bool isF32 = dt == DType::Float32 || dt == DType::Float16 ||
                 dt == DType::Int8 || dt == DType::Int16;
    bool isInt = dt == DType::Byte || dt == DType::UInt16;
    if (snapx && snapy)
    {
        long long rx = spanx / nx, ry = spany / ny;
        // the fast float kernel is only used when the source window
        // holds no invalid pixel; otherwise the generic double
        // accumulation runs for every cell
        bool anyInvalid = false;
        if (srcHasNodata)
            for (long long iy = std::max(wy0, 0LL);
                 iy < std::min(wy1, (long long)sh) && !anyInvalid; iy++)
                for (long long ix = std::max(wx0, 0LL);
                     ix < std::min(wx1, (long long)sw); ix++)
                    if (!svalid[(size_t)iy * sw + ix] ||
                        std::isnan(sv[(size_t)iy * sw + ix]))
                    {
                        anyInvalid = true;
                        break;
                    }
        bool kern2 = isF32 && !anyInvalid && rx == 2 && ry == 2;
        for (int r = dr0; r < dr1; r++)
        {
            long long iy0 = wy0 + (long long)(r - dr0) * ry;
            for (int cb = dc0; cb < dc1;)
            {
                int group = 1;
                if (kern2 && dc1 - cb >= 4)
                    group = 4;
                for (int c = cb; c < cb + group; c++)
                {
                    long long ix0 = wx0 + (long long)(c - dc0) * rx;
                    double acc = 0;
                    float facc = 0;
                    int cnt = 0;
                    if (group == 4)
                    {
                        // vertical add then horizontal pair add
                        for (long long ix = ix0; ix < ix0 + 2; ix++)
                        {
                            if (ix < 0 || ix >= sw)
                                continue;
                            float col = 0;
                            int ncol = 0;
                            for (long long iy = iy0; iy < iy0 + 2;
                                 iy++)
                            {
                                if (iy < 0 || iy >= sh)
                                    continue;
                                size_t si = (size_t)iy * sw + ix;
                                if (!svalid[si])
                                    continue;
                                col += (float)sv[si];
                                ncol++;
                            }
                            if (ncol)
                            {
                                facc += col;
                                cnt += ncol;
                            }
                        }
                    }
                    else
                    {
                        for (long long iy = std::max(iy0, 0LL);
                             iy < std::min(iy0 + ry, (long long)sh);
                             iy++)
                            for (long long ix = std::max(ix0, 0LL);
                                 ix < std::min(ix0 + rx,
                                               (long long)sw);
                                 ix++)
                            {
                                size_t si = (size_t)iy * sw + ix;
                                if (!svalid[si])
                                    continue;
                                acc += sv[si];
                                facc += (float)sv[si];
                                cnt++;
                            }
                    }
                    if (cnt > 0)
                    {
                        double out;
                        if (isInt)
                        {
                            long long t = (long long)acc, n = cnt;
                            out = (double)((t + n / 2) / n);
                        }
                        else if (isF32)
                        {
                            if (group == 4 || !anyInvalid)
                                out = (double)(facc / (float)cnt);
                            else
                                out = (double)(float)(acc / cnt);
                        }
                        else
                            out = acc / cnt;
                        dv[(size_t)r * dw + c] = out;
                        dvalid[(size_t)r * dw + c] = true;
                    }
                }
                cb += group;
            }
        }
        return;
    }
    for (int r = dr0; r < dr1; r++)
    {
        double Y0 = dgt[3] + r * dgt[5];
        double Y1 = Y0 + dgt[5];
        double sy0 = toSrcY(Y0), sy1 = toSrcY(Y1);
        if (sy0 > sy1)
            std::swap(sy0, sy1);
        int iy0 = std::max((int)std::floor(sy0), 0);
        int iy1 = std::min((int)std::ceil(sy1), sh);
        for (int c = dc0; c < dc1; c++)
        {
            double X0 = dgt[0] + c * dgt[1];
            double X1 = X0 + dgt[1];
            double sx0 = toSrcX(X0), sx1 = toSrcX(X1);
            if (sx0 > sx1)
                std::swap(sx0, sx1);
            int ix0 = std::max((int)std::floor(sx0), 0);
            int ix1 = std::min((int)std::ceil(sx1), sw);
            double acc = 0, wsum = 0;
            for (int iy = iy0; iy < iy1; iy++)
            {
                double wy = std::min((double)iy + 1, sy1) -
                            std::max((double)iy, sy0);
                if (wy <= 0)
                    continue;
                double rs = 0, rw = 0;
                for (int ix = ix0; ix < ix1; ix++)
                {
                    double wx = std::min((double)ix + 1, sx1) -
                                std::max((double)ix, sx0);
                    if (wx <= 0)
                        continue;
                    size_t si = (size_t)iy * sw + ix;
                    if (!svalid[si])
                        continue;
                    rs += sv[si] * wx;
                    rw += wx;
                }
                acc += rs * wy;
                wsum += rw * wy;
            }
            if (wsum > 0)
            {
                double out = acc / wsum;
                if (isF32)
                    out = (double)(float)out;
                else if (isInt)
                    out = std::floor(out + 0.5);
                dv[(size_t)r * dw + c] = out;
                dvalid[(size_t)r * dw + c] = true;
            }
        }
    }
}

// scanline even-odd polygon fill in pixel space: a pixel is covered
// when its center lies inside the rings
void zsFillPolygon(const std::vector<std::vector<double>> &rings, int w,
                   int h, std::vector<uint8_t> &mask)
{
    double ymin = 1e300, ymax = -1e300;
    for (const auto &rg : rings)
        for (size_t i = 1; i < rg.size(); i += 2)
        {
            ymin = std::min(ymin, rg[i]);
            ymax = std::max(ymax, rg[i]);
        }
    int y0 = std::max(0, (int)std::floor(ymin));
    int y1 = std::min(h - 1, (int)std::ceil(ymax));
    std::vector<double> xs;
    auto span = [&](int iy, double xa, double xb)
    {
        // pixel centers in (xa, xb]
        int cx0 = (int)std::floor(xa + 0.5);
        int cx1 = (int)std::floor(xb - 0.5);
        cx0 = std::max(cx0, 0);
        cx1 = std::min(cx1, w - 1);
        for (int cx = cx0; cx <= cx1; cx++)
            mask[(size_t)iy * w + cx] = 1;
    };
    for (int iy = y0; iy <= y1; iy++)
    {
        double dfY = iy + 0.5;
        xs.clear();
        for (const auto &rg : rings)
        {
            size_t nv = rg.size() / 2;
            if (nv < 2)
                continue;
            for (size_t i = 0; i < nv; i++)
            {
                size_t j = (i + 1) % nv;
                double x1 = rg[2 * i], yy1 = rg[2 * i + 1];
                double x2 = rg[2 * j], yy2 = rg[2 * j + 1];
                if (yy1 == yy2)
                {
                    // horizontal edges on the scanline fill their run,
                    // but only when traversed right to left
                    if (yy1 == dfY && x1 > x2)
                        span(iy, x2, x1);
                    continue;
                }
                if (yy1 > yy2)
                {
                    std::swap(x1, x2);
                    std::swap(yy1, yy2);
                }
                if (dfY < yy1 || dfY >= yy2)
                    continue;
                xs.push_back(x1 + (dfY - yy1) * (x2 - x1) /
                                      (yy2 - yy1));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2)
            span(iy, xs[k], xs[k + 1]);
    }
}

// Sutherland-Hodgman clip of one ring against an axis-aligned cell,
// returning the absolute shoelace area of the clipped ring
double zsClipRingArea(const std::vector<double> &rg, double x0, double y0,
                      double x1, double y1)
{
    std::vector<double> cur(rg), nxt;
    auto clipEdge = [&](int axis, double bound, int keepBelow)
    {
        nxt.clear();
        size_t nv = cur.size() / 2;
        for (size_t i = 0; i < nv; i++)
        {
            size_t j = (i + 1) % nv;
            double ax = cur[2 * i], ay = cur[2 * i + 1];
            double bx = cur[2 * j], by = cur[2 * j + 1];
            double av = axis ? ay : ax, bv = axis ? by : bx;
            bool ain = keepBelow ? av <= bound : av >= bound;
            bool bin = keepBelow ? bv <= bound : bv >= bound;
            if (ain)
            {
                nxt.push_back(ax);
                nxt.push_back(ay);
            }
            if (ain != bin)
            {
                double t = (bound - av) / (bv - av);
                nxt.push_back(axis ? ax + t * (bx - ax) : bound);
                nxt.push_back(axis ? bound : ay + t * (by - ay));
            }
        }
        cur.swap(nxt);
    };
    clipEdge(0, x0, 0);
    clipEdge(0, x1, 1);
    clipEdge(1, y0, 0);
    clipEdge(1, y1, 1);
    size_t nv = cur.size() / 2;
    double a = 0;
    for (size_t i = 0; i < nv; i++)
    {
        size_t j = (i + 1) % nv;
        a += cur[2 * i] * cur[2 * j + 1] - cur[2 * j] * cur[2 * i + 1];
    }
    return std::fabs(a) * 0.5;
}

// all-touched boundary burner: segments are swapped left to right and
// walked column by column against the line equation y = M*x + B; each
// column burns the floor rows between its entry and exit values, an
// exact lattice hit on the upper value belongs to the cell below when
// the segment ascends, and edges running exactly along the lattice add
// nothing beyond the interior fill
void zsBurnLine(double x1, double y1, double x2, double y2, int w,
                int h, std::vector<uint8_t> &mask)
{
    auto burn = [&](long long ix, long long iy)
    {
        if (ix >= 0 && ix < w && iy >= 0 && iy < h)
            mask[(size_t)iy * w + ix] = 1;
    };
    if (x1 > x2)
    {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    if (x1 == x2)
    {
        if (x1 == std::floor(x1))
            return;
        double ylo = std::min(y1, y2), yhi = std::max(y1, y2);
        long long ix = (long long)std::floor(x1);
        long long ia = (long long)std::floor(ylo);
        long long ib = (long long)std::floor(yhi);
        if (yhi == (double)ib)
            ib--;
        ia = std::max(ia, 0LL);
        ib = std::min(ib, (long long)h - 1);
        for (long long iy = ia; iy <= ib; iy++)
            burn(ix, iy);
        return;
    }
    if (y1 == y2)
    {
        if (y1 == std::floor(y1))
            return;
        long long iy = (long long)std::floor(y1);
        long long ia = (long long)std::floor(x1);
        long long ib = (long long)std::floor(x2);
        if (x2 == (double)ib)
            ib--;
        ia = std::max(ia, 0LL);
        ib = std::min(ib, (long long)w - 1);
        for (long long ix = ia; ix <= ib; ix++)
            burn(ix, iy);
        return;
    }
    double M = (y2 - y1) / (x2 - x1);
    double B = y1 - M * x1;
    bool pos = y2 > y1;
    double ymax = std::max(y1, y2);
    long long iX = (long long)std::floor(x1);
    long long iXEnd = (long long)std::floor(x2);
    for (; iX <= iXEnd; iX++)
    {
        double xl = std::max(x1, (double)iX);
        double xr = std::min(x2, (double)(iX + 1));
        if (xl == xr)
            continue;
        double yl = xl == x1 ? y1 : M * xl + B;
        double yr = xr == x2 ? y2 : M * xr + B;
        double lo, hi;
        bool ephi;
        if (yl <= yr)
        {
            lo = yl;
            hi = yr;
            ephi = xr == x2;
        }
        else
        {
            lo = yr;
            hi = yl;
            ephi = xl == x1;
        }
        long long ia = (long long)std::floor(lo);
        long long ib = (long long)std::floor(hi);
        if (hi == (double)ib)
        {
            if (pos)
                ib--;
            else if (ephi && hi == ymax)
                ib--;
        }
        ia = std::max(ia, 0LL);
        ib = std::min(ib, (long long)h - 1);
        for (long long iy = ia; iy <= ib; iy++)
            burn(iX, iy);
    }
}

void zsAllTouched(const std::vector<std::vector<std::vector<double>>>
                      &polys,
                  int w, int h, std::vector<uint8_t> &mask)
{
    for (const auto &poly : polys)
    {
        zsFillPolygon(poly, w, h, mask);
        for (const auto &rg : poly)
        {
            for (size_t i = 0; i + 3 < rg.size(); i += 2)
                zsBurnLine(rg[i], rg[i + 1], rg[i + 2], rg[i + 3], w,
                           h, mask);
            // a lattice vertex the ring passes through monotonically in
            // both axes also touches its own floor cell
            size_t nv = rg.size() / 2;
            if (nv >= 3 && rg[0] == rg[2 * (nv - 1)] &&
                rg[1] == rg[2 * (nv - 1) + 1])
                nv--;
            if (nv < 3)
                continue;
            for (size_t i = 0; i < nv; i++)
            {
                double px = rg[2 * i], py = rg[2 * i + 1];
                if (px != std::floor(px) || py != std::floor(py))
                    continue;
                size_t ip = (i + nv - 1) % nv, in = (i + 1) % nv;
                double ax = rg[2 * ip], ay = rg[2 * ip + 1];
                double cx = rg[2 * in], cy = rg[2 * in + 1];
                bool mx = (ax < px && px < cx) || (ax > px && px > cx);
                bool my = (ay < py && py < cy) || (ay > py && py > cy);
                if (mx && my && px >= 0 && px < w && py >= 0 && py < h)
                    mask[(size_t)py * w + (long long)px] = 1;
            }
        }
    }
}

// West's incremental weighted variance (exactextract semantics)
struct ZsWest
{
    double sumw = 0, mean = 0, m2 = 0;
    void add(double x, double w)
    {
        sumw += w;
        double mo = mean;
        mean = mo + (w / sumw) * (x - mo);
        m2 += w * (x - mo) * (x - mean);
    }
    double variance() const
    {
        return sumw > 0 ? m2 / sumw
                        : std::numeric_limits<double>::quiet_NaN();
    }
};

struct ZsAcc
{
    double count = 0, sum = 0;
    ZsWest var, wvar;
    double wvsum = 0;
    bool haveW = false;
    bool anyMinMax = false;
    double mn = 0, mx = 0;
    double mncx = 0, mncy = 0, mxcx = 0, mxcy = 0;
    std::vector<double> vals, cxs, cys, ws;
    std::unordered_map<double, double> uniq;

    void add(double v, double cx, double cy, bool hasW, double w)
    {
        count += 1.0;
        sum += v;
        var.add(v, 1.0);
        vals.push_back(v);
        cxs.push_back(cx);
        cys.push_back(cy);
        if (!std::isnan(v))
        {
            if (!anyMinMax || v < mn)
            {
                mn = v;
                mncx = cx;
                mncy = cy;
            }
            if (!anyMinMax || v > mx)
            {
                mx = v;
                mxcx = cx;
                mxcy = cy;
            }
            anyMinMax = true;
        }
        uniq[v] += 1.0;
        if (hasW)
        {
            haveW = true;
            ws.push_back(w);
            wvsum += w * v;
            wvar.add(v, w);
        }
    }
};

double zsNan()
{
    return std::numeric_limits<double>::quiet_NaN();
}

void zsSetD(OgrFeature &f, size_t idx, double v)
{
    f.values[idx].set = true;
    f.values[idx].v.type = JVal::DOUBLE;
    f.values[idx].v.d = v;
}

void zsSetI(OgrFeature &f, size_t idx, long long v)
{
    f.values[idx].set = true;
    f.values[idx].v.type = JVal::INT;
    f.values[idx].v.i = v;
}

void zsSetArr(OgrFeature &f, size_t idx, const std::vector<double> &a)
{
    f.values[idx].set = true;
    f.values[idx].v.type = JVal::ARRAY;
    f.values[idx].v.arr.clear();
    f.values[idx].v.arr.reserve(a.size());
    for (double d : a)
    {
        JVal e;
        e.type = JVal::DOUBLE;
        e.d = d;
        f.values[idx].v.arr.push_back(std::move(e));
    }
}

bool zsStatIsArray(const std::string &s)
{
    return s == "center_x" || s == "center_y" || s == "coverage" ||
           s == "frac" || s == "unique" || s == "values" ||
           s == "weights";
}

bool zsStatNeedsWeights(const std::string &s)
{
    return s == "weighted_mean" || s == "weighted_stdev" ||
           s == "weighted_sum" || s == "weighted_variance" ||
           s == "weights";
}

// emit one stat value for the accumulator; scalar NaNs are skipped by
// the writer with the shared once-warning
void zsEmitStat(OgrFeature &f, size_t idx, const std::string &s,
                const ZsAcc &a)
{
    if (s == "count")
        zsSetD(f, idx, a.count);
    else if (s == "sum")
        zsSetD(f, idx, a.sum);
    else if (s == "mean")
        zsSetD(f, idx, a.count > 0 ? a.sum / a.count : zsNan());
    else if (s == "min")
    {
        if (a.anyMinMax)
            zsSetD(f, idx, a.mn);
    }
    else if (s == "max")
    {
        if (a.anyMinMax)
            zsSetD(f, idx, a.mx);
    }
    else if (s == "min_center_x")
    {
        if (a.anyMinMax)
            zsSetD(f, idx, a.mncx);
    }
    else if (s == "min_center_y")
    {
        if (a.anyMinMax)
            zsSetD(f, idx, a.mncy);
    }
    else if (s == "max_center_x")
    {
        if (a.anyMinMax)
            zsSetD(f, idx, a.mxcx);
    }
    else if (s == "max_center_y")
    {
        if (a.anyMinMax)
            zsSetD(f, idx, a.mxcy);
    }
    else if (s == "variance")
        zsSetD(f, idx, a.var.variance());
    else if (s == "stdev")
        zsSetD(f, idx, std::sqrt(a.var.variance()));
    else if (s == "weighted_mean")
        zsSetD(f, idx, a.wvsum / a.wvar.sumw);
    else if (s == "weighted_sum")
        zsSetD(f, idx, a.wvsum);
    else if (s == "weighted_variance")
        zsSetD(f, idx, a.wvar.variance());
    else if (s == "weighted_stdev")
        zsSetD(f, idx, std::sqrt(a.wvar.variance()));
    else if (s == "variety")
        zsSetI(f, idx, (long long)a.uniq.size());
    else if (s == "mode" || s == "minority")
    {
        bool best = false;
        double bv = 0, bc = 0;
        for (const auto &kv : a.uniq)
        {
            bool take;
            if (!best)
                take = true;
            else if (s == "mode")
                take = kv.second > bc ||
                       (kv.second == bc && kv.first > bv);
            else
                take = kv.second < bc ||
                       (kv.second == bc && kv.first < bv);
            if (take)
            {
                bv = kv.first;
                bc = kv.second;
                best = true;
            }
        }
        if (best)
            zsSetD(f, idx, bv);
    }
    else if (s == "values")
        zsSetArr(f, idx, a.vals);
    else if (s == "center_x")
        zsSetArr(f, idx, a.cxs);
    else if (s == "center_y")
        zsSetArr(f, idx, a.cys);
    else if (s == "coverage")
        zsSetArr(f, idx, std::vector<double>(a.vals.size(), 1.0));
    else if (s == "weights")
        zsSetArr(f, idx, a.ws);
    else if (s == "unique")
    {
        std::vector<double> u;
        u.reserve(a.uniq.size());
        for (const auto &kv : a.uniq)
            u.push_back(kv.first);
        zsSetArr(f, idx, u);
    }
    else if (s == "frac")
    {
        std::vector<double> fr;
        fr.reserve(a.uniq.size());
        for (const auto &kv : a.uniq)
            fr.push_back(kv.second / a.count);
        zsSetArr(f, idx, fr);
    }
}

bool zsCollectRings(const OgrGeometry &g,
                    std::vector<std::vector<std::vector<double>>> &polys,
                    const double *inv)
{
    auto ringConv = [&](const OgrGeometry &ring)
    {
        std::vector<double> rv;
        for (size_t i = 0; i + 1 < ring.coords.size(); i += 3)
        {
            double X = ring.coords[i], Y = ring.coords[i + 1];
            rv.push_back(inv[0] + X * inv[1] + Y * inv[2]);
            rv.push_back(inv[3] + X * inv[4] + Y * inv[5]);
        }
        return rv;
    };
    auto polyPush = [&](const OgrGeometry &poly)
    {
        std::vector<std::vector<double>> pr;
        for (const auto &rg : poly.parts)
            pr.push_back(ringConv(rg));
        polys.push_back(std::move(pr));
    };
    if (g.type == 3)
    {
        polyPush(g);
        return true;
    }
    if (g.type == 6)
    {
        for (const auto &poly : g.parts)
        {
            if (poly.type != 3)
                return false;
            polyPush(poly);
        }
        return true;
    }
    return false;
}

struct ZsGrid
{
    int w = 0, h = 0;
    double gt[6] = {0, 1, 0, 0, 0, 1};
};

bool zsSameGrid(const ZsGrid &a, const ZsGrid &b)
{
    if (a.w != b.w || a.h != b.h)
        return false;
    for (int i = 0; i < 6; i++)
        if (a.gt[i] != b.gt[i])
            return false;
    return true;
}

struct ZsExtent
{
    double xmin, xmax, ymin, ymax;
};

ZsExtent zsExtentOf(const ZsGrid &g)
{
    double x0 = g.gt[0], x1 = g.gt[0] + g.w * g.gt[1];
    double y0 = g.gt[3], y1 = g.gt[3] + g.h * g.gt[5];
    return {std::min(x0, x1), std::max(x0, x1), std::min(y0, y1),
            std::max(y0, y1)};
}

bool zsContains(const ZsExtent &a, const ZsExtent &b)
{
    return b.xmin >= a.xmin && b.xmax <= a.xmax && b.ymin >= a.ymin &&
           b.ymax <= a.ymax;
}

bool zsCellIntersects(const ZsGrid &g, int r, int c, const ZsExtent &e)
{
    double X0 = g.gt[0] + c * g.gt[1], X1 = X0 + g.gt[1];
    double Y0 = g.gt[3] + r * g.gt[5], Y1 = Y0 + g.gt[5];
    if (X0 > X1)
        std::swap(X0, X1);
    if (Y0 > Y1)
        std::swap(Y0, Y1);
    return X1 > e.xmin && X0 < e.xmax && Y1 > e.ymin && Y0 < e.ymax;
}

bool zsAlignedGrids(const ZsGrid &src, const ZsGrid &dst)
{
    if (src.gt[1] != dst.gt[1] || src.gt[5] != dst.gt[5])
        return false;
    double dx = (dst.gt[0] - src.gt[0]) / src.gt[1];
    double dy = (dst.gt[3] - src.gt[3]) / src.gt[5];
    return dx == std::floor(dx) && dy == std::floor(dy);
}

// window copy between lattice-aligned grids; out-of-range cells become
// invalid
void zsWindowCopy(const ZsGrid &sg, const std::vector<double> &sv,
                  const std::vector<bool> &svalid, const ZsGrid &dg,
                  std::vector<double> &dv, std::vector<bool> &dvalid)
{
    int dx = (int)std::lround((dg.gt[0] - sg.gt[0]) / sg.gt[1]);
    int dy = (int)std::lround((dg.gt[3] - sg.gt[3]) / sg.gt[5]);
    dv.assign((size_t)dg.w * dg.h, 0.0);
    dvalid.assign((size_t)dg.w * dg.h, false);
    for (int r = 0; r < dg.h; r++)
        for (int c = 0; c < dg.w; c++)
        {
            int sr = r + dy, sc = c + dx;
            if (sr < 0 || sr >= sg.h || sc < 0 || sc >= sg.w)
                continue;
            size_t si = (size_t)sr * sg.w + sc;
            dv[(size_t)r * dg.w + c] = sv[si];
            dvalid[(size_t)r * dg.w + c] = svalid[si];
        }
}

int zonalHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    std::string zonesPath = r.str("zones");
    std::string weightsPath = r.str("weights");
    bool quiet = r.flag("quiet");
    bool fOw = r.flag("overwrite");
    bool fApp = r.flag("append");
    bool fUpd = r.flag("update");
    bool fOwl = r.flag("overwrite-layer");
    bool fUps = r.flag("upsert");

    std::string driver;
    vectorOutputDriverResolve(r.str("output-format"), driver);
    if (driver.empty())
    {
        driver = rpfGuessDriver(output);
        if (strEndsWith(strToLower(output), ".gdalg.json"))
            driver.clear();
        if (driver.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "zonal-stats: Cannot guess driver for " + output);
            return 1;
        }
    }
    if (driver == "GDALG" || driver == "stream")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "zonal-stats: Cannot find driver " + driver);
        return 1;
    }
    // the reference creates the output dataset before the remaining
    // validation, leaving an empty file behind on those error paths
    bool existedAtStart = rpfFileExists(output);
    bool touchedOutput = false;
    if ((driver == "GeoJSON" || driver == "GeoJSONSeq") &&
        !existedAtStart)
    {
        writeStringToFile(output, "");
        touchedOutput = true;
    }

    std::string derr;
    auto ds = openRaster(input, derr);
    if (!ds)
    {
        if (derr == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
        else if (derr != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        return 1;
    }
    int nb = (int)ds->bands.size();
    std::vector<int> bands;
    {
        const ArgValue *bv = r.get("band");
        if (bv && bv->set)
            for (const auto &s : bv->values)
                bands.push_back(atoi(s.c_str()));
        else
            for (int i = 1; i <= nb; i++)
                bands.push_back(i);
        for (int b : bands)
            if (b < 1 || b > nb)
                return 1;
    }

    // zones: raster first, vector otherwise
    std::string zerr;
    cplPushQuietHandler();
    auto zds = openRaster(zonesPath, zerr);
    cplPopHandler();
    std::unique_ptr<OgrDataset> zvds;
    if (!zds)
    {
        std::string verr;
        zvds = openVectorDataset(zonesPath, verr, {});
        if (!zvds)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + zonesPath +
                            "' not recognized as being in a supported "
                            "file format.");
            return 1;
        }
    }

    int zonesBand = 0;
    const OgrLayer *zLayer = nullptr;
    const ArgValue *zbArg = r.get("zones-band");
    const ArgValue *zlArg = r.get("zones-layer");
    bool zbSet = zbArg && zbArg->set;
    bool zlSet = zlArg && zlArg->set;
    if (zds)
    {
        if (zlSet)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Specified zones layer '" + r.str("zones-layer") +
                            "' not found");
            return 1;
        }
        int znb = (int)zds->bands.size();
        if (zbSet)
        {
            zonesBand = atoi(r.str("zones-band").c_str());
            if (zonesBand < 1 || zonesBand > znb)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Invalid zones band: " +
                                r.str("zones-band"));
                return 1;
            }
        }
        else if (znb > 1)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Zones dataset has more than one band or layer. "
                        "Use the --zone-band or --zone-layer argument to "
                        "specify which should be used.");
            return 1;
        }
        else
            zonesBand = 1;
    }
    else
    {
        if (zbSet)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid zones band: " + r.str("zones-band"));
            return 1;
        }
        if (zlSet)
        {
            std::string ln = r.str("zones-layer");
            for (const auto &l : zvds->layers)
                if (l.name == ln)
                    zLayer = &l;
            if (!zLayer)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Specified zones layer '" + ln +
                                "' not found");
                return 1;
            }
        }
        else if (zvds->layers.size() > 1)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Zones dataset has more than one band or layer. "
                        "Use the --zone-band or --zone-layer argument to "
                        "specify which should be used.");
            return 1;
        }
        else if (!zvds->layers.empty())
            zLayer = &zvds->layers[0];
    }

    auto dtypeSupported = [](DType t)
    {
        switch (t)
        {
            case DType::Byte:
            case DType::Int8:
            case DType::UInt16:
            case DType::Int16:
            case DType::UInt32:
            case DType::Int32:
            case DType::Float16:
            case DType::Float32:
            case DType::Float64:
                return true;
            default:
                return false;
        }
    };
    for (const auto &b : ds->bands)
        if (!dtypeSupported(b.type))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GDALRasterZonalStats: Source data type " +
                            std::string(dtypeName(b.type)) +
                            " is not supported");
            return 1;
        }

    // weights band resolution
    std::unique_ptr<RasterDatasetBase> wds;
    int weightsBand = 1;
    bool hasWeights = !weightsPath.empty();
    if (hasWeights)
    {
        std::string werr;
        cplPushQuietHandler();
        wds = openRaster(weightsPath, werr);
        cplPopHandler();
        const ArgValue *wb = r.get("weights-band");
        if (wb && wb->set)
            weightsBand = atoi(r.str("weights-band").c_str());
        if (!wds || weightsBand < 1 ||
            weightsBand > (int)wds->bands.size())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GDALRasterZonalStats: invalid weights band");
            return 1;
        }
        for (const auto &b : wds->bands)
            if (!dtypeSupported(b.type))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "GDALRasterZonalStats: Weights data type " +
                                std::string(dtypeName(b.type)) +
                                " is not supported");
                return 1;
            }
    }

    if (r.str("strategy") == "raster" && !zds)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "The GEOS library is required to iterate over blocks "
                    "of the input rasters. Processing can be performed by "
                    "iterating over the input features instead.");
        return 1;
    }
    if (r.str("pixels") == "fractional")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Fractional pixel coverage calculation requires a "
                    "GDAL build against GEOS >= 3.14");
        return 1;
    }
    bool allTouched = r.str("pixels") == "all-touched";

    // stats: CLI order, first occurrence wins
    std::vector<std::string> stats;
    for (const auto &s : r.list("stat"))
    {
        if (s == "median")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid stat: median");
            return 1;
        }
        if (zsStatNeedsWeights(s) && !hasWeights)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Stat " + s +
                            " requires weights but none were provided");
            return 1;
        }
        if (std::find(stats.begin(), stats.end(), s) == stats.end())
            stats.push_back(s);
    }

    // include-field validation
    std::vector<int> inclIdx;
    std::vector<std::string> inclNames = r.list("include-field");
    if (!inclNames.empty())
    {
        if (zds)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot include fields from raster zones");
            return 1;
        }
        for (const auto &fn : inclNames)
        {
            int idx = -1;
            if (zLayer)
                for (size_t i = 0; i < zLayer->fields.size(); i++)
                    if (zLayer->fields[i].name == fn)
                        idx = (int)i;
            if (idx < 0)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Field " + fn + " not found.");
                return 1;
            }
            inclIdx.push_back(idx);
        }
    }

    // SRS advisory
    {
        bool zHas = zds ? zds->hasSrs : (zLayer && zLayer->hasSrs);
        const Srs *zsrs = zds ? &zds->srs
                              : (zLayer ? &zLayer->srs : nullptr);
        if (ds->hasSrs && zHas && zsrs &&
            !ds->srs.isEquivalentTo(*zsrs))
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Inputs and zones do not have the same SRS");
    }

    // compute grid: zones grid for raster zones, input grid otherwise
    ZsGrid ig, cg;
    ig.w = ds->width;
    ig.h = ds->height;
    memcpy(ig.gt, ds->gt, sizeof(ig.gt));
    if (zds)
    {
        cg.w = zds->width;
        cg.h = zds->height;
        memcpy(cg.gt, zds->gt, sizeof(cg.gt));
    }
    else
        cg = ig;

    // per-band input data on the compute grid
    std::vector<std::vector<double>> bandVals(bands.size());
    std::vector<std::vector<bool>> bandValid(bands.size());
    {
        bool regrid = zds && !zsSameGrid(ig, cg);
        bool aligned = regrid && zsAlignedGrids(ig, cg);
        ZsExtent ie = zsExtentOf(ig);
        bool covered = !regrid || zsContains(ie, zsExtentOf(cg));
        if (!covered)
        {
            if (ds->bands[bands[0] - 1].hasNodata)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Source raster does not fully cover zones "
                            "raster.Pixels that do not intersect the "
                            "values raster will be treated as having a "
                            "NoData value.");
            else
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Source raster does not fully cover zones "
                            "raster. Pixels that do not intersect the "
                            "value raster will be treated as having "
                            "value of zero.");
        }
        if (hasWeights && zds)
        {
            ZsGrid wg;
            wg.w = wds->width;
            wg.h = wds->height;
            memcpy(wg.gt, wds->gt, sizeof(wg.gt));
            if (!zsSameGrid(wg, cg) &&
                !zsContains(zsExtentOf(wg), zsExtentOf(cg)))
            {
                if (ds->bands[bands[0] - 1].hasNodata)
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        "Weighting raster does not fully cover zones "
                        "raster.Pixels that do not intersect the "
                        "weighting raster will be treated as having a "
                        "NoData weight.");
                else
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        "Weighting raster does not fully cover zones "
                        "raster. Pixels that do not intersect the "
                        "weighting raster will be treated as having "
                        "a weight of zero.");
            }
        }
        for (size_t bi = 0; bi < bands.size(); bi++)
        {
            std::vector<double> v;
            std::vector<bool> ok;
            if (!zsBandToDoubles(*ds, bands[bi], v, ok))
                return 1;
            if (!regrid)
            {
                bandVals[bi] = std::move(v);
                bandValid[bi] = std::move(ok);
                continue;
            }
            if (aligned)
                zsWindowCopy(ig, v, ok, cg, bandVals[bi], bandValid[bi]);
            else
            {
                zsResampleAverage(
                    ig.w, ig.h, ig.gt, v, ok, cg.w, cg.h, cg.gt,
                    bandVals[bi], bandValid[bi],
                    ds->bands[bands[bi] - 1].type,
                    ds->bands[bands[bi] - 1].hasNodata);
                if (ds->bands[bands[bi] - 1].hasNodata)
                {
                    double ndq = zsQuantNodata(
                        ds->bands[bands[bi] - 1].type,
                        ds->bands[bands[bi] - 1].nodata);
                    for (size_t i = 0; i < bandVals[bi].size(); i++)
                        if (bandValid[bi][i] && bandVals[bi][i] == ndq)
                            bandValid[bi][i] = false;
                }
            }
            if (!ds->bands[bands[bi] - 1].hasNodata)
                for (int rr = 0; rr < cg.h; rr++)
                    for (int cc = 0; cc < cg.w; cc++)
                    {
                        size_t i = (size_t)rr * cg.w + cc;
                        if (!bandValid[bi][i] &&
                            !zsCellIntersects(cg, rr, cc, ie))
                        {
                            bandVals[bi][i] = 0.0;
                            bandValid[bi][i] = true;
                        }
                    }
        }
        if (regrid && !aligned)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Resampled source raster to match zones using "
                        "average resampling.");
    }

    // weights on the compute grid: uncovered cells weigh zero, nodata
    // cells inside the weights extent stay NaN
    std::vector<double> wVals;
    bool perFeatW = false;
    ZsGrid pfWg;
    std::vector<double> pfWv;
    std::vector<bool> pfWok;
    DType pfWdt = DType::Float64;
    bool pfWnd = false;
    double pfWndv = 0;
    if (hasWeights)
    {
        ZsGrid wg;
        wg.w = wds->width;
        wg.h = wds->height;
        memcpy(wg.gt, wds->gt, sizeof(wg.gt));
        std::vector<double> wv;
        std::vector<bool> wok;
        if (!zsBandToDoubles(*wds, weightsBand, wv, wok))
            return 1;
        if (zsSameGrid(wg, cg))
        {
            for (size_t i = 0; i < wv.size(); i++)
                if (!wok[i])
                    wv[i] = zsNan();
            wVals = std::move(wv);
        }
        else
        {
            ZsExtent we = zsExtentOf(wg);
            std::vector<double> rv;
            std::vector<bool> rok;
            if (zsAlignedGrids(wg, cg))
            {
                zsWindowCopy(wg, wv, wok, cg, rv, rok);
                double unc = wds->bands[weightsBand - 1].hasNodata
                                 ? zsNan()
                                 : 0.0;
                for (int rr = 0; rr < cg.h; rr++)
                    for (int cc = 0; cc < cg.w; cc++)
                    {
                        size_t i = (size_t)rr * cg.w + cc;
                        if (!rok[i])
                            rv[i] = zsCellIntersects(cg, rr, cc, we)
                                        ? zsNan()
                                        : unc;
                    }
            }
            else if (zds)
            {
                zsResampleAverage(
                    wg.w, wg.h, wg.gt, wv, wok, cg.w, cg.h, cg.gt, rv,
                    rok, wds->bands[weightsBand - 1].type,
                    wds->bands[weightsBand - 1].hasNodata);
                // the resampled band keeps the nodata marker, so an
                // average landing exactly on it reads back as nodata
                if (wds->bands[weightsBand - 1].hasNodata)
                {
                    double ndq =
                        zsQuantNodata(wds->bands[weightsBand - 1].type,
                                      wds->bands[weightsBand - 1].nodata);
                    for (size_t i = 0; i < rv.size(); i++)
                        if (rok[i] && rv[i] == ndq)
                            rv[i] = zsNan();
                }
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Resampled weighting raster to match zones "
                            "using average resampling.");
                double unc = wds->bands[weightsBand - 1].hasNodata
                                 ? zsNan()
                                 : 0.0;
                for (int rr = 0; rr < cg.h; rr++)
                    for (int cc = 0; cc < cg.w; cc++)
                    {
                        size_t i = (size_t)rr * cg.w + cc;
                        if (!rok[i])
                            rv[i] = zsCellIntersects(cg, rr, cc, we)
                                        ? zsNan()
                                        : unc;
                    }
            }
            else
            {
                // vector zones: the reference resamples the weights
                // separately for every feature over its envelope
                perFeatW = true;
                pfWg = wg;
                pfWv = std::move(wv);
                pfWok = std::move(wok);
                pfWdt = wds->bands[weightsBand - 1].type;
                pfWnd = wds->bands[weightsBand - 1].hasNodata;
                pfWndv = zsQuantNodata(pfWdt,
                                       wds->bands[weightsBand - 1].nodata);
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Resampled weights to match source raster "
                            "using average resampling.");
                rv.clear();
            }
            wVals = std::move(rv);
        }
    }

    // output layer skeleton
    OgrLayer lyr;
    lyr.name = "stats";
    lyr.geomType = 101;
    lyr.hasGeomField = true;
    if (zds)
    {
        OgrFieldDefn fd;
        fd.name = "value";
        fd.type = OFTReal;
        lyr.fields.push_back(fd);
    }
    for (int ii : inclIdx)
        lyr.fields.push_back(zLayer->fields[ii]);
    for (size_t bi = 0; bi < bands.size(); bi++)
        for (const auto &s : stats)
        {
            OgrFieldDefn fd;
            fd.name = bands.size() > 1
                          ? s + strPrintf("_band_%d", bands[bi])
                          : s;
            fd.type = s == "variety" ? OFTInteger
                      : zsStatIsArray(s) ? OFTRealList
                                         : OFTReal;
            lyr.fields.push_back(fd);
        }

    int rcCompute = 0;
    if (zds)
    {
        std::vector<double> zv;
        std::vector<bool> zok;
        if (!zsBandToDoubles(*zds, zonesBand, zv, zok))
            return 1;
        // ascending zone order; accumulators per zone and band
        std::map<double, std::vector<ZsAcc>> zones;
        for (int rr = 0; rr < cg.h; rr++)
            for (int cc = 0; cc < cg.w; cc++)
            {
                size_t i = (size_t)rr * cg.w + cc;
                double zid = zv[i];
                auto it = zones.find(zid);
                if (it == zones.end())
                    it = zones
                             .emplace(zid,
                                      std::vector<ZsAcc>(bands.size()))
                             .first;
                // the reference derives cell centers from the source
                // geotransform even on the zones grid
                double cx = ig.gt[0] + (cc + 0.5) * ig.gt[1] +
                            (rr + 0.5) * ig.gt[2];
                double cy = ig.gt[3] + (cc + 0.5) * ig.gt[4] +
                            (rr + 0.5) * ig.gt[5];
                for (size_t bi = 0; bi < bands.size(); bi++)
                {
                    if (!bandValid[bi][i])
                        continue;
                    it->second[bi].add(bandVals[bi][i], cx, cy,
                                       hasWeights,
                                       hasWeights ? wVals[i] : 1.0);
                }
            }
        for (const auto &kv : zones)
        {
            OgrFeature f;
            f.values.resize(lyr.fields.size());
            zsSetD(f, 0, kv.first);
            size_t idx = 1;
            for (size_t bi = 0; bi < bands.size(); bi++)
                for (const auto &s : stats)
                    zsEmitStat(f, idx++, s, kv.second[bi]);
            lyr.features.push_back(std::move(f));
        }
    }
    else if (zLayer)
    {
        double det = cg.gt[1] * cg.gt[5] - cg.gt[2] * cg.gt[4];
        double inv[6] = {0, 1, 0, 0, 0, 1};
        if (det != 0)
        {
            inv[1] = cg.gt[5] / det;
            inv[2] = -cg.gt[2] / det;
            inv[4] = -cg.gt[4] / det;
            inv[5] = cg.gt[1] / det;
            inv[0] = -(cg.gt[0] * inv[1] + cg.gt[3] * inv[2]);
            inv[3] = -(cg.gt[0] * inv[4] + cg.gt[3] * inv[5]);
        }
        for (const OgrFeature &zf : zLayer->features)
        {
            std::vector<ZsAcc> accs(bands.size());
            bool geomOk = true;
            if (zf.hasGeom && !zf.geom.empty)
            {
                std::vector<std::vector<std::vector<double>>> polys;
                if (!zsCollectRings(zf.geom, polys, inv))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Non-polygonal geometry encountered.");
                    rcCompute = 1;
                    geomOk = false;
                }
                else
                {
                    std::vector<uint8_t> mask((size_t)cg.w * cg.h, 0);
                    if (allTouched)
                        zsAllTouched(polys, cg.w, cg.h, mask);
                    else
                        for (const auto &poly : polys)
                            zsFillPolygon(poly, cg.w, cg.h, mask);
                    const std::vector<double> *wp = &wVals;
                    std::vector<double> fw;
                    if (perFeatW)
                    {
                        double mnx = 1e300, mxx = -1e300;
                        double mny = 1e300, mxy = -1e300;
                        for (const auto &poly : polys)
                            for (const auto &rg : poly)
                                for (size_t i = 0; i + 1 < rg.size();
                                     i += 2)
                                {
                                    mnx = std::min(mnx, rg[i]);
                                    mxx = std::max(mxx, rg[i]);
                                    mny = std::min(mny, rg[i + 1]);
                                    mxy = std::max(mxy, rg[i + 1]);
                                }
                        int fc0 = std::max(0, (int)std::floor(mnx));
                        int fc1 = std::min(cg.w, (int)std::ceil(mxx));
                        int fr0 = std::max(0, (int)std::floor(mny));
                        int fr1 = std::min(cg.h, (int)std::ceil(mxy));
                        std::vector<bool> fok;
                        zsResampleAverage(pfWg.w, pfWg.h, pfWg.gt, pfWv,
                                          pfWok, cg.w, cg.h, cg.gt, fw,
                                          fok, pfWdt, pfWnd, fc0, fc1,
                                          fr0, fr1);
                        if (pfWnd)
                            for (int rr = fr0; rr < fr1; rr++)
                                for (int cc = fc0; cc < fc1; cc++)
                                {
                                    size_t i = (size_t)rr * cg.w + cc;
                                    if (fok[i] && fw[i] == pfWndv)
                                        fw[i] = zsNan();
                                }
                        ZsExtent we = zsExtentOf(pfWg);
                        double unc = pfWnd ? zsNan() : 0.0;
                        for (int rr = fr0; rr < fr1; rr++)
                            for (int cc = fc0; cc < fc1; cc++)
                            {
                                size_t i = (size_t)rr * cg.w + cc;
                                if (!fok[i])
                                    fw[i] =
                                        zsCellIntersects(cg, rr, cc, we)
                                            ? zsNan()
                                            : unc;
                            }
                        wp = &fw;
                    }
                    for (int rr = 0; rr < cg.h; rr++)
                        for (int cc = 0; cc < cg.w; cc++)
                        {
                            size_t i = (size_t)rr * cg.w + cc;
                            if (!mask[i])
                                continue;
                            double cx = cg.gt[0] + (cc + 0.5) * cg.gt[1] +
                                        (rr + 0.5) * cg.gt[2];
                            double cy = cg.gt[3] + (cc + 0.5) * cg.gt[4] +
                                        (rr + 0.5) * cg.gt[5];
                            for (size_t bi = 0; bi < bands.size(); bi++)
                            {
                                if (!bandValid[bi][i])
                                    continue;
                                accs[bi].add(
                                    bandVals[bi][i], cx, cy, hasWeights,
                                    hasWeights ? (*wp)[i] : 1.0);
                            }
                        }
                }
            }
            if (!geomOk)
                break;
            OgrFeature f;
            f.values.resize(lyr.fields.size());
            size_t idx = 0;
            for (int ii : inclIdx)
            {
                if (ii < (int)zf.values.size())
                    f.values[idx] = zf.values[ii];
                idx++;
            }
            for (size_t bi = 0; bi < bands.size(); bi++)
                for (const auto &s : stats)
                    zsEmitStat(f, idx++, s, accs[bi]);
            lyr.features.push_back(std::move(f));
        }
    }

    // update-family targets: zonal always creates a fresh layer, which
    // the GeoJSON driver refuses on an existing file
    bool fam = fApp || fUpd || fOwl || fUps;
    if (fam && existedAtStart)
    {
        std::string terr;
        auto tds = openVectorDataset(output, terr, {});
        if (tds && tds->driverShort == "GeoJSON")
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "GeoJSON driver doesn't support creating a layer "
                        "on a read-only datasource");
            return 1;
        }
    }

    if (driver == "GeoJSONSeq")
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "No SRS set on layer. Assuming it is long/lat on "
                    "WGS84 ellipsoid");

    auto ods = std::make_unique<OgrDataset>();
    ods->path = input;
    ods->driverShort = "MEM";
    ods->driverLong = "In Memory raster, vector and multidimensional "
                      "raster";
    ods->layers.push_back(std::move(lyr));

    zsForceArg(r, "output-format", driver);
    if (fApp && !existedAtStart)
        zsClearArg(r, "append");
    if (touchedOutput)
        remove(output.c_str());
    (void)quiet;
    (void)fOw;
    int rc = vvDelegateVerb(r, "zonal-stats", std::move(ods), "", driver,
                            false, nullptr);
    return rcCompute ? rcCompute : rc;
}

int zonalPreValidator(const CmdSpec &, ParseResult &r)
{
    const ArgValue *cs = r.get("chunk-size");
    if (cs && cs->set)
    {
        const std::string &v = r.str("chunk-size");
        const char *p = v.c_str();
        while (*p == ' ' || *p == '\t')
            p++;
        char *end = nullptr;
        strtod(p, &end);
        if (end == p)
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "Received non-numeric value: " + v);
            handlerPrintUsage();
            return 1;
        }
        p = end;
        while (*p == ' ' || *p == '\t')
            p++;
        std::string unit = p;
        static const char *units[] = {"B",  "KB", "MB", "GB", "TB",
                                      "K",  "M",  "G",  "T",  "%"};
        bool ok = false;
        for (const char *u : units)
            if (strEqualNoCase(unit, u))
                ok = true;
        if (!ok)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "zonal-stats: Memory size must have a unit or be "
                        "a percentage of usable RAM (2GB, 5%, etc.)");
            handlerPrintUsage();
            return 1;
        }
    }
    const ArgValue *bv = r.get("band");
    if (bv && bv->set)
        for (const auto &s : bv->values)
            if (atoi(s.c_str()) < 1)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Value of 'band' should greater or equal "
                            "to 1.");
                handlerPrintUsage();
                return 1;
            }
    return 0;
}

bool zonalPostValidator(const CmdSpec &, ParseResult &r, bool failed)
{
    (void)failed;
    const ArgValue *bv = r.get("band");
    if (!bv || !bv->set)
        return false;
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    if (input.empty() || !rpfFileExists(input))
        return false;
    cplPushQuietHandler();
    std::string err;
    auto ds = openRaster(input, err);
    cplPopHandler();
    if (!ds)
        return false;
    int nb = (int)ds->bands.size();
    for (const auto &s : bv->values)
        if (atoi(s.c_str()) > nb)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("zonal-stats: Value of 'band' should "
                                  "be greater or equal than 1 and less "
                                  "or equal than %d.",
                                  nb));
            return true;
        }
    return false;
}

}  // namespace

// output-exists processing between the input and zones dataset opens:
// refusal without a mode flag, early delete with --overwrite, missing
// target for the update family
void zonalStatsValidateOutput(ParseResult &r, bool &failed)
{
    std::string output = r.str("output");
    if (output.empty())
        return;
    std::string of = r.str("output-format");
    if (strEqualNoCase(of, "MEM") || strEqualNoCase(of, "stream"))
        return;
    bool ow = r.flag("overwrite");
    bool fApp = r.flag("append");
    bool fUpd = r.flag("update");
    bool fOwl = r.flag("overwrite-layer");
    bool fUps = r.flag("upsert");
    // a conflict inside the update-flag family (reported later by the
    // engine mutex pass) suppresses the output processing entirely
    if (fApp && fUps)
        return;
    bool fam = fApp || fUpd || fOwl || fUps;
    bool exists = rpfFileExists(output);
    if (!exists && (fUpd || fOwl || fUps))
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    datasetMissingMessage(output));
        failed = true;
        return;
    }
    if (!exists)
        return;
    if (!ow && !fam)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "zonal-stats: " + outputExistsKind(output) + " '" +
                        output +
                        "' already exists. You may specify the "
                        "--overwrite/--overwrite-layer/--append/--update "
                        "option.");
        failed = true;
        return;
    }
    if (ow && !fam)
    {
        if (outputExistsKind(output) == "Directory")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "zonal-stats: Directory '" + output +
                            "' already exists, but is not recognized as "
                            "a valid GDAL dataset. Please manually "
                            "delete it before retrying");
            failed = true;
            return;
        }
        overwriteDeleteFileset(output);
    }
}

void registerRasterZonalStatsHandler()
{
    registerHandler("raster_zonal-stats", zonalHandler);
    registerArgValueCheck(
        "raster_zonal-stats",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "output-format")
            {
                std::string drv;
                return vectorOutputDriverResolve(value, drv);
            }
            return "";
        });
    registerPreValidator("raster_zonal-stats", zonalPreValidator);
    registerPostValidator("raster_zonal-stats", zonalPostValidator);
}
