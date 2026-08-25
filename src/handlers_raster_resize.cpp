#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "util.h"
#include "vsi.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

// ------------------------------------------------------------------
// resampling kernels (RasterIO semantics: the convolution/average/mode
// chains match the overview generators, nearest is the translate
// decimation variant)
// ------------------------------------------------------------------

struct KMeta
{
    DType dt = DType::Byte;
    bool isInt = true;
    double clampLo = 0, clampHi = 255;
    bool hasNodata = false;
    double nodata = 0;
};

KMeta kmetaFor(DType dt, bool hasNodata, double nodata)
{
    KMeta m;
    m.dt = dt;
    m.hasNodata = hasNodata;
    m.nodata = nodata;
    switch (dt)
    {
        case DType::Byte:
            m.clampLo = 0;
            m.clampHi = 255;
            break;
        case DType::Int8:
            m.clampLo = -128;
            m.clampHi = 127;
            break;
        case DType::UInt16:
            m.clampLo = 0;
            m.clampHi = 65535;
            break;
        case DType::Int16:
            m.clampLo = -32768;
            m.clampHi = 32767;
            break;
        case DType::UInt32:
            m.clampLo = 0;
            m.clampHi = 4294967295.0;
            break;
        case DType::Int32:
            m.clampLo = -2147483648.0;
            m.clampHi = 2147483647.0;
            break;
        default:
            m.isInt = false;
            break;
    }
    return m;
}

double roundStore(double v, const KMeta &m)
{
    if (!m.isInt)
        return v;
    if (v < m.clampLo)
        v = m.clampLo;
    if (v > m.clampHi)
        v = m.clampHi;
    return v < 0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5);
}

double kernelWeight(const std::string &m, double t)
{
    double a = std::fabs(t);
    if (m == "bilinear")
        return a < 1.0 ? 1.0 - a : 0.0;
    if (m == "cubic")
    {
        if (a <= 1.0)
            return (1.5 * a - 2.5) * a * a + 1.0;
        if (a < 2.0)
            return -0.5 * (((a - 5.0) * a + 8.0) * a - 4.0);
        return 0.0;
    }
    if (m == "cubicspline")
    {
        double xp2 = t + 2.0, xp1 = t + 1.0, xm1 = t - 1.0;
        double xp2c = xp2 > 0.0 ? xp2 * xp2 * xp2 : 0.0;
        double r = xp2 > 0.0 ? xp2c : 0.0;
        if (xp1 > 0.0)
            r -= 4.0 * xp1 * xp1 * xp1;
        if (t > 0.0)
            r += 6.0 * t * t * t;
        if (xm1 > 0.0)
            r -= 4.0 * xm1 * xm1 * xm1;
        return r;
    }
    if (m == "lanczos")
    {
        if (a >= 3.0)
            return 0.0;
        if (a == 0.0)
            return 1.0;
        double px = M_PI * a;
        double pxr = px / 3.0;
        return std::sin(px) * std::sin(pxr) / (px * pxr);
    }
    return 0.0;
}

double kernelRadius(const std::string &m)
{
    if (m == "bilinear")
        return 1.0;
    if (m == "lanczos")
        return 3.0;
    return 2.0;  // cubic, cubicspline
}

// convolution sum with the reference's 4-way unroll into two accumulators
template <class W>
W convSumW(const W *vals, const W *ws, int n)
{
    W v1 = 0, v2 = 0;
    int i = 0;
    for (; i + 3 < n; i += 4)
    {
        v1 += vals[i] * ws[i];
        v1 += vals[i + 1] * ws[i + 1];
        v2 += vals[i + 2] * ws[i + 2];
        v2 += vals[i + 3] * ws[i + 3];
    }
    for (; i < n; ++i)
        v1 += vals[i] * ws[i];
    return v1 + v2;
}

// the RasterIO resamplers run their accumulations in the reference's
// working type: float covers everything up to 16-bit ints and Float32,
// the wider formats accumulate in double
bool floatWork(DType dt)
{
    return dt == DType::Byte || dt == DType::Int8 ||
           dt == DType::UInt16 || dt == DType::Int16 ||
           dt == DType::Float32;
}

// resample one whole band held as doubles; mirrors the RasterIO
// resampled-read semantics (which differ from the overview generators in
// masked-convolution structure, average windows and final store precision)

// when the requested value collides with the nodata marker the reference
// nudges it to the closest representable neighbour
double nodataReplacement(double v, const KMeta &m)
{
    if (m.isInt)
    {
        double lo = 0.0, hi = 255.0;
        switch (m.dt)
        {
            case DType::Byte: lo = 0; hi = 255; break;
            case DType::Int8: lo = -128; hi = 127; break;
            case DType::UInt16: lo = 0; hi = 65535; break;
            case DType::Int16: lo = -32768; hi = 32767; break;
            case DType::UInt32: lo = 0; hi = 4294967295.0; break;
            case DType::Int32: lo = -2147483648.0; hi = 2147483647.0; break;
            default: break;
        }
        (void)lo;
        return v >= hi ? v - 1 : v + 1;
    }
    if (m.dt == DType::Float32)
    {
        float f = (float)v;
        float r = std::nextafterf(f, std::numeric_limits<float>::infinity());
        if (!(r == r) || r == std::numeric_limits<float>::infinity())
            r = std::nextafterf(f, -std::numeric_limits<float>::infinity());
        return (double)r;
    }
    double r = std::nextafter(v, std::numeric_limits<double>::infinity());
    if (!(r == r) || r == std::numeric_limits<double>::infinity())
        r = std::nextafter(v, -std::numeric_limits<double>::infinity());
    return r;
}

// final store: everything up to 16-bit ints and Float32 goes through a
// float working buffer before the destination conversion
double convStore(double v, const KMeta &m, bool nd, double ndv)
{
    if (floatWork(m.dt))
        v = (double)(float)v;
    double out = roundStore(v, m);
    if (nd && out == ndv)
        out = nodataReplacement(out, m);
    return out;
}

void convResample(const std::vector<double> &src, int sw, int sh,
                  std::vector<double> &dst, int dw, int dh,
                  const std::string &method, const KMeta &m)
{
    bool nd = m.hasNodata;
    double ndv = m.nodata;
    double rx = (double)sw / dw, ry = (double)sh / dh;
    int radius = kernelRadius(method);
    double sxr = dw >= sw ? 1.0 : (double)dw / sw;
    double syr = dh >= sh ? 1.0 : (double)dh / sh;
    double rxk = radius / sxr, ryk = radius / syr;
    std::vector<double> xw, yw, hbuf;
    for (int y = 0; y < dh; ++y)
    {
        double cy = (y + 0.5) * ry - 0.5;
        int iy0 = (int)ceil(cy - ryk), iy1 = (int)floor(cy + ryk);
        if (iy0 < 0)
            iy0 = 0;
        if (iy1 >= sh)
            iy1 = sh - 1;
        int ny = iy1 - iy0 + 1;
        yw.resize(ny);
        {
            // the reference accumulates the kernel argument incrementally
            double a = syr * ((double)iy0 - ((y + 0.5) * ry) + 0.5);
            double ws = 0.0;
            for (int k = 0; k < ny; ++k, a += syr)
            {
                yw[k] = kernelWeight(method, a);
                ws += yw[k];
            }
            if (!nd && ws != 0.0)
            {
                double inv = 1.0 / ws;
                for (int k = 0; k < ny; ++k)
                    yw[k] *= inv;
            }
        }
        for (int x = 0; x < dw; ++x)
        {
            double cx = (x + 0.5) * rx - 0.5;
            int ix0 = (int)ceil(cx - rxk), ix1 = (int)floor(cx + rxk);
            if (ix0 < 0)
                ix0 = 0;
            if (ix1 >= sw)
                ix1 = sw - 1;
            int nx = ix1 - ix0 + 1;
            xw.resize(nx);
            {
                double a = sxr * ((double)ix0 - ((x + 0.5) * rx) + 0.5);
                double ws = 0.0;
                for (int k = 0; k < nx; ++k, a += sxr)
                {
                    xw[k] = kernelWeight(method, a);
                    ws += xw[k];
                }
                if (!nd && ws != 0.0)
                {
                    double inv = 1.0 / ws;
                    for (int k = 0; k < nx; ++k)
                        xw[k] *= inv;
                }
            }
            if (!nd)
            {
                hbuf.resize(ny);
                for (int k = 0; k < ny; ++k)
                    hbuf[k] = convSumW(&src[(size_t)(iy0 + k) * sw + ix0],
                                       xw.data(), nx);
                double v = convSumW(hbuf.data(), yw.data(), ny);
                dst[(size_t)y * dw + x] = convStore(v, m, nd, ndv);
            }
            else
            {
                // masked convolution renormalizes each horizontal row over
                // its valid pixels; rows only count when their valid weight
                // is positive, and a non-positive row-weight total yields
                // nodata
                double tot = 0.0, wsum = 0.0;
                for (int k = 0; k < ny; ++k)
                {
                    const double *row = &src[(size_t)(iy0 + k) * sw + ix0];
                    double rv = 0.0, rw = 0.0;
                    for (int j = 0; j < nx; ++j)
                    {
                        if (row[j] == ndv)
                            continue;
                        rv += row[j] * xw[j];
                        rw += xw[j];
                    }
                    if (rw > 0)
                    {
                        tot += yw[k] * (rv / rw);
                        wsum += yw[k];
                    }
                }
                if (wsum > 0)
                    dst[(size_t)y * dw + x] = convStore(tot / wsum, m, nd, ndv);
                else
                    dst[(size_t)y * dw + x] = ndv;
            }
        }
    }
}

void resampleBand(const std::vector<double> &src, int sw, int sh,
                  std::vector<double> &dst, int dw, int dh,
                  const std::string &method, const KMeta &m)
{
    dst.assign((size_t)dw * dh, 0.0);
    double rx = (double)sw / dw, ry = (double)sh / dh;
    bool nd = m.hasNodata;
    double ndv = m.nodata;
    if (dw == sw && dh == sh)
    {
        // identical dimensions skip resampling entirely, whatever the method
        dst = src;
        return;
    }
    if ((m.dt == DType::Int16 || m.dt == DType::Int8) && method == "average")
    {
        KMeta mf = m;
        mf.dt = DType::Float32;
        mf.isInt = false;
        resampleBand(src, sw, sh, dst, dw, dh, method, mf);
        for (double &v : dst)
            if (!nd || v != ndv)
                v = roundStore((double)(float)v, m);
        return;
    }
    if (method == "nearest")
    {
        for (int y = 0; y < dh; ++y)
        {
            int sy = (int)((y + 0.5) * ry);
            if (sy >= sh)
                sy = sh - 1;
            for (int x = 0; x < dw; ++x)
            {
                int sx = (int)((x + 0.5) * rx);
                if (sx >= sw)
                    sx = sw - 1;
                dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
            }
        }
        return;
    }
    if (method == "average")
    {
        bool intRatio = rx == floor(rx) && ry == floor(ry) && rx >= 1 && ry >= 1;
        if (intRatio)
        {
            int fx = (int)rx, fy = (int)ry;
            for (int y = 0; y < dh; ++y)
            {
                for (int x = 0; x < dw; ++x)
                {
                    double tot = 0.0;
                    int cnt = 0;
                    for (int yy = y * fy; yy < (y + 1) * fy && yy < sh; ++yy)
                        for (int xx = x * fx; xx < (x + 1) * fx && xx < sw;
                             ++xx)
                        {
                            double v = src[(size_t)yy * sw + xx];
                            if (nd && v == ndv)
                                continue;
                            tot += v;
                            ++cnt;
                        }
                    if (cnt == 0)
                        dst[(size_t)y * dw + x] = nd ? ndv : 0.0;
                    else if (m.dt == DType::Float32)
                    {
                        if (fx == 2 && fy == 2 && cnt == 4)
                        {
                            const double *r0 =
                                &src[(size_t)(y * 2) * sw + x * 2];
                            const double *r1 = r0 + sw;
                            float s = ((float)r0[0] + (float)r1[0]) +
                                      ((float)r0[1] + (float)r1[1]);
                            double v = (double)(s * 0.25f);
                            if (nd && v == ndv)
                                v = nodataReplacement(v, m);
                            dst[(size_t)y * dw + x] = v;
                        }
                        else
                        {
                            double v = (double)(float)(tot / cnt);
                            if (nd && v == ndv)
                                v = nodataReplacement(v, m);
                            dst[(size_t)y * dw + x] = v;
                        }
                    }
                    else
                    {
                        double v = roundStore(tot / cnt, m);
                        if (nd && v == ndv)
                            v = nodataReplacement(v, m);
                        dst[(size_t)y * dw + x] = v;
                    }
                }
            }
            return;
        }
        // fractional windows: epsilon-adjusted integer bounds, edge weights
        // taken from the window start (the trailing clip is only applied to
        // the last cell of multi-cell windows)
        for (int y = 0; y < dh; ++y)
        {
            double ay = y * ry, by = (y + 1) * ry;
            int y0 = (int)(ay + 1e-8);
            int y1 = (int)ceil(by - 1e-8);
            if (y1 > sh)
                y1 = sh;
            if (y1 <= y0)
                y1 = y0 + 1;
            for (int x = 0; x < dw; ++x)
            {
                double ax = x * rx, bx = (x + 1) * rx;
                int x0 = (int)(ax + 1e-8);
                int x1 = (int)ceil(bx - 1e-8);
                if (x1 > sw)
                    x1 = sw;
                if (x1 <= x0)
                    x1 = x0 + 1;
                if (rx == 2.0 && y1 - y0 == 2 && !nd)
                {
                    // the two-column fast path averages the four pixels with
                    // full weight; floats pair the columns first
                    const double *r0 = &src[(size_t)y0 * sw + x0];
                    const double *r1 = r0 + sw;
                    if (m.isInt)
                    {
                        long long s = (long long)r0[0] + (long long)r0[1] +
                                      (long long)r1[0] + (long long)r1[1];
                        long long q = s >= 0 ? (s + 2) / 4 : -((-s + 2) / 4);
                        dst[(size_t)y * dw + x] = roundStore((double)q, m);
                    }
                    else if (m.dt == DType::Float32)
                    {
                        float v = (((float)r0[0] + (float)r1[0]) +
                                   ((float)r0[1] + (float)r1[1])) *
                                  0.25f;
                        dst[(size_t)y * dw + x] = (double)v;
                    }
                    else
                    {
                        double v =
                            ((r0[0] + r1[0]) + (r0[1] + r1[1])) * 0.25;
                        dst[(size_t)y * dw + x] = roundStore(v, m);
                    }
                    continue;
                }
                double tot = 0.0, wsum = 0.0;
                int cnt = 0;
                for (int yy = y0; yy < y1; ++yy)
                {
                    double wy = 1.0;
                    if (yy == y0)
                        wy = 1.0 - (ay - y0);
                    else if (yy + 1 == y1)
                        wy = 1.0 - (y1 - by);
                    double rt = 0.0, rw = 0.0;
                    for (int xx = x0; xx < x1; ++xx)
                    {
                        double v = src[(size_t)yy * sw + xx];
                        if (nd && v == ndv)
                            continue;
                        double wx = 1.0;
                        if (xx == x0)
                            wx = 1.0 - (ax - x0);
                        else if (xx + 1 == x1)
                            wx = 1.0 - (x1 - bx);
                        rt += v * wx;
                        rw += wx;
                        ++cnt;
                    }
                    tot += rt * wy;
                    wsum += rw * wy;
                }
                if (cnt == 0 || wsum == 0.0)
                {
                    dst[(size_t)y * dw + x] = nd ? ndv : 0.0;
                    continue;
                }
                double v = tot / wsum;
                double out = roundStore(v, m);
                if (nd && out == ndv)
                    out = nodataReplacement(out, m);
                dst[(size_t)y * dw + x] = out;
            }
        }
        return;
    }
    if (method == "mode")
    {
        for (int y = 0; y < dh; ++y)
        {
            int y0 = (int)(y * ry), y1 = (int)ceil((y + 1) * ry);
            if (y1 > sh)
                y1 = sh;
            if (y1 <= y0)
                y1 = y0 + 1;
            for (int x = 0; x < dw; ++x)
            {
                int x0 = (int)(x * rx), x1 = (int)ceil((x + 1) * rx);
                if (x1 > sw)
                    x1 = sw;
                if (x1 <= x0)
                    x1 = x0 + 1;
                std::map<double, int> counts;
                std::vector<double> order;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx)
                    {
                        double v = src[(size_t)yy * sw + xx];
                        if (nd && v == ndv)
                            continue;
                        if (counts.find(v) == counts.end())
                            order.push_back(v);
                        counts[v]++;
                    }
                // windows with no valid pixel keep nodata under the
                // histogram-based small-int path but come out as zero for
                // the wider types
                bool histo = m.dt == DType::Byte || m.dt == DType::Int8 ||
                             m.dt == DType::UInt16;
                double best = histo && nd ? ndv : 0.0;
                int bestCnt = 0;
                for (double v : order)
                    if (counts[v] > bestCnt)
                    {
                        bestCnt = counts[v];
                        best = v;
                    }
                dst[(size_t)y * dw + x] = best;
            }
        }
        return;
    }
    // convolution kernels: bilinear / cubic / cubicspline / lanczos
    convResample(src, sw, sh, dst, dw, dh, method, m);
}

// ------------------------------------------------------------------
// size/resolution argument parsing
// ------------------------------------------------------------------

// GDAL validates size tokens with CPLStrtod semantics (no hexadecimal),
// then consumes them with atoi, so "1e2" passes validation but yields 1
bool strictRealParse(const std::string &s, double &v)
{
    if (numLooksHex(s))
        return false;
    const char *c = s.c_str();
    char *end = nullptr;
    v = strtod(c, &end);
    if (end == c || *end != '\0')
        return false;
    return true;
}

struct SizeTok
{
    std::string raw;
    bool pct = false;
    double pctVal = 0;  // percentage as given (e.g. 33 for "33%")
    int val = 0;        // non-pct integer value
};

bool sizeTokenValid(const std::string &tok, SizeTok &out)
{
    out.raw = tok;
    if (!tok.empty() && tok.back() == '%')
    {
        out.pct = true;
        std::string pre = tok.substr(0, tok.size() - 1);
        if (pre.empty())
        {
            out.pctVal = 0;
            return true;
        }
        double v;
        if (!strictRealParse(pre, v) || v < 0)
            return false;
        out.pctVal = v;
        return true;
    }
    double v;
    if (!strictRealParse(tok, v))
        return false;
    if (v < 0 || v > 2147483647.0 || v != std::floor(v))
        return false;
    out.val = atoi(tok.c_str());
    return true;
}

std::vector<std::string> nonEmptySizeValues(const ParseResult &r)
{
    std::vector<std::string> vals;
    for (const auto &v : r.list("size"))
        if (!v.empty())
            vals.push_back(v);
    return vals;
}

// ------------------------------------------------------------------
// resized wrapper dataset
// ------------------------------------------------------------------

class ResizeDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    std::string method;

    ResizeDataset(std::unique_ptr<RasterDatasetBase> s, int newW, int newH,
                  const std::string &meth, const double *newGt, bool gtSet)
        : src(std::move(s)), method(meth)
    {
        path = src->path;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = newW;
        height = newH;
        hasGT = gtSet;
        if (gtSet)
            memcpy(gt, newGt, sizeof gt);
        srs = std::move(src->srs);
        hasSrs = src->hasSrs;
        srsSynthetic = src->srsSynthetic;
        metadata = src->metadata;
        domainOrder = src->domainOrder;
        sortedDomains = src->sortedDomains;
        xmlDomains = src->xmlDomains;
        files = src->files;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();
        for (size_t i = 0; i < src->bands.size(); ++i)
        {
            Band b = src->bands[i];
            b.blockX = newW;
            b.blockY = newH;
            b.index = (int)i + 1;
            bands.push_back(std::move(b));
        }
    }

    bool readResampled(int band, std::vector<double> &out)
    {
        std::vector<double> sv;
        if (!src->readBand(band, sv))
            return false;
        const Band &b = src->bands[band - 1];
        KMeta m = kmetaFor(b.type, b.hasNodata, b.nodata);
        resampleBand(sv, src->width, src->height, out, width, height,
                     method, m);
        return true;
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        return readResampled(band, out);
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        std::vector<double> vals;
        if (!readResampled(band, vals))
            return false;
        const Band &b = bands[band - 1];
        int bpe = dtypeSizeBytes(b.type);
        out.assign((size_t)width * height * bpe, 0);
        for (size_t i = 0; i < vals.size(); ++i)
            rasterEncodeReal(b.type, &out[i * bpe], vals[i], 0.0);
        return true;
    }

    bool readBandRawStrict(int band, std::vector<uint8_t> &out) override
    {
        return readBandRaw(band, out);
    }

    bool vrtWrapperRects(WrapRects &wr) override
    {
        wr.srcW = src->width;
        wr.srcH = src->height;
        int bx = src->width, by = src->height;
        src->realBlockDims(bx, by);
        wr.srcBlockX = bx;
        wr.srcBlockY = by;
        wr.sx = 0;
        wr.sy = 0;
        wr.sw = src->width;
        wr.sh = src->height;
        wr.dx = 0;
        wr.dy = 0;
        wr.dw = width;
        wr.dh = height;
        wr.resampling = method;
        return true;
    }
};

// ------------------------------------------------------------------
// argument checks / validation ordering
// ------------------------------------------------------------------

int resizeArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName == "output-format")
    {
        std::string drv;
        std::string issue = rasterOutFormatIssue(r.str("output-format"),
                                                 drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "resize: " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    else if (argName == "size")
    {
        std::vector<std::string> vals = nonEmptySizeValues(r);
        if ((long long)vals.size() != 2)
        {
            long long cnt = (long long)vals.size();
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("resize: %lld value%s been specified "
                                  "for argument 'size', whereas exactly "
                                  "2 were expected.",
                                  cnt, cnt == 1 ? " has" : "s have"));
            handlerPrintUsage();
            return 1;
        }
        for (const auto &v : vals)
        {
            SizeTok t;
            if (!sizeTokenValid(v, t))
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "resize: Invalid size value: " + v + "'");
                handlerPrintUsage();
                return 1;
            }
        }
    }
    return 0;
}

bool sameFileAs(const std::string &a, const std::string &b)
{
    struct stat sa, sb;
    if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

// GDALAlgorithm-style validation-time output processing replicated for
// this leaf (the engine's shared pass only covers convert-like verbs):
// input open failure, exists refusal / overwrite delete, missing size,
// mutual exclusions, all stacked before one usage block
int resizePreValidator(const CmdSpec &, ParseResult &r)
{
    bool fail = false;
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool ow = r.flag("overwrite");
    bool append = r.flag("append");

    const ArgValue *inv = r.get("input");
    if (inv && !input.empty() && input.rfind("GTIFF_DIR:", 0) != 0)
    {
        struct stat sb;
        if (stat(input.c_str(), &sb) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
            fail = true;
        }
        else if (!datasetIdentify(input, {"raster"}))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
            fail = true;
        }
    }

    std::string of = r.str("output-format");
    bool skipOut = strEqualNoCase(of, "MEM") ||
                   strEqualNoCase(of, "Memory") ||
                   strEqualNoCase(of, "stream");
    const ArgValue *outv = r.get("output");
    struct stat ost;
    if (!skipOut && outv && !output.empty() &&
        stat(output.c_str(), &ost) == 0)
    {
        std::string kind = outputExistsKind(output);
        if (!ow && !append)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "resize: " + kind + " '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--append option.");
            fail = true;
        }
        else if (ow && !append)
        {
            if (kind == "Directory")
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "resize: Directory '" + output +
                                "' already exists, but is not recognized "
                                "as a valid GDAL dataset. Please manually "
                                "delete it before retrying");
                fail = true;
            }
            else if (!sameFileAs(input, output))
                overwriteDeleteFileset(output);
        }
    }

    if (!r.get("size") && !r.get("resolution"))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "resize: Required argument 'size' has not been "
                    "specified.");
        fail = true;
    }

    if (ow && append)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "resize: Argument 'append' is mutually exclusive "
                    "with 'overwrite'.");
        fail = true;
    }
    if (r.get("resolution") && r.get("size"))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "resize: Argument 'size' is mutually exclusive with "
                    "'resolution'.");
        fail = true;
    }

    if (fail)
    {
        handlerPrintUsage();
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------
// handler
// ------------------------------------------------------------------

int rasterResizeHandler(const CmdSpec &, ParseResult &r)
{
    bool prefixWasEmpty = g_pipelineStepPrefix.empty();
    if (prefixWasEmpty)
        g_pipelineStepPrefix = "resize";
    struct PrefixReset
    {
        bool active;
        ~PrefixReset()
        {
            if (active)
                g_pipelineStepPrefix.clear();
        }
    } reset{prefixWasEmpty};

    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");
    std::string format = r.str("output-format");
    std::string method = strToLower(r.str("resampling", "nearest"));
    bool rSet = r.get("resampling") != nullptr;
    bool haveRes = r.get("resolution") != nullptr;

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

    OpenOptions oo;
    oo.allowedDrivers = r.list("input-format");
    for (const auto &kv : r.list("open-option"))
    {
        size_t eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
        oo.raw.emplace_back(key, val);
    }

    std::string err;
    auto ds = openRaster(input, err, oo);
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

    std::vector<std::string> sizeVals = nonEmptySizeValues(r);
    std::vector<std::string> resVals = r.list("resolution");

    std::string extra;
    if (haveRes)
        extra += " --resolution " + strJoin(resVals, ",");
    else
        extra += " --size " + strJoin(sizeVals, ",");
    if (rSet)
        extra += " --resampling " + r.str("resampling");

    auto materialize =
        [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        int srcW = d->width, srcH = d->height;
        int nW = 0, nH = 0;
        double ngt[6];
        memcpy(ngt, d->gt, sizeof ngt);
        bool gtSet = d->hasGT;
        if (haveRes)
        {
            if (!d->hasGT || d->gt[2] != 0 || d->gt[4] != 0)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "The -tr option was used, but there's no "
                            "geotransform or it is\nrotated.  This "
                            "configuration is not supported.");
                return 1;
            }
            double xres = strtod(resVals[0].c_str(), nullptr);
            double yres = strtod(resVals[1].c_str(), nullptr);
            nW = (int)(0.5 + srcW * std::fabs(d->gt[1]) / xres);
            nH = (int)(0.5 + srcH * std::fabs(d->gt[5]) / yres);
            ngt[1] = xres;
            ngt[5] = d->gt[5] < 0 ? -yres : yres;
        }
        else
        {
            SizeTok tw, th;
            sizeTokenValid(sizeVals[0], tw);
            sizeTokenValid(sizeVals[1], th);
            if (tw.pct)
            {
                double dfW = tw.pctVal / 100.0 * srcW;
                nW = (int)dfW;
                if (dfW != 0 && nW <= 0)
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                strPrintf("Invalid output width: %g",
                                          dfW));
                    return 1;
                }
            }
            else
                nW = tw.val;
            if (th.pct)
            {
                double dfH = th.pctVal / 100.0 * srcH;
                nH = (int)dfH;
                if (dfH != 0 && nH <= 0)
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                strPrintf("Invalid output height: %g",
                                          dfH));
                    return 1;
                }
            }
            else
                nH = th.val;
            if (nW == 0 && nH == 0)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "-outsize 0 0 invalid.");
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "Attempt to create new tiff file `' failed: "
                            ": No such file or directory");
                return 1;
            }
            if (nW == 0)
                nW = (int)(0.5 + (double)nH * srcW / srcH);
            if (nH == 0)
                nH = (int)(0.5 + (double)nW * srcH / srcW);
            if (gtSet)
            {
                double kx = srcW / (double)nW;
                double ky = srcH / (double)nH;
                ngt[1] *= kx;
                ngt[2] *= kx;
                ngt[4] *= ky;
                ngt[5] *= ky;
            }
        }
        d = std::make_unique<ResizeDataset>(std::move(d), nW, nH, method,
                                            ngt, gtSet);
        return 0;
    };

    return rasterConvertWriteOutput(ds, r, input, output, quiet, overwrite,
                                    append, drv, extra, materialize);
}

struct Reg
{
    Reg()
    {
        registerHandler("raster_resize", rasterResizeHandler);
        registerPreValidator("raster_resize", resizePreValidator);
        registerArgCheck("raster_resize", resizeArgCheck);
    }
};

}  // namespace

void registerRasterResizeHandler()
{
    static Reg reg;
}
