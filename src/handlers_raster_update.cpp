#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "ogr.h"
#include "srs.h"
#include "tiff.h"
#include "util.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

// ------------------------------------------------------------------
// raw little-endian classic-TIFF helpers (in-place strip/tile pixel
// patching plus the reference's rewrite-directory-to-EOF behavior)
// ------------------------------------------------------------------

bool updLoadWhole(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)sz);
    size_t rd = sz ? fread(out.data(), 1, (size_t)sz, f) : 0;
    fclose(f);
    return rd == (size_t)sz;
}

bool updSaveWhole(const std::string &path, const std::vector<uint8_t> &d)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    size_t wr = d.empty() ? 0 : fwrite(d.data(), 1, d.size(), f);
    fclose(f);
    return wr == d.size();
}

uint16_t rd16(const std::vector<uint8_t> &d, size_t o)
{
    return (uint16_t)(d[o] | (d[o + 1] << 8));
}

uint32_t rd32(const std::vector<uint8_t> &d, size_t o)
{
    return (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) |
                      ((uint32_t)d[o + 3] << 24));
}

void wr32(std::vector<uint8_t> &d, size_t o, uint32_t v)
{
    d[o] = (uint8_t)(v & 0xff);
    d[o + 1] = (uint8_t)((v >> 8) & 0xff);
    d[o + 2] = (uint8_t)((v >> 16) & 0xff);
    d[o + 3] = (uint8_t)((v >> 24) & 0xff);
}

int tiffTypeSize(int t)
{
    switch (t)
    {
        case 1:
        case 2:
        case 6:
        case 7:
            return 1;
        case 3:
        case 8:
            return 2;
        case 4:
        case 9:
        case 11:
        case 13:
            return 4;
        case 5:
        case 10:
        case 12:
        case 16:
        case 17:
            return 8;
        default:
            return 1;
    }
}

struct RawEntry
{
    uint16_t id = 0, type = 0;
    uint32_t count = 0, val = 0;
};

struct RawIfd
{
    uint32_t offset = 0;
    uint32_t next = 0;
    std::vector<RawEntry> entries;

    const RawEntry *find(uint16_t id) const
    {
        for (const auto &e : entries)
            if (e.id == id)
                return &e;
        return nullptr;
    }
};

bool parseRawIfd(const std::vector<uint8_t> &d, uint32_t off, RawIfd &out)
{
    if (off + 2 > d.size())
        return false;
    out.offset = off;
    uint16_t n = rd16(d, off);
    if (off + 2 + (size_t)n * 12 + 4 > d.size())
        return false;
    out.entries.clear();
    for (int i = 0; i < n; ++i)
    {
        size_t e = off + 2 + (size_t)i * 12;
        RawEntry re;
        re.id = rd16(d, e);
        re.type = rd16(d, e + 2);
        re.count = rd32(d, e + 4);
        re.val = rd32(d, e + 8);
        out.entries.push_back(re);
    }
    out.next = rd32(d, off + 2 + (size_t)n * 12);
    return true;
}

// values of an integer-array tag (SHORT or LONG), inline or remote
std::vector<uint64_t> tagIntValues(const std::vector<uint8_t> &d,
                                   const RawEntry &e)
{
    std::vector<uint64_t> out;
    int ts = tiffTypeSize(e.type);
    size_t total = (size_t)ts * e.count;
    uint8_t inlineBuf[4];
    const uint8_t *p;
    if (total <= 4)
    {
        inlineBuf[0] = (uint8_t)(e.val & 0xff);
        inlineBuf[1] = (uint8_t)((e.val >> 8) & 0xff);
        inlineBuf[2] = (uint8_t)((e.val >> 16) & 0xff);
        inlineBuf[3] = (uint8_t)((e.val >> 24) & 0xff);
        p = inlineBuf;
    }
    else
    {
        if ((size_t)e.val + total > d.size())
            return out;
        p = d.data() + e.val;
    }
    for (uint32_t i = 0; i < e.count; ++i)
    {
        if (ts == 2)
            out.push_back((uint64_t)(p[i * 2] | (p[i * 2 + 1] << 8)));
        else
            out.push_back((uint64_t)(p[i * 4] | (p[i * 4 + 1] << 8) |
                                     (p[i * 4 + 2] << 16) |
                                     ((uint32_t)p[i * 4 + 3] << 24)));
    }
    return out;
}

// pixel geometry of one IFD; contiguous planar config only
struct RawGrid
{
    int w = 0, h = 0, spp = 1, bpe = 1;
    bool tiled = false;
    int tw = 0, th = 0;         // tile dims
    int rps = 0;                // rows per strip
    std::vector<uint64_t> offs;  // strip/tile offsets

    bool sampleOffset(int x, int y, int band, size_t &fileOff) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;
        if (tiled)
        {
            int across = (w + tw - 1) / tw;
            int ti = (y / th) * across + (x / tw);
            if (ti >= (int)offs.size())
                return false;
            size_t inTile =
                ((size_t)(y % th) * tw + (x % tw)) * spp + band;
            fileOff = (size_t)offs[ti] + inTile * bpe;
            return true;
        }
        int si = y / rps;
        if (si >= (int)offs.size())
            return false;
        size_t inStrip = ((size_t)(y % rps) * w + x) * spp + band;
        fileOff = (size_t)offs[si] + inStrip * bpe;
        return true;
    }
};

bool gridFromIfd(const std::vector<uint8_t> &d, const RawIfd &ifd,
                 RawGrid &g)
{
    const RawEntry *ew = ifd.find(256), *eh = ifd.find(257);
    if (!ew || !eh)
        return false;
    g.w = (int)ew->val;
    g.h = (int)eh->val;
    const RawEntry *comp = ifd.find(259);
    if (comp && comp->val != 1)
        return false;
    const RawEntry *pc = ifd.find(284);
    if (pc && pc->val != 1)
        return false;
    const RawEntry *spp = ifd.find(277);
    g.spp = spp ? (int)spp->val : 1;
    const RawEntry *bps = ifd.find(258);
    int bits = 8;
    if (bps)
    {
        if ((size_t)tiffTypeSize(bps->type) * bps->count <= 4)
            bits = (int)(bps->val & 0xffff);
        else
        {
            std::vector<uint64_t> v = tagIntValues(d, *bps);
            bits = v.empty() ? 8 : (int)v[0];
        }
    }
    if (bits % 8)
        return false;
    g.bpe = bits / 8;
    if (ifd.find(324))
    {
        g.tiled = true;
        const RawEntry *tw = ifd.find(322), *th = ifd.find(323);
        if (!tw || !th)
            return false;
        g.tw = (int)tw->val;
        g.th = (int)th->val;
        g.offs = tagIntValues(d, *ifd.find(324));
        return !g.offs.empty();
    }
    const RawEntry *so = ifd.find(273);
    if (!so)
        return false;
    const RawEntry *rps = ifd.find(278);
    g.rps = rps ? (int)rps->val : g.h;
    if (g.rps <= 0)
        g.rps = g.h;
    g.offs = tagIntValues(d, *so);
    return !g.offs.empty();
}

// the reference flushes a dirtied dataset by rewriting the whole main
// directory at EOF (word-aligned), remote payloads re-emitted after the
// entries in tag order, header pointer repointed; overview directories
// and the old bytes stay where they were
void relocateFirstIfd(std::vector<uint8_t> &d)
{
    uint32_t first = rd32(d, 4);
    RawIfd ifd;
    if (!parseRawIfd(d, first, ifd))
        return;
    if (d.size() & 1)
        d.push_back(0);
    uint32_t newOff = (uint32_t)d.size();
    uint16_t n = (uint16_t)ifd.entries.size();
    d.push_back((uint8_t)(n & 0xff));
    d.push_back((uint8_t)(n >> 8));
    size_t entriesStart = d.size();
    for (const auto &e : ifd.entries)
    {
        d.push_back((uint8_t)(e.id & 0xff));
        d.push_back((uint8_t)(e.id >> 8));
        d.push_back((uint8_t)(e.type & 0xff));
        d.push_back((uint8_t)(e.type >> 8));
        size_t p = d.size();
        d.resize(p + 8);
        wr32(d, p, e.count);
        wr32(d, p + 4, e.val);
    }
    size_t nextPos = d.size();
    d.resize(nextPos + 4);
    wr32(d, nextPos, ifd.next);
    for (size_t i = 0; i < ifd.entries.size(); ++i)
    {
        const RawEntry &e = ifd.entries[i];
        size_t total = (size_t)tiffTypeSize(e.type) * e.count;
        if (total <= 4)
            continue;
        uint32_t payloadOff = (uint32_t)d.size();
        if ((size_t)e.val + total <= d.size())
            d.insert(d.end(), d.begin() + e.val,
                     d.begin() + e.val + total);
        else
            d.insert(d.end(), total, 0);
        wr32(d, entriesStart + i * 12 + 8, payloadOff);
    }
    wr32(d, 4, newOff);
}

// ------------------------------------------------------------------
// clipping geometry containment (even-odd rule over polygon rings)
// ------------------------------------------------------------------

bool ringCrossings(const OgrGeometry &ring, double x, double y, int &cnt)
{
    size_t n = ring.coords.size() / 3;
    if (n < 3)
        return true;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        double xi = ring.coords[i * 3], yi = ring.coords[i * 3 + 1];
        double xj = ring.coords[j * 3], yj = ring.coords[j * 3 + 1];
        if ((yi > y) != (yj > y) &&
            x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            ++cnt;
    }
    return true;
}

bool geomContainsPoint(const OgrGeometry &g, double x, double y)
{
    if (g.type == 6)
    {
        for (const auto &part : g.parts)
            if (geomContainsPoint(part, x, y))
                return true;
        return false;
    }
    int cnt = 0;
    for (const auto &ring : g.parts)
        ringCrossings(ring, x, y, cnt);
    return (cnt & 1) != 0;
}

// ------------------------------------------------------------------
// value store rounding (integer destinations round half away from zero
// and clamp to the type range)
// ------------------------------------------------------------------

double updRound(DType t, double v)
{
    double lo, hi;
    switch (t)
    {
        case DType::Byte: lo = 0; hi = 255; break;
        case DType::Int8: lo = -128; hi = 127; break;
        case DType::UInt16: lo = 0; hi = 65535; break;
        case DType::Int16: lo = -32768; hi = 32767; break;
        case DType::UInt32: lo = 0; hi = 4294967295.0; break;
        case DType::Int32: lo = -2147483648.0; hi = 2147483647.0; break;
        default:
            return v;
    }
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v < 0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5);
}

void updEncode(DType t, std::vector<uint8_t> &d, size_t off, double v)
{
    uint8_t buf[16];
    rasterEncodeReal(t, buf, v, 0.0);
    int sz = dtypeSizeBytes(t);
    if (off + sz <= d.size())
        memcpy(&d[off], buf, sz);
}

double updDecode(DType t, const std::vector<uint8_t> &d, size_t off)
{
    size_t sz = (size_t)dtypeSizeBytes(t);
    if (off + sz > d.size())
        return 0.0;
    switch (t)
    {
        case DType::Byte:
            return d[off];
        case DType::Int8:
            return (int8_t)d[off];
        case DType::UInt16:
            return rd16(d, off);
        case DType::Int16:
            return (int16_t)rd16(d, off);
        case DType::UInt32:
            return rd32(d, off);
        case DType::Int32:
            return (int32_t)rd32(d, off);
        case DType::Float32:
        {
            uint32_t u = rd32(d, off);
            float f;
            memcpy(&f, &u, 4);
            return f;
        }
        case DType::Float64:
        {
            uint64_t u = rd32(d, off) |
                         ((uint64_t)rd32(d, off + 4) << 32);
            double v;
            memcpy(&v, &u, 8);
            return v;
        }
        case DType::Int64:
        {
            uint64_t u = rd32(d, off) |
                         ((uint64_t)rd32(d, off + 4) << 32);
            return (double)(int64_t)u;
        }
        case DType::UInt64:
        {
            uint64_t u = rd32(d, off) |
                         ((uint64_t)rd32(d, off + 4) << 32);
            return (double)u;
        }
        default:
            return 0.0;
    }
}

int updateError(int cls, const std::string &msg)
{
    cplErrorStr(CE_Failure, cls, msg);
    return 1;
}

// ------------------------------------------------------------------
// validation hooks
// ------------------------------------------------------------------

int updateArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName == "input-format")
    {
        for (const auto &d : r.list("input-format"))
        {
            std::string err = inputFormatCapError(false, d);
            if (!err.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "update: " + err);
                handlerPrintUsage();
                return 1;
            }
        }
    }
    else if (argName == "geometry-crs")
    {
        bool ok = false;
        cplPushQuietHandler();
        Srs s = Srs::fromCliInput(r.str("geometry-crs"), ok);
        cplPopHandler();
        if (!ok)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "update: Invalid value for 'geometry-crs' "
                        "argument");
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

int updatePreValidator(const CmdSpec &, ParseResult &r)
{
    bool fail = false;
    std::string input = r.str("input");
    std::string output = r.str("output");

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
    const ArgValue *outv = r.get("output");
    if (outv && !output.empty() && output.rfind("GTIFF_DIR:", 0) != 0)
    {
        struct stat sb;
        if (stat(output.c_str(), &sb) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(output));
            fail = true;
        }
        else if (!datasetIdentify(output, {"raster"}))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + output +
                            "' not recognized as being in a supported "
                            "file format.");
            fail = true;
        }
    }
    if (fail)
        handlerPrintUsage();
    return fail ? 1 : 0;
}

// ------------------------------------------------------------------
// overview resampling kernels (mirror of the overview generator: the
// update verb refreshes internal overviews with the chosen resampling,
// cascading each level from the previous one)
// ------------------------------------------------------------------

struct MainInfo
{
    DType dt = DType::Byte;
    bool isInt = true;
    double clampLo = 0, clampHi = 255;
    bool hasNodata = false;
    double nodata = 0;
};

MainInfo updMainInfo(const Band &b)
{
    MainInfo m;
    m.dt = b.type;
    m.isInt = true;
    switch (b.type)
    {
        case DType::Byte: m.clampLo = 0; m.clampHi = 255; break;
        case DType::Int8: m.clampLo = -128; m.clampHi = 127; break;
        case DType::UInt16: m.clampLo = 0; m.clampHi = 65535; break;
        case DType::Int16: m.clampLo = -32768; m.clampHi = 32767; break;
        case DType::UInt32: m.clampLo = 0; m.clampHi = 4294967295.0; break;
        case DType::Int32:
            m.clampLo = -2147483648.0;
            m.clampHi = 2147483647.0;
            break;
        default: m.isInt = false; break;
    }
    m.hasNodata = b.hasNodata;
    m.nodata = b.nodata;
    return m;
}

double roundStore(double v, const MainInfo &m)
{
    if (!m.isInt)
        return v;
    if (v < m.clampLo)
        v = m.clampLo;
    if (v > m.clampHi)
        v = m.clampHi;
    return v < 0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5);
}

typedef std::vector<std::vector<double>> BandGrid;  // per band, row-major

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
        // reference B-spline omits the /6 (normalization absorbs it)
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
double convSum(const double *vals, const double *ws, int n)
{
    double v1 = 0.0, v2 = 0.0;
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

void resampleBand(const std::vector<double> &src, int sw, int sh,
                  std::vector<double> &dst, int dw, int dh,
                  const std::string &method, const MainInfo &m)
{
    dst.assign((size_t)dw * dh, 0.0);
    double rx = (double)sw / dw, ry = (double)sh / dh;
    bool nd = m.hasNodata;
    double ndv = m.nodata;
    if ((m.dt == DType::Int16 || m.dt == DType::Int8) &&
        (method == "average" || method == "rms"))
    {
        // Int16/Int8 run the Float32 working pipeline, results are
        // float-quantized before the integer store rounding
        MainInfo mf = m;
        mf.dt = DType::Float32;
        mf.isInt = false;
        resampleBand(src, sw, sh, dst, dw, dh, method, mf);
        for (double &v : dst)
            if (!nd || v != ndv)
                v = roundStore((double)(float)v, m);
        return;
    }
    bool f32 = m.dt == DType::Float32;
    if (method == "nearest")
    {
        for (int y = 0; y < dh; ++y)
        {
            int sy = (int)(0.5 + y * ry);
            if (sy >= sh)
                sy = sh - 1;
            for (int x = 0; x < dw; ++x)
            {
                int sx = (int)(0.5 + x * rx);
                if (sx >= sw)
                    sx = sw - 1;
                dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
            }
        }
        return;
    }
    if (method == "average" || method == "rms")
    {
        bool sq = method == "rms";
        bool intRatio = rx == std::floor(rx) && ry == std::floor(ry);
        if (intRatio)
        {
            int fx = (int)rx, fy = (int)ry;
            if (f32 && !nd && fx == 2 && fy == 2)
            {
                // reference's SSE2 Float32 2x2 fast paths
                for (int y = 0; y < dh; ++y)
                {
                    const double *r0 = &src[(size_t)(2 * y) * sw];
                    const double *r1 = &src[(size_t)(2 * y + 1) * sw];
                    int x = 0;
                    if (sq)
                    {
                        // RMS: max-normalized float pipeline; vector
                        // batches pair rows, the scalar tail sums
                        // sequentially
                        int batchEnd = (dw / 4) * 4;
                        for (; x < dw; ++x)
                        {
                            float a0 = std::fabs((float)r0[2 * x]);
                            float a1 = std::fabs((float)r0[2 * x + 1]);
                            float a2 = std::fabs((float)r1[2 * x]);
                            float a3 = std::fabs((float)r1[2 * x + 1]);
                            float mx = std::max(std::max(a0, a1),
                                                std::max(a2, a3));
                            float inv = mx == 0.0f ? 0.0f : 1.0f / mx;
                            float n0 = a0 * inv, n1 = a1 * inv;
                            float n2 = a2 * inv, n3 = a3 * inv;
                            float sum =
                                x < batchEnd
                                    ? (n0 * n0 + n1 * n1) +
                                          (n2 * n2 + n3 * n3)
                                    : ((n0 * n0 + n1 * n1) + n2 * n2) +
                                          n3 * n3;
                            dst[(size_t)y * dw + x] =
                                mx * std::sqrt(sum * 0.25f);
                        }
                    }
                    else
                    {
                        // average: vector groups of 4 sum columns first,
                        // scalar tail sums row-major
                        int batchEnd = (dw / 4) * 4;
                        for (; x < batchEnd; ++x)
                        {
                            float v0 = (float)r0[2 * x];
                            float v1 = (float)r0[2 * x + 1];
                            float v2 = (float)r1[2 * x];
                            float v3 = (float)r1[2 * x + 1];
                            float s = (v0 + v2) + (v1 + v3);
                            dst[(size_t)y * dw + x] = s * 0.25f;
                        }
                        for (; x < dw; ++x)
                        {
                            float v0 = (float)r0[2 * x];
                            float v1 = (float)r0[2 * x + 1];
                            float v2 = (float)r1[2 * x];
                            float v3 = (float)r1[2 * x + 1];
                            float tot = ((v0 + v1) + v2) + v3;
                            dst[(size_t)y * dw + x] = tot / 4.0f;
                        }
                    }
                }
                return;
            }
            if (m.dt == DType::Float64 && !nd && !sq && fx == 2 &&
                fy == 2)
            {
                // SSE2 double pipeline sums column pairs
                for (int y = 0; y < dh; ++y)
                {
                    const double *r0 = &src[(size_t)(2 * y) * sw];
                    const double *r1 = &src[(size_t)(2 * y + 1) * sw];
                    for (int x = 0; x < dw; ++x)
                        dst[(size_t)y * dw + x] =
                            ((r0[2 * x] + r1[2 * x]) +
                             (r0[2 * x + 1] + r1[2 * x + 1])) *
                            0.25;
                }
                return;
            }
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x)
                {
                    if (f32 && !nd)
                    {
                        double tot = 0.0;
                        for (int sy = y * fy; sy < (y + 1) * fy; ++sy)
                            for (int sx = x * fx; sx < (x + 1) * fx;
                                 ++sx)
                            {
                                double v = src[(size_t)sy * sw + sx];
                                tot += sq ? v * v : v;
                            }
                        double r = tot / (fx * fy);
                        dst[(size_t)y * dw + x] =
                            (float)(sq ? std::sqrt(r) : r);
                    }
                    else if (!f32 && !nd && sq &&
                             m.dt == DType::Float64)
                    {
                        // max-normalized RMS, sequential accumulation
                        double mx = 0.0;
                        for (int sy = y * fy; sy < (y + 1) * fy; ++sy)
                            for (int sx = x * fx; sx < (x + 1) * fx;
                                 ++sx)
                                mx = std::max(
                                    mx,
                                    std::fabs(src[(size_t)sy * sw + sx]));
                        double inv = mx == 0.0 ? 0.0 : 1.0 / mx;
                        double s = 0.0;
                        for (int sy = y * fy; sy < (y + 1) * fy; ++sy)
                            for (int sx = x * fx; sx < (x + 1) * fx;
                                 ++sx)
                            {
                                double nv =
                                    std::fabs(src[(size_t)sy * sw + sx]) *
                                    inv;
                                s += nv * nv;
                            }
                        dst[(size_t)y * dw + x] =
                            mx * std::sqrt(s / (fx * fy));
                    }
                    else if (sq && (m.dt == DType::Byte ||
                                    m.dt == DType::UInt16))
                    {
                        // reference integer RMS: truncated integer
                        // quotient, then rounded integer square root;
                        // the no-nodata 2x2 vector path rounds on the
                        // exact quotient instead
                        uint64_t ssum = 0;
                        int cnt = 0;
                        for (int sy = y * fy; sy < (y + 1) * fy; ++sy)
                            for (int sx = x * fx; sx < (x + 1) * fx;
                                 ++sx)
                            {
                                double v = src[(size_t)sy * sw + sx];
                                if (nd && v == ndv)
                                    continue;
                                uint64_t iv = (uint64_t)v;
                                ssum += iv * iv;
                                cnt++;
                            }
                        double out;
                        if (!cnt)
                            out = nd ? ndv : 0.0;
                        else if (!nd && fx == 2 && fy == 2)
                        {
                            // 2x2 vector path: round up only past the
                            // squared midpoint plus 2
                            uint64_t q = ssum / 4;
                            uint64_t c = (uint64_t)std::sqrt((double)q);
                            while (c > 0 && c * c > q)
                                --c;
                            while ((c + 1) * (c + 1) <= q)
                                ++c;
                            if (ssum >= 4 * c * (c + 1) + 3)
                                ++c;
                            out = (double)c;
                        }
                        else
                        {
                            uint64_t q = ssum / (uint64_t)cnt;
                            uint64_t c = (uint64_t)std::sqrt((double)q);
                            while (c > 0 && c * c > q)
                                --c;
                            while ((c + 1) * (c + 1) <= q)
                                ++c;
                            if (q - c * c > c)
                                ++c;
                            out = (double)c;
                        }
                        dst[(size_t)y * dw + x] = out;
                    }
                    else
                    {
                        double tot = 0.0;
                        int cnt = 0;
                        for (int sy = y * fy; sy < (y + 1) * fy; ++sy)
                            for (int sx = x * fx; sx < (x + 1) * fx;
                                 ++sx)
                            {
                                double v = src[(size_t)sy * sw + sx];
                                if (nd && v == ndv)
                                    continue;
                                tot += sq ? v * v : v;
                                cnt++;
                            }
                        double out;
                        if (!cnt)
                            out = nd ? ndv : 0.0;
                        else
                        {
                            out = tot / cnt;
                            if (sq)
                                out = std::sqrt(out);
                            out = roundStore(out, m);
                        }
                        dst[(size_t)y * dw + x] = out;
                    }
                }
            return;
        }
        for (int y = 0; y < dh; ++y)
        {
            double ay = y * ry, by = (y + 1) * ry;
            if (by > sh)
                by = sh;
            int y0 = (int)std::floor(ay), y1 = (int)std::ceil(by);
            // the reference's x-ratio-2 specialization drops fractional
            // row weights whenever the row window spans exactly two
            // source rows (only the first/last rows of a 2x cascade with
            // odd source height qualify)
            if (rx == 2.0 && y1 - y0 == 2 && y1 <= sh)
            {
                const double *r0 = &src[(size_t)y0 * sw];
                const double *r1 = &src[(size_t)(y0 + 1) * sw];
                int batchEnd = (dw / 4) * 4;
                for (int x = 0; x < dw; ++x)
                {
                    double v0 = r0[2 * x], v1 = r0[2 * x + 1];
                    double v2 = r1[2 * x], v3 = r1[2 * x + 1];
                    double out;
                    if (f32 && !nd)
                    {
                        if (sq)
                        {
                            float a0 = std::fabs((float)v0);
                            float a1 = std::fabs((float)v1);
                            float a2 = std::fabs((float)v2);
                            float a3 = std::fabs((float)v3);
                            float mx = std::max(std::max(a0, a1),
                                                std::max(a2, a3));
                            float inv = mx == 0.0f ? 0.0f : 1.0f / mx;
                            float n0 = a0 * inv, n1 = a1 * inv;
                            float n2 = a2 * inv, n3 = a3 * inv;
                            float sum =
                                x < batchEnd
                                    ? (n0 * n0 + n1 * n1) +
                                          (n2 * n2 + n3 * n3)
                                    : ((n0 * n0 + n1 * n1) + n2 * n2) +
                                          n3 * n3;
                            out = mx * std::sqrt(sum * 0.25f);
                        }
                        else if (x < batchEnd)
                            out = (float)(((float)v0 + (float)v2) +
                                          ((float)v1 + (float)v3)) *
                                  0.25f;
                        else
                            out = ((((float)v0 + (float)v1) + (float)v2) +
                                   (float)v3) /
                                  4.0f;
                    }
                    else if (!f32 && !nd && sq && m.dt == DType::Float64)
                    {
                        double mx = std::max(
                            std::max(std::fabs(v0), std::fabs(v1)),
                            std::max(std::fabs(v2), std::fabs(v3)));
                        double inv = mx == 0.0 ? 0.0 : 1.0 / mx;
                        double s = 0.0;
                        for (double v : {v0, v1, v2, v3})
                        {
                            double nv = std::fabs(v) * inv;
                            s += nv * nv;
                        }
                        out = mx * std::sqrt(s / 4.0);
                    }
                    else if (sq &&
                             (m.dt == DType::Byte || m.dt == DType::UInt16))
                    {
                        uint64_t ssum = 0;
                        int cnt = 0;
                        for (double v : {v0, v1, v2, v3})
                        {
                            if (nd && v == ndv)
                                continue;
                            uint64_t iv = (uint64_t)v;
                            ssum += iv * iv;
                            cnt++;
                        }
                        if (!cnt)
                            out = nd ? ndv : 0.0;
                        else if (!nd)
                        {
                            uint64_t q = ssum / 4;
                            uint64_t c = (uint64_t)std::sqrt((double)q);
                            while (c > 0 && c * c > q)
                                --c;
                            while ((c + 1) * (c + 1) <= q)
                                ++c;
                            if (ssum >= 4 * c * (c + 1) + 3)
                                ++c;
                            out = (double)c;
                        }
                        else
                        {
                            uint64_t q = ssum / (uint64_t)cnt;
                            uint64_t c = (uint64_t)std::sqrt((double)q);
                            while (c > 0 && c * c > q)
                                --c;
                            while ((c + 1) * (c + 1) <= q)
                                ++c;
                            if (q - c * c > c)
                                ++c;
                            out = (double)c;
                        }
                    }
                    else if (m.dt == DType::Float64 && !nd && !sq)
                        out = ((v0 + v2) + (v1 + v3)) * 0.25;
                    else
                    {
                        double tot = 0.0;
                        int cnt = 0;
                        for (double v : {v0, v1, v2, v3})
                        {
                            if (nd && v == ndv)
                                continue;
                            tot += sq ? v * v : v;
                            cnt++;
                        }
                        if (!cnt)
                            out = nd ? ndv : 0.0;
                        else
                        {
                            out = tot / cnt;
                            if (sq)
                                out = std::sqrt(out);
                            out = roundStore(out, m);
                        }
                    }
                    dst[(size_t)y * dw + x] = out;
                }
                continue;
            }
            for (int x = 0; x < dw; ++x)
            {
                double ax = x * rx, bx = (x + 1) * rx;
                if (bx > sw)
                    bx = sw;
                int x0 = (int)std::floor(ax), x1 = (int)std::ceil(bx);
                double tot = 0, wsum = 0;
                if (!nd && sq && m.dt == DType::Float64)
                {
                    // max-normalized RMS over the fractional window,
                    // same per-row accumulation as the average chain
                    int nC = std::min(x1, sw) - x0;
                    double wl = std::min((double)x0 + 1, bx) -
                                std::max((double)x0, ax);
                    double wr =
                        nC > 1 ? bx - (double)(x0 + nC - 1) : 0.0;
                    double fullline = wl;
                    for (int c = 1; c < nC - 1; ++c)
                        fullline += 1.0;
                    if (nC > 1)
                        fullline += wr;
                    double mx = 0.0;
                    for (int sy = y0; sy < y1 && sy < sh; ++sy)
                    {
                        const double *row = &src[(size_t)sy * sw];
                        for (int c = 0; c < nC; ++c)
                            mx = std::max(mx, std::fabs(row[x0 + c]));
                    }
                    double inv = mx == 0.0 ? 0.0 : 1.0 / mx;
                    for (int sy = y0; sy < y1 && sy < sh; ++sy)
                    {
                        double wy = std::min((double)sy + 1, by) -
                                    std::max((double)sy, ay);
                        if (wy <= 0)
                            continue;
                        const double *row = &src[(size_t)sy * sw];
                        double n0 = std::fabs(row[x0]) * inv;
                        double rowsum = (n0 * n0) * wl;
                        for (int c = 1; c < nC - 1; ++c)
                        {
                            double nv = std::fabs(row[x0 + c]) * inv;
                            rowsum += nv * nv;
                        }
                        if (nC > 1)
                        {
                            double nv =
                                std::fabs(row[x0 + nC - 1]) * inv;
                            rowsum += (nv * nv) * wr;
                        }
                        tot += rowsum * wy;
                        wsum += fullline * wy;
                    }
                    dst[(size_t)y * dw + x] =
                        mx * std::sqrt(tot / wsum);
                    continue;
                }
                if (!nd)
                {
                    // reference accumulation: per-row sum with edge x
                    // weights, then one multiply by the row weight; the
                    // weight sum uses the precomputed full-line weight
                    int nC = std::min(x1, sw) - x0;
                    double wl = std::min((double)x0 + 1, bx) -
                                std::max((double)x0, ax);
                    double wr =
                        nC > 1 ? bx - (double)(x0 + nC - 1) : 0.0;
                    double fullline = wl;
                    for (int c = 1; c < nC - 1; ++c)
                        fullline += 1.0;
                    if (nC > 1)
                        fullline += wr;
                    for (int sy = y0; sy < y1 && sy < sh; ++sy)
                    {
                        double wy = std::min((double)sy + 1, by) -
                                    std::max((double)sy, ay);
                        if (wy <= 0)
                            continue;
                        const double *row = &src[(size_t)sy * sw];
                        double v0 = row[x0];
                        double rowsum = (sq ? v0 * v0 : v0) * wl;
                        for (int c = 1; c < nC - 1; ++c)
                        {
                            double v = row[x0 + c];
                            rowsum += sq ? v * v : v;
                        }
                        if (nC > 1)
                        {
                            double v = row[x0 + nC - 1];
                            rowsum += (sq ? v * v : v) * wr;
                        }
                        tot += rowsum * wy;
                        wsum += fullline * wy;
                    }
                }
                else
                    for (int sy = y0; sy < y1 && sy < sh; ++sy)
                    {
                        double wy = std::min((double)sy + 1, by) -
                                    std::max((double)sy, ay);
                        if (wy <= 0)
                            continue;
                        for (int sx = x0; sx < x1 && sx < sw; ++sx)
                        {
                            double wx = std::min((double)sx + 1, bx) -
                                        std::max((double)sx, ax);
                            if (wx <= 0)
                                continue;
                            double v = src[(size_t)sy * sw + sx];
                            if (nd && v == ndv)
                                continue;
                            double w = wx * wy;
                            tot += w * (sq ? v * v : v);
                            wsum += w;
                        }
                    }
                double out;
                if (wsum <= 0)
                    out = nd ? ndv : 0.0;
                else if (!nd && sq &&
                         (m.dt == DType::Byte || m.dt == DType::UInt16))
                {
                    // integer RMS on the weighted mean: round up only
                    // past c*(c+1)+0.5
                    double q = tot / wsum;
                    uint64_t c = (uint64_t)std::sqrt(q);
                    while (c > 0 && (double)(c * c) > q)
                        --c;
                    while ((double)((c + 1) * (c + 1)) <= q)
                        ++c;
                    if (q > (double)(c * (c + 1)) + 0.5)
                        ++c;
                    out = (double)c;
                }
                else
                {
                    out = tot / wsum;
                    if (sq)
                        out = std::sqrt(out);
                    out = roundStore(out, m);
                }
                dst[(size_t)y * dw + x] = out;
            }
        }
        return;
    }
    if (method == "mode")
    {
        std::vector<std::pair<double, int>> counts;
        for (int y = 0; y < dh; ++y)
        {
            int y0 = (int)(y * ry), y1 = (int)((y + 1) * ry);
            if (y1 <= y0)
                y1 = y0 + 1;
            if (y1 > sh)
                y1 = sh;
            for (int x = 0; x < dw; ++x)
            {
                int x0 = (int)(x * rx), x1 = (int)((x + 1) * rx);
                if (x1 <= x0)
                    x1 = x0 + 1;
                if (x1 > sw)
                    x1 = sw;
                counts.clear();
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                    {
                        double v = src[(size_t)sy * sw + sx];
                        if (nd && v == ndv)
                            continue;
                        bool found = false;
                        for (auto &c : counts)
                            if (c.first == v)
                            {
                                c.second++;
                                found = true;
                                break;
                            }
                        if (!found)
                            counts.emplace_back(v, 1);
                    }
                double best = nd ? ndv : 0.0;
                int bestN = 0;
                for (const auto &c : counts)
                    if (c.second > bestN)
                    {
                        bestN = c.second;
                        best = c.first;
                    }
                dst[(size_t)y * dw + x] = best;
            }
        }
        return;
    }
    if (method == "gauss")
    {
        static const int g3[3] = {1, 2, 1};
        static const int g5[5] = {1, 4, 6, 4, 1};
        static const int g7[7] = {1, 6, 15, 20, 15, 6, 1};
        int fac = (int)(0.5 + rx);
        int dim = 3;
        const int *gw = g3;
        if (fac > 2 && fac <= 4)
        {
            dim = 5;
            gw = g5;
        }
        else if (fac > 4)
        {
            dim = 7;
            gw = g7;
        }
        for (int y = 0; y < dh; ++y)
        {
            int oy = (int)(0.5 + y * ry);
            for (int x = 0; x < dw; ++x)
            {
                int ox = (int)(0.5 + x * rx);
                double tot = 0, wsum = 0;
                for (int dy = 0; dy < dim; ++dy)
                {
                    int sy = oy + dy;
                    if (sy < 0 || sy >= sh)
                        continue;
                    for (int dxk = 0; dxk < dim; ++dxk)
                    {
                        int sx = ox + dxk;
                        if (sx < 0 || sx >= sw)
                            continue;
                        double v = src[(size_t)sy * sw + sx];
                        if (nd && v == ndv)
                            continue;
                        double w = gw[dy] * gw[dxk];
                        tot += w * v;
                        wsum += w;
                    }
                }
                double out;
                if (wsum <= 0)
                    out = nd ? ndv : 0.0;
                else
                    out = roundStore(tot / wsum, m);
                dst[(size_t)y * dw + x] = out;
            }
        }
        return;
    }
    // separable convolution kernels
    double radius = kernelRadius(method);
    double sx = std::min(1.0, (double)dw / sw);
    double sy = std::min(1.0, (double)dh / sh);
    double rxk = radius / sx, ryk = radius / sy;
    bool twoPass = !nd && !m.isInt;
    std::vector<double> xw, yw, hbuf, vals;
    for (int y = 0; y < dh; ++y)
    {
        double cy = (y + 0.5) * ry - 0.5;
        int iy0 = (int)std::ceil(cy - ryk), iy1 = (int)std::floor(cy + ryk);
        if (iy0 < 0)
            iy0 = 0;
        if (iy1 >= sh)
            iy1 = sh - 1;
        yw.clear();
        {
            // reference accumulates the kernel argument incrementally
            double a = sy * ((double)iy0 - ((y + 0.5) * ry) + 0.5);
            for (int iy = iy0; iy <= iy1; ++iy, a += sy)
                yw.push_back(kernelWeight(method, a));
        }
        if (twoPass)
        {
            double s = 0;
            for (double w : yw)
                s += w;
            if (s != 0)
            {
                double inv = 1.0 / s;
                for (double &w : yw)
                    w *= inv;
            }
        }
        for (int x = 0; x < dw; ++x)
        {
            double cx = (x + 0.5) * rx - 0.5;
            int ix0 = (int)std::ceil(cx - rxk),
                ix1 = (int)std::floor(cx + rxk);
            if (ix0 < 0)
                ix0 = 0;
            if (ix1 >= sw)
                ix1 = sw - 1;
            xw.clear();
            {
                double a = sx * ((double)ix0 - ((x + 0.5) * rx) + 0.5);
                for (int ix = ix0; ix <= ix1; ++ix, a += sx)
                    xw.push_back(kernelWeight(method, a));
            }
            double out;
            if (twoPass)
            {
                double s = 0;
                for (double w : xw)
                    s += w;
                if (s != 0)
                {
                    double inv = 1.0 / s;
                    for (double &w : xw)
                        w *= inv;
                }
                hbuf.clear();
                for (int iy = iy0; iy <= iy1; ++iy)
                    hbuf.push_back(convSum(&src[(size_t)iy * sw + ix0],
                                           xw.data(), (int)xw.size()));
                out = convSum(hbuf.data(), yw.data(), (int)hbuf.size());
                out = roundStore(out, m);
            }
            else
            {
                double tot = 0, wsum = 0;
                for (int iy = iy0; iy <= iy1; ++iy)
                {
                    double wy = yw[iy - iy0];
                    if (wy == 0)
                        continue;
                    for (int ix = ix0; ix <= ix1; ++ix)
                    {
                        double wx = xw[ix - ix0];
                        if (wx == 0)
                            continue;
                        double v = src[(size_t)iy * sw + ix];
                        if (nd && v == ndv)
                            continue;
                        tot += wx * wy * v;
                        wsum += wx * wy;
                    }
                }
                if (wsum <= 0)
                    out = nd ? ndv : 0.0;
                else
                    out = roundStore(tot / wsum, m);
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
}

// ------------------------------------------------------------------
// the verb
// ------------------------------------------------------------------

int rasterUpdateHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool noUpdOvr = r.flag("no-update-overviews");
    std::string resamp = strToLower(r.str("resampling", "nearest"));
    const ArgValue *geomArg = r.get("geometry");
    std::string geomText = r.str("geometry");
    std::string geomCrs = r.str("geometry-crs");
    std::vector<std::string> tos = r.list("transform-option");
    std::string initDest;
    bool haveInit = false;
    for (const auto &wo : r.list("warp-option"))
    {
        size_t eq = wo.find('=');
        if (eq != std::string::npos &&
            strEqualNoCase(wo.substr(0, eq), "INIT_DEST"))
        {
            initDest = wo.substr(eq + 1);
            haveInit = true;
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
    auto src = openRaster(input, err, oo);
    if (!src)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    // clipping geometry parse (and polygonal check) precedes the
    // same-file and output checks
    OgrGeometry clipGeom;
    bool haveClip = false;
    if (geomArg)
    {
        if (!clipGeometryParseText(geomText, clipGeom))
            return updateError(CPLE_AppDefined,
                               "update: Clipping geometry is neither a "
                               "valid WKT or GeoJSON geometry");
        if (clipGeom.type != 3 && clipGeom.type != 6)
            return updateError(CPLE_AppDefined,
                               "Cannot open " + ogrWkt(clipGeom) + ".");
        haveClip = true;
    }

    if (input == output)
        return updateError(CPLE_NotSupported,
                           "update: Source and destination datasets "
                           "must be different");

    std::string oerr;
    OpenOptions ooOut;
    auto dst = openRaster(output, oerr, ooOut);
    if (!dst)
    {
        if (oerr != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + output +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    if (dst->driverShort == "VRT")
    {
        cplErrorStr(CE_Warning, CPLE_NotSupported,
                    "VRT output not compatible with existing dataset.");
        return 1;
    }

    // a source carrying an SRS dirties the destination directory as soon
    // as it reaches the warp machinery: pixels always land in place, but
    // the directory is rewritten at EOF on close, even when a later
    // validation fails
    bool reloc = src->hasSrs;
    auto failReloc = [&](const std::string &msg) -> int
    {
        if (reloc)
        {
            std::vector<uint8_t> f;
            if (updLoadWhole(output, f) && f.size() >= 8 &&
                f[0] == 'I' && f[1] == 'I' && rd16(f, 2) == 42)
            {
                relocateFirstIfd(f);
                updSaveWhole(output, f);
            }
        }
        return updateError(CPLE_AppDefined, msg);
    };

    for (const auto &to : tos)
    {
        if (to.rfind("METHOD=", 0) == 0)
        {
            std::string m = to.substr(7);
            if (m != "GEOTRANSFORM")
                return failReloc(
                    strPrintf("Unable to compute a %s based "
                              "transformation between pixel/line and "
                              "georeferenced coordinates for %s.",
                              m.c_str(), input.c_str()));
        }
    }

    if (!src->hasGT)
        return failReloc(
            "Unable to compute a transformation between pixel/line and "
            "georeferenced coordinates for " +
            input +
            ". There is no affine transformation and no GCPs. "
            "Specify transformation option SRC_METHOD=NO_GEOTRANSFORM "
            "to bypass this check.");
    if (!dst->hasGT)
        return failReloc(
            "Unable to compute a transformation between pixel/line and "
            "georeferenced coordinates for " +
            output +
            ". There is no affine transformation and no GCPs. "
            "Specify transformation option DST_METHOD=NO_GEOTRANSFORM "
            "to bypass this check.");

    if (src->bands.size() > dst->bands.size())
        return failReloc(
            strPrintf("Destination dataset has %d bands, but at least "
                      "%d are needed",
                      (int)dst->bands.size(), (int)src->bands.size()));

    if (haveClip)
    {
        if (!geomCrs.empty() && dst->hasSrs)
        {
            bool ok = false;
            cplPushQuietHandler();
            Srs gs = Srs::fromCliInput(geomCrs, ok);
            cplPopHandler();
            if (ok)
            {
                void *op = vectorCrsOpCreate(gs, dst->srs);
                if (op)
                {
                    vectorCrsOpApply(op, clipGeom);
                    vectorCrsOpFree(op);
                }
            }
        }
        if (src->hasSrs)
            cplErrorStr(
                CE_Warning, CPLE_AppDefined,
                "the source raster dataset has a SRS, but the cutline "
                "features\nnot.  We assume that the cutline coordinates "
                "are expressed in the destination SRS.\nIf not, cutline "
                "results may be incorrect.");
    }

    // CRS relationship: identical (or missing on either side) keeps the
    // affine path, anything else goes through a coordinate operation
    void *crsOp = nullptr;
    struct OpGuard
    {
        void *&op;
        ~OpGuard()
        {
            if (op)
                vectorCrsOpFree(op);
        }
    } opGuard{crsOp};
    if (src->hasSrs && dst->hasSrs)
    {
        std::string a = src->srs.wkt2_2019();
        std::string b = dst->srs.wkt2_2019();
        if (a != b)
            crsOp = vectorCrsOpCreate(dst->srs, src->srs);
    }

    int sw = src->width, sh = src->height;
    int dw = dst->width, dh = dst->height;
    const double *sgt = src->gt;
    const double *dgt = dst->gt;

    // aligned same-grid detection: equal resolutions, no rotation, and
    // integral pixel offsets let every kernel collapse to a plain paste
    bool aligned = false;
    if (!crsOp && sgt[2] == 0 && sgt[4] == 0 && dgt[2] == 0 &&
        dgt[4] == 0 && sgt[1] == dgt[1] && sgt[5] == dgt[5])
    {
        double fx = (sgt[0] - dgt[0]) / dgt[1];
        double fy = (sgt[3] - dgt[3]) / dgt[5];
        if (fx == std::floor(fx) && fy == std::floor(fy))
            aligned = true;
    }

    if (!aligned && resamp != "nearest" && resamp != "bilinear")
        return updateError(CPLE_NotSupported,
                           "update: Resampling '" + resamp +
                               "' is not implemented in this build");

    // source pixels, one band at a time, as doubles
    int nBands = (int)src->bands.size();
    std::vector<std::vector<double>> srcVals((size_t)nBands);
    for (int b = 1; b <= nBands; ++b)
        if (!src->readBand(b, srcVals[(size_t)b - 1]))
            return updateError(CPLE_AppDefined,
                               "update: cannot read " + input);

    // work out the produced value (or skip) for every destination cell
    double sInv[6];
    {
        // GDALInvGeoTransform's exact arithmetic (multiplications by the
        // reciprocal determinant); the last-ulp placement decides ties
        double det = sgt[1] * sgt[5] - sgt[2] * sgt[4];
        if (det == 0)
            return updateError(CPLE_AppDefined,
                               "update: cannot invert source "
                               "geotransform");
        double idet = 1.0 / det;
        sInv[1] = sgt[5] * idet;
        sInv[2] = -sgt[2] * idet;
        sInv[4] = -sgt[4] * idet;
        sInv[5] = sgt[1] * idet;
        sInv[0] = (sgt[2] * sgt[3] - sgt[0] * sgt[5]) * idet;
        sInv[3] = (-sgt[1] * sgt[3] + sgt[0] * sgt[4]) * idet;
    }

    // the cutline masks SOURCE pixels: the polygon is taken to source
    // pixel space and scanline-rasterized with the reference's
    // conventions (a row scan at j+0.5 crosses an edge over the
    // half-open [ymin, ymax) span; a run fills the columns whose
    // centers land in (x1, x2], i.e. floor(x+0.5) bounds)
    std::vector<char> srcMask;
    if (haveClip)
    {
        srcMask.assign((size_t)sw * sh, 0);
        auto toSrcPx = [&](double X, double Y, double &px, double &py)
        {
            if (crsOp)
            {
                OgrGeometry pt;
                pt.type = 1;
                pt.coords = {X, Y, 0.0};
                if (!vectorCrsOpApply(crsOp, pt) ||
                    pt.coords.size() < 2)
                    return false;
                X = pt.coords[0];
                Y = pt.coords[1];
            }
            px = sInv[0] + sInv[1] * X + sInv[2] * Y;
            py = sInv[3] + sInv[4] * X + sInv[5] * Y;
            return true;
        };
        auto fillPoly = [&](const OgrGeometry &poly)
        {
            std::vector<std::vector<std::pair<double, double>>> rings;
            for (const auto &ring : poly.parts)
            {
                std::vector<std::pair<double, double>> r;
                for (size_t c = 0; c + 1 < ring.coords.size(); c += 3)
                {
                    double px, py;
                    if (toSrcPx(ring.coords[c], ring.coords[c + 1], px,
                                py))
                        r.emplace_back(px, py);
                }
                if (r.size() >= 2 && r.front() == r.back())
                    r.pop_back();
                if (r.size() >= 3)
                    rings.push_back(std::move(r));
            }
            std::vector<double> xs;
            for (int j = 0; j < sh; ++j)
            {
                double dfY = j + 0.5;
                xs.clear();
                for (const auto &r : rings)
                {
                    size_t n = r.size();
                    for (size_t k = 0; k < n; ++k)
                    {
                        double y1 = r[k].second;
                        double y2 = r[(k + 1) % n].second;
                        if (y1 == y2)
                            continue;
                        double x1 = r[k].first;
                        double x2 = r[(k + 1) % n].first;
                        if (y1 > y2)
                        {
                            std::swap(y1, y2);
                            std::swap(x1, x2);
                        }
                        if (dfY < y1 || dfY >= y2)
                            continue;
                        xs.push_back(x1 +
                                     (dfY - y1) / (y2 - y1) * (x2 - x1));
                    }
                }
                std::sort(xs.begin(), xs.end());
                for (size_t k = 0; k + 1 < xs.size(); k += 2)
                {
                    int x0 = (int)std::floor(xs[k] + 0.5);
                    int x1 = (int)std::floor(xs[k + 1] + 0.5) - 1;
                    if (x0 < 0)
                        x0 = 0;
                    if (x1 > sw - 1)
                        x1 = sw - 1;
                    for (int i = x0; i <= x1; ++i)
                        srcMask[(size_t)j * sw + i] = 1;
                }
            }
        };
        if (clipGeom.type == 6)
            for (const auto &part : clipGeom.parts)
                fillPoly(part);
        else
            fillPoly(clipGeom);
    }

    std::vector<std::vector<double>> newVals((size_t)nBands);
    std::vector<std::vector<char>> newSet((size_t)nBands);
    for (int b = 0; b < nBands; ++b)
    {
        newVals[(size_t)b].assign((size_t)dw * dh, 0.0);
        newSet[(size_t)b].assign((size_t)dw * dh, 0);
    }
    long long wx0 = dw, wx1 = -1, wy0 = dh, wy1 = -1;
    if (haveInit)
    {
        // INIT_DEST floods the source's bands over the whole raster
        // before the paste; NO_DATA means each band's own nodata
        bool ndComplained = false;
        for (int b = 0; b < nBands; ++b)
        {
            const Band &db = dst->bands[(size_t)b];
            double v = 0.0;
            if (strEqualNoCase(initDest, "NO_DATA"))
            {
                // a missing nodata value complains (non-fatally) and
                // floods zero instead
                if (!db.hasNodata && !ndComplained)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "INIT_DEST was set to NO_DATA, but a "
                                "NoData value was not defined.");
                    ndComplained = true;
                }
                v = db.hasNodata ? db.nodata : 0.0;
            }
            else
                v = strtod(initDest.c_str(), nullptr);
            v = updRound(db.type, v);
            newVals[(size_t)b].assign((size_t)dw * dh, v);
            newSet[(size_t)b].assign((size_t)dw * dh, 1);
        }
        wx0 = 0;
        wy0 = 0;
        wx1 = dw - 1;
        wy1 = dh - 1;
    }
    for (int y = 0; y < dh; ++y)
    {
        for (int x = 0; x < dw; ++x)
        {
            double gx = dgt[0] + (x + 0.5) * dgt[1] + (y + 0.5) * dgt[2];
            double gy = dgt[3] + (x + 0.5) * dgt[4] + (y + 0.5) * dgt[5];
            double tx = gx, ty = gy;
            if (crsOp)
            {
                OgrGeometry pt;
                pt.type = 1;
                pt.coords = {gx, gy, 0.0};
                if (!vectorCrsOpApply(crsOp, pt) ||
                    pt.coords.size() < 2)
                    continue;
                tx = pt.coords[0];
                ty = pt.coords[1];
            }
            double px = sInv[0] + sInv[1] * tx + sInv[2] * ty;
            double py = sInv[3] + sInv[4] * tx + sInv[5] * ty;
            if (resamp == "bilinear" && !aligned)
            {
                if (px < 0 || px >= sw || py < 0 || py >= sh)
                    continue;
                // warp bilinear: an axis downsampled 2x or more switches
                // both axes to the XSCALE/YSCALE-widened triangle kernel
                // (normalized by the product of the axis weight sums);
                // everything else runs the plain 4-sample formula. Masked
                // taps drop out with per-tap renormalization.
                double rx2 = 1.0, ry2 = 1.0;
                if (!crsOp && dgt[1] != 0 && dgt[5] != 0)
                {
                    rx2 = std::fabs(sgt[1] / dgt[1]);
                    ry2 = std::fabs(sgt[5] / dgt[5]);
                }
                bool general = rx2 <= 0.5 || ry2 <= 0.5;
                double sclX = general && rx2 < 1 ? rx2 : 1.0;
                double sclY = general && ry2 < 1 ? ry2 : 1.0;
                double u = px - 0.5, v0 = py - 0.5;
                // masked runs first gate on the validity of the source
                // pixel containing the transformed center
                int icx = (int)std::floor(px + 1e-10);
                int icy = (int)std::floor(py + 1e-10);
                if (icx > sw - 1) icx = sw - 1;
                if (icy > sh - 1) icy = sh - 1;
                if (haveClip && !srcMask[(size_t)icy * sw + icx])
                    continue;
                bool any = false;
                for (int b = 0; b < nBands; ++b)
                {
                    const Band &sb = src->bands[(size_t)b];
                    const std::vector<double> &sv = srcVals[(size_t)b];
                    bool masked = sb.hasNodata || haveClip;
                    if (sb.hasNodata &&
                        sv[(size_t)icy * sw + icx] == sb.nodata)
                        continue;
                    bool got = false;
                    double V = 0.0;
                    if (!general && !masked)
                    {
                        int ix = (int)std::floor(px - 0.5);
                        int iy = (int)std::floor(py - 0.5);
                        if (ix >= 0 && ix + 1 < sw && iy >= 0 &&
                            iy + 1 < sh)
                        {
                            // interior 4-sample: successive-lerp form
                            double dfx = px - 0.5 - ix;
                            double dfy = py - 0.5 - iy;
                            double v1 = sv[(size_t)iy * sw + ix];
                            double v2 = sv[(size_t)iy * sw + ix + 1];
                            double v3 = sv[(size_t)(iy + 1) * sw + ix];
                            double v4 =
                                sv[(size_t)(iy + 1) * sw + ix + 1];
                            double r0 = v1 + (v2 - v1) * dfx;
                            double r1 = v3 + (v4 - v3) * dfx;
                            V = r0 + (r1 - r0) * dfy;
                            got = true;
                        }
                        else
                        {
                            double fx = 1.5 - (px - ix);
                            double fy = 1.5 - (py - iy);
                            double m1 = fx * fy, m2 = (1.0 - fx) * fy;
                            double m3 = fx * (1.0 - fy);
                            double m4 = (1.0 - fx) * (1.0 - fy);
                            double acc = 0.0, div = 0.0;
                            auto tap = [&](int i, int j, double m)
                            {
                                if (i < 0 || i >= sw || j < 0 ||
                                    j >= sh)
                                    return;
                                acc += sv[(size_t)j * sw + i] * m;
                                div += m;
                            };
                            tap(ix, iy, m1);
                            tap(ix + 1, iy, m2);
                            tap(ix, iy + 1, m3);
                            tap(ix + 1, iy + 1, m4);
                            if (div > 0)
                            {
                                V = acc / div;
                                got = true;
                            }
                        }
                    }
                    else
                    {
                        int ix0 = (int)std::ceil(u - 1.0 / sclX);
                        int ix1 = (int)std::floor(u + 1.0 / sclX);
                        int iy0 = (int)std::ceil(v0 - 1.0 / sclY);
                        int iy1 = (int)std::floor(v0 + 1.0 / sclY);
                        if (ix0 < 0)
                            ix0 = 0;
                        if (iy0 < 0)
                            iy0 = 0;
                        if (ix1 > sw - 1)
                            ix1 = sw - 1;
                        if (iy1 > sh - 1)
                            iy1 = sh - 1;
                        if (!masked)
                        {
                            double wh = 0.0, wv = 0.0, acc = 0.0;
                            for (int i = ix0; i <= ix1; ++i)
                            {
                                double wx = 1.0 -
                                            std::fabs(u - i) * sclX;
                                if (wx > 0)
                                    wh += wx;
                            }
                            for (int j = iy0; j <= iy1; ++j)
                            {
                                double wy = 1.0 -
                                            std::fabs(v0 - j) * sclY;
                                if (wy <= 0)
                                    continue;
                                wv += wy;
                                double row = 0.0;
                                for (int i = ix0; i <= ix1; ++i)
                                {
                                    double wx =
                                        1.0 - std::fabs(u - i) * sclX;
                                    if (wx <= 0)
                                        continue;
                                    row += sv[(size_t)j * sw + i] * wx;
                                }
                                acc += row * wy;
                            }
                            if (wh > 0 && wv > 0)
                            {
                                V = acc / (wh * wv);
                                got = true;
                            }
                        }
                        else
                        {
                            double acc = 0.0, wsum = 0.0;
                            for (int j = iy0; j <= iy1; ++j)
                            {
                                double wy = 1.0 -
                                            std::fabs(v0 - j) * sclY;
                                if (wy <= 0)
                                    continue;
                                for (int i = ix0; i <= ix1; ++i)
                                {
                                    double wx =
                                        1.0 - std::fabs(u - i) * sclX;
                                    if (wx <= 0)
                                        continue;
                                    if (haveClip &&
                                        !srcMask[(size_t)j * sw + i])
                                        continue;
                                    double val = sv[(size_t)j * sw + i];
                                    if (sb.hasNodata && val == sb.nodata)
                                        continue;
                                    acc += val * wx * wy;
                                    wsum += wx * wy;
                                }
                            }
                            if (wsum > 0)
                            {
                                V = acc / wsum;
                                got = true;
                            }
                        }
                    }
                    if (!got)
                        continue;
                    DType dt = dst->bands[(size_t)b].type;
                    if (dt != DType::Float32 && dt != DType::Float64)
                    {
                        double lo = 0, hi = 255;
                        switch (dt)
                        {
                            case DType::Int8: lo = -128; hi = 127; break;
                            case DType::UInt16: hi = 65535; break;
                            case DType::Int16:
                                lo = -32768; hi = 32767; break;
                            case DType::UInt32:
                                hi = 4294967295.0; break;
                            case DType::Int32:
                                lo = -2147483648.0;
                                hi = 2147483647.0; break;
                            default: break;
                        }
                        if (V < lo)
                            V = lo;
                        if (V > hi)
                            V = hi;
                        V = std::floor(V + 0.5);
                    }
                    newVals[(size_t)b][(size_t)y * dw + x] = V;
                    newSet[(size_t)b][(size_t)y * dw + x] = 1;
                    any = true;
                }
                if (any)
                {
                    if (x < wx0) wx0 = x;
                    if (x > wx1) wx1 = x;
                    if (y < wy0) wy0 = y;
                    if (y > wy1) wy1 = y;
                }
                continue;
            }
            int isx = (int)std::floor(px + 1e-10);
            int isy = (int)std::floor(py + 1e-10);
            if (isx < 0 || isx >= sw || isy < 0 || isy >= sh)
                continue;
            if (haveClip && !srcMask[(size_t)isy * sw + isx])
                continue;
            bool any = false;
            for (int b = 0; b < nBands; ++b)
            {
                const Band &sb = src->bands[(size_t)b];
                double v = srcVals[(size_t)b][(size_t)isy * sw + isx];
                if (sb.hasNodata && v == sb.nodata)
                    continue;
                newVals[(size_t)b][(size_t)y * dw + x] =
                    updRound(dst->bands[(size_t)b].type, v);
                newSet[(size_t)b][(size_t)y * dw + x] = 1;
                any = true;
            }
            if (any)
            {
                if (x < wx0) wx0 = x;
                if (x > wx1) wx1 = x;
                if (y < wy0) wy0 = y;
                if (y > wy1) wy1 = y;
            }
        }
    }

    // overview refresh choreography: a georeferencing presence mismatch
    // between the two datasets forfeits the refresh with a warning; a
    // resampling outside the overview set aborts (after the pixels have
    // been written) with a doubled ERROR 5 at the 75% progress mark;
    // otherwise every level is recomputed with the chosen resampling,
    // cascading from the previous level, and only the cells under the
    // source footprint are rewritten
    bool ovrPresent = !noUpdOvr && !dst->overviews.empty();
    bool ovrSrsMismatch = src->hasSrs != dst->hasSrs;
    bool ovrValidResamp =
        resamp == "nearest" || resamp == "average" || resamp == "cubic" ||
        resamp == "cubicspline" || resamp == "lanczos" ||
        resamp == "bilinear" || resamp == "gauss" ||
        resamp == "average_magphase" || resamp == "rms" ||
        resamp == "mode";

    // geometric refresh window: the source footprint in destination
    // pixels (independent of nodata skips, cutlines and INIT_DEST)
    long long gwx0 = 0, gwx1 = -1, gwy0 = 0, gwy1 = -1;
    if (!crsOp)
    {
        double det = dgt[1] * dgt[5] - dgt[2] * dgt[4];
        if (det != 0)
        {
            double idet = 1.0 / det;
            double dInv[6];
            dInv[1] = dgt[5] * idet;
            dInv[2] = -dgt[2] * idet;
            dInv[4] = -dgt[4] * idet;
            dInv[5] = dgt[1] * idet;
            dInv[0] = (dgt[2] * dgt[3] - dgt[0] * dgt[5]) * idet;
            dInv[3] = (-dgt[1] * dgt[3] + dgt[0] * dgt[4]) * idet;
            double mnx = 0, mxx = 0, mny = 0, mxy = 0;
            bool first = true;
            const double cx[4] = {0.0, (double)sw, 0.0, (double)sw};
            const double cy[4] = {0.0, 0.0, (double)sh, (double)sh};
            for (int k = 0; k < 4; ++k)
            {
                double gx = sgt[0] + cx[k] * sgt[1] + cy[k] * sgt[2];
                double gy = sgt[3] + cx[k] * sgt[4] + cy[k] * sgt[5];
                double px = dInv[0] + dInv[1] * gx + dInv[2] * gy;
                double py = dInv[3] + dInv[4] * gx + dInv[5] * gy;
                if (first || px < mnx) mnx = px;
                if (first || px > mxx) mxx = px;
                if (first || py < mny) mny = py;
                if (first || py > mxy) mxy = py;
                first = false;
            }
            gwx0 = (long long)std::floor(mnx);
            gwx1 = (long long)std::ceil(mxx) - 1;
            gwy0 = (long long)std::floor(mny);
            gwy1 = (long long)std::ceil(mxy) - 1;
            if (gwx0 < 0) gwx0 = 0;
            if (gwy0 < 0) gwy0 = 0;
            if (gwx1 > dw - 1) gwx1 = dw - 1;
            if (gwy1 > dh - 1) gwy1 = dh - 1;
        }
    }
    else
    {
        // reprojected sources fall back to the cells actually written
        gwx0 = wx0;
        gwx1 = wx1;
        gwy0 = wy0;
        gwy1 = wy1;
    }

    // updated whole-band views feed the overview recomputation
    std::vector<std::vector<double>> baseVals;
    bool wantOvr = ovrPresent && !ovrSrsMismatch && ovrValidResamp &&
                   gwx1 >= gwx0 && gwy1 >= gwy0;
    if (wantOvr)
    {
        baseVals.resize(dst->bands.size());
        for (size_t b = 0; b < dst->bands.size(); ++b)
        {
            if (!dst->readBand((int)b + 1, baseVals[b]))
            {
                wantOvr = false;
                break;
            }
            if ((int)b < nBands)
                for (size_t i = 0; i < baseVals[b].size(); ++i)
                    if (newSet[b][i])
                        baseVals[b][i] = newVals[b][i];
        }
    }

    // in-place TIFF write: strip/tile pixels first, then the directory
    // rewrite at EOF
    std::vector<uint8_t> file;
    if (!updLoadWhole(output, file) || file.size() < 8 ||
        file[0] != 'I' || file[1] != 'I' || rd16(file, 2) != 42)
        return updateError(CPLE_AppDefined,
                           "update: this output is not supported in "
                           "this build");

    std::vector<uint32_t> chain;
    {
        uint32_t off = rd32(file, 4);
        int guard = 0;
        while (off && guard++ < 100)
        {
            RawIfd ifd;
            if (!parseRawIfd(file, off, ifd))
                break;
            chain.push_back(off);
            off = ifd.next;
        }
    }
    if (chain.empty())
        return updateError(CPLE_AppDefined,
                           "update: this output is not supported in "
                           "this build");

    RawIfd mainIfd;
    parseRawIfd(file, chain[0], mainIfd);
    RawGrid mainGrid;
    if (!gridFromIfd(file, mainIfd, mainGrid) || mainGrid.w != dw ||
        mainGrid.h != dh)
        return updateError(CPLE_AppDefined,
                           "update: this output is not supported in "
                           "this build");

    for (int b = 0; b < nBands; ++b)
    {
        DType t = dst->bands[(size_t)b].type;
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
            {
                if (!newSet[(size_t)b][(size_t)y * dw + x])
                    continue;
                size_t off;
                if (mainGrid.sampleOffset(x, y, b, off))
                    updEncode(t, file, off,
                              newVals[(size_t)b][(size_t)y * dw + x]);
            }
    }

    // refresh internal overviews over the source-footprint window with
    // the chosen resampling, each level cascading from the previous one
    // (the way fresh overviews are laid down in this build)
    if (wantOvr)
    {
        std::vector<std::vector<double>> cur = baseVals;
        int cw = dw, ch = dh;
        for (const auto &e : dst->overviews)
        {
            if (e.page - 1 < 0 || e.page - 1 >= (int)chain.size())
                continue;
            RawIfd oifd;
            if (!parseRawIfd(file, chain[(size_t)e.page - 1], oifd))
                continue;
            RawGrid og;
            if (!gridFromIfd(file, oifd, og))
                continue;
            int ow = og.w, oh = og.h;
            int ox0 = (int)((double)gwx0 * ow / dw);
            int ox1 = (int)std::ceil(((double)gwx1 + 1) * ow / dw);
            int oy0 = (int)((double)gwy0 * oh / dh);
            int oy1 = (int)std::ceil(((double)gwy1 + 1) * oh / dh);
            if (ox0 < 0) ox0 = 0;
            if (oy0 < 0) oy0 = 0;
            if (ox1 > ow) ox1 = ow;
            if (oy1 > oh) oy1 = oh;
            for (size_t b = 0; b < cur.size(); ++b)
            {
                MainInfo m = updMainInfo(dst->bands[b]);
                std::vector<double> lv;
                resampleBand(cur[b], cw, ch, lv, ow, oh, resamp, m);
                DType t = dst->bands[b].type;
                for (int y = oy0; y < oy1; ++y)
                    for (int x = ox0; x < ox1; ++x)
                    {
                        size_t off;
                        if (og.sampleOffset(x, y, (int)b, off))
                            updEncode(t, file, off,
                                      lv[(size_t)y * ow + x]);
                    }
                // the next level cascades from the stored grid: stale
                // values outside the window, refreshed ones inside
                cur[b].assign((size_t)ow * oh, 0.0);
                for (int y = 0; y < oh; ++y)
                    for (int x = 0; x < ow; ++x)
                    {
                        size_t off;
                        if (og.sampleOffset(x, y, (int)b, off))
                            cur[b][(size_t)y * ow + x] =
                                updDecode(t, file, off);
                    }
            }
            cw = ow;
            ch = oh;
        }
    }

    if (reloc)
        relocateFirstIfd(file);
    if (!updSaveWhole(output, file))
        return updateError(CPLE_AppDefined,
                           "update: cannot write " + output);

    if (ovrPresent && ovrSrsMismatch)
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "update: Overviews can not be updated");
    else if (ovrPresent && !ovrValidResamp)
    {
        // the refresh rejects the resampling at the 75% progress mark,
        // after the pixels have already been written
        if (!quiet)
        {
            fputs("0...10...20...30...40...50...60...70..", stdout);
            fflush(stdout);
        }
        std::string msg =
            "Invalid value '" + resamp +
            "' for string argument 'resampling'. Should be one among "
            "'nearest', 'average', 'cubic', 'cubicspline', 'lanczos', "
            "'bilinear', 'gauss', 'average_magphase', 'rms', 'mode'.";
        cplErrorStr(CE_Failure, CPLE_IllegalArg, msg);
        cplErrorStr(CE_Failure, CPLE_IllegalArg, msg);
        return 1;
    }

    if (!quiet)
        printProgress();
    return 0;
}

struct Reg
{
    Reg()
    {
        registerHandler("raster_update", rasterUpdateHandler);
        registerPreValidator("raster_update", updatePreValidator);
        registerArgCheck("raster_update", updateArgCheck);
    }
};

}  // namespace

void registerRasterUpdateHandler()
{
    static Reg reg;
}
