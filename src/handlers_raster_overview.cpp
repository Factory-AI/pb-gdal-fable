#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "gtiff_write.h"
#include "jpeg_ijg.h"
#include "tiff.h"
#include "util.h"
#include "webp_shim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C"
{
    int compress2(unsigned char *dest, unsigned long *destLen,
                  const unsigned char *source, unsigned long sourceLen,
                  int level);
    unsigned long compressBound(unsigned long sourceLen);
}

// tile codecs shared with the GTiff writer
std::vector<uint8_t> gtiffZstdBlock(const std::vector<uint8_t> &in,
                                    int level);
std::vector<uint8_t> gtiffDeflateBlock(const std::vector<uint8_t> &in,
                                       int level);
std::vector<uint8_t> gtiffLzwEncode(const uint8_t *p, size_t n);
std::vector<uint8_t> gtiffPackbitsEncode(const uint8_t *p, size_t n,
                                         size_t rowBytes);
void gtiffHorDiff(std::vector<uint8_t> &row, int sampleBytes, int stride);
void gtiffFpDiff(std::vector<uint8_t> &row, int sampleBytes, int stride);

namespace
{

struct MainInfo
{
    int w = 0, h = 0;
    uint32_t spp = 1, bps = 8, comp = 1, pred = 1, phot = 1, sf = 1;
    uint32_t planar = 1;
    DType dt = DType::Byte;
    int bytes = 1;
    bool isInt = true;
    bool hasCt = false;  // band color table (real or synthesized)
    double clampLo = 0, clampHi = 255;
    std::vector<uint64_t> colorMap;
    std::vector<uint64_t> extraSamples;
    int jpegQuality = -1;     // main IMAGE_STRUCTURE JPEG_QUALITY
    int jpegTablesMode = -1;  // main IMAGE_STRUCTURE JPEGTABLESMODE
    int webpLevel = -1;       // main IMAGE_STRUCTURE WEBP_LEVEL
    bool webpLossless = false;
    std::vector<uint8_t> refBWRaw;  // main tag 532 bytes
    bool hasNodata = false;
    double nodata = 0;
    std::string nodataRaw;  // exact GDAL_NODATA bytes (with NUL)
};

bool fillMainInfo(RasterDatasetBase &ds, const TiffIfd &ifd, MainInfo &m,
                  std::string &why)
{
    m.w = ds.width;
    m.h = ds.height;
    m.spp = (uint32_t)ifd.getInt(277, 1);
    m.bps = (uint32_t)ifd.getInt(258, 1);
    m.comp = (uint32_t)ifd.getInt(259, 1);
    m.pred = (uint32_t)ifd.getInt(317, 1);
    m.phot = (uint32_t)ifd.getInt(262, 1);
    m.dt = ds.bands.empty() ? DType::Byte : ds.bands[0].type;
    m.bytes = dtypeSizeBytes(m.dt);
    m.planar = m.spp > 1 ? (uint32_t)ifd.getInt(284, 1) : 1;
    switch (m.dt)
    {
        case DType::Byte:
            m.clampLo = 0;
            m.clampHi = 255;
            m.sf = 1;
            break;
        case DType::Int8:
            m.clampLo = -128;
            m.clampHi = 127;
            m.sf = 2;
            break;
        case DType::UInt16:
            m.clampLo = 0;
            m.clampHi = 65535;
            m.sf = 1;
            break;
        case DType::Int16:
            m.clampLo = -32768;
            m.clampHi = 32767;
            m.sf = 2;
            break;
        case DType::UInt32:
            m.clampLo = 0;
            m.clampHi = 4294967295.0;
            m.sf = 1;
            break;
        case DType::Int32:
            m.clampLo = -2147483648.0;
            m.clampHi = 2147483647.0;
            m.sf = 2;
            break;
        case DType::Float32:
            m.isInt = false;
            m.sf = 3;
            break;
        case DType::Float64:
            m.isInt = false;
            m.sf = 3;
            break;
        default:
            why = "unsupported band type";
            return false;
    }
    if (m.bps != (uint32_t)m.bytes * 8)
    {
        why = "sub-byte sample width";
        return false;
    }
    if (const std::vector<uint64_t> *cm = ifd.getInts(320))
        m.colorMap = *cm;
    m.hasCt = !ds.bands.empty() && !ds.bands[0].colorTable.empty();
    if (m.hasCt && m.colorMap.empty())
    {
        const auto &ct = ds.bands[0].colorTable;
        for (int ch = 0; ch < 3; ++ch)
            for (const auto &e : ct)
                m.colorMap.push_back(
                    (uint64_t)(ch == 0 ? e.c1 : ch == 1 ? e.c2 : e.c3) *
                    257);
    }
    if (ifd.has(532))
        m.refBWRaw = ifd.tags.at(532).raw;
    if (const std::string *q = ds.getMd("IMAGE_STRUCTURE", "JPEG_QUALITY"))
        m.jpegQuality = atoi(q->c_str());
    if (m.comp == 50001)
    {
        if (const std::string *q =
                ds.getMd("IMAGE_STRUCTURE", "WEBP_LEVEL"))
        {
            int lv = atoi(q->c_str());
            if (lv >= 1 && lv <= 100)
                m.webpLevel = lv;
        }
        if (const std::string *rv =
                ds.getMd("IMAGE_STRUCTURE", "COMPRESSION_REVERSIBILITY"))
            m.webpLossless = *rv == "LOSSLESS";
    }
    if (m.comp == 7)
    {
        // the tables mode is recovered from the JPEGTables structure
        // itself, not the reported metadata (modes 0 and 2 report none)
        m.jpegTablesMode = 0;
        if (ifd.has(347))
        {
            const auto &t = ifd.tags.at(347).raw;
            for (size_t i = 2; i + 4 <= t.size() && (uint8_t)t[i] == 0xFF;)
            {
                uint8_t mk = (uint8_t)t[i + 1];
                if (mk == 0xD9)
                    break;
                if (mk == 0xDB)
                    m.jpegTablesMode |= 1;
                else if (mk == 0xC4)
                    m.jpegTablesMode |= 2;
                size_t len =
                    ((size_t)(uint8_t)t[i + 2] << 8) | (uint8_t)t[i + 3];
                if (len < 2 || i + 2 + len > t.size())
                    break;
                i += 2 + len;
            }
        }
    }
    if (const std::vector<uint64_t> *es = ifd.getInts(338))
        m.extraSamples = *es;
    if (ifd.has(42113))
    {
        const TiffTag &t = ifd.tags.at(42113);
        m.nodataRaw.assign(t.raw.begin(), t.raw.end());
        if (m.nodataRaw.empty() || m.nodataRaw.back() != '\0')
            m.nodataRaw.push_back('\0');
    }
    if (!ds.bands.empty() && ds.bands[0].hasNodata)
    {
        m.hasNodata = true;
        m.nodata = ds.bands[0].nodata;
    }
    return true;
}

// ------------------------------------------------------------------
// creation options
// ------------------------------------------------------------------

struct CoOpts
{
    int comp = -1;  // -1: inherit container default
    int pred = -1;
    bool predSet = false;
    int zlevel = -1;
    bool zlibFallback = false;  // out-of-range ZLEVEL codec fallback
    int zstdLevel = -1;
    bool zstdFallback = false;  // out-of-range ZSTD_LEVEL clamps to 22
    int jpegQuality = -1;
    int webpLevel = -1;
    bool webpLossless = false;
    bool webpLosslessSet = false;
    int blockSize = 128;
    bool blockSet = false;
    int phot = -1;
    std::string unsupportedCodec;
};

bool isPow2(long v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

void warnUnexpected(const std::string &key, const std::string &val,
                    const std::string &type)
{
    cplErrorStr(CE_Warning, CPLE_NotSupported,
                "'" + val + "' is an unexpected value for " + key +
                    " overview creation option of type " + type + ".");
}

bool parseIntFull(const std::string &s, long &out)
{
    if (s.empty())
        return false;
    char *end = nullptr;
    out = strtol(s.c_str(), &end, 10);
    return end && *end == '\0';
}

bool matchSelect(const std::string &val, const char *const *names, int n,
                 int *idx)
{
    for (int i = 0; i < n; ++i)
        if (strEqualNoCase(val, names[i]))
        {
            if (idx)
                *idx = i;
            return true;
        }
    return false;
}

static const char *const kCompNames[] = {
    "NONE",      "LZW",  "PACKBITS", "JPEG", "CCITTRLE", "CCITTFAX3",
    "CCITTFAX4", "DEFLATE", "ZSTD",  "WEBP", "LZMA"};
static const int kCompCodes[] = {1,     5,     32773, 7,     2,    3,
                                 4,     8,     50000, 50001, 34925};
static const char *const kPhotNames[] = {
    "MINISBLACK", "MINISWHITE", "RGB",    "PALETTE", "CMYK",
    "YCBCR",      "CIELAB",     "ICCLAB", "ITULAB"};
static const int kPhotCodes[] = {1, 0, 2, 3, 5, 6, 8, 9, 10};

void warnRange(const std::string &key, const std::string &val, bool isMin,
               int bound)
{
    cplErrorStr(CE_Warning, CPLE_NotSupported,
                "'" + val + "' is an unexpected value for " + key +
                    " overview creation option that should be " +
                    (isMin ? ">= " : "<= ") + strPrintf("%d", bound) + ".");
}

CoOpts parseCo(const std::vector<std::string> &co)
{
    CoOpts o;
    std::vector<std::pair<std::string, std::string>> kvs;
    // validation pass, in argument order
    for (const auto &kv : co)
    {
        size_t eq = kv.find('=');
        std::string key = strToUpper(eq == std::string::npos
                                         ? kv
                                         : kv.substr(0, eq));
        std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
        kvs.emplace_back(key, val);
        long v;
        if (key == "COMPRESS")
        {
            if (!matchSelect(val, kCompNames, 11, nullptr))
                warnUnexpected(key, val, "string-select");
        }
        else if (key == "PHOTOMETRIC")
        {
            if (!matchSelect(val, kPhotNames, 9, nullptr))
                warnUnexpected(key, val, "string-select");
        }
        else if (key == "INTERLEAVE")
        {
            static const char *const k[] = {"PIXEL", "BAND"};
            if (!matchSelect(val, k, 2, nullptr))
                warnUnexpected(key, val, "string-select");
        }
        else if (key == "BIGTIFF")
        {
            static const char *const k[] = {"YES", "NO", "IF_NEEDED",
                                            "IF_SAFER"};
            if (!matchSelect(val, k, 4, nullptr))
                warnUnexpected(key, val, "string-select");
        }
        else if (key == "PREDICTOR" || key == "WEBP_LEVEL")
        {
            if (!parseIntFull(val, v))
                warnUnexpected(key, val, "int");
        }
        else if (key == "ZLEVEL")
        {
            if (!parseIntFull(val, v))
                warnUnexpected(key, val, "int");
            else if (v < 1)
                warnRange(key, val, true, 1);
            else if (v > 12)
                warnRange(key, val, false, 12);
        }
        else if (key == "ZSTD_LEVEL")
        {
            if (!parseIntFull(val, v))
                warnUnexpected(key, val, "int");
            else if (v < 1)
                warnRange(key, val, true, 1);
            else if (v > 22)
                warnRange(key, val, false, 22);
        }
        else if (key == "JPEG_QUALITY")
        {
            if (!parseIntFull(val, v))
                warnUnexpected(key, val, "int");
            else if (v < 1)
                warnRange(key, val, true, 1);
            else if (v > 100)
                warnRange(key, val, false, 100);
        }
        else if (key == "JPEGTABLESMODE")
        {
            // validated as int but the value itself is ignored: levels
            // always inherit the main dataset's tables mode
            if (!val.empty() && !parseIntFull(val, v))
                warnUnexpected(key, val, "int");
        }
        else if (key == "BLOCKSIZE")
        {
            if (!parseIntFull(val, v))
                warnUnexpected(key, val, "int");
            else if (v < 64)
                warnRange(key, val, true, 64);
        }
        else if (key == "NUM_THREADS")
        {
        }
        else
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "driver GTiff does not support overview creation "
                        "option " +
                            key);
    }
    // consumption pass: first occurrence per key, fixed key order
    auto first = [&kvs](const char *key) -> const std::string * {
        for (const auto &kv : kvs)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    };
    if (const std::string *val = first("COMPRESS"))
    {
        int idx;
        if (matchSelect(*val, kCompNames, 11, &idx))
        {
            o.comp = kCompCodes[idx];
            if (o.comp != 1 && o.comp != 5 && o.comp != 7 &&
                o.comp != 8 && o.comp != 32773 && o.comp != 50000 &&
                o.comp != 50001)
                o.unsupportedCodec = kCompNames[idx];
        }
        else
            cplErrorStr(CE_Warning, CPLE_IllegalArg,
                        "COMPRESS=" + *val +
                            " value not recognised, ignoring.");
    }
    if (const std::string *val = first("INTERLEAVE"))
    {
        if (!strEqualNoCase(*val, "PIXEL") && !strEqualNoCase(*val, "BAND"))
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "INTERLEAVE=" + *val +
                            " unsupported, value must be PIXEL or BAND. "
                            "ignoring");
    }
    if (const std::string *val = first("PHOTOMETRIC"))
    {
        int idx;
        if (matchSelect(*val, kPhotNames, 9, &idx))
            o.phot = kPhotCodes[idx];
        else
            cplErrorStr(CE_Warning, CPLE_IllegalArg,
                        "PHOTOMETRIC=" + *val +
                            " value not recognised, ignoring.");
    }
    if (const std::string *val = first("BLOCKSIZE"))
    {
        o.blockSet = true;
        long v = atol(val->c_str());
        if (v >= 64 && v <= 4096 && isPow2(v))
            o.blockSize = (int)v;
        else
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    "Wrong value for BLOCKSIZE : " + *val +
                        ". Should be a power of 2 between 64 and 4096. "
                        "Defaulting to 128. Further messages of this "
                        "type will be suppressed.");
            }
            o.blockSize = 128;
        }
    }
    if (const std::string *val = first("PREDICTOR"))
    {
        o.pred = atoi(val->c_str());
        o.predSet = true;
    }
    if (const std::string *val = first("ZLEVEL"))
    {
        int z = atoi(val->c_str());
        if (z >= 1 && z <= 12)
            o.zlevel = z;
        else if (z > 12)
            o.zlibFallback = true;
    }
    if (const std::string *val = first("ZSTD_LEVEL"))
    {
        int z = atoi(val->c_str());
        if (z >= 1 && z <= 22)
            o.zstdLevel = z;
        else if (z > 22)
            o.zstdFallback = true;
    }
    if (const std::string *val = first("JPEG_QUALITY"))
    {
        int q = atoi(val->c_str());
        if (q >= 1 && q <= 100)
            o.jpegQuality = q;
    }
    if (const std::string *val = first("WEBP_LEVEL"))
    {
        int q = atoi(val->c_str());
        if (q >= 1 && q <= 100)
            o.webpLevel = q;
    }
    if (const std::string *val = first("WEBP_LOSSLESS"))
    {
        o.webpLosslessSet = true;
        o.webpLossless =
            !(strEqualNoCase(*val, "NO") || strEqualNoCase(*val, "FALSE") ||
              strEqualNoCase(*val, "OFF") || *val == "0");
    }
    return o;
}

// ------------------------------------------------------------------
// resampling kernels
// ------------------------------------------------------------------

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

void storeNative(uint8_t *dst, double v, DType dt)
{
    switch (dt)
    {
        case DType::Byte:
        {
            *dst = (uint8_t)v;
            break;
        }
        case DType::Int8:
        {
            int8_t t = (int8_t)v;
            memcpy(dst, &t, 1);
            break;
        }
        case DType::UInt16:
        {
            uint16_t t = (uint16_t)v;
            memcpy(dst, &t, 2);
            break;
        }
        case DType::Int16:
        {
            int16_t t = (int16_t)v;
            memcpy(dst, &t, 2);
            break;
        }
        case DType::UInt32:
        {
            uint32_t t = (uint32_t)v;
            memcpy(dst, &t, 4);
            break;
        }
        case DType::Int32:
        {
            int32_t t = (int32_t)v;
            memcpy(dst, &t, 4);
            break;
        }
        case DType::Float32:
        {
            float t = (float)v;
            memcpy(dst, &t, 4);
            break;
        }
        case DType::Float64:
        {
            memcpy(dst, &v, 8);
            break;
        }
        default:
            break;
    }
}

struct TileParams
{
    uint32_t comp = 1;
    uint32_t pred = 1;
    int zlevel = 6;
    int zstdLevel = 9;
    int blockSize = 128;
    bool predFail = false;
    bool zlibFallback = false;
    int phot = 1;
    int jpegQuality = 75;
    int jpegMode = 1;  // JPEGTABLESMODE bitmask
    bool jpegWarn = true;
    int webpLevel = 75;
    bool webpLossless = false;
    uint32_t planar = 1;
    bool *jpegPhotErrOnce = nullptr;  // one setup error per run
};

std::vector<uint8_t> zlibDeflateBlock(const std::vector<uint8_t> &in)
{
    unsigned long bound = compressBound((unsigned long)in.size());
    std::vector<uint8_t> out(bound);
    unsigned long n = bound;
    if (compress2(out.data(), &n, in.data(), (unsigned long)in.size(),
                  9) != 0)
        return {};
    out.resize(n);
    return out;
}

// tiles of one level, row-major over the tile grid (planes outermost
// for separate-plane containers)
std::vector<std::vector<uint8_t>> buildTiles(const BandGrid &data, int w,
                                             int h, const MainInfo &m,
                                             const TileParams &tp)
{
    int bs = tp.blockSize;
    int tx = (w + bs - 1) / bs, ty = (h + bs - 1) / bs;
    const int nplanes = (tp.planar == 2 && m.spp > 1) ? (int)m.spp : 1;
    const uint32_t tileSpp = nplanes > 1 ? 1 : m.spp;
    size_t pxBytes = (size_t)tileSpp * m.bytes;
    size_t rowBytes = (size_t)bs * pxBytes;
    bool compressed = tp.comp == 5 || tp.comp == 8 || tp.comp == 50000;
    bool predOk = tp.pred == 2 || tp.pred == 3;
    std::vector<std::vector<uint8_t>> tiles;
    if (tp.predFail)
    {
        tiles.assign((size_t)tx * ty * nplanes, {});
        return tiles;
    }
    if (tp.comp == 7 && tp.phot == 3)
    {
        // the codec refuses the palette at setup; every tile stays
        // unwritten and only the first attempt reports
        if (tp.jpegPhotErrOnce && !*tp.jpegPhotErrOnce)
        {
            *tp.jpegPhotErrOnce = true;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "JPEGSetupEncode:PhotometricInterpretation 3 "
                        "not allowed for JPEG");
        }
        tiles.assign((size_t)tx * ty * nplanes, {});
        return tiles;
    }
    tiles.reserve((size_t)tx * ty * nplanes);
    for (int pl = 0; pl < nplanes; ++pl)
        for (int t = 0; t < tx * ty; ++t)
        {
            int tcol = t % tx, trow = t / tx;
            std::vector<uint8_t> raw(rowBytes * bs, 0);
            int y0 = trow * bs, x0 = tcol * bs;
            int ny = std::min(bs, h - y0), nx = std::min(bs, w - x0);
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x)
                {
                    size_t off =
                        (size_t)y * rowBytes + (size_t)x * pxBytes;
                    for (uint32_t b = 0; b < tileSpp; ++b)
                        storeNative(
                            &raw[off + (size_t)b * m.bytes],
                            data[nplanes > 1 ? pl : b]
                                [(size_t)(y0 + y) * w + (x0 + x)],
                            m.dt);
                }
            if (tp.comp == 7)
            {
                for (int y = 0; y < ny; ++y)
                {
                    uint8_t *row = &raw[(size_t)y * rowBytes];
                    for (int x = nx; x < bs; ++x)
                        memcpy(row + (size_t)x * pxBytes,
                               row + (size_t)(nx - 1) * pxBytes, pxBytes);
                }
                for (int y = ny; y < bs; ++y)
                    memcpy(&raw[(size_t)y * rowBytes],
                           &raw[(size_t)(ny - 1) * rowBytes], rowBytes);
                tiles.push_back(jpegBlock(
                    raw, bs, bs, (int)tileSpp, (uint16_t)tp.phot,
                    nplanes > 1 ? pl : -1, tp.jpegQuality, tp.jpegWarn,
                    tp.jpegMode));
                continue;
            }
            if (tp.comp == 50001)
            {
                bool badBands = tileSpp != 3 && tileSpp != 4;
                if (badBands || m.bps != 8)
                {
                    if (tp.jpegPhotErrOnce && !*tp.jpegPhotErrOnce)
                    {
                        *tp.jpegPhotErrOnce = true;
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            badBands
                                ? strPrintf(
                                      "WebPSetupEncode:WEBP driver "
                                      "doesn't support %d bands. Must "
                                      "be 3 (RGB) or 4 (RGBA) bands.",
                                      (int)tileSpp)
                                : std::string(
                                      "WebPSetupEncode:WEBP driver "
                                      "requires 8 bit unsigned data"));
                    }
                    tiles.push_back({});
                    continue;
                }
                // WEBP overview tiles keep the zero padding beyond the
                // image edge (no replication)
                if (nplanes > 1)
                {
                    std::vector<uint8_t> buf((size_t)bs * bs * m.spp, 0);
                    memcpy(buf.data(), raw.data(), (size_t)bs * bs);
                    tiles.push_back(webpEncodeBlock(
                        buf.data(), bs, bs, (int)m.spp, tp.webpLevel,
                        tp.webpLossless));
                }
                else
                    tiles.push_back(webpEncodeBlock(
                        raw.data(), bs, bs, (int)m.spp, tp.webpLevel,
                        tp.webpLossless));
                continue;
            }
            if (compressed && predOk)
            {
                std::vector<uint8_t> row(rowBytes);
                for (int y = 0; y < bs; ++y)
                {
                    memcpy(row.data(), &raw[(size_t)y * rowBytes],
                           rowBytes);
                    if (tp.pred == 3)
                        gtiffFpDiff(row, m.bytes, (int)tileSpp);
                    else
                        gtiffHorDiff(row, m.bytes, (int)tileSpp);
                    memcpy(&raw[(size_t)y * rowBytes], row.data(),
                           rowBytes);
                }
            }
            if (tp.comp == 8 && tp.zlibFallback)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "ZIPEncode:Cannot allocate compressor");
                tiles.push_back(zlibDeflateBlock(raw));
            }
            else if (tp.comp == 8)
                tiles.push_back(gtiffDeflateBlock(raw, tp.zlevel));
            else if (tp.comp == 50000)
                tiles.push_back(gtiffZstdBlock(raw, tp.zstdLevel));
            else if (tp.comp == 5)
                tiles.push_back(gtiffLzwEncode(raw.data(), raw.size()));
            else if (tp.comp == 32773)
                tiles.push_back(gtiffPackbitsEncode(raw.data(), raw.size(),
                                                    rowBytes));
            else
                tiles.push_back(std::move(raw));
        }
    return tiles;
}

// lossy levels cascade from what the reader would see: re-encode the
// computed grid and decode it back
void jpegRoundTripGrid(BandGrid &grid, int w, int h, const MainInfo &m,
                       const TileParams &tp)
{
    TileParams etp = tp;
    etp.jpegWarn = false;
    auto tiles = buildTiles(grid, w, h, m, etp);
    std::vector<std::vector<uint16_t>> qts;
    qts.push_back(jpegQuantTable(false, tp.jpegQuality));
    if (tp.phot == 6)
        qts.push_back(jpegQuantTable(true, tp.jpegQuality));
    std::vector<uint8_t> tables = jpegTablesStream(
        qts, (tp.jpegMode & 2) != 0, (tp.jpegMode & 1) != 0);
    int bs = tp.blockSize;
    int tx = (w + bs - 1) / bs, ty = (h + bs - 1) / bs;
    int nplanes = (tp.planar == 2 && m.spp > 1) ? (int)m.spp : 1;
    for (int pl = 0; pl < nplanes; ++pl)
        for (int t = 0; t < tx * ty; ++t)
        {
            const auto &blob = tiles[(size_t)pl * tx * ty + t];
            if (blob.empty())
                continue;
            JpegDecoded dec;
            if (!jpegDecodeStream(blob.data(), blob.size(), tables.data(),
                                  tables.size(), tp.phot == 6, dec))
                continue;
            int x0 = (t % tx) * bs, y0 = (t / tx) * bs;
            int nx = std::min(bs, w - x0), ny = std::min(bs, h - y0);
            int nb = nplanes > 1 ? 1 : (int)m.spp;
            if (dec.ncomp < nb || dec.w < nx || dec.h < ny)
                continue;
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x)
                    for (int b = 0; b < nb; ++b)
                        grid[nplanes > 1 ? pl : b]
                            [(size_t)(y0 + y) * w + (x0 + x)] =
                            dec.pixels[((size_t)y * dec.w + x) *
                                           dec.ncomp +
                                       b];
        }
}

void webpRoundTripGrid(BandGrid &grid, int w, int h, const MainInfo &m,
                       const TileParams &tp)
{
    auto tiles = buildTiles(grid, w, h, m, tp);
    int bs = tp.blockSize;
    int tx = (w + bs - 1) / bs, ty = (h + bs - 1) / bs;
    int nb = (int)m.spp;
    int fileSpp = nb == 4 ? 4 : 3;
    std::vector<uint8_t> dec((size_t)bs * bs * fileSpp);
    for (int t = 0; t < tx * ty; ++t)
    {
        const auto &blob = tiles[t];
        if (blob.empty() ||
            !webpDecodeBlock(blob.data(), blob.size(), bs, bs, fileSpp,
                             dec.data()))
            continue;
        int x0 = (t % tx) * bs, y0 = (t / tx) * bs;
        int nx = std::min(bs, w - x0), ny = std::min(bs, h - y0);
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                for (int b = 0; b < nb; ++b)
                    grid[b][(size_t)(y0 + y) * w + (x0 + x)] =
                        dec[((size_t)y * bs + x) * fileSpp + b];
    }
}

std::string metaXml(const std::string &method, bool pretty,
                    bool grayInterp = false)
{
    std::string up = strToUpper(method);
    if (pretty)
        return "<GDALMetadata>\n  <Item name=\"RESAMPLING\" "
               "sample=\"0\">" +
               up + "</Item>\n" +
               (grayInterp ? "  <Item name=\"COLORINTERP\" "
                             "sample=\"0\" "
                             "role=\"colorinterp\">Gray</Item>\n"
                           : "") +
               "</GDALMetadata>\n";
    if (grayInterp)
        return "<GDALMetadata><Item name=\"RESAMPLING\" sample=\"0\">" +
               up +
               "</Item><Item sample=\"0\" name=\"COLORINTERP\" "
               "role=\"colorinterp\">Gray</Item></GDALMetadata>";
    return "<GDALMetadata><Item name=\"RESAMPLING\" sample=\"0\">" + up +
           "</Item></GDALMetadata>";
}

struct TagOut
{
    uint16_t id, type;
    uint32_t count;
    std::vector<uint8_t> data;
};

void put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}
void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back((uint8_t)(x >> (8 * i)));
}
void put64(std::vector<uint8_t> &v, uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v.push_back((uint8_t)(x >> (8 * i)));
}
bool isBigTiff(const std::vector<uint8_t> &d)
{
    return d.size() > 3 && d[2] == 43;
}

TagOut mkShorts(uint16_t id, const std::vector<uint16_t> &vals)
{
    TagOut t{id, 3, (uint32_t)vals.size(), {}};
    for (uint16_t v : vals)
        put16(t.data, v);
    return t;
}
TagOut mkLong(uint16_t id, uint32_t v)
{
    TagOut t{id, 4, 1, {}};
    put32(t.data, v);
    return t;
}
TagOut mkAscii(uint16_t id, const std::string &s, bool hasNul)
{
    TagOut t{id, 2, 0, {}};
    t.data.assign(s.begin(), s.end());
    if (!hasNul)
        t.data.push_back('\0');
    t.count = (uint32_t)t.data.size();
    return t;
}

// one overview IFD: tag list in id order
std::vector<TagOut> buildOvrTags(const MainInfo &m, int w, int h,
                                 const std::vector<uint64_t> &tileCounts,
                                 const std::vector<uint64_t> &tileOffs,
                                 const std::string &metaStr,
                                 const TileParams &tp, int phot,
                                 bool bt = false)
{
    std::vector<TagOut> tags;
    tags.push_back(mkLong(254, 1));
    tags.push_back(mkShorts(256, {(uint16_t)w}));
    tags.push_back(mkShorts(257, {(uint16_t)h}));
    tags.push_back(mkShorts(
        258, std::vector<uint16_t>(m.spp, (uint16_t)m.bps)));
    tags.push_back(mkShorts(259, {(uint16_t)tp.comp}));
    tags.push_back(mkShorts(262, {(uint16_t)phot}));
    tags.push_back(mkShorts(277, {(uint16_t)m.spp}));
    tags.push_back(mkShorts(
        284, {(uint16_t)((tp.planar == 2 && m.spp > 1) ? 2 : 1)}));
    if (tp.comp == 5 || tp.comp == 8 || tp.comp == 50000)
        tags.push_back(mkShorts(317, {(uint16_t)tp.pred}));
    if (!m.colorMap.empty() && phot == 3)
    {
        std::vector<uint16_t> cm;
        cm.reserve(m.colorMap.size());
        for (uint64_t v : m.colorMap)
            cm.push_back((uint16_t)v);
        tags.push_back(mkShorts(320, cm));
    }
    tags.push_back(mkShorts(322, {(uint16_t)tp.blockSize}));
    tags.push_back(mkShorts(323, {(uint16_t)tp.blockSize}));
    {
        TagOut t{324, (uint16_t)(bt ? 16 : 4), (uint32_t)tileOffs.size(),
                 {}};
        for (uint64_t v : tileOffs)
        {
            if (bt)
                put64(t.data, v);
            else
                put32(t.data, (uint32_t)v);
        }
        tags.push_back(t);
    }
    {
        uint64_t mx = 0;
        for (uint64_t v : tileCounts)
            mx = std::max(mx, v);
        // libtiff emits SHORT counts only when they are predictable
        // (uncompressed) and the array is out-of-line
        if (tp.comp == 1 && tileCounts.size() > 1 && mx <= 0xFFFF)
        {
            std::vector<uint16_t> sc;
            for (uint64_t v : tileCounts)
                sc.push_back((uint16_t)v);
            tags.push_back(mkShorts(325, sc));
        }
        else if (bt && (tileCounts.size() <= 1 ||
                        !(tp.comp == 1 || tp.comp == 5 || tp.comp == 8 ||
                          tp.comp == 50000 || tp.comp == 50001)))
        {
            TagOut t{325, 16, (uint32_t)tileCounts.size(), {}};
            for (uint64_t v : tileCounts)
                put64(t.data, v);
            tags.push_back(t);
        }
        else
        {
            TagOut t{325, 4, (uint32_t)tileCounts.size(), {}};
            for (uint64_t v : tileCounts)
                put32(t.data, (uint32_t)v);
            tags.push_back(t);
        }
    }
    if (!m.extraSamples.empty())
    {
        std::vector<uint16_t> es;
        for (uint64_t v : m.extraSamples)
            es.push_back((uint16_t)v);
        tags.push_back(mkShorts(338, es));
    }
    tags.push_back(mkShorts(
        339, std::vector<uint16_t>(m.spp, (uint16_t)m.sf)));
    if (tp.comp == 7)
    {
        if (tp.jpegWarn &&
            jpegCoarseTables((uint16_t)phot, (int)m.spp, tp.jpegQuality))
            jpegCoarseWarn();
        if (tp.jpegMode & 3)
        {
            std::vector<std::vector<uint16_t>> qts;
            qts.push_back(jpegQuantTable(false, tp.jpegQuality));
            if (phot == 6)
                qts.push_back(jpegQuantTable(true, tp.jpegQuality));
            TagOut jt{347, 7, 0,
                      jpegTablesStream(qts, (tp.jpegMode & 2) != 0,
                                       (tp.jpegMode & 1) != 0)};
            jt.count = (uint32_t)jt.data.size();
            tags.push_back(jt);
        }
        if (phot == 6)
        {
            tags.push_back(mkShorts(530, {2, 2}));
            if (!m.refBWRaw.empty())
                tags.push_back(TagOut{
                    532, 5, (uint32_t)(m.refBWRaw.size() / 8),
                    m.refBWRaw});
        }
    }
    if (!metaStr.empty())
        tags.push_back(mkAscii(42112, metaStr, false));
    if (!m.nodataRaw.empty())
        tags.push_back(mkAscii(42113, m.nodataRaw, true));
    std::sort(tags.begin(), tags.end(),
              [](const TagOut &a, const TagOut &b) { return a.id < b.id; });
    return tags;
}

// serializes IFDs + their out-of-line data + tile payloads at the end of
// `file`; returns the offset of the first IFD written
uint64_t appendIfds(std::vector<uint8_t> &file,
                    std::vector<std::vector<TagOut>> &ifds,
                    const std::vector<std::vector<std::vector<uint8_t>>>
                        &tilesPerIfd,
                    std::vector<std::vector<uint64_t>> *tileOffsOut,
                    int nplanes = 1)
{
    // strile data blobs go counts-then-offsets in id order; the color
    // table (set on the band after the directory exists) lands after
    // all standard blobs, the codec's JPEGTables after that, and the
    // GDAL ascii tags last
    auto blobOrder = [](const std::vector<TagOut> &tags)
    {
        std::vector<size_t> idx(tags.size());
        for (size_t i = 0; i < idx.size(); ++i)
            idx[i] = i;
        std::sort(idx.begin(), idx.end(),
                  [&](size_t a, size_t b)
                  {
                      uint16_t ia = tags[a].id, ib = tags[b].id;
                      auto key = [](uint16_t id)
                      {
                          if (id == 325)
                              return 324 * 10 - 1;
                          if (id == 320)
                              return 500000;
                          if (id == 347)
                              return 600000;
                          if (id >= 42112)
                              return 700000 + (int)id;
                          return (int)id * 10;
                      };
                      return key(ia) < key(ib);
                  });
        return idx;
    };
    const bool bt = isBigTiff(file);
    const size_t entSize = bt ? 20 : 12;
    const size_t cntSize = bt ? 8 : 2;
    const size_t ptrSize = bt ? 8 : 4;
    const size_t inlineMax = bt ? 8 : 4;
    // layout pass
    if (file.size() & 1)
        file.push_back(0);
    uint64_t pos = file.size();
    std::vector<uint64_t> ifdOff(ifds.size());
    std::vector<std::map<size_t, uint64_t>> blobOff(ifds.size());
    for (size_t k = 0; k < ifds.size(); ++k)
    {
        if (pos & 1)
            pos++;
        ifdOff[k] = pos;
        pos += cntSize + ifds[k].size() * entSize + ptrSize;
        for (size_t i : blobOrder(ifds[k]))
        {
            if (ifds[k][i].data.size() > inlineMax)
            {
                if (pos & 1)
                    pos++;
                blobOff[k][i] = pos;
                pos += ifds[k][i].data.size();
            }
        }
    }
    // tile data: regeneration walks band by band, so separate-plane
    // containers interleave each plane's tiles across all the levels
    std::vector<std::vector<uint64_t>> allOffs(ifds.size());
    for (size_t k = 0; k < ifds.size(); ++k)
        allOffs[k].assign(tilesPerIfd[k].size(), 0);
    if (nplanes < 1)
        nplanes = 1;
    for (int pl = 0; pl < nplanes; ++pl)
        for (size_t k = 0; k < ifds.size(); ++k)
        {
            size_t total = tilesPerIfd[k].size();
            size_t perPlane = total / (size_t)nplanes;
            for (size_t t = perPlane * pl; t < perPlane * (pl + 1); ++t)
            {
                allOffs[k][t] = tilesPerIfd[k][t].empty() ? 0 : pos;
                pos += tilesPerIfd[k][t].size();
            }
        }
    for (size_t k = 0; k < ifds.size(); ++k)
    {
        if (tilesPerIfd[k].empty())
            continue;
        if (tileOffsOut)
            (*tileOffsOut)[k] = allOffs[k];
        // patch the 324 tag values
        for (auto &tag : ifds[k])
            if (tag.id == 324)
            {
                tag.data.clear();
                for (uint64_t v : allOffs[k])
                {
                    if (tag.type == 16)
                        put64(tag.data, v);
                    else
                        put32(tag.data, (uint32_t)v);
                }
            }
    }
    // serialize
    for (size_t k = 0; k < ifds.size(); ++k)
    {
        while (file.size() < ifdOff[k])
            file.push_back(0);
        std::vector<uint8_t> blk;
        if (bt)
            put64(blk, ifds[k].size());
        else
            put16(blk, (uint16_t)ifds[k].size());
        for (size_t i = 0; i < ifds[k].size(); ++i)
        {
            const TagOut &t = ifds[k][i];
            put16(blk, t.id);
            put16(blk, t.type);
            if (bt)
                put64(blk, t.count);
            else
                put32(blk, t.count);
            if (t.data.size() <= inlineMax)
            {
                std::vector<uint8_t> d = t.data;
                d.resize(inlineMax, 0);
                blk.insert(blk.end(), d.begin(), d.end());
            }
            else if (bt)
                put64(blk, blobOff[k][i]);
            else
                put32(blk, (uint32_t)blobOff[k][i]);
        }
        uint64_t nxt = k + 1 < ifds.size() ? ifdOff[k + 1] : 0;
        if (bt)
            put64(blk, nxt);
        else
            put32(blk, (uint32_t)nxt);
        file.insert(file.end(), blk.begin(), blk.end());
        for (size_t i : blobOrder(ifds[k]))
            if (ifds[k][i].data.size() > inlineMax)
            {
                while (file.size() < blobOff[k][i])
                    file.push_back(0);
                file.insert(file.end(), ifds[k][i].data.begin(),
                            ifds[k][i].data.end());
            }
    }
    for (int pl = 0; pl < nplanes; ++pl)
        for (size_t k = 0; k < ifds.size(); ++k)
        {
            size_t total = tilesPerIfd[k].size();
            size_t perPlane = total / (size_t)nplanes;
            for (size_t t = perPlane * pl; t < perPlane * (pl + 1); ++t)
                file.insert(file.end(), tilesPerIfd[k][t].begin(),
                            tilesPerIfd[k][t].end());
        }
    return ifdOff[0];
}

bool loadWhole(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    bool ok = n == 0 || fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

bool saveWhole(const std::string &path, const std::vector<uint8_t> &data)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) ==
                                  data.size();
    fclose(f);
    return ok;
}

uint32_t rd32(const std::vector<uint8_t> &d, uint64_t off)
{
    uint32_t v;
    memcpy(&v, &d[off], 4);
    return v;
}
uint16_t rd16(const std::vector<uint8_t> &d, uint64_t off)
{
    uint16_t v;
    memcpy(&v, &d[off], 2);
    return v;
}
void wr32(std::vector<uint8_t> &d, uint64_t off, uint32_t v)
{
    memcpy(&d[off], &v, 4);
}
uint64_t rd64(const std::vector<uint8_t> &d, uint64_t off)
{
    uint64_t v;
    memcpy(&v, &d[off], 8);
    return v;
}
// IFD-pointer slot access parameterized on the container format
uint64_t rdPtr(const std::vector<uint8_t> &d, uint64_t off)
{
    return isBigTiff(d) ? rd64(d, off) : rd32(d, off);
}
void wrPtr(std::vector<uint8_t> &d, uint64_t off, uint64_t v)
{
    if (isBigTiff(d))
        memcpy(&d[off], &v, 8);
    else
        wr32(d, off, (uint32_t)v);
}
uint64_t firstIfdSlot(const std::vector<uint8_t> &d)
{
    return isBigTiff(d) ? 8 : 4;
}
uint64_t dirCount(const std::vector<uint8_t> &d, uint64_t off)
{
    return isBigTiff(d) ? rd64(d, off) : rd16(d, off);
}
uint64_t nextSlotOf(const std::vector<uint8_t> &d, uint64_t off)
{
    return isBigTiff(d) ? off + 8 + dirCount(d, off) * 20
                        : off + 2 + dirCount(d, off) * 12;
}

// file offset of the next-IFD slot at the end of the chain
uint64_t chainTailSlot(const std::vector<uint8_t> &d)
{
    uint64_t slot = firstIfdSlot(d);
    uint64_t off = rdPtr(d, slot);
    int guard = 0;
    while (off && guard++ < 1000)
    {
        slot = nextSlotOf(d, off);
        off = rdPtr(d, slot);
    }
    return slot;
}

// replaces the chain entry pointing at oldOff with a new IFD written at
// EOF, keeping the old successor (or an explicitly remembered one when
// the old slot has since been repointed at unlinked directories)
void spliceRewrittenIfd(std::vector<uint8_t> &file, uint64_t oldOff,
                        std::vector<TagOut> tags,
                        uint64_t nextAfter = UINT64_MAX)
{
    uint64_t prevSlot = firstIfdSlot(file);
    uint64_t off = rdPtr(file, prevSlot);
    while (off)
    {
        uint64_t slot = nextSlotOf(file, off);
        uint64_t nxt = rdPtr(file, slot);
        if (off == oldOff)
        {
            if (nextAfter == UINT64_MAX)
                nextAfter = nxt;
            break;
        }
        prevSlot = slot;
        off = nxt;
    }
    if (nextAfter == UINT64_MAX)
        nextAfter = 0;
    std::vector<std::vector<TagOut>> one;
    one.push_back(std::move(tags));
    std::vector<std::vector<std::vector<uint8_t>>> noTiles(1);
    uint64_t at = appendIfds(file, one, noTiles, nullptr);
    wrPtr(file, prevSlot, at);
    wrPtr(file, nextSlotOf(file, at), nextAfter);
}

struct Level
{
    int factor = 0;
    int w = 0, h = 0;
    bool reuse = false;
    int page = 0;  // 1-based IFD index in the container when reusing
    BandGrid data;
};

std::string storedResampling(const TiffIfd &ifd)
{
    std::string md = ifd.getAscii(42112);
    size_t p = md.find("name=\"RESAMPLING\"");
    if (p == std::string::npos)
        return "";
    size_t gt = md.find('>', p);
    size_t lt = md.find('<', gt);
    if (gt == std::string::npos || lt == std::string::npos)
        return "";
    return strToLower(md.substr(gt + 1, lt - gt - 1));
}

int overviewError(const std::string &msg)
{
    cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
    return 1;
}

std::unique_ptr<RasterDatasetBase> openInput(const std::string &input,
                                             int &rc)
{
    std::string err;
    std::unique_ptr<RasterDatasetBase> ds = openRaster(input, err);
    if (!ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        rc = 1;
    }
    return ds;
}

void emitScanBlock(RasterDatasetBase &ds, bool overviewScan)
{
    if (ds.driverShort == "GTiff")
        cplDebug("GTiff", "ScanDirectories()");
    ds.replayDeferred();
    if (overviewScan && ds.overviews.empty())
        ds.debugOverviewScan();
}

static double gProgressStartTime = 0;

static double progressNow()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

void progressStart(bool quiet)
{
    if (!quiet)
    {
        gProgressStartTime = progressNow();
        fputs("0", stdout);
        fflush(stdout);
    }
    gdalDebugCacheMaxOnce();
}

void progressEnd(bool quiet)
{
    if (!quiet)
    {
        fputs("...10...20...30...40...50...60...70...80...90...100",
              stdout);
        double elapsed = progressNow() - gProgressStartTime;
        if (elapsed >= 5.0)
        {
            unsigned s = (unsigned)elapsed;
            printf(" - done in %02u:%02u:%02u.\n", s / 3600,
                   (s % 3600) / 60, s % 60);
        }
        else
            fputs(" - done.\n", stdout);
        fflush(stdout);
    }
}

bool readBaseGrids(RasterDatasetBase &ds, BandGrid &out)
{
    out.resize(ds.bands.size());
    for (size_t b = 0; b < ds.bands.size(); ++b)
        if (!ds.readBand((int)b + 1, out[b]))
            return false;
    return true;
}

int factorOfEntry(int mainW, int ovrW)
{
    return ovrW > 0 ? (mainW + ovrW - 1) / ovrW : 0;
}

const char *kExternalOpenMsg =
    "File open for read-only accessing, creating overviews externally.";

void emitOvrPassTraces(const std::string &target, const std::string &ptr,
                       const std::vector<std::pair<int, int>> &pageDims,
                       bool withScanDirs)
{
    cplDebug("GDAL",
             "GDALOpen(" + target + ", this=" + ptr + ") succeeds as GTiff.");
    if (withScanDirs)
        cplDebug("GTiff", "ScanDirectories()");
    if (pageDims.size() <= 1)
        cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
    else
        for (size_t i = 1; i < pageDims.size(); ++i)
            cplDebug("GTiff", strPrintf("Opened %dx%d overview.",
                                        pageDims[i].first,
                                        pageDims[i].second));
}

// shared build core for add and refresh
struct GenRun
{
    RasterDatasetBase *ds = nullptr;
    TiffFile *tf = nullptr;
    MainInfo m;
    std::string input;
    bool external = false;
    bool quiet = false;
    bool rSet = false;
    std::string resamp;  // effective explicit -r (lower case)
    std::vector<int> factors;
    CoOpts co;
    bool refreshMode = false;
    std::vector<int> refreshIdx;  // displayed indices, for debug lines
};

int runGenerate(GenRun &g)
{
    RasterDatasetBase &ds = *g.ds;
    const MainInfo &m = g.m;

    // per-container existing level list
    const std::vector<RasterDatasetBase::OvrEntry> &existing =
        g.external ? ds.extOverviews : ds.overviews;

    std::vector<Level> levels;
    for (int f : g.factors)
    {
        Level lv;
        lv.factor = f;
        lv.w = (m.w + f - 1) / f;
        lv.h = (m.h + f - 1) / f;
        for (const auto &e : existing)
            if (e.w == lv.w && e.h == lv.h)
            {
                lv.reuse = true;
                lv.page = e.page;
            }
        levels.push_back(lv);
    }

    if (g.refreshMode)
        for (int i : g.refreshIdx)
            cplDebug("GDAL", strPrintf("Refreshing overview idx %d", i));

    // resampling method resolution: an explicit -r wins; otherwise the
    // first displayed overview's stored method applies to the whole run
    TiffFile dispTf;
    bool haveDispTf = false;
    const std::vector<RasterDatasetBase::OvrEntry> &displayed =
        ds.dispOverviews();
    bool dispExternal = ds.overviews.empty() && !ds.extOverviews.empty();
    std::string reusedMeth;
    if (!g.rSet)
        for (const auto &e : displayed)
        {
            std::string s;
            if (dispExternal)
            {
                std::string oerr;
                if (!haveDispTf)
                    haveDispTf =
                        TiffFile::open(ds.extOvrPath, dispTf, oerr);
                if (haveDispTf && e.page - 1 < (int)dispTf.ifds.size())
                    s = storedResampling(dispTf.ifds[e.page - 1]);
            }
            else if (g.tf && e.page - 1 < (int)g.tf->ifds.size())
                s = storedResampling(g.tf->ifds[e.page - 1]);
            if (!s.empty())
            {
                reusedMeth = s;
                break;
            }
        }
    std::string effMeth =
        g.rSet || reusedMeth.empty() ? g.resamp : reusedMeth;
    if (!g.rSet && !reusedMeth.empty())
        cplDebug("GDAL", "Reusing resampling method " +
                             strToUpper(reusedMeth) +
                             " from existing overview");

    if (g.external)
    {
        cplDebug("GTiff", kExternalOpenMsg);
        if (!ds.overviews.empty())
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        g.input + ": Cannot add external overviews when "
                                  "there are already internal overviews");
            debugCloseDataset(ds);
            return 1;
        }
    }

    if (levels.empty())
    {
        debugCloseDataset(ds);
        return 0;
    }

    if (!g.co.unsupportedCodec.empty())
    {
        debugCloseDataset(ds);
        return overviewError("add: COMPRESS=" + g.co.unsupportedCodec +
                             " is not supported in this build");
    }

    // container defaults: internal inherits the main IFD codec, external
    // defaults to uncompressed
    TileParams newTp;
    if (!g.external)
    {
        newTp.comp = m.comp;
        newTp.pred = m.pred;
    }
    if (g.co.comp >= 0)
        newTp.comp = (uint32_t)g.co.comp;
    if (g.co.predSet)
        newTp.pred = (uint32_t)g.co.pred;
    // externally the codec is initialized when the .ovr container is
    // opened, so ZLEVEL never reaches it (no fallback errors either)
    if (g.co.zlevel >= 0 && !g.external)
        newTp.zlevel = g.co.zlevel;
    newTp.zlibFallback = g.co.zlibFallback && !g.external;
    newTp.blockSize = g.co.blockSize;
    if (!g.co.blockSet && !ds.bands.empty())
    {
        // default overview block size follows the source band block
        // (post strip-chop) when it is a square power of two in 64..4096
        int bx = ds.bands[0].blockX, by = ds.bands[0].blockY;
        if (bx == by && bx >= 64 && bx <= 4096 && isPow2(bx))
            newTp.blockSize = bx;
    }
    if (newTp.comp == 1)
        newTp.pred = 1;
    bool anyNew = false;
    for (const auto &lv : levels)
        if (!lv.reuse)
            anyNew = true;
    if (newTp.comp == 8 && newTp.zlibFallback)
        for (const auto &lv : levels)
            if (!lv.reuse)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "ZIPVSetField:Invalid ZipQuality value. "
                            "Should be in [-1,12] range");
    // ZSTD_LEVEL reaches the codec only when the option pipeline
    // (re)initializes it: external fresh containers, or an internal add
    // whose codec differs from the main IFD's; out-of-range clamps to 22
    // with codec warnings (external: two fixed; internal: one per level)
    if (newTp.comp == 50000 && (g.external || m.comp != 50000))
    {
        if (g.co.zstdLevel >= 0)
            newTp.zstdLevel = g.co.zstdLevel;
        if (g.co.zstdFallback)
        {
            newTp.zstdLevel = 22;
            int warnCount = 2;
            if (!g.external)
            {
                warnCount = 0;
                for (const auto &lv : levels)
                    if (!lv.reuse)
                        warnCount++;
            }
            for (int i = 0; i < warnCount; ++i)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "ZSTDVSetField:ZSTD_LEVEL should be between "
                            "1 and 22");
        }
    }
    if (anyNew &&
        (newTp.comp == 5 || newTp.comp == 8 || newTp.comp == 50000) &&
        newTp.pred != 1)
    {
        bool floatFmt =
            m.dt == DType::Float32 || m.dt == DType::Float64;
        if (newTp.pred != 2 && newTp.pred != 3)
        {
            newTp.predFail = true;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("PredictorSetup:\"Predictor\" value %d "
                                  "not supported",
                                  (int)(int32_t)newTp.pred));
        }
        else if (newTp.pred == 3 && !floatFmt)
        {
            int sf = (m.dt == DType::Int8 || m.dt == DType::Int16 ||
                      m.dt == DType::Int32)
                         ? 2
                         : 1;
            newTp.predFail = true;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("PredictorSetup:Floating point "
                                  "\"Predictor\" not supported with %d "
                                  "data format",
                                  sf));
        }
    }
    // external containers are always band-separate and never carry the
    // YCbCr color space
    int newPhot = g.co.phot >= 0
                      ? g.co.phot
                      : (m.hasCt ? 3
                                 : (g.external && m.phot == 6
                                        ? 2
                                        : (int)m.phot));
    // external containers are band-separate, except WEBP whose codec
    // only accepts contiguous storage
    newTp.planar = g.external
                       ? ((m.spp > 1 && newTp.comp != 50001) ? 2u : 1u)
                       : m.planar;
    if (newTp.comp != 1 && newTp.comp != 5 && newTp.comp != 7 &&
        newTp.comp != 8 && newTp.comp != 32773 && newTp.comp != 50000 &&
        newTp.comp != 50001)
    {
        debugCloseDataset(ds);
        return overviewError(
            "add: this compression is not supported in this build");
    }
    // JPEG parameters: internal levels inherit the main dataset's
    // recovered quality and tables mode (mode 3 when the main isn't
    // JPEG); external containers re-init with quant-only tables at the
    // option default
    bool jpegPhotErrEmitted = false;
    newTp.phot = newPhot;
    newTp.jpegPhotErrOnce = &jpegPhotErrEmitted;
    if (g.co.jpegQuality > 0)
        newTp.jpegQuality = g.co.jpegQuality;
    else if (!g.external && m.jpegQuality > 0)
        newTp.jpegQuality = m.jpegQuality;
    newTp.jpegMode = g.external ? 1 : m.comp == 7 ? m.jpegTablesMode : 3;
    if (g.co.webpLevel > 0)
        newTp.webpLevel = g.co.webpLevel;
    else if (!g.external && m.webpLevel > 0)
        newTp.webpLevel = m.webpLevel;
    newTp.webpLossless = g.co.webpLosslessSet
                             ? g.co.webpLossless
                             : (!g.external && m.webpLossless);

    // target container state
    std::string target = g.external ? g.input + ".ovr" : g.input;
    std::vector<uint8_t> file;
    bool freshOvr = false;
    if (g.external)
    {
        struct stat st;
        if (stat(target.c_str(), &st) != 0)
        {
            freshOvr = true;
            file = {'I', 'I', 42, 0, 0, 0, 0, 0};
        }
        else if (!loadWhole(target, file))
        {
            debugCloseDataset(ds);
            return overviewError("add: cannot read " + target);
        }
    }
    else if (!loadWhole(target, file))
    {
        debugCloseDataset(ds);
        return overviewError("add: cannot read " + target);
    }

    TiffFile curTf;
    std::vector<std::pair<int, int>> oldDims;
    if (g.external)
    {
        if (!freshOvr)
        {
            std::string oerr;
            if (!TiffFile::open(target, curTf, oerr))
            {
                debugCloseDataset(ds);
                return overviewError("add: cannot parse " + target);
            }
            for (const auto &pg : curTf.ifds)
                oldDims.emplace_back((int)pg.getInt(256),
                                     (int)pg.getInt(257));
        }
    }
    else if (g.tf)
        curTf = *g.tf;

    // external trace choreography around the write
    std::string ovrPtr;
    if (g.external)
    {
        if (ds.extOvrOpened)
        {
            cplDebug("GDAL", "GDALClose(" + target + ", this=" +
                                 ds.extOvrDebugPtr + ")");
            ds.extOvrOpened = false;
        }
        if (!freshOvr)
        {
            std::string p = cplDebugPtr();
            emitOvrPassTraces(target, p, oldDims, true);
            cplDebug("GDAL", "GDALClose(" + target + ", this=" + p + ")");
        }
    }

    // compute pixel grids: cascade across the requested levels; the
    // reference sources nearest external levels from the base each time
    BandGrid base;
    if (!readBaseGrids(ds, base))
    {
        debugCloseDataset(ds);
        return overviewError("add: read error");
    }
    // uncompressed external containers regenerate band-by-band, which
    // sources every nearest level from the base; all other combinations
    // cascade from the previous level of the run
    // separate-plane containers regenerate per band in one pass over the
    // base, so every level is sourced from the main grid
    bool fromMainAlways =
        (g.external && newTp.comp == 1 && effMeth == "nearest") ||
        (m.planar == 2 && m.spp > 1 && effMeth == "nearest");
    {
        const BandGrid *src = &base;
        int sw = m.w, sh = m.h;
        BandGrid jpegSrc;
        for (auto &lv : levels)
        {
            lv.data.resize(m.spp);
            for (uint32_t b = 0; b < m.spp; ++b)
            {
                if (fromMainAlways)
                    resampleBand(base[b], m.w, m.h, lv.data[b], lv.w,
                                 lv.h, effMeth, m);
                else
                    resampleBand((*src)[b], sw, sh, lv.data[b], lv.w,
                                 lv.h, effMeth, m);
                // cascade reads back stored values: quantize like the
                // on-disk representation
                if (m.dt == DType::Float32)
                    for (double &v : lv.data[b])
                        v = (double)(float)v;
            }
            // the per-band regenerators (single-band and separate-plane
            // mains) reach the stored (lossy) level through the file;
            // the multi-band contiguous one still sees the raw computed
            // blocks in the cache -- except when the level spans more
            // than one tile, where the next level reads stored values
            int ltx = (lv.w + newTp.blockSize - 1) / newTp.blockSize;
            int lty = (lv.h + newTp.blockSize - 1) / newTp.blockSize;
            bool multiTile = ltx * lty > 1;
            if (newTp.comp == 7 && newPhot != 3 &&
                (m.spp == 1 || m.planar == 2 || multiTile) &&
                &lv != &levels.back())
            {
                jpegSrc = lv.data;
                jpegRoundTripGrid(jpegSrc, lv.w, lv.h, m, newTp);
                src = &jpegSrc;
            }
            else if (newTp.comp == 50001 && !newTp.webpLossless &&
                     m.spp >= 3 && m.planar == 1 && multiTile &&
                     &lv != &levels.back())
            {
                jpegSrc = lv.data;
                webpRoundTripGrid(jpegSrc, lv.w, lv.h, m, newTp);
                src = &jpegSrc;
            }
            else
                src = &lv.data;
            sw = lv.w;
            sh = lv.h;
        }
    }

    // write phase: reused levels rewrite tiles in place (with an IFD
    // rewrite at EOF on a method change); new levels are appended
    std::vector<std::vector<TagOut>> newIfds;
    std::vector<std::vector<std::vector<uint8_t>>> newTiles;
    size_t newCount = 0;
    std::vector<std::pair<int, int>> newDims = oldDims;
    if (!g.external)
        newDims.clear();
    // a run that changes the resampling of an existing level only
    // regenerates; requested levels that don't exist yet are dropped
    bool anyReuseMethodChange = false;
    for (const auto &lv : levels)
        if (lv.reuse &&
            storedResampling(curTf.ifds[lv.page - 1]) != effMeth)
            anyReuseMethodChange = true;
    struct ReuseWork
    {
        uint64_t ifdOff;
        TileParams rp;
        std::vector<std::vector<uint8_t>> tiles;
        std::vector<uint64_t> nOffs, nCnts;
        int w, h, oldPhot;
        bool methodChanged;
        uint64_t origNext;
    };
    std::vector<ReuseWork> reuseWork;
    for (auto &lv : levels)
    {
        if (lv.reuse)
        {
            const TiffIfd &oifd = curTf.ifds[lv.page - 1];
            TileParams rp;
            rp.comp = (uint32_t)oifd.getInt(259, 1);
            rp.pred = (uint32_t)oifd.getInt(317, 1);
            rp.blockSize = (int)oifd.getInt(322, 128);
            if (g.co.zlevel >= 0 && !g.external)
                rp.zlevel = g.co.zlevel;
            if (g.co.zstdLevel >= 0 && !g.external)
                rp.zstdLevel = g.co.zstdLevel;
            rp.phot = (int)oifd.getInt(262, 1);
            rp.jpegQuality = newTp.jpegQuality;
            rp.jpegMode = newTp.jpegMode;
            rp.webpLevel = newTp.webpLevel;
            rp.webpLossless = newTp.webpLossless;
            rp.jpegPhotErrOnce = &jpegPhotErrEmitted;
            rp.planar = (uint32_t)oifd.getInt(284, 1);
            auto tiles = buildTiles(lv.data, lv.w, lv.h, m, rp);
            // a rewrite reuses the stored tables; only the per-tile
            // encodes go through the codec setup again
            rp.jpegWarn = false;
            const std::vector<uint64_t> *offs = oifd.getInts(324);
            const std::vector<uint64_t> *cnts = oifd.getInts(325);
            if (!offs || !cnts || offs->size() != tiles.size())
                continue;
            bool methodChanged = storedResampling(oifd) != effMeth;
            std::vector<uint64_t> nOffs = *offs, nCnts = *cnts;
            for (size_t t = 0; t < tiles.size(); ++t)
            {
                if (tiles[t].empty())
                    continue;
                if (!methodChanged && rp.comp != 1 &&
                    tiles[t].size() != (*cnts)[t])
                {
                    tiles[t].clear();  // size drift: keep old tile
                    continue;
                }
                nCnts[t] = tiles[t].size();
                // regenerated blocks land back in their old slot when
                // they fit, otherwise at the end of the file after the
                // (possibly unlinked) new directories
                if ((*offs)[t] && tiles[t].size() <= (*cnts)[t] &&
                    (*offs)[t] + tiles[t].size() <= file.size())
                {
                    memcpy(&file[(*offs)[t]], tiles[t].data(),
                           tiles[t].size());
                    tiles[t].clear();
                }
                else
                    nOffs[t] = UINT64_MAX;
            }
            if (methodChanged ||
                std::find(nOffs.begin(), nOffs.end(), UINT64_MAX) !=
                    nOffs.end())
                reuseWork.push_back(ReuseWork{
                    oifd.offset, rp, std::move(tiles), std::move(nOffs),
                    std::move(nCnts), lv.w, lv.h,
                    (int)oifd.getInt(262, 1), methodChanged,
                    rdPtr(file, nextSlotOf(file, oifd.offset))});
            continue;
        }
        TileParams lvTp = newTp;
        // externally ZSTD_LEVEL only survives until the first directory
        // of a fresh container is written; later levels re-init at 9
        if (g.external && lvTp.comp == 50000 &&
            !(freshOvr && newCount == 0))
            lvTp.zstdLevel = 9;
        auto tiles = buildTiles(lv.data, lv.w, lv.h, m, lvTp);
        std::vector<uint64_t> cnts;
        for (const auto &t : tiles)
            cnts.push_back(t.size());
        // synthesized palettes keep the band's Gray interpretation on
        // record in external containers
        bool grayInterp =
            g.external && newPhot == 3 && m.phot != 3;
        newIfds.push_back(buildOvrTags(
            m, lv.w, lv.h, cnts, std::vector<uint64_t>(tiles.size(), 0),
            metaXml(effMeth, false, grayInterp), lvTp, newPhot,
            isBigTiff(file)));
        newTiles.push_back(std::move(tiles));
        if (!(anyReuseMethodChange && !g.external))
        {
            newDims.emplace_back(lv.w, lv.h);
            newCount++;
        }
    }
    bool regenPaletteFail = false;
    auto flushReuse = [&]()
    {
        for (auto &rw : reuseWork)
        {
            for (size_t t = 0; t < rw.tiles.size(); ++t)
            {
                if (rw.nOffs[t] != UINT64_MAX)
                    continue;
                rw.nOffs[t] = file.size();
                file.insert(file.end(), rw.tiles[t].begin(),
                            rw.tiles[t].end());
            }
            // rewriting a palette JPEG directory trips the codec setup,
            // and that failure propagates to the exit code (the file is
            // already fully written by then)
            if (rw.methodChanged && rw.rp.comp == 7 && rw.oldPhot == 3)
                regenPaletteFail = true;
            if (rw.methodChanged)
                spliceRewrittenIfd(
                    file, rw.ifdOff,
                    buildOvrTags(m, rw.w, rw.h, rw.nCnts, rw.nOffs,
                                 metaXml(effMeth, true,
                                         rw.oldPhot == 3 &&
                                             m.phot != 3),
                                 rw.rp, rw.oldPhot, isBigTiff(file)),
                    rw.origNext);
        }
    };
    // external containers rewrite the changed directory first and link
    // the new levels behind it; internally the new directories go in
    // first and a resampling change leaves them orphaned behind the old
    // tail
    if (g.external)
        flushReuse();
    if (!newIfds.empty())
    {
        uint64_t tailSlot =
            g.external && freshOvr ? 4 : chainTailSlot(file);
        std::vector<std::vector<uint64_t>> tOffs(newIfds.size());
        uint64_t first =
            appendIfds(file, newIfds, newTiles, &tOffs,
                       (newTp.planar == 2 && m.spp > 1) ? (int)m.spp : 1);
        wrPtr(file, tailSlot, first);
    }
    if (!g.external)
        flushReuse();
    if (!saveWhole(target, file))
    {
        debugCloseDataset(ds);
        return overviewError("add: cannot write " + target);
    }

    if (g.external)
    {
        ovrPtr = cplDebugPtr();
        emitOvrPassTraces(target, ovrPtr, newDims, true);
        progressStart(g.quiet);
        if (newCount)
        {
            cplDebug("GDAL",
                     "GDALClose(" + target + ", this=" + ovrPtr + ")");
            ovrPtr = cplDebugPtr();
            emitOvrPassTraces(target, ovrPtr, newDims, true);
        }
        std::string mp = ds.debugPtr;
        if (!mp.empty())
            cplDebug("GDAL", "GDALClose(" + g.input + ", this=" + mp + ")");
        cplDebug("GDAL", "GDALClose(" + target + ", this=" + ovrPtr + ")");
    }
    else
    {
        progressStart(g.quiet);
        debugCloseDataset(ds);
    }
    progressEnd(g.quiet);
    return regenPaletteFail ? 1 : 0;
}

int overviewAddHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string input = r.str("input");
    bool quiet = r.flag("quiet");
    bool external = r.flag("external");
    bool rSet = r.get("resampling") != nullptr;
    std::string resamp = strToLower(r.str("resampling", "nearest"));
    std::vector<std::string> levelStrs = r.list("levels");
    std::vector<std::string> ovrSrc = r.list("overview-src");
    int minSize = atoi(r.str("min-size", "256").c_str());
    if (minSize <= 0)
        minSize = 256;

    int rc = 0;
    std::unique_ptr<RasterDatasetBase> ds = openInput(input, rc);
    if (!ds)
        return rc;
    TiffFile *tf = ds->tiffFile();
    if (!tf || tf->bigEndian)
    {
        debugCloseDataset(*ds);
        return overviewError(
            "add: this input is not supported in this build");
    }
    emitScanBlock(*ds, true);
    if (!ovrSrc.empty())
    {
        debugCloseDataset(*ds);
        return overviewError(
            "add: --overview-src is not implemented in this build");
    }
    const TiffIfd *mainIfd = nullptr;
    for (size_t i = 0; i < tf->ifds.size(); ++i)
        if (tf->ifds[i].offset == ds->tiffIfdOffset())
            mainIfd = &tf->ifds[i];
    MainInfo m;
    std::string why;
    if (!mainIfd || !fillMainInfo(*ds, *mainIfd, m, why))
    {
        debugCloseDataset(*ds);
        return overviewError("add: " + why +
                             " is not supported in this build");
    }

    CoOpts co = parseCo(r.list("creation-option"));

    std::vector<int> factors;
    if (!levelStrs.empty())
    {
        for (const auto &s : levelStrs)
            factors.push_back(atoi(s.c_str()));
    }
    else if (std::max(m.w, m.h) > minSize)
    {
        for (int f = 2;; f *= 2)
        {
            factors.push_back(f);
            if ((m.w + f - 1) / f <= minSize &&
                (m.h + f - 1) / f <= minSize)
                break;
        }
    }
    std::sort(factors.begin(), factors.end());
    factors.erase(std::unique(factors.begin(), factors.end()),
                  factors.end());

    GenRun g;
    g.ds = ds.get();
    g.tf = tf;
    g.m = m;
    g.input = input;
    g.external = external;
    g.quiet = quiet;
    g.rSet = rSet;
    g.resamp = resamp;
    g.factors = factors;
    g.co = co;
    return runGenerate(g);
}

int overviewDeleteHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string input = r.str("input");
    bool external = r.flag("external");
    int rc = 0;
    std::unique_ptr<RasterDatasetBase> ds = openInput(input, rc);
    if (!ds)
        return rc;
    if (external)
    {
        // the reference opens the file without adopting the sidecar and
        // scans lazily after announcing external-overview access
        if (ds->driverShort == "GTiff")
            cplDebug("GTiff", "ScanDirectories()");
        ds->replayDeferred();
        cplDebug("GTiff", kExternalOpenMsg);
        if (!ds->overviews.empty())
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        input + ": Cannot add external overviews when "
                                "there are already internal overviews");
            ds->extOvrOpened = false;
            debugCloseDataset(*ds);
            return 1;
        }
        ds->debugOverviewScan();
        std::string target = input + ".ovr";
        struct stat st;
        bool haveOvr = stat(target.c_str(), &st) == 0;
        if (haveOvr)
        {
            if (ds->extOvrOpened)
            {
                cplDebug("GDAL", "GDALClose(" + target + ", this=" +
                                     ds->extOvrDebugPtr + ")");
                ds->extOvrOpened = false;
            }
            std::string p = cplDebugPtr();
            cplDebug("GDAL", "GDALOpen(" + target + ", this=" + p +
                                 ") succeeds as GTiff.");
            cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
            cplDebug("GDAL", "GDALClose(" + target + ", this=" + p + ")");
        }
        debugCloseDataset(*ds);
        if (haveOvr)
            unlink(target.c_str());
        return 0;
    }
    emitScanBlock(*ds, true);
    TiffFile *tf = ds->tiffFile();
    if (!ds->overviews.empty())
    {
        if (!tf || tf->bigEndian)
        {
            debugCloseDataset(*ds);
            return overviewError(
                "delete: this input is not supported in this build");
        }
        std::vector<uint8_t> file;
        if (!loadWhole(input, file))
        {
            debugCloseDataset(*ds);
            return overviewError("delete: cannot read " + input);
        }
        // unlink the reduced-resolution IFDs last-first, matching the
        // reference's chain rewrites (each orphan's next slot ends zero)
        std::vector<uint64_t> dropOffsets;
        for (const auto &e : ds->overviews)
            if (!e.ext && e.page - 1 < (int)tf->ifds.size())
                dropOffsets.push_back(tf->ifds[e.page - 1].offset);
        std::sort(dropOffsets.begin(), dropOffsets.end());
        for (auto it = dropOffsets.rbegin(); it != dropOffsets.rend();
             ++it)
        {
            uint64_t slot = firstIfdSlot(file);
            uint64_t off = rdPtr(file, slot);
            int guard = 0;
            while (off && guard++ < 1000)
            {
                uint64_t nslot = nextSlotOf(file, off);
                uint64_t nxt = rdPtr(file, nslot);
                if (off == *it)
                {
                    wrPtr(file, slot, nxt);
                    wrPtr(file, nslot, 0);
                    break;
                }
                slot = nslot;
                off = nxt;
            }
        }
        if (!saveWhole(input, file))
        {
            debugCloseDataset(*ds);
            return overviewError("delete: cannot write " + input);
        }
        debugCloseDataset(*ds);
        return 0;
    }
    if (!ds->extOverviews.empty())
    {
        std::string target = ds->extOvrPath;
        if (ds->extOvrOpened)
        {
            cplDebug("GDAL", "GDALClose(" + target + ", this=" +
                                 ds->extOvrDebugPtr + ")");
            ds->extOvrOpened = false;
        }
        std::string p = cplDebugPtr();
        cplDebug("GDAL", "GDALOpen(" + target + ", this=" + p +
                             ") succeeds as GTiff.");
        cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
        cplDebug("GDAL", "GDALClose(" + target + ", this=" + p + ")");
        debugCloseDataset(*ds);
        unlink(target.c_str());
        return 0;
    }
    debugCloseDataset(*ds);
    return 0;
}

int overviewRefreshHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string input = r.str("input");
    bool quiet = r.flag("quiet");
    bool external = r.flag("external");
    bool rSet = r.get("resampling") != nullptr;
    std::string resamp = strToLower(r.str("resampling", "nearest"));
    std::vector<std::string> levelStrs = r.list("levels");
    bool useTs = r.flag("use-source-timestamp");
    const ArgValue *bbox = r.get("bbox");
    std::vector<std::string> like = r.list("like");

    int rc = 0;
    std::unique_ptr<RasterDatasetBase> ds = openInput(input, rc);
    if (!ds)
        return rc;
    TiffFile *tf = ds->tiffFile();
    if (!tf || tf->bigEndian)
    {
        debugCloseDataset(*ds);
        return overviewError(
            "refresh: this input is not supported in this build");
    }
    emitScanBlock(*ds, true);

    const std::vector<RasterDatasetBase::OvrEntry> &displayed =
        ds->dispOverviews();
    bool dispExternal = ds->overviews.empty() && !ds->extOverviews.empty();
    if (displayed.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "refresh: No overviews to refresh");
        debugCloseDataset(*ds);
        return 1;
    }
    if (useTs)
    {
        if (dispExternal)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "--use-source-timestamp only works on a VRT or "
                        "GTI dataset");
            debugCloseDataset(*ds);
            return 1;
        }
        struct stat st;
        if (stat((input + ".ovr").c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot find " + input + ".ovr");
            debugCloseDataset(*ds);
            return 1;
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "--use-source-timestamp only works on a VRT or GTI "
                    "dataset");
    }
    if (bbox || !like.empty())
    {
        if (!ds->hasGT)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Dataset has no geotransform");
            debugCloseDataset(*ds);
            return 1;
        }
    }

    const TiffIfd *mainIfd = nullptr;
    for (size_t i = 0; i < tf->ifds.size(); ++i)
        if (tf->ifds[i].offset == ds->tiffIfdOffset())
            mainIfd = &tf->ifds[i];
    MainInfo m;
    std::string why;
    if (!mainIfd || !fillMainInfo(*ds, *mainIfd, m, why))
    {
        debugCloseDataset(*ds);
        return overviewError("refresh: " + why +
                             " is not supported in this build");
    }

    // levels to refresh: all displayed, or the --levels subset
    std::vector<int> refreshIdx;
    if (levelStrs.empty())
        for (size_t i = 0; i < displayed.size(); ++i)
            refreshIdx.push_back((int)i);
    else
        for (const auto &s : levelStrs)
        {
            int f = atoi(s.c_str());
            int found = -1;
            for (size_t i = 0; i < displayed.size(); ++i)
                if (factorOfEntry(m.w, displayed[i].w) == f)
                    found = (int)i;
            if (found < 0)
            {
                debugCloseDataset(*ds);
                return overviewError(
                    strPrintf("Cannot find overview level with "
                              "subsampling factor of %d",
                              f));
            }
            refreshIdx.push_back(found);
        }
    std::sort(refreshIdx.begin(), refreshIdx.end());
    refreshIdx.erase(std::unique(refreshIdx.begin(), refreshIdx.end()),
                     refreshIdx.end());

    std::vector<int> factors;
    for (int i : refreshIdx)
        factors.push_back(factorOfEntry(m.w, displayed[i].w));

    GenRun g;
    g.ds = ds.get();
    g.tf = tf;
    g.m = m;
    g.input = input;
    g.external = external;
    g.quiet = quiet;
    g.rSet = rSet;
    g.resamp = resamp;
    g.factors = factors;
    g.refreshMode = true;
    g.refreshIdx = refreshIdx;
    return runGenerate(g);
}

struct Reg
{
    Reg()
    {
        registerHandler("raster_overview_add", overviewAddHandler);
        registerHandler("raster_overview_delete", overviewDeleteHandler);
        registerHandler("raster_overview_refresh", overviewRefreshHandler);
    }
} reg;

}  // namespace

// COG overview generation: one cascade step through the same
// store-rounded kernels the overview verbs use
void cogResampleLevel(const std::vector<std::vector<double>> &src, int sw,
                      int sh, std::vector<std::vector<double>> &dst,
                      int dw, int dh, const std::string &method, DType dt,
                      bool hasNodata, double nodata)
{
    MainInfo m;
    m.w = sw;
    m.h = sh;
    m.spp = (uint32_t)src.size();
    m.dt = dt;
    m.bytes = dtypeSizeBytes(dt);
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
    m.hasNodata = hasNodata;
    m.nodata = nodata;
    dst.assign(src.size(), {});
    for (size_t b = 0; b < src.size(); ++b)
    {
        resampleBand(src[b], sw, sh, dst[b], dw, dh, method, m);
        if (dt == DType::Float32)
            for (double &v : dst[b])
                v = (double)(float)v;
    }
}
