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
// resampling kernels (per-verb copy of the RasterIO resampled-read
// chain, same as the resize handler keeps)
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

bool floatWork(DType dt)
{
    return dt == DType::Byte || dt == DType::Int8 ||
           dt == DType::UInt16 || dt == DType::Int16 ||
           dt == DType::Float32;
}

double nodataReplacement(double v, const KMeta &m)
{
    if (m.isInt)
    {
        double hi = 255.0;
        switch (m.dt)
        {
            case DType::Byte: hi = 255; break;
            case DType::Int8: hi = 127; break;
            case DType::UInt16: hi = 65535; break;
            case DType::Int16: hi = 32767; break;
            case DType::UInt32: hi = 4294967295.0; break;
            case DType::Int32: hi = 2147483647.0; break;
            default: break;
        }
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
    convResample(src, sw, sh, dst, dw, dh, method, m);
}

// ------------------------------------------------------------------
// spectral references and the pansharpened wrapper dataset
// ------------------------------------------------------------------

struct SpectralTok
{
    std::string raw;    // as typed (for the GDALG echo)
    std::string path;
    int band = 0;       // 0 = all bands
};

std::string fmt17(double d)
{
    if (d == (double)(long long)d && std::fabs(d) < 1e15)
        return strPrintf("%lld", (long long)d);
    return strPrintf("%.17g", d);
}

SpectralTok parseSpectralTok(const std::string &tok)
{
    SpectralTok t;
    t.raw = tok;
    t.path = tok;
    size_t pos = tok.rfind(",band=");
    if (pos != std::string::npos)
    {
        t.path = tok.substr(0, pos);
        t.band = atoi(tok.c_str() + pos + 6);
    }
    return t;
}

struct Win
{
    int x0 = 0, y0 = 0, w = 0, h = 0;
};

class PansharpenDataset final : public RasterDatasetBase
{
  public:
    bool pansharpenProduced() const override { return true; }
    int pansharpenNbits() const override { return nbits; }
    int nbits = 0;
    std::unique_ptr<RasterDatasetBase> pan;
    std::vector<std::unique_ptr<RasterDatasetBase>> spectralDs;
    struct Ref
    {
        int dsIdx = 0;
        int band = 1;
    };
    std::vector<Ref> refs;
    std::string method;
    std::vector<double> weights;
    bool useNodata = false;
    double ndVal = 0;
    double clampMax = 255;
    bool outIsInt = true;
    Win panWin;                 // window of the pan raster covered by out
    std::vector<Win> msWins;    // per spectral dataset
    bool computed = false;
    bool computeOk = true;
    std::vector<std::vector<double>> cache;

    PansharpenDataset(std::unique_ptr<RasterDatasetBase> p,
                      std::vector<std::unique_ptr<RasterDatasetBase>> sds,
                      std::vector<Ref> rf, int outW, int outH,
                      const double *outGt)
        : pan(std::move(p)), spectralDs(std::move(sds)), refs(std::move(rf))
    {
        path = pan->path;
        driverShort = pan->driverShort;
        driverLong = pan->driverLong;
        width = outW;
        height = outH;
        hasGT = pan->hasGT;
        if (hasGT)
            memcpy(gt, outGt, sizeof gt);
        srs = pan->srs;
        hasSrs = pan->hasSrs;
        for (size_t i = 0; i < refs.size(); ++i)
        {
            const Band &sb =
                spectralDs[refs[i].dsIdx]->bands[refs[i].band - 1];
            Band b;
            b.index = (int)i + 1;
            b.type = sb.type;
            b.blockX = outW;
            b.blockY = outH;
            bands.push_back(std::move(b));
        }
    }

    static void cropBand(const std::vector<double> &src, int sw, int sh,
                         const Win &w, std::vector<double> &dst)
    {
        dst.assign((size_t)w.w * w.h, 0.0);
        for (int y = 0; y < w.h; ++y)
        {
            int sy = w.y0 + y;
            if (sy < 0 || sy >= sh)
                continue;
            for (int x = 0; x < w.w; ++x)
            {
                int sx = w.x0 + x;
                if (sx < 0 || sx >= sw)
                    continue;
                dst[(size_t)y * w.w + x] = src[(size_t)sy * sw + sx];
            }
        }
    }

    bool computeAll()
    {
        if (computed)
            return computeOk;
        computed = true;
        size_t n = refs.size();
        size_t px = (size_t)width * height;
        cache.assign(n, {});

        std::vector<double> panFull, panv;
        if (!pan->readBand(1, panFull))
            return computeOk = false;
        if (panWin.x0 == 0 && panWin.y0 == 0 && panWin.w == pan->width &&
            panWin.h == pan->height && width == pan->width &&
            height == pan->height)
            panv = std::move(panFull);
        else
            cropBand(panFull, pan->width, pan->height, panWin, panv);

        std::vector<std::vector<double>> ums(n);
        std::vector<double> sv, cropped;
        for (size_t i = 0; i < n; ++i)
        {
            RasterDatasetBase *ds = spectralDs[refs[i].dsIdx].get();
            const Band &sb = ds->bands[refs[i].band - 1];
            if (!ds->readBand(refs[i].band, sv))
                return computeOk = false;
            const Win &w = msWins[refs[i].dsIdx];
            const std::vector<double> *src = &sv;
            if (!(w.x0 == 0 && w.y0 == 0 && w.w == ds->width &&
                  w.h == ds->height))
            {
                cropBand(sv, ds->width, ds->height, w, cropped);
                src = &cropped;
            }
            KMeta m = kmetaFor(sb.type, useNodata, ndVal);
            resampleBand(*src, w.w, w.h, ums[i], width, height, method, m);
        }

        // overshooting kernels get their upsampled buffer clamped to the
        // bit-depth ceiling; non-overshooting ones feed the ratio raw
        if (nbits > 0 && (method == "cubic" || method == "cubicspline" ||
                          method == "lanczos"))
        {
            for (auto &b : ums)
                for (double &v : b)
                    if (v > clampMax)
                        v = clampMax;
        }

        KMeta outMeta = kmetaFor(bands[0].type, false, 0);
        for (size_t i = 0; i < n; ++i)
            cache[i].assign(px, 0.0);
        for (size_t k = 0; k < px; ++k)
        {
            double p = panv[k];
            double ps = 0.0;
            for (size_t i = 0; i < n; ++i)
                ps += weights[i] * ums[i][k];
            if (useNodata)
            {
                bool nd = p == ndVal || ps == 0.0;
                for (size_t i = 0; i < n && !nd; ++i)
                    if (ums[i][k] == ndVal)
                        nd = true;
                if (nd)
                {
                    for (size_t i = 0; i < n; ++i)
                        cache[i][k] = ndVal;
                    continue;
                }
            }
            double ratio = ps != 0.0 ? p / ps : 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                double t = ums[i][k] * ratio;
                double outv;
                if (outIsInt)
                {
                    long long q = (long long)(t + 0.5);
                    if ((double)q > clampMax)
                        q = (long long)clampMax;
                    outv = (double)q;
                }
                else
                    outv = t > clampMax ? clampMax : t;
                if (useNodata && outv == ndVal)
                    outv = nodataReplacement(outv, outMeta);
                cache[i][k] = outv;
            }
        }
        return computeOk = true;
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        if (!computeAll())
            return false;
        out = cache[band - 1];
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        std::vector<double> vals;
        if (!readBand(band, vals))
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
};

class GdalgStubDataset final : public RasterDatasetBase
{
  public:
    bool readBand(int, std::vector<double> &) override { return false; }
    bool readBandRaw(int, std::vector<uint8_t> &) override { return false; }
};

// ------------------------------------------------------------------
// argument checks / validation
// ------------------------------------------------------------------

bool numThreadsValid(const std::string &v)
{
    if (strEqualNoCase(v, "ALL_CPUS"))
        return true;
    if (v.empty())
        return false;
    for (char c : v)
        if (c < '0' || c > '9')
            return false;
    return true;
}

int pansharpenArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName == "output-format")
    {
        std::string drv;
        std::string issue =
            rasterOutFormatIssue(r.str("output-format"), drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pansharpen: " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    else if (argName == "num-threads")
    {
        if (!numThreadsValid(r.str("num-threads")))
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "pansharpen: Invalid value for 'num-threads' "
                        "argument");
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

bool psSameFileAs(const std::string &a, const std::string &b)
{
    struct stat sa, sb;
    if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int pansharpenPreValidator(const CmdSpec &, ParseResult &r)
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
                        "pansharpen: " + kind + " '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--append option.");
            fail = true;
        }
        else if (ow && !append)
        {
            if (kind == "Directory")
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "pansharpen: Directory '" + output +
                                "' already exists, but is not recognized "
                                "as a valid GDAL dataset. Please manually "
                                "delete it before retrying");
                fail = true;
            }
            else if (!psSameFileAs(input, output))
                overwriteDeleteFileset(output);
        }
    }

    if (ow && append)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "pansharpen: Argument 'append' is mutually exclusive "
                    "with 'overwrite'.");
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

int rasterPansharpenHandler(const CmdSpec &, ParseResult &r)
{
    bool prefixWasEmpty = g_pipelineStepPrefix.empty();
    if (prefixWasEmpty)
        g_pipelineStepPrefix = "pansharpen";
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
    std::string method = strToLower(r.str("resampling", "cubic"));
    bool rSet = r.get("resampling") != nullptr;

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

    std::vector<SpectralTok> toks;
    for (const auto &t : r.list("spectral"))
        toks.push_back(parseSpectralTok(t));

    std::vector<std::string> weightVals = r.list("weights");
    std::vector<double> weights;
    for (const auto &w : weightVals)
        weights.push_back(strtod(w.c_str(), nullptr));

    std::string adjust = r.str("spatial-extent-adjustment", "union");
    bool useNodata = r.get("nodata") != nullptr;
    double ndVal = useNodata ? strtod(r.str("nodata").c_str(), nullptr) : 0;

    std::string extra;
    if (rSet && method != "cubic")
        extra += " --resampling " + method;
    if (!weightVals.empty())
    {
        extra += " --weights ";
        for (size_t i = 0; i < weights.size(); ++i)
        {
            if (i)
                extra += ",";
            extra += fmt17(weights[i]);
        }
    }
    if (useNodata)
        extra += " --nodata " + fmt17(ndVal);
    if (r.get("bit-depth"))
        extra += strPrintf(" --bit-depth %lld",
                           atoll(r.str("bit-depth").c_str()));
    if (r.get("spatial-extent-adjustment") && adjust != "union")
        extra += " --spatial-extent-adjustment " + adjust;
    if (r.get("num-threads") &&
        !strEqualNoCase(r.str("num-threads"), "ALL_CPUS"))
        extra += " --num-threads " + r.str("num-threads");

    std::string inputEcho = input;
    for (const auto &t : toks)
    {
        inputEcho += " --spectral ";
        if (t.raw.find(',') != std::string::npos)
            inputEcho += "\"" + t.raw + "\"";
        else
            inputEcho += t.raw;
    }

    // ---- GDALG serialization: no dataset opening or run-stage checks ----
    std::string dg = drv;
    if (dg.empty() &&
        strEndsWith(strToLower(output), ".gdalg.json"))
        dg = "GDALG";
    if (dg == "GDALG")
    {
        struct stat st;
        if (stat(output.c_str(), &st) != 0 &&
            output.compare(0, 4, "/vsi") != 0)
        {
            FILE *f = fopen(output.c_str(), "wb");
            if (!f)
            {
                cplErrorStr(CE_Failure, CPLE_NoWriteAccess,
                            "File " + output +
                                " cannot be opened for writing");
                return 1;
            }
            fclose(f);
            remove(output.c_str());
        }
        std::unique_ptr<RasterDatasetBase> base =
            std::make_unique<GdalgStubDataset>();
        auto noop = [](std::unique_ptr<RasterDatasetBase> &) -> int
        { return 0; };
        return rasterConvertWriteOutput(base, r, inputEcho, output, true,
                                        overwrite, append, drv, extra,
                                        noop, nullptr);
    }

    std::string err;
    auto pan = openRaster(input, err, oo);
    if (!pan)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    if (pan->bands.size() != 1)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "pansharpen: Input panchromatic dataset must have a "
                    "single band");
        return 1;
    }

    // spectral datasets open in the run phase: their failures skip the
    // usage block
    auto sds = std::make_shared<
        std::vector<std::unique_ptr<RasterDatasetBase>>>();
    std::vector<PansharpenDataset::Ref> refs;
    for (size_t i = 0; i < toks.size(); ++i)
    {
        struct stat sb;
        if (stat(toks[i].path.c_str(), &sb) != 0 &&
            toks[i].path.rfind("GTIFF_DIR:", 0) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(toks[i].path));
            return 1;
        }
        auto ds = openRaster(toks[i].path, err, oo);
        if (!ds)
        {
            if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + toks[i].path +
                                "' not recognized as being in a "
                                "supported file format.");
            return 1;
        }
        int nb = (int)ds->bands.size();
        if (toks[i].band > 0)
        {
            if (toks[i].band > nb)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("pansharpen: Invalid band number "
                                      "%d for dataset %s",
                                      toks[i].band,
                                      toks[i].path.c_str()));
                return 1;
            }
            refs.push_back({(int)sds->size(), toks[i].band});
        }
        else
            for (int b = 1; b <= nb; ++b)
                refs.push_back({(int)sds->size(), b});
        sds->push_back(std::move(ds));
    }

    if (!weights.empty() && weights.size() != refs.size())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%d weights defined, but %d input spectral "
                              "bands",
                              (int)weights.size(), (int)refs.size()));
        return 1;
    }
    if (weights.empty())
        weights.assign(refs.size(), 1.0 / (double)refs.size());

    std::string panWkt = pan->hasSrs ? pan->srs.wkt1Gdal() : std::string();
    for (const auto &ref : refs)
    {
        RasterDatasetBase *ds = (*sds)[ref.dsIdx].get();
        std::string sWkt = ds->hasSrs ? ds->srs.wkt1Gdal() : std::string();
        if (panWkt != sWkt)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Pan dataset and " + toks[ref.dsIdx].path +
                            " do not seem to have same projection. "
                            "Results might be incorrect");
    }

    DType outDt = (*sds)[refs[0].dsIdx]->bands[refs[0].band - 1].type;
    KMeta outMeta = kmetaFor(outDt, false, 0);
    double clampMax = outMeta.clampHi;
    int outNbits = 0;
    if (r.get("bit-depth"))
    {
        long long bd = atoll(r.str("bit-depth").c_str());
        int dtBits = dtypeSizeBytes(outDt) * 8;
        if (bd > dtBits)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Invalid value nBitDepth = %d for type "
                                  "%s",
                                  (int)bd, dtypeName(outDt)));
            return 1;
        }
        if (bd >= 1 && bd < 63)
            clampMax = (double)((1LL << bd) - 1);
        if (bd >= 1 && bd <= dtBits)
            outNbits = (int)bd;
    }

    // output grid at the pan resolution, per the extent adjustment mode
    double pgt[6];
    memcpy(pgt, pan->gt, sizeof pgt);
    double dx = pgt[1], dy = pgt[5];
    double ex0 = pgt[0], etop = pgt[3];
    double ex1 = ex0 + pan->width * dx;
    double ebot = etop + pan->height * dy;
    if (adjust == "union" || adjust == "intersection")
    {
        for (const auto &dsp : *sds)
        {
            double sx0 = dsp->gt[0], stop = dsp->gt[3];
            double sx1 = sx0 + dsp->width * dsp->gt[1];
            double sbot = stop + dsp->height * dsp->gt[5];
            if (adjust == "union")
            {
                ex0 = std::min(ex0, sx0);
                ex1 = std::max(ex1, sx1);
                etop = std::max(etop, stop);
                ebot = std::min(ebot, sbot);
            }
            else
            {
                ex0 = std::max(ex0, sx0);
                ex1 = std::min(ex1, sx1);
                etop = std::min(etop, stop);
                ebot = std::max(ebot, sbot);
            }
        }
    }
    int outW = (int)std::floor((ex1 - ex0) / dx + 0.5);
    int outH = (int)std::floor((ebot - etop) / dy + 0.5);
    if (outW <= 0 || outH <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "pansharpen: Datasets do not intersect");
        return 1;
    }
    double ogt[6] = {ex0, dx, pgt[2], etop, pgt[4], dy};

    Win panWin;
    panWin.x0 = (int)std::floor((ex0 - pgt[0]) / dx + 0.5);
    panWin.y0 = (int)std::floor((etop - pgt[3]) / dy + 0.5);
    panWin.w = outW;
    panWin.h = outH;
    std::vector<Win> msWins;
    for (const auto &dsp : *sds)
    {
        Win w;
        if (adjust == "none" || adjust == "none-without-warning")
        {
            w.x0 = 0;
            w.y0 = 0;
            w.w = dsp->width;
            w.h = dsp->height;
        }
        else
        {
            double sdx = dsp->gt[1], sdy = dsp->gt[5];
            w.x0 = (int)std::floor((ex0 - dsp->gt[0]) / sdx + 0.5);
            w.y0 = (int)std::floor((etop - dsp->gt[3]) / sdy + 0.5);
            w.w = (int)std::floor((ex1 - ex0) / sdx + 0.5);
            w.h = (int)std::floor((ebot - etop) / sdy + 0.5);
            if (w.w <= 0)
                w.w = 1;
            if (w.h <= 0)
                w.h = 1;
        }
        msWins.push_back(w);
    }

    auto materialize =
        [sds, refs, method, weights, useNodata, ndVal, clampMax, outMeta,
         outW, outH, ogt, panWin, msWins,
         outNbits](std::unique_ptr<RasterDatasetBase> &d) -> int {
        auto ps = std::make_unique<PansharpenDataset>(
            std::move(d), std::move(*sds), refs, outW, outH, ogt);
        ps->method = method;
        ps->weights = weights;
        ps->useNodata = useNodata;
        ps->ndVal = ndVal;
        ps->clampMax = clampMax;
        ps->nbits = outNbits;
        ps->outIsInt = outMeta.isInt;
        ps->panWin = panWin;
        ps->msWins = msWins;
        d = std::move(ps);
        return 0;
    };

    return rasterConvertWriteOutput(pan, r, inputEcho, output, quiet,
                                    overwrite, append, drv, extra,
                                    materialize);
}

struct Reg
{
    Reg()
    {
        registerHandler("raster_pansharpen", rasterPansharpenHandler);
        registerPreValidator("raster_pansharpen", pansharpenPreValidator);
        registerArgCheck("raster_pansharpen", pansharpenArgCheck);
    }
};

}  // namespace

void registerRasterPansharpenHandler()
{
    static Reg reg;
}
