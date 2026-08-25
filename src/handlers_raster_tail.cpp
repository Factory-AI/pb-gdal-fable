#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "json.h"
#include "ogr.h"
#include "srs.h"
#include "util.h"
#include "vrt.h"
#include "warp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

float tailHalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t x;
    if (exp == 0)
    {
        if (!mant)
            x = sign;
        else
        {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400))
            {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            x = sign | (exp << 23) | (mant << 13);
        }
    }
    else if (exp == 31)
        x = sign | 0x7f800000 | (mant << 13);
    else
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

uint16_t tailFloatToHalf(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t absx = x & 0x7fffffff;
    if (absx >= 0x7f800000)
        return sign | 0x7c00 | (absx > 0x7f800000 ? 0x200 : 0);
    int32_t exp = (int32_t)(absx >> 23) - 127 + 15;
    uint32_t mant = absx & 0x7fffff;
    if (exp >= 31)
        return sign | 0x7c00;
    if (exp <= 0)
    {
        // truncate-toward-zero like the normal path, keeping subnormals
        if (exp < -10)
            return sign;
        mant |= 0x800000;
        return sign | (uint16_t)(mant >> (uint32_t)(14 - exp));
    }
    return sign | (uint16_t)(((uint32_t)exp << 10) | (mant >> 13));
}

namespace
{

std::string fmtG(double v)
{
    return strPrintf("%g", v);
}

std::string fmt18(double v)
{
    return strPrintf("%.18g", v);
}

bool typeRange(DType t, double &lo, double &hi)
{
    switch (t)
    {
        case DType::Byte:
            lo = 0;
            hi = 255;
            return true;
        case DType::Int8:
            lo = -128;
            hi = 127;
            return true;
        case DType::UInt16:
            lo = 0;
            hi = 65535;
            return true;
        case DType::Int16:
        case DType::CInt16:
            lo = -32768;
            hi = 32767;
            return true;
        case DType::UInt32:
            lo = 0;
            hi = 4294967295.0;
            return true;
        case DType::Int32:
        case DType::CInt32:
            lo = -2147483648.0;
            hi = 2147483647.0;
            return true;
        case DType::UInt64:
            lo = 0;
            hi = 18446744073709551615.0;
            return true;
        case DType::Int64:
            lo = -9223372036854775808.0;
            hi = 9223372036854775807.0;
            return true;
        default:
            return false;
    }
}

// GDALCopyWords semantics: round half away from zero, clamp, NaN -> 0
double finishReal(double v, DType t)
{
    if (t == DType::Int64 || t == DType::UInt64)
    {
        // the exact bounds are not representable as double; the encode
        // step applies the reference's threshold semantics
        return std::isnan(v) ? 0 : std::round(v);
    }
    double lo, hi;
    if (typeRange(t, lo, hi))
    {
        if (std::isnan(v))
            return 0;
        double r = std::round(v);
        if (r < lo)
            r = lo;
        if (r > hi)
            r = hi;
        return r;
    }
    if (t == DType::Float32 || t == DType::CFloat32)
    {
        // finite overflow saturates at FLT_MAX; infinities pass through
        if (!std::isinf(v) && v > 3.4028234663852886e38)
            return 3.4028234663852886e38;
        if (!std::isinf(v) && v < -3.4028234663852886e38)
            return -3.4028234663852886e38;
        return (double)(float)v;
    }
    if (t == DType::Float16)
    {
        // half saturates at +/-65504 even for infinite inputs
        if (std::isnan(v))
            return v;
        if (v > 65504.0)
            v = 65504.0;
        if (v < -65504.0)
            v = -65504.0;
        return (double)tailHalfToFloat(tailFloatToHalf((float)v));
    }
    return v;
}

struct BandMap
{
    enum Kind
    {
        Copy,
        Linear,  // out = v*ratio + offset
        Expo     // out = dstMin + dstRange*pow(base, exponent)
    };
    Kind kind = Copy;
    double ratio = 1, offset = 0;
    double srcMin = 0, srcMax = 0, dstMin = 0, dstMax = 0, exponent = 1;
    bool clip = false;
    // the reference streams scaled pixels through a Float32 working
    // buffer when the destination type fits one (Byte/Int8/Int16/
    // UInt16); wider integer types keep the double result
    bool wrkFloat = false;
    bool passNodata = false;
    double nodata = 0;
    // VRT serialization
    bool complexTag = false;
    bool emitScalePair = false;  // ScaleOffset/ScaleRatio children
    bool emitExpo = false;       // Exponent/SrcMin/../Clip children

    double apply(double v) const
    {
        if (passNodata &&
            (v == nodata || (std::isnan(v) && std::isnan(nodata))))
            return v;
        switch (kind)
        {
            case Copy:
                return v;
            case Linear:
            {
                double r = v * ratio + offset;
                return wrkFloat ? (double)(float)r : r;
            }
            case Expo:
            {
                // IEEE division on purpose: a degenerate source range
                // yields inf/NaN exactly like the reference
                double base = (v - srcMin) / (srcMax - srcMin);
                if (clip)
                {
                    if (base < 0)
                        base = 0;
                    if (base > 1)
                        base = 1;
                }
                double r = dstMin +
                           (dstMax - dstMin) * std::pow(base, exponent);
                return wrkFloat ? (double)(float)r : r;
            }
        }
        return v;
    }
};

void decodeReals(DType t, const uint8_t *p, size_t n, double *re,
                 double *im)
{
    size_t sz = (size_t)dtypeSizeBytes(t);
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t *q = p + i * sz;
        double r = 0, m = 0;
        switch (t)
        {
            case DType::Byte:
                r = *q;
                break;
            case DType::Int8:
                r = *(const int8_t *)q;
                break;
            case DType::UInt16:
            {
                uint16_t x;
                memcpy(&x, q, 2);
                r = x;
                break;
            }
            case DType::Int16:
            {
                int16_t x;
                memcpy(&x, q, 2);
                r = x;
                break;
            }
            case DType::UInt32:
            {
                uint32_t x;
                memcpy(&x, q, 4);
                r = x;
                break;
            }
            case DType::Int32:
            {
                int32_t x;
                memcpy(&x, q, 4);
                r = x;
                break;
            }
            case DType::UInt64:
            {
                uint64_t x;
                memcpy(&x, q, 8);
                r = (double)x;
                break;
            }
            case DType::Int64:
            {
                int64_t x;
                memcpy(&x, q, 8);
                r = (double)x;
                break;
            }
            case DType::Float16:
            {
                uint16_t x;
                memcpy(&x, q, 2);
                r = tailHalfToFloat(x);
                break;
            }
            case DType::Float32:
            {
                float x;
                memcpy(&x, q, 4);
                r = x;
                break;
            }
            case DType::Float64:
                memcpy(&r, q, 8);
                break;
            case DType::CInt16:
            {
                int16_t x, y;
                memcpy(&x, q, 2);
                memcpy(&y, q + 2, 2);
                r = x;
                m = y;
                break;
            }
            case DType::CInt32:
            {
                int32_t x, y;
                memcpy(&x, q, 4);
                memcpy(&y, q + 4, 4);
                r = x;
                m = y;
                break;
            }
            case DType::CFloat32:
            {
                float x, y;
                memcpy(&x, q, 4);
                memcpy(&y, q + 4, 4);
                r = x;
                m = y;
                break;
            }
            case DType::CFloat64:
                memcpy(&r, q, 8);
                memcpy(&m, q + 8, 8);
                break;
            default:
                break;
        }
        re[i] = r;
        if (im)
            im[i] = m;
    }
}

void encodeReal(DType t, uint8_t *q, double r, double m)
{
    switch (t)
    {
        case DType::Byte:
            *q = (uint8_t)r;
            break;
        case DType::Int8:
            *(int8_t *)q = (int8_t)r;
            break;
        case DType::UInt16:
        {
            uint16_t x = (uint16_t)r;
            memcpy(q, &x, 2);
            break;
        }
        case DType::Int16:
        {
            int16_t x = (int16_t)r;
            memcpy(q, &x, 2);
            break;
        }
        case DType::UInt32:
        {
            uint32_t x = (uint32_t)r;
            memcpy(q, &x, 4);
            break;
        }
        case DType::Int32:
        {
            int32_t x = (int32_t)r;
            memcpy(q, &x, 4);
            break;
        }
        case DType::UInt64:
        {
            // reference semantics: above 2^64 saturates, exactly 2^64
            // wraps to 0 (its own overflowing cast), negatives clamp
            uint64_t x;
            if (r > 18446744073709551616.0)
                x = UINT64_MAX;
            else if (r >= 18446744073709551616.0)
                x = 0;
            else
                x = r <= 0 ? 0 : (uint64_t)r;
            memcpy(q, &x, 8);
            break;
        }
        case DType::Int64:
        {
            int64_t x = r >= 9223372036854775808.0
                            ? INT64_MAX
                            : (r <= -9223372036854775808.0
                                   ? INT64_MIN
                                   : (int64_t)r);
            memcpy(q, &x, 8);
            break;
        }
        case DType::Float16:
        {
            uint16_t x = tailFloatToHalf((float)r);
            memcpy(q, &x, 2);
            break;
        }
        case DType::Float32:
        {
            float x = (float)r;
            memcpy(q, &x, 4);
            break;
        }
        case DType::Float64:
            memcpy(q, &r, 8);
            break;
        case DType::CInt16:
        {
            int16_t x = (int16_t)r, y = (int16_t)m;
            memcpy(q, &x, 2);
            memcpy(q + 2, &y, 2);
            break;
        }
        case DType::CInt32:
        {
            int32_t x = (int32_t)r, y = (int32_t)m;
            memcpy(q, &x, 4);
            memcpy(q + 4, &y, 4);
            break;
        }
        case DType::CFloat32:
        {
            float x = (float)r, y = (float)m;
            memcpy(q, &x, 4);
            memcpy(q + 4, &y, 4);
            break;
        }
        case DType::CFloat64:
            memcpy(q, &r, 8);
            memcpy(q + 8, &m, 8);
            break;
        default:
            break;
    }
}

// the reference's lossy-conversion predicate, mapped empirically from
// the full 17x17 set-type stats-keeping matrix; it is NOT a value-range
// containment test: any complex pair differing at all is lossy, 64-bit
// integers are exempt from the size checks, and int->float keeps only
// Float64 (even Byte->Float32 counts as lossy)
bool dtypeConversionLossy(DType src, DType dst)
{
    if (src == dst)
        return false;
    if (dtypeIsComplex(src) || dtypeIsComplex(dst))
        return true;
    struct Info
    {
        bool isFloat, isSigned;
        int bits;
    };
    auto info = [](DType t) -> Info {
        switch (t)
        {
            case DType::Byte:
                return {false, false, 8};
            case DType::Int8:
                return {false, true, 8};
            case DType::UInt16:
                return {false, false, 16};
            case DType::Int16:
                return {false, true, 16};
            case DType::UInt32:
                return {false, false, 32};
            case DType::Int32:
                return {false, true, 32};
            case DType::UInt64:
                return {false, false, 64};
            case DType::Int64:
                return {false, true, 64};
            case DType::Float16:
                return {true, true, 16};
            case DType::Float32:
                return {true, true, 32};
            default:
                return {true, true, 64};
        }
    };
    Info a = info(src), b = info(dst);
    if (a.isFloat && b.isFloat)
        return b.bits < a.bits;
    if (a.isFloat && !b.isFloat)
        return true;
    if (!a.isFloat && b.isFloat)
        return b.bits != 64;
    if (a.isSigned && !b.isSigned)
        return true;
    if (a.bits == 64 || b.bits == 64)
        return false;
    if (a.bits > b.bits)
        return true;
    return a.bits == b.bits && !a.isSigned && b.isSigned;
}

class PixelMapDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    std::vector<BandMap> maps;

    PixelMapDataset(std::unique_ptr<RasterDatasetBase> s,
                    const std::vector<BandMap> &m,
                    const std::vector<DType> &outTypes, bool dropScale,
                    bool stripStats = false)
        : src(std::move(s)), maps(m)
    {
        path = src->path;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = src->width;
        height = src->height;
        hasGT = src->hasGT;
        memcpy(gt, src->gt, sizeof gt);
        srs = std::move(src->srs);
        hasSrs = src->hasSrs;
        srsSynthetic = src->srsSynthetic;
        metadata = src->metadata;
        domainOrder = src->domainOrder;
        sortedDomains = src->sortedDomains;
        xmlDomains = src->xmlDomains;
        files = src->files;
        gcps = src->gcps;
        gcpSrs = std::move(src->gcpSrs);
        hasGcpSrs = src->hasGcpSrs;
        gcpMapping = src->gcpMapping;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();
        pamPath = src->pamPath;
        pamExists = src->pamExists;
        pamSrsRaw = src->pamSrsRaw;
        pamSrsMapping = src->pamSrsMapping;
        pamGtRaw = src->pamGtRaw;
        pamMdi = src->pamMdi;
        pamXmlDomains = src->pamXmlDomains;
        pamBands = src->pamBands;
        pamSuppressItems = true;
        for (size_t i = 0; i < src->bands.size(); ++i)
        {
            Band b = src->bands[i];
            b.type = outTypes[i];
            b.index = (int)i + 1;
            if (dropScale)
            {
                b.hasOffset = false;
                b.hasScale = false;
                b.offset = 0;
                b.scale = 1;
            }
            if (b.hasNodata && !nodataFits(b.nodata, b.type))
            {
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Source nodata value was not copied to "
                            "output band, as it cannot be represented "
                            "on its data type.");
                b.hasNodata = false;
                b.nodata = 0;
            }
            // a non-identity mapping breaks histogram delegation to the
            // source sidecar, cache included
            if (!identityMap(maps[i]))
                b.pamHists.clear();
            // the reference's scaling verbs discard band statistics
            // metadata even when the mapping is an identity; set-type
            // discards them only across lossy type conversions
            if (stripStats ||
                dtypeConversionLossy(src->bands[i].type, outTypes[i]))
            {
                static const char *const kStatKeys[] = {
                    "STATISTICS_MINIMUM",       "STATISTICS_MAXIMUM",
                    "STATISTICS_MEAN",          "STATISTICS_STDDEV",
                    "STATISTICS_VALID_PERCENT", "STATISTICS_APPROXIMATE",
                };
                for (const char *k : kStatKeys)
                    b.removeMd("", k);
            }
            bands.push_back(std::move(b));
        }
    }

    bool bandMinMaxHint(int band, double &mn, double &mx) override
    {
        // GetMinimum/GetMaximum forward to the source band only across
        // value-identity mappings
        if (!identityMap(maps[(size_t)band - 1]))
            return false;
        const Band &sb = src->bands[(size_t)band - 1];
        const std::string *lo = sb.getMd("", "STATISTICS_MINIMUM");
        const std::string *hi = sb.getMd("", "STATISTICS_MAXIMUM");
        if (lo && hi)
        {
            mn = strtod(lo->c_str(), nullptr);
            mx = strtod(hi->c_str(), nullptr);
            return true;
        }
        return src->bandMinMaxHint(band, mn, mx);
    }

    // only the 64-bit integer band types refuse a nodata value they
    // cannot represent exactly; smaller types keep out-of-range nodata
    static bool nodataFits(double v, DType t)
    {
        if (t == DType::Int64)
            return !std::isnan(v) && v == std::floor(v) &&
                   v >= -9223372036854775808.0 &&
                   v < 9223372036854775808.0;
        if (t == DType::UInt64)
            return !std::isnan(v) && v == std::floor(v) && v >= 0 &&
                   v < 18446744073709551616.0;
        return true;
    }

    static bool identityMap(const BandMap &m)
    {
        return !m.emitExpo && !m.emitScalePair;
    }

    int histPamMode(int band) override
    {
        if (!identityMap(maps[(size_t)band - 1]))
            return 0;
        int inner = src->histPamMode(band);
        return inner == 0 ? 0 : 1;
    }

    RasterDatasetBase *histDelegateWrap(int band) override
    {
        if (!identityMap(maps[(size_t)band - 1]))
            return nullptr;
        RasterDatasetBase *inner = src->histDelegateWrap(band);
        return inner ? inner : src.get();
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        if (!src->readBand(band, out))
            return false;
        const BandMap &m = maps[(size_t)band - 1];
        DType dt = bands[(size_t)band - 1].type;
        for (double &v : out)
            v = finishReal(m.apply(v), dt);
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        return rawImpl(band, out, false);
    }

    bool readBandRawStrict(int band, std::vector<uint8_t> &out) override
    {
        return rawImpl(band, out, true);
    }

    bool geoDoubleOrphanHint() override
    {
        return src->geoDoubleOrphanHint();
    }

    void realBlockDims(int &bw, int &bh) const override
    {
        src->realBlockDims(bw, bh);
    }

    bool vrtWrapperSource(int band, bool &complexTag, DType &srcType,
                          std::string &childrenXml) override
    {
        const BandMap &m = maps[(size_t)band - 1];
        const Band &sb = src->bands[(size_t)band - 1];
        complexTag = m.complexTag;
        srcType = sb.type;
        std::string k;
        if (m.complexTag && m.passNodata)
            k += "      <NODATA>" + fmt18(m.nodata) + "</NODATA>\n";
        if (m.emitExpo)
        {
            k += "      <Exponent>" + fmtG(m.exponent) + "</Exponent>\n";
            k += "      <SrcMin>" + fmtG(m.srcMin) + "</SrcMin>\n";
            k += "      <SrcMax>" + fmtG(m.srcMax) + "</SrcMax>\n";
            k += "      <DstMin>" + fmtG(m.dstMin) + "</DstMin>\n";
            k += "      <DstMax>" + fmtG(m.dstMax) + "</DstMax>\n";
            k += std::string("      <Clip>") + (m.clip ? "true" : "false") +
                 "</Clip>\n";
        }
        else if (m.emitScalePair)
        {
            k += "      <ScaleOffset>" + fmtG(m.offset) +
                 "</ScaleOffset>\n";
            k += "      <ScaleRatio>" + fmtG(m.ratio) + "</ScaleRatio>\n";
        }
        childrenXml = k;
        return true;
    }

  private:
    bool rawImpl(int band, std::vector<uint8_t> &out, bool strict)
    {
        const BandMap &m = maps[(size_t)band - 1];
        DType st = src->bands[(size_t)band - 1].type;
        DType dt = bands[(size_t)band - 1].type;
        std::vector<uint8_t> raw;
        bool ok = strict ? src->readBandRawStrict(band, raw)
                         : src->readBandRaw(band, raw);
        if (!ok)
            return false;
        size_t n = (size_t)width * (size_t)height;
        if (m.kind == BandMap::Copy)
        {
            // 64-bit integer values exceed double precision; identity
            // maps must convert without the double round-trip
            if (st == dt)
            {
                out = std::move(raw);
                return true;
            }
            bool s64 = st == DType::Int64 || st == DType::UInt64;
            bool d64 = dt == DType::Int64 || dt == DType::UInt64;
            if (s64 && d64)
            {
                out.assign(n * 8, 0);
                for (size_t i = 0; i < n; ++i)
                {
                    if (st == DType::Int64)
                    {
                        int64_t v;
                        memcpy(&v, raw.data() + i * 8, 8);
                        uint64_t x = v < 0 ? 0 : (uint64_t)v;
                        memcpy(out.data() + i * 8, &x, 8);
                    }
                    else
                    {
                        uint64_t v;
                        memcpy(&v, raw.data() + i * 8, 8);
                        int64_t x = v > (uint64_t)INT64_MAX
                                        ? INT64_MAX
                                        : (int64_t)v;
                        memcpy(out.data() + i * 8, &x, 8);
                    }
                }
                return true;
            }
        }
        std::vector<double> re(n), im(n);
        decodeReals(st, raw.data(), n, re.data(), im.data());
        out.assign(n * (size_t)dtypeSizeBytes(dt), 0);
        bool dstComplex = dtypeIsComplex(dt);
        for (size_t i = 0; i < n; ++i)
        {
            double r = finishReal(m.apply(re[i]), dt);
            double mi = 0;
            if (dstComplex && dtypeIsComplex(st))
                mi = finishReal(m.kind == BandMap::Copy ? im[i]
                                                        : m.apply(im[i]),
                                dt);
            encodeReal(dt, out.data() + i * (size_t)dtypeSizeBytes(dt), r,
                       mi);
        }
        return true;
    }
};

std::string canonChoice(const CmdSpec &cmd, const std::string &argName,
                        const std::string &val)
{
    for (const auto &a : cmd.args)
        if (a.name == argName)
            for (const auto &c : a.choices)
                if (strEqualNoCase(c, val))
                    return c;
    // "Byte" is a hidden choice (UInt8 is the visible spelling); its
    // canonical form keeps the hidden spelling
    if (strEqualNoCase(val, "byte"))
        return "Byte";
    return val;
}

struct TailCommon
{
    std::string input, output, drv;
    bool quiet = false, overwrite = false, append = false;
    std::unique_ptr<RasterDatasetBase> ds;
};

// shared head: prefix, format validation, open input; rc>=0 means "return"
int tailBegin(const std::string &verb, ParseResult &r, TailCommon &tc)
{
    std::string format = r.str("output-format");
    tc.input = r.str("input");
    tc.output = r.str("output");
    tc.quiet = r.flag("quiet");
    tc.overwrite = r.flag("overwrite");
    tc.append = r.flag("append");
    (void)verb;
    {
        std::string issue = rasterOutFormatIssue(format, tc.drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    std::string err;
    tc.ds = openRaster(tc.input, err);
    if (!tc.ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + tc.input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    return -1;
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

// ------------------------------------------------------------------
// shared verb core
// ------------------------------------------------------------------

struct TailParams
{
    bool hasOt = false;
    std::string otName;
    DType ot = DType::Unknown;
    bool hasBand = false;
    int band = 0;
    bool hasSrcMin = false, hasSrcMax = false;
    bool hasDstMin = false, hasDstMax = false;
    bool hasExp = false, noClip = false;
    double srcMin = 0, srcMax = 0, dstMin = 0, dstMax = 0, exp = 1;
};

using TailGetter =
    std::function<const std::vector<std::string> *(const std::string &)>;

TailParams tailFillParams(const std::string &verb, const TailGetter &get)
{
    TailParams p;
    const CmdSpec *cs = Spec::instance().findById("raster_" + verb);
    auto first = [&](const std::string &n) -> const std::string * {
        const auto *v = get(n);
        return v && !v->empty() ? &(*v)[0] : nullptr;
    };
    if (const std::string *v = first("output-data-type"))
    {
        p.hasOt = true;
        p.otName = cs ? canonChoice(*cs, "output-data-type", *v) : *v;
        p.ot = dtypeFromName(p.otName);
    }
    if (const std::string *v = first("band"))
    {
        p.hasBand = true;
        p.band = atoi(v->c_str());
    }
    if (const std::string *v = first("src-min"))
    {
        p.hasSrcMin = true;
        p.srcMin = atof(v->c_str());
    }
    if (const std::string *v = first("src-max"))
    {
        p.hasSrcMax = true;
        p.srcMax = atof(v->c_str());
    }
    if (const std::string *v = first("dst-min"))
    {
        p.hasDstMin = true;
        p.dstMin = atof(v->c_str());
    }
    if (const std::string *v = first("dst-max"))
    {
        p.hasDstMax = true;
        p.dstMax = atof(v->c_str());
    }
    if (const std::string *v = first("exponent"))
    {
        p.hasExp = true;
        p.exp = atof(v->c_str());
    }
    if (const std::string *v = first("no-clip"))
        p.noClip = *v == "true";
    return p;
}

// canonical argument echo shared by leaf GDALG extras and pipeline echoes
std::string tailArgsEcho(const std::string &verb, const TailParams &p)
{
    std::string e;
    (void)verb;
    if (p.hasOt)
        e += " --output-data-type " + p.otName;
    if (p.hasBand)
        e += strPrintf(" --band %d", p.band);
    if (p.hasSrcMin)
        e += " --src-min " + fmtG(p.srcMin);
    if (p.hasSrcMax)
        e += " --src-max " + fmtG(p.srcMax);
    if (p.hasDstMin)
        e += " --dst-min " + fmtG(p.dstMin);
    if (p.hasDstMax)
        e += " --dst-max " + fmtG(p.dstMax);
    if (p.hasExp)
        e += " --exponent " + fmtG(p.exp);
    if (p.noClip)
        e += " --no-clip";
    return e;
}

DType unscaleDefaultType(DType src)
{
    switch (src)
    {
        case DType::Float64:
            return DType::Float64;
        case DType::CInt16:
        case DType::CInt32:
        case DType::CFloat32:
            return DType::CFloat32;
        case DType::CFloat64:
            return DType::CFloat64;
        default:
            return DType::Float32;
    }
}

void scaleDstDefault(DType t, double &lo, double &hi)
{
    // the 64-bit defaults are the reference's nearest-representable
    // interior doubles, not the exact type bounds
    if (t == DType::Int64)
    {
        lo = -9223372036854774784.0;
        hi = 9223372036854772736.0;
        return;
    }
    if (t == DType::UInt64)
    {
        lo = 0;
        hi = 18446744073709549568.0;
        return;
    }
    if (!typeRange(t, lo, hi))
    {
        lo = 0;
        hi = 1;
    }
}

bool bandMinMax(RasterDatasetBase &ds, int band, double &mn, double &mx)
{
    std::vector<double> vals;
    if (!readBandValues(ds, band, vals))
        return false;
    const Band &b = ds.bands[(size_t)band - 1];
    // ComputeRasterMinMax(approxOK): sample every sqrt(nBlocks)-th block
    int bx = b.blockX > 0 ? b.blockX : ds.width;
    int by = b.blockY > 0 ? b.blockY : ds.height;
    int nbx = (ds.width + bx - 1) / bx;
    int nby = (ds.height + by - 1) / by;
    int rate = std::max(1, (int)sqrt((double)nbx * nby));
    if (rate == nbx && nbx > 1)
        ++rate;
    bool first = true;
    for (int ib = 0; ib < nbx * nby; ib += rate)
    {
        int x0 = (ib % nbx) * bx, y0 = (ib / nbx) * by;
        int x1 = std::min(x0 + bx, ds.width);
        int y1 = std::min(y0 + by, ds.height);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                double v = vals[(size_t)y * ds.width + x];
                if (std::isnan(v))
                    continue;
                if (b.hasNodata && v == b.nodata)
                    continue;
                if (first || v < mn)
                    mn = v;
                if (first || v > mx)
                    mx = v;
                first = false;
            }
    }
    if (first)
    {
        // non-fatal in the reference: the error prints but the run
        // continues with a zero range
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s, band %d: Failed to compute min/max, "
                              "no valid pixels found in sampling.",
                              ds.path.c_str(), band));
        mn = 0;
        mx = 0;
    }
    return true;
}

// wraps d according to verb+params; on failure prints the error (usage
// only in leaf mode) and returns nonzero
int tailWrap(const std::string &verb, const TailParams &p,
             std::unique_ptr<RasterDatasetBase> &d, bool leafUsage)
{
    auto need = [&](bool a, bool b, const char *an, const char *bn) -> bool {
        if (a && !b)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        verb + ": " + bn + " must be specified when " + an +
                            " is specified");
            return true;
        }
        return false;
    };
    if (verb == "scale")
    {
        if (need(p.hasSrcMin, p.hasSrcMax, "src-min", "src-max") ||
            need(p.hasSrcMax, p.hasSrcMin, "src-max", "src-min") ||
            need(p.hasDstMin, p.hasDstMax, "dst-min", "dst-max") ||
            need(p.hasDstMax, p.hasDstMin, "dst-max", "dst-min"))
            return 1;
    }

    int nb = (int)d->bands.size();
    std::vector<BandMap> maps;
    std::vector<DType> types;

    if (verb == "set-type")
    {
        maps.assign((size_t)nb, BandMap());
        types.assign((size_t)nb, p.ot);
        d = std::make_unique<PixelMapDataset>(std::move(d), maps, types,
                                              false);
        return 0;
    }

    if (verb == "unscale")
    {
        for (const Band &b : d->bands)
        {
            BandMap m;
            m.kind = BandMap::Linear;
            m.ratio = b.hasScale ? b.scale : 1;
            m.offset = b.hasOffset ? b.offset : 0;
            m.complexTag = true;
            // explicit but valueless scaling serializes as identity
            m.emitScalePair = (b.hasScale || b.hasOffset) &&
                              !(m.ratio == 1 && m.offset == 0);
            if (!m.emitScalePair)
                m.kind = BandMap::Copy;
            if (b.hasNodata)
            {
                m.passNodata = true;
                m.nodata = b.nodata;
            }
            maps.push_back(m);
            types.push_back(p.hasOt ? p.ot : unscaleDefaultType(b.type));
        }
        d = std::make_unique<PixelMapDataset>(std::move(d), maps, types,
                                              true, true);
        return 0;
    }

    // scale
    if (p.hasBand && p.band > nb)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Value of 'band' should be greater "
                              "or equal than 1 and less or equal "
                              "than %d.",
                              verb.c_str(), nb));
        if (leafUsage)
            handlerPrintUsage();
        return 1;
    }
    int bandSel = p.hasBand ? p.band : 0;
    for (int bi = 1; bi <= nb; ++bi)
    {
        const Band &b = d->bands[(size_t)bi - 1];
        DType dt = p.hasOt ? p.ot : b.type;
        types.push_back(dt);
        BandMap m;
        if (bandSel && bi != bandSel)
        {
            maps.push_back(m);
            continue;
        }
        double sMin = p.srcMin, sMax = p.srcMax;
        if (!p.hasSrcMin)
        {
            int dgBand = bi;
            RasterDatasetBase *dg = d->statsDelegate(bi, dgBand);
            if (dg != d.get())
            {
                // an auto range through a full-cover wrapper reads the
                // source band's cached statistics, or computes exact
                // ones that land in the source PAM sidecar
                Band &db = dg->bands[(size_t)dgBand - 1];
                const std::string *lo = db.getMd("", "STATISTICS_MINIMUM");
                const std::string *hi = db.getMd("", "STATISTICS_MAXIMUM");
                if (lo && hi)
                {
                    sMin = strtod(lo->c_str(), nullptr);
                    sMax = strtod(hi->c_str(), nullptr);
                }
                else
                {
                    // approx-forced like the reference: the source keeps
                    // exact-or-sampled stats in its PAM, a file-backed
                    // VRT adopts them inline flagged APPROXIMATE
                    StatsResult rs = vrtAwareForcedStats(
                        *d, d->bands[(size_t)bi - 1], true, nullptr, 0,
                        0);
                    if (dg->pamDirty)
                        dg->persistPam();
                    if (d->pamDirty && dynamic_cast<VrtDataset *>(d.get()))
                        d->persistPam();
                    sMin = rs.ok ? rs.mn : 0;
                    sMax = rs.ok ? rs.mx : 0;
                }
            }
            else if (!bandMinMax(*d, bi, sMin, sMax))
                return 1;
        }
        // the reference nudges a degenerate source range by 0.1
        if (sMax == sMin)
            sMax += 0.1;
        double dMin = p.dstMin, dMax = p.dstMax;
        if (!p.hasDstMin)
            scaleDstDefault(dt, dMin, dMax);
        m.complexTag = true;
        m.srcMin = sMin;
        m.srcMax = sMax;
        m.dstMin = dMin;
        m.dstMax = dMax;
        m.exponent = p.exp;
        m.clip = !p.noClip;
        m.wrkFloat = dt == DType::Byte || dt == DType::Int8 ||
                     dt == DType::UInt16 || dt == DType::Int16;
        if (b.hasNodata)
        {
            m.passNodata = true;
            m.nodata = b.nodata;
        }
        if (!p.noClip || p.hasExp)
        {
            // exponential scaling never touches complex data: the map is
            // serialized but reads pass through unchanged
            m.kind = dtypeIsComplex(b.type) ? BandMap::Copy
                                            : BandMap::Expo;
            m.emitExpo = true;
        }
        else
        {
            m.kind = BandMap::Linear;
            m.ratio = (dMax - dMin) / (sMax - sMin);
            m.offset = dMin - sMin * m.ratio;
            m.emitScalePair = true;
        }
        maps.push_back(m);
    }
    d = std::make_unique<PixelMapDataset>(std::move(d), maps, types,
                                          false, true);
    return 0;
}

// ------------------------------------------------------------------
// dem family (slope / aspect / hillshade / roughness / tpi / tri)
// ------------------------------------------------------------------

enum class DemVerb
{
    Slope,
    Hillshade,
    Aspect,
    Roughness,
    Tpi,
    Tri
};

struct DemParams
{
    std::string verb;
    DemVerb kind = DemVerb::Slope;
    bool hasBand = false;
    int band = 1;
    bool hasZ = false;
    double z = 1.0;
    bool hasXs = false, hasYs = false;
    double xs = 1.0, ys = 1.0;
    bool hasAz = false;
    double az = 315.0;
    bool hasAlt = false;
    double alt = 45.0;
    bool hasGrad = false;
    std::string gradName = "Horn";
    bool zt = false;
    bool hasUnit = false;
    std::string unitName = "degree";
    bool percent = false;
    bool hasConv = false;
    std::string convName = "azimuth";
    bool trigAngle = false;
    bool zeroForFlat = false;
    bool hasVariant = false;
    std::string variantName = "regular";
    int variant = 0;  // 0 regular, 1 combined, 2 multidirectional, 3 Igor
    bool hasAlg = false;
    std::string algName = "Riley";
    bool wilson = false;
    bool noEdges = false;
};

bool demVerbName(const std::string &n, DemVerb &kind)
{
    if (n == "slope")
        kind = DemVerb::Slope;
    else if (n == "hillshade")
        kind = DemVerb::Hillshade;
    else if (n == "aspect")
        kind = DemVerb::Aspect;
    else if (n == "roughness")
        kind = DemVerb::Roughness;
    else if (n == "tpi")
        kind = DemVerb::Tpi;
    else if (n == "tri")
        kind = DemVerb::Tri;
    else
        return false;
    return true;
}

DemParams demFillParams(const std::string &verb, const TailGetter &get)
{
    DemParams p;
    p.verb = verb;
    demVerbName(verb, p.kind);
    const CmdSpec *cs = Spec::instance().findById("raster_" + verb);
    auto first = [&](const std::string &n) -> const std::string * {
        const auto *v = get(n);
        return v && !v->empty() ? &(*v)[0] : nullptr;
    };
    auto canon = [&](const std::string &arg, const std::string &v)
    { return cs ? canonChoice(*cs, arg, v) : v; };
    if (const std::string *v = first("band"))
    {
        p.hasBand = true;
        p.band = atoi(v->c_str());
    }
    if (const std::string *v = first("zfactor"))
    {
        p.hasZ = true;
        p.z = atof(v->c_str());
    }
    if (const std::string *v = first("xscale"))
    {
        p.hasXs = true;
        p.xs = atof(v->c_str());
    }
    if (const std::string *v = first("yscale"))
    {
        p.hasYs = true;
        p.ys = atof(v->c_str());
    }
    if (const std::string *v = first("azimuth"))
    {
        p.hasAz = true;
        p.az = atof(v->c_str());
    }
    if (const std::string *v = first("altitude"))
    {
        p.hasAlt = true;
        p.alt = atof(v->c_str());
    }
    if (const std::string *v = first("gradient-alg"))
    {
        p.hasGrad = true;
        p.gradName = canon("gradient-alg", *v);
        p.zt = p.gradName == "ZevenbergenThorne";
    }
    if (const std::string *v = first("unit"))
    {
        p.hasUnit = true;
        p.unitName = canon("unit", *v);
        p.percent = p.unitName == "percent";
    }
    if (const std::string *v = first("convention"))
    {
        p.hasConv = true;
        p.convName = canon("convention", *v);
        p.trigAngle = p.convName == "trigonometric-angle";
    }
    if (const std::string *v = first("zero-for-flat"))
        p.zeroForFlat = *v == "true";
    if (const std::string *v = first("variant"))
    {
        p.hasVariant = true;
        p.variantName = canon("variant", *v);
        if (p.variantName == "combined")
            p.variant = 1;
        else if (p.variantName == "multidirectional")
            p.variant = 2;
        else if (p.variantName == "Igor")
            p.variant = 3;
    }
    if (const std::string *v = first("algorithm"))
    {
        p.hasAlg = true;
        p.algName = canon("algorithm", *v);
        p.wilson = p.algName == "Wilson";
    }
    if (const std::string *v = first("no-edges"))
        p.noEdges = *v == "true";
    return p;
}

std::string demArgsEcho(const DemParams &p)
{
    std::string e;
    if (p.hasBand)
        e += strPrintf(" --band %d", p.band);
    if (p.hasZ)
        e += " --zfactor " + fmtG(p.z);
    if (p.hasUnit)
        e += " --unit " + p.unitName;
    if (p.hasConv)
        e += " --convention " + p.convName;
    if (p.hasXs)
        e += " --xscale " + fmtG(p.xs);
    if (p.hasYs)
        e += " --yscale " + fmtG(p.ys);
    if (p.hasAz)
        e += " --azimuth " + fmtG(p.az);
    if (p.hasAlt)
        e += " --altitude " + fmtG(p.alt);
    if (p.hasGrad)
        e += " --gradient-alg " + p.gradName;
    if (p.hasVariant)
        e += " --variant " + p.variantName;
    if (p.hasAlg)
        e += " --algorithm " + p.algName;
    if (p.zeroForFlat)
        e += " --zero-for-flat";
    if (p.noEdges)
        e += " --no-edges";
    return e;
}

// the reference computes the whole dem chain in float32, with
// precomputed reciprocal gradient factors; every operation below
// mirrors that arithmetic exactly
class DemDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    DemParams p;
    float fx = 0, fy = 0;
    bool srcHasNd = false;
    float srcNd = 0;
    float ndOut = 0;
    std::vector<float> data;    // float32 verbs
    std::vector<uint8_t> bytes;  // hillshade
    bool computed = false;

    DemDataset(std::unique_ptr<RasterDatasetBase> s, const DemParams &pp)
        : src(std::move(s)), p(pp)
    {
        width = src->width;
        height = src->height;
        hasGT = src->hasGT;
        demWriteDefaultGt = !src->hasGT && p.kind == DemVerb::Hillshade;
        memcpy(gt, src->gt, sizeof gt);
        hasSrs = src->hasSrs;
        srsSynthetic = src->srsSynthetic;
        inMemoryVrtCopy = true;  // already presented (no driver, no files)
        const Band &sb = src->bands[(size_t)p.band - 1];
        srcHasNd = sb.hasNodata;
        srcNd = (float)sb.nodata;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();

        double xs = 1.0, ys = 1.0;
        if (p.hasXs)
        {
            xs = p.xs;
            ys = p.ys;
        }
        else if (src->hasSrs && src->srs.isGeographic())
        {
            double a = src->srs.semiMajor();
            if (a > 0)
            {
                double ky = a * M_PI / 180.0;
                double latc = gt[3] + gt[4] * (width / 2.0) +
                              gt[5] * (height / 2.0);
                xs = ky * std::cos(latc * M_PI / 180.0);
                ys = ky;
            }
        }
        double d = p.zt ? 2.0 : 8.0;
        fx = (float)p.z / (float)(d * gt[1] * xs);
        fy = (float)p.z / (float)(d * gt[5] * ys);

        srs = std::move(src->srs);

        Band b;
        b.index = 1;
        b.type = p.kind == DemVerb::Hillshade ? DType::Byte
                                              : DType::Float32;
        b.blockX = width;
        b.blockY = 1;
        b.colorInterp = "Undefined";
        // aspect --zero-for-flat writes zeros into masked cells and
        // declares no nodata value at all
        if (p.kind == DemVerb::Aspect && p.zeroForFlat)
        {
            ndOut = 0.0f;
        }
        else
        {
            b.hasNodata = true;
            b.nodata = p.kind == DemVerb::Hillshade ? 0.0 : -9999.0;
            ndOut = p.kind == DemVerb::Hillshade ? 0.0f : -9999.0f;
        }
        bands.push_back(std::move(b));
    }

    bool isnd(float v) const { return srcHasNd && v == srcNd; }

    float interpol(float pcl, float q) const
    {
        if (isnd(pcl) || isnd(q))
            return srcNd;
        return 2.0f * pcl - q;
    }

    // 3x3 window around (x,y) with the reference's edge synthesis
    void window(const std::vector<float> &a, int x, int y,
                float w[9]) const
    {
        const int W = width, H = height;
        auto at = [&](int yy, int xx) { return a[(size_t)yy * W + xx]; };
        if (y == 0 || y == H - 1)
        {
            int jm = x == 0 ? x : x - 1;
            int jp = x == W - 1 ? x : x + 1;
            const int js[3] = {jm, x, jp};
            if (y == 0)
            {
                for (int k = 0; k < 3; ++k)
                    w[k] = interpol(at(0, js[k]), at(1, js[k]));
                for (int k = 0; k < 3; ++k)
                    w[3 + k] = at(0, js[k]);
                for (int k = 0; k < 3; ++k)
                    w[6 + k] = at(1, js[k]);
            }
            else
            {
                for (int k = 0; k < 3; ++k)
                    w[k] = at(H - 2, js[k]);
                for (int k = 0; k < 3; ++k)
                    w[3 + k] = at(H - 1, js[k]);
                for (int k = 0; k < 3; ++k)
                    w[6 + k] =
                        interpol(at(H - 1, js[k]), at(H - 2, js[k]));
            }
            return;
        }
        int i = 0;
        for (int yy = y - 1; yy <= y + 1; ++yy)
        {
            if (x == 0)
            {
                w[i++] = interpol(at(yy, 0), at(yy, 1));
                w[i++] = at(yy, 0);
                w[i++] = at(yy, 1);
            }
            else if (x == W - 1)
            {
                w[i++] = at(yy, W - 2);
                w[i++] = at(yy, W - 1);
                w[i++] = interpol(at(yy, W - 1), at(yy, W - 2));
            }
            else
            {
                w[i++] = at(yy, x - 1);
                w[i++] = at(yy, x);
                w[i++] = at(yy, x + 1);
            }
        }
    }

    static float fsum(float a, float b, float c)
    {
        return ((a + b) + b) + c;
    }

    void numerators(const float w[9], float &dxn, float &dyn) const
    {
        if (p.zt)
        {
            dxn = w[3] - w[5];
            dyn = w[7] - w[1];
        }
        else
        {
            dxn = fsum(w[0], w[3], w[6]) - fsum(w[2], w[5], w[8]);
            dyn = fsum(w[6], w[7], w[8]) - fsum(w[0], w[1], w[2]);
        }
    }

    static int toByte(float v)
    {
        int i = (int)(v + 0.5);
        return i < 0 ? 0 : i > 255 ? 255 : i;
    }

    void compute()
    {
        if (computed)
            return;
        computed = true;
        const int W = width, H = height;
        std::vector<double> dsrc;
        src->readBand(p.band, dsrc);
        std::vector<float> a(dsrc.size());
        for (size_t i = 0; i < dsrc.size(); ++i)
            a[i] = (float)dsrc[i];
        dsrc.clear();
        dsrc.shrink_to_fit();

        const bool byteOut = p.kind == DemVerb::Hillshade;
        if (byteOut)
            bytes.assign((size_t)W * H, 0);
        else
            data.assign((size_t)W * H, ndOut);

        if (W < 2 || H < 2)
            return;

        const float kRad2Deg = (float)(180.0 / M_PI);
        // hillshade constants
        const float sinAlt = (float)std::sin(p.alt * M_PI / 180.0);
        const float cosAlt = (float)std::cos(p.alt * M_PI / 180.0);
        const float azr = (float)(p.az * M_PI / 180.0);
        const float sinAz = (float)std::sin((double)azr);
        const float cosAz = (float)std::cos((double)azr);
        const float s225 = (float)std::sin(225.0 * M_PI / 180.0);
        const float c225 = (float)std::cos(225.0 * M_PI / 180.0);
        const float s270 = (float)std::sin(270.0 * M_PI / 180.0);
        const float c270 = (float)std::cos(270.0 * M_PI / 180.0);
        const float s315 = (float)std::sin(315.0 * M_PI / 180.0);
        const float c315 = (float)std::cos(315.0 * M_PI / 180.0);
        const float s360 = (float)std::sin(360.0 * M_PI / 180.0);
        const float c360 = (float)std::cos(360.0 * M_PI / 180.0);
        const float hp = (float)((M_PI / 2) * (M_PI / 2));
        const float igorR = (float)((p.az - 270.0) * M_PI / 180.0);

        auto creg = [&](float dx, float dy, float sAz, float cAz)
        {
            float num = sinAlt - (dy * cAz - dx * sAz) * cosAlt;
            float den = sqrtf(1.0f + (dx * dx + dy * dy));
            return num / den;
        };

        float w[9];
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                const size_t o = (size_t)y * W + x;
                auto out = [&](float v)
                {
                    if (byteOut)
                        bytes[o] = (uint8_t)(int)v;
                    else
                        data[o] = v;
                };
                const float ndv = ndOut;
                if (p.noEdges &&
                    (x == 0 || y == 0 || x == W - 1 || y == H - 1))
                {
                    out(ndv);
                    continue;
                }
                window(a, x, y, w);
                if (isnd(w[4]))
                {
                    out(ndv);
                    continue;
                }
                bool anyNd = false;
                for (int k = 0; k < 9; ++k)
                    if (isnd(w[k]))
                    {
                        anyNd = true;
                        w[k] = w[4];
                    }
                if (p.noEdges && anyNd)
                {
                    out(ndv);
                    continue;
                }

                if (p.kind == DemVerb::Roughness)
                {
                    float mx = w[0], mn = w[0];
                    for (int k = 1; k < 9; ++k)
                    {
                        if (w[k] > mx)
                            mx = w[k];
                        if (w[k] < mn)
                            mn = w[k];
                    }
                    out(mx - mn);
                    continue;
                }
                if (p.kind == DemVerb::Tpi)
                {
                    float s = 0.0f;
                    for (int k : {0, 1, 2, 3, 5, 6, 7, 8})
                        s = s + w[k];
                    out(w[4] - s / 8.0f);
                    continue;
                }
                if (p.kind == DemVerb::Tri)
                {
                    if (p.wilson)
                    {
                        float s = 0.0f;
                        for (int k : {0, 1, 2, 3, 5, 6, 7, 8})
                            s = s + fabsf(w[k] - w[4]);
                        out(s / 8.0f);
                        continue;
                    }
                    // Riley accumulates the squared differences in
                    // double, unlike every other float32 reduction
                    double s = 0.0;
                    for (int k : {0, 1, 2, 3, 5, 6, 7, 8})
                    {
                        float d = w[k] - w[4];
                        s += (double)d * (double)d;
                    }
                    out((float)std::sqrt(s));
                    continue;
                }

                float dxn, dyn;
                numerators(w, dxn, dyn);

                if (p.kind == DemVerb::Aspect)
                {
                    float dxa = -dxn, dya = dyn;
                    float A = atan2f(dya, -dxa) * kRad2Deg;
                    if (dxa == 0 && dya == 0)
                    {
                        out(p.zeroForFlat ? 0.0f : -9999.0f);
                        continue;
                    }
                    if (p.trigAngle)
                    {
                        if (A < 0)
                            A = A + 360.0f;
                        if (A == 360.0f)
                            A = 0.0f;
                    }
                    else
                        A = A > 90.0f ? 450.0f - A : 90.0f - A;
                    out(A);
                    continue;
                }

                float dx = dxn * fx, dy = dyn * fy;
                if (p.kind == DemVerb::Slope)
                {
                    float key = dx * dx + dy * dy;
                    float s = sqrtf(key);
                    out(p.percent ? 100.0f * s
                                  : atanf(s) * kRad2Deg);
                    continue;
                }

                // hillshade
                float shade;
                if (p.variant == 3)
                {
                    float key = dx * dx + dy * dy;
                    float ss = atanf(sqrtf(key)) / (float)(M_PI / 2);
                    float asp = atan2f(-dyn, dxn);
                    float dd = fmodf(fabsf(asp - igorR),
                                     (float)(2 * M_PI));
                    if (dd > (float)M_PI)
                        dd = (float)(2 * M_PI) - dd;
                    float asr = 1.0f - dd / (float)M_PI;
                    float sh = 1.0f - ss * asr;
                    out((float)toByte(255.0f * sh));
                    continue;
                }
                if (p.variant == 2)
                {
                    float asp = atan2f(dy, dx);
                    float sn = sinf(asp), cs = cosf(asp);
                    float w225 = 0.5f - sn * cs;
                    float w270 = cs * cs;
                    float w315 = 0.5f + sn * cs;
                    float w360 = sn * sn;
                    float k225 = creg(dx, dy, s225, c225);
                    float k270 = creg(dx, dy, s270, c270);
                    float k315 = creg(dx, dy, s315, c315);
                    float k360 = creg(dx, dy, s360, c360);
                    if (k225 < 0)
                        k225 = 0;
                    if (k270 < 0)
                        k270 = 0;
                    if (k315 < 0)
                        k315 = 0;
                    if (k360 < 0)
                        k360 = 0;
                    shade = (w225 * k225 + w270 * k270 + w315 * k315 +
                             w360 * k360) /
                            2.0f;
                }
                else
                {
                    float c = creg(dx, dy, sinAz, cosAz);
                    if (p.variant == 1)
                    {
                        float key = dx * dx + dy * dy;
                        float sl = atanf(sqrtf(key));
                        c = 1.0f - (acosf(c) * sl) / hp;
                    }
                    shade = c;
                }
                float v = shade <= 0 ? 1.0f : 1.0f + 254.0f * shade;
                out((float)toByte(v));
            }
        }
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        (void)band;
        compute();
        out.resize((size_t)width * height);
        if (p.kind == DemVerb::Hillshade)
            for (size_t i = 0; i < bytes.size(); ++i)
                out[i] = bytes[i];
        else
            for (size_t i = 0; i < data.size(); ++i)
                out[i] = data[i];
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        (void)band;
        compute();
        if (p.kind == DemVerb::Hillshade)
        {
            out = bytes;
            return true;
        }
        out.resize(data.size() * 4);
        memcpy(out.data(), data.data(), out.size());
        return true;
    }
};

// run-stage checks + wrap; leaf validation-stage errors (band range,
// VRT refusal) are handled by the leaf handler's pre-write hook
int demWrap(const DemParams &p, std::unique_ptr<RasterDatasetBase> &d,
            bool leafUsage)
{
    int nb = (int)d->bands.size();
    if (p.band > nb)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Value of 'band' should be greater "
                              "or equal than 1 and less or equal "
                              "than %d.",
                              p.verb.c_str(), nb));
        if (leafUsage)
            handlerPrintUsage();
        return 1;
    }
    if (p.kind == DemVerb::Hillshade)
    {
        if (p.variant == 2 && p.hasAz)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "'azimuth' argument cannot be used with "
                        "multidirectional variant");
            return 1;
        }
        if (p.variant == 3 && p.hasAlt)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "'altitude' argument cannot be used with Igor "
                        "variant");
            return 1;
        }
    }
    if (p.hasXs != p.hasYs)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "When one of -xscale or -yscale is specified, both "
                    "must be specified.");
        // slope funnels this through the classic library, which has
        // already begun creating the (nameless) output; hillshade
        // bails out before that point
        if (p.kind != DemVerb::Hillshade)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Attempt to create new tiff file `' failed: : No "
                        "such file or directory");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unable to create dataset ");
        }
        return 1;
    }
    d = std::make_unique<DemDataset>(std::move(d), p);
    return 0;
}

// ------------------------------------------------------------------
// clip
// ------------------------------------------------------------------

struct WktScan
{
    const char *p;
};

void wktSkip(WktScan &s)
{
    while (*s.p == ' ' || *s.p == '\t' || *s.p == '\n' || *s.p == '\r')
        ++s.p;
}

bool wktLit(WktScan &s, char c)
{
    wktSkip(s);
    if (*s.p != c)
        return false;
    ++s.p;
    return true;
}

std::string wktWord(WktScan &s)
{
    wktSkip(s);
    std::string w;
    while (isalpha((unsigned char)*s.p))
        w += (char)toupper((unsigned char)*s.p++);
    return w;
}

bool wktParseGeom(WktScan &s, OgrGeometry &g);

bool wktPoint(WktScan &s, OgrGeometry &g, bool zFlag, bool mFlag)
{
    double n[4];
    int cnt = 0;
    while (cnt < 4)
    {
        wktSkip(s);
        char *e = nullptr;
        double v = strtod(s.p, &e);
        if (e == s.p)
            break;
        s.p = e;
        n[cnt++] = v;
    }
    if (cnt < 2)
        return false;
    double x = n[0], y = n[1], z = 0, m = 0;
    bool hasZ = zFlag, hasM = mFlag;
    if (zFlag && mFlag)
    {
        if (cnt != 4)
            return false;
        z = n[2];
        m = n[3];
    }
    else if (mFlag)
    {
        if (cnt != 3)
            return false;
        m = n[2];
    }
    else if (zFlag)
    {
        if (cnt != 3)
            return false;
        z = n[2];
    }
    else if (cnt == 3)
    {
        hasZ = true;
        z = n[2];
    }
    else if (cnt == 4)
    {
        hasZ = true;
        hasM = true;
        z = n[2];
        m = n[3];
    }
    g.coords.push_back(x);
    g.coords.push_back(y);
    g.coords.push_back(z);
    if (hasM)
        g.m.push_back(m);
    g.hasZ = g.hasZ || hasZ;
    g.hasM = g.hasM || hasM;
    return true;
}

bool wktPointSeq(WktScan &s, OgrGeometry &g, bool zFlag, bool mFlag)
{
    if (!wktLit(s, '('))
        return false;
    do
    {
        if (!wktPoint(s, g, zFlag, mFlag))
            return false;
    } while (wktLit(s, ','));
    return wktLit(s, ')');
}

bool wktRingList(WktScan &s, OgrGeometry &g, bool zFlag, bool mFlag)
{
    if (!wktLit(s, '('))
        return false;
    do
    {
        OgrGeometry ring;
        if (!wktPointSeq(s, ring, zFlag, mFlag))
            return false;
        g.hasZ = g.hasZ || ring.hasZ;
        g.hasM = g.hasM || ring.hasM;
        g.parts.push_back(std::move(ring));
    } while (wktLit(s, ','));
    return wktLit(s, ')');
}

bool wktParseGeom(WktScan &s, OgrGeometry &g)
{
    std::string name = wktWord(s);
    static const struct
    {
        const char *n;
        int t;
    } kTypes[] = {
        {"POINT", 1},           {"LINESTRING", 2},
        {"POLYGON", 3},         {"MULTIPOINT", 4},
        {"MULTILINESTRING", 5}, {"MULTIPOLYGON", 6},
        {"GEOMETRYCOLLECTION", 7},
    };
    int type = 0;
    for (const auto &k : kTypes)
        if (name == k.n)
            type = k.t;
    if (!type)
        return false;
    g.type = type;
    bool zFlag = false, mFlag = false;
    {
        WktScan save = s;
        std::string dims = wktWord(s);
        if (dims == "Z")
            zFlag = true;
        else if (dims == "M")
            mFlag = true;
        else if (dims == "ZM")
            zFlag = mFlag = true;
        else if (dims == "EMPTY")
        {
            g.empty = true;
            g.hasZ = zFlag;
            g.hasM = mFlag;
            return true;
        }
        else if (!dims.empty())
            return false;
        else
            s = save;
    }
    g.hasZ = zFlag;
    g.hasM = mFlag;
    {
        WktScan save = s;
        if (wktWord(s) == "EMPTY")
        {
            g.empty = true;
            return true;
        }
        s = save;
    }
    switch (type)
    {
        case 1:
        {
            if (!wktLit(s, '('))
                return false;
            if (!wktPoint(s, g, zFlag, mFlag))
                return false;
            return wktLit(s, ')');
        }
        case 2:
            return wktPointSeq(s, g, zFlag, mFlag);
        case 3:
            return wktRingList(s, g, zFlag, mFlag);
        case 4:
        {
            if (!wktLit(s, '('))
                return false;
            do
            {
                OgrGeometry pt;
                pt.type = 1;
                wktSkip(s);
                bool paren = *s.p == '(';
                if (paren)
                    ++s.p;
                if (!wktPoint(s, pt, zFlag, mFlag))
                    return false;
                if (paren && !wktLit(s, ')'))
                    return false;
                g.hasZ = g.hasZ || pt.hasZ;
                g.hasM = g.hasM || pt.hasM;
                g.parts.push_back(std::move(pt));
            } while (wktLit(s, ','));
            return wktLit(s, ')');
        }
        case 5:
        {
            if (!wktLit(s, '('))
                return false;
            do
            {
                OgrGeometry ls;
                ls.type = 2;
                if (!wktPointSeq(s, ls, zFlag, mFlag))
                    return false;
                g.hasZ = g.hasZ || ls.hasZ;
                g.hasM = g.hasM || ls.hasM;
                g.parts.push_back(std::move(ls));
            } while (wktLit(s, ','));
            return wktLit(s, ')');
        }
        case 6:
        {
            if (!wktLit(s, '('))
                return false;
            do
            {
                OgrGeometry poly;
                poly.type = 3;
                if (!wktRingList(s, poly, zFlag, mFlag))
                    return false;
                g.hasZ = g.hasZ || poly.hasZ;
                g.hasM = g.hasM || poly.hasM;
                g.parts.push_back(std::move(poly));
            } while (wktLit(s, ','));
            return wktLit(s, ')');
        }
        case 7:
        {
            if (!wktLit(s, '('))
                return false;
            do
            {
                OgrGeometry sub;
                if (!wktParseGeom(s, sub))
                    return false;
                g.hasZ = g.hasZ || sub.hasZ;
                g.hasM = g.hasM || sub.hasM;
                g.parts.push_back(std::move(sub));
            } while (wktLit(s, ','));
            return wktLit(s, ')');
        }
    }
    return false;
}

bool clipParseGeometry(const std::string &text, OgrGeometry &g)
{
    WktScan s{text.c_str()};
    OgrGeometry wg;
    if (wktParseGeom(s, wg))
    {
        wktSkip(s);
        if (*s.p == '\0')
        {
            g = std::move(wg);
            return true;
        }
    }
    bool ok = false;
    JVal j = JVal::parse(text, &ok);
    if (!ok)
        return false;
    return ogrGeometryFromJsonValue(j, g);
}

void geomEnvelopeWalk(const OgrGeometry &g, double &xmin, double &ymin,
                      double &xmax, double &ymax, bool &any)
{
    for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
    {
        double x = g.coords[i], y = g.coords[i + 1];
        if (!any || x < xmin)
            xmin = x;
        if (!any || x > xmax)
            xmax = x;
        if (!any || y < ymin)
            ymin = y;
        if (!any || y > ymax)
            ymax = y;
        any = true;
    }
    for (const OgrGeometry &p : g.parts)
        geomEnvelopeWalk(p, xmin, ymin, xmax, ymax, any);
}

bool geomEnvelope(const OgrGeometry &g, double &xmin, double &ymin,
                  double &xmax, double &ymax)
{
    bool any = false;
    xmin = ymin = xmax = ymax = 0;
    geomEnvelopeWalk(g, xmin, ymin, xmax, ymax, any);
    return any;
}

// mirrors OGRPolygon::IsRectangle after ring closing: one 5-point ring
// whose edges never change both x and y
bool geomIsRectangle(const OgrGeometry &g)
{
    if (g.type != 3 || g.empty || g.parts.size() != 1)
        return false;
    std::vector<double> pts;
    for (size_t i = 0; i + 2 < g.parts[0].coords.size() + 1; i += 3)
    {
        pts.push_back(g.parts[0].coords[i]);
        pts.push_back(g.parts[0].coords[i + 1]);
    }
    size_t n = pts.size() / 2;
    if (n >= 3 && (pts[0] != pts[2 * (n - 1)] || pts[1] != pts[2 * n - 1]))
    {
        pts.push_back(pts[0]);
        pts.push_back(pts[1]);
        ++n;
    }
    if (n != 5)
        return false;
    for (size_t i = 0; i < 4; ++i)
    {
        double dx = pts[2 * (i + 1)] - pts[2 * i];
        double dy = pts[2 * (i + 1) + 1] - pts[2 * i + 1];
        if (dx != 0 && dy != 0)
            return false;
    }
    return true;
}

class SubWindowDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    long long rx, ry, rw, rh;
    long long cx = 0, cy = 0, cw = 0, ch = 0, dxo = 0, dyo = 0;
    bool noOverlap = false;
    bool fullCover = false;

    SubWindowDataset(std::unique_ptr<RasterDatasetBase> s, long long x0,
                     long long y0, long long w0, long long h0)
        : src(std::move(s)), rx(x0), ry(y0), rw(w0), rh(h0)
    {
        path = src->path;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = (int)rw;
        height = (int)rh;
        hasGT = src->hasGT;
        memcpy(gt, src->gt, sizeof gt);
        if (hasGT)
        {
            gt[0] = src->gt[0] + rx * src->gt[1] + ry * src->gt[2];
            gt[3] = src->gt[3] + rx * src->gt[4] + ry * src->gt[5];
        }
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
        pamPath = src->pamPath;
        pamExists = src->pamExists;
        pamSrsRaw = src->pamSrsRaw;
        pamSrsMapping = src->pamSrsMapping;
        pamGtRaw = src->pamGtRaw;
        pamMdi = src->pamMdi;
        pamXmlDomains = src->pamXmlDomains;
        pamBands = src->pamBands;
        pamSuppressItems = true;
        cx = std::max(rx, 0LL);
        cy = std::max(ry, 0LL);
        long long ex = std::min(rx + rw, (long long)src->width);
        long long ey = std::min(ry + rh, (long long)src->height);
        cw = ex - cx;
        ch = ey - cy;
        if (cw <= 0 || ch <= 0)
        {
            noOverlap = true;
            cx = rx;
            cy = ry;
            cw = rw;
            ch = rh;
            dxo = dyo = 0;
        }
        else
        {
            dxo = cx - rx;
            dyo = cy - ry;
        }
        fullCover = rx == 0 && ry == 0 && rw == src->width &&
                    rh == src->height;
        for (size_t i = 0; i < src->bands.size(); ++i)
        {
            Band b = src->bands[i];
            b.index = (int)i + 1;
            // GDALTranslate keeps the source block only when the final
            // SrcRect offsets sit on block boundaries
            bool keepBlock = b.blockX > 0 && b.blockY > 0 &&
                             cx % b.blockX == 0 && cy % b.blockY == 0;
            if (!keepBlock)
            {
                b.blockX = std::min(width, 128);
                b.blockY = std::min(height, 128);
            }
            if (!fullCover)
            {
                static const char *const kStatKeys[] = {
                    "STATISTICS_MINIMUM",       "STATISTICS_MAXIMUM",
                    "STATISTICS_MEAN",          "STATISTICS_STDDEV",
                    "STATISTICS_VALID_PERCENT", "STATISTICS_APPROXIMATE",
                };
                for (const char *k : kStatKeys)
                    b.removeMd("", k);
                b.pamHists.clear();
            }
            bands.push_back(std::move(b));
        }
    }

    int histPamMode(int band) override
    {
        int inner = src->histPamMode(band);
        return inner == 0 ? 0 : 1;
    }

    RasterDatasetBase *histDelegateWrap(int band) override
    {
        if (!fullCover)
            return nullptr;
        RasterDatasetBase *inner = src->histDelegateWrap(band);
        return inner ? inner : src.get();
    }

    bool bandMinMaxHint(int band, double &mn, double &mx) override
    {
        if (!fullCover)
            return false;
        const Band &sb = src->bands[(size_t)band - 1];
        const std::string *lo = sb.getMd("", "STATISTICS_MINIMUM");
        const std::string *hi = sb.getMd("", "STATISTICS_MAXIMUM");
        if (lo && hi)
        {
            mn = strtod(lo->c_str(), nullptr);
            mx = strtod(hi->c_str(), nullptr);
            return true;
        }
        return src->bandMinMaxHint(band, mn, mx);
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        if (fullCover)
            return src->readBand(band, out);
        const Band &b = bands[(size_t)band - 1];
        double fill = b.hasNodata ? b.nodata : 0.0;
        out.assign((size_t)rw * (size_t)rh, fill);
        if (noOverlap)
            return true;
        std::vector<double> full;
        if (!src->readBand(band, full))
            return false;
        for (long long j = 0; j < ch; ++j)
        {
            const double *sp =
                full.data() + (cy + j) * (long long)src->width + cx;
            double *dp = out.data() + (dyo + j) * rw + dxo;
            for (long long i = 0; i < cw; ++i)
                dp[i] = sp[i];
        }
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        return rawWindow(band, out, false);
    }

    bool readBandRawStrict(int band, std::vector<uint8_t> &out) override
    {
        return rawWindow(band, out, true);
    }

    bool geoDoubleOrphanHint() override
    {
        return src->geoDoubleOrphanHint();
    }

    bool vrtWrapperRects(WrapRects &wr) override
    {
        wr.srcW = src->width;
        wr.srcH = src->height;
        wr.srcBlockX = src->bands.empty() ? src->width
                                          : src->bands[0].blockX;
        wr.srcBlockY = src->bands.empty() ? src->height
                                          : src->bands[0].blockY;
        wr.sx = cx;
        wr.sy = cy;
        wr.sw = cw;
        wr.sh = ch;
        wr.dx = noOverlap ? 0 : dxo;
        wr.dy = noOverlap ? 0 : dyo;
        wr.dw = noOverlap ? rw : cw;
        wr.dh = noOverlap ? rh : ch;
        return true;
    }

  private:
    bool rawWindow(int band, std::vector<uint8_t> &out, bool strict)
    {
        if (fullCover)
            return strict ? src->readBandRawStrict(band, out)
                          : src->readBandRaw(band, out);
        const Band &b = bands[(size_t)band - 1];
        size_t es = (size_t)dtypeSizeBytes(b.type);
        out.assign((size_t)rw * (size_t)rh * es, 0);
        if (b.hasNodata && b.nodata != 0)
        {
            std::vector<uint8_t> one(es, 0);
            encodeReal(b.type, one.data(), finishReal(b.nodata, b.type),
                       0);
            for (size_t i = 0; i < (size_t)rw * (size_t)rh; ++i)
                memcpy(out.data() + i * es, one.data(), es);
        }
        if (noOverlap)
            return true;
        std::vector<uint8_t> full;
        bool ok = strict ? src->readBandRawStrict(band, full)
                         : src->readBandRaw(band, full);
        if (!ok)
            return false;
        for (long long j = 0; j < ch; ++j)
        {
            const uint8_t *sp =
                full.data() +
                ((cy + j) * (long long)src->width + cx) * (long long)es;
            uint8_t *dp =
                out.data() + ((dyo + j) * rw + dxo) * (long long)es;
            memcpy(dp, sp, (size_t)cw * es);
        }
        return true;
    }
};

struct ClipParams
{
    bool hasBbox = false;
    double bbox[4] = {0, 0, 0, 0};
    bool hasWindow = false;
    long long win[4] = {0, 0, 0, 0};
    bool hasGeometry = false;
    std::string geometry;
    bool hasLike = false;
    std::string like;
    std::string bboxCrs, geometryCrs, likeSql, likeLayer, likeWhere;
    bool onlyBbox = false, allowOutside = false, addAlpha = false;
};

ClipParams clipFillParams(const TailGetter &get)
{
    ClipParams p;
    auto nums = [&](const char *n, std::vector<std::string> &out) {
        const auto *v = get(n);
        if (!v)
            return false;
        for (const auto &e : *v)
        {
            auto parts = strSplit(e, ',');
            out.insert(out.end(), parts.begin(), parts.end());
        }
        return !out.empty();
    };
    std::vector<std::string> b;
    if (nums("bbox", b) && b.size() == 4)
    {
        p.hasBbox = true;
        for (int i = 0; i < 4; i++)
            p.bbox[i] = atof(b[(size_t)i].c_str());
    }
    std::vector<std::string> w;
    if (nums("window", w) && w.size() == 4)
    {
        p.hasWindow = true;
        for (int i = 0; i < 4; i++)
            p.win[i] = strtoll(w[(size_t)i].c_str(), nullptr, 10);
    }
    auto str1 = [&](const char *n, std::string &dst) -> bool {
        const auto *v = get(n);
        if (v && !v->empty())
        {
            dst = (*v)[0];
            return true;
        }
        return false;
    };
    p.hasGeometry = str1("geometry", p.geometry);
    p.hasLike = str1("like", p.like);
    str1("bbox-crs", p.bboxCrs);
    str1("geometry-crs", p.geometryCrs);
    str1("like-sql", p.likeSql);
    str1("like-layer", p.likeLayer);
    str1("like-where", p.likeWhere);
    auto flag = [&](const char *n) {
        const auto *v = get(n);
        return v && !v->empty() && (*v)[0] == "true";
    };
    p.onlyBbox = flag("only-bbox");
    p.allowOutside = flag("allow-bbox-outside-source");
    p.addAlpha = flag("add-alpha");
    return p;
}

std::string gdalgQuote(const std::string &s)
{
    if (!s.empty() && s.find_first_of(" \t\"") == std::string::npos)
        return s;
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"')
            out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string fmt17(double v)
{
    return strPrintf("%.17g", v);
}

std::string clipArgsEcho(const ClipParams &p)
{
    std::string e;
    if (p.hasBbox)
        e += " --bbox " + fmt17(p.bbox[0]) + "," + fmt17(p.bbox[1]) + "," +
             fmt17(p.bbox[2]) + "," + fmt17(p.bbox[3]);
    if (!p.bboxCrs.empty())
        e += " --bbox-crs " + gdalgQuote(p.bboxCrs);
    if (p.hasWindow)
        e += strPrintf(" --window %lld,%lld,%lld,%lld", p.win[0], p.win[1],
                       p.win[2], p.win[3]);
    if (p.hasGeometry)
        e += " --geometry " + gdalgQuote(p.geometry);
    if (!p.geometryCrs.empty())
        e += " --geometry-crs " + gdalgQuote(p.geometryCrs);
    if (p.hasLike)
        e += " --like " + gdalgQuote(p.like);
    if (!p.likeSql.empty())
        e += " --like-sql " + gdalgQuote(p.likeSql);
    if (!p.likeLayer.empty())
        e += " --like-layer " + gdalgQuote(p.likeLayer);
    if (!p.likeWhere.empty())
        e += " --like-where " + gdalgQuote(p.likeWhere);
    if (p.onlyBbox)
        e += " --only-bbox";
    if (p.allowOutside)
        e += " --allow-bbox-outside-source";
    if (p.addAlpha)
        e += " --add-alpha";
    return e;
}

// bounds of the like dataset (raster corners or vector layer handling);
// rc >= 0 aborts with that code
int clipLikeBounds(const ClipParams &p, bool leafUsage, double &xmin,
                   double &ymin, double &xmax, double &ymax, Srs &bboxSrs,
                   bool &haveSrs)
{
    std::string err;
    cplPushQuietHandler();
    auto r = openRaster(p.like, err);
    cplPopHandler();
    if (r)
    {
        if (!r->hasGT)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Dataset '" + p.like +
                            "' has no geotransform matrix. Its bounds "
                            "cannot be established.");
            return 1;
        }
        if (!(r->hasSrs && r->srs.valid()))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Dataset '" + p.like +
                            "' has no CRS. Its bounds cannot be used.");
            return 1;
        }
        double xs[4], ys[4];
        int k = 0;
        for (int j = 0; j <= 1; j++)
            for (int i = 0; i <= 1; i++)
            {
                double px = i ? r->width : 0, py = j ? r->height : 0;
                xs[k] = r->gt[0] + px * r->gt[1] + py * r->gt[2];
                ys[k] = r->gt[3] + px * r->gt[4] + py * r->gt[5];
                ++k;
            }
        xmin = xmax = xs[0];
        ymin = ymax = ys[0];
        for (int i = 1; i < 4; i++)
        {
            xmin = std::min(xmin, xs[i]);
            xmax = std::max(xmax, xs[i]);
            ymin = std::min(ymin, ys[i]);
            ymax = std::max(ymax, ys[i]);
        }
        bboxSrs = std::move(r->srs);
        haveSrs = true;
        return -1;
    }
    std::string verr;
    cplPushQuietHandler();
    auto v = openVectorDataset(p.like, verr, {}, {}, false);
    cplPopHandler();
    if (v && !v->layers.empty())
    {
        const OgrLayer *lyr = nullptr;
        if (!p.likeLayer.empty())
        {
            for (const auto &l : v->layers)
                if (l.name == p.likeLayer)
                    lyr = &l;
        }
        else
            lyr = &v->layers[0];
        if (!lyr)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Failed to identify source layer from "
                        "clipping dataset.");
            return 1;
        }
        for (size_t fi = 0; fi < lyr->features.size(); ++fi)
        {
            if (!lyr->features[fi].hasGeom)
                continue;
            // this build has no GEOS: the validity pre-check always fails
            // on the first geometry
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "GEOS support not enabled.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("clip: Geometry of feature %lld of %s "
                                  "is invalid. You may be able to correct "
                                  "it with 'gdal vector geom make-valid'.",
                                  (long long)fi, p.like.c_str()));
            return 1;
        }
        if (lyr->hasExtent)
        {
            xmin = lyr->extent[0];
            ymin = lyr->extent[1];
            xmax = lyr->extent[2];
            ymax = lyr->extent[3];
            if (lyr->hasSrs && lyr->srs.valid())
            {
                bool ok = false;
                Srs copy = Srs::fromUserInput(lyr->srs.wkt2SingleLine(), ok);
                if (ok)
                {
                    bboxSrs = std::move(copy);
                    haveSrs = true;
                }
            }
            return -1;
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "clip: No clipping geometry found");
        return 1;
    }
    err.clear();
    auto loud = openRaster(p.like, err);
    if (!loud && err != "reported")
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "`" + p.like +
                        "' not recognized as being in a supported file "
                        "format.");
    if (leafUsage)
        handlerPrintUsage();
    return 1;
}

int clipWrap(const ClipParams &p, std::unique_ptr<RasterDatasetBase> &d,
             bool leafUsage)
{
    if (p.hasWindow && (p.win[2] <= 0 || p.win[3] <= 0))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Value of 'window' should be col,line,width,height "
                    "with width > 0 and height > 0");
        if (leafUsage)
            handlerPrintUsage();
        return 1;
    }
    if (p.hasBbox && (p.bbox[0] > p.bbox[2] || p.bbox[1] > p.bbox[3]))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Value of 'bbox' should be xmin,ymin,xmax,ymax with "
                    "xmin <= xmax and ymin <= ymax");
        if (leafUsage)
            handlerPrintUsage();
        return 1;
    }
    long long rx = 0, ry = 0, rw = 0, rh = 0;
    bool fromWindow = false;
    if (p.hasWindow)
    {
        if (p.addAlpha)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "clip: 'alpha' argument is not supported with "
                        "'window'");
            return 1;
        }
        fromWindow = true;
        rx = p.win[0];
        ry = p.win[1];
        rw = p.win[2];
        rh = p.win[3];
    }
    else
    {
        if (!d->hasGT ||
            d->gt[1] * d->gt[5] - d->gt[2] * d->gt[4] == 0)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "clip: Clipping is not supported on a raster "
                        "without a geotransform");
            return 1;
        }
        if (!p.hasBbox && !p.hasGeometry && !p.hasLike)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: --bbox, --geometry or --like must be "
                        "specified");
            return 1;
        }
        double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
        Srs bboxSrs;
        bool haveSrs = false;
        if (p.hasBbox)
        {
            xmin = p.bbox[0];
            ymin = p.bbox[1];
            xmax = p.bbox[2];
            ymax = p.bbox[3];
            if (!p.bboxCrs.empty())
            {
                bool ok = false;
                cplPushQuietHandler();
                bboxSrs = Srs::fromCliInput(p.bboxCrs, ok);
                cplPopHandler();
                haveSrs = ok;
            }
        }
        else if (p.hasGeometry)
        {
            OgrGeometry g;
            if (!clipParseGeometry(p.geometry, g))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "clip: Clipping geometry is neither a valid "
                            "WKT or GeoJSON geometry");
                return 1;
            }
            if (g.type != 3 && g.type != 6)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Cannot open " + ogrWkt(g) + ".");
                return 1;
            }
            bool rect = geomIsRectangle(g);
            bool haveEnv = geomEnvelope(g, xmin, ymin, xmax, ymax);
            if (!p.onlyBbox && !rect)
            {
                // the cutline path (warped VRT) is not built; keep its
                // pre-check choreography, then refuse honestly
                if (!p.allowOutside)
                {
                    bool insideExtent = false;
                    if (haveEnv && d->hasGT)
                    {
                        double ex0 = d->gt[0];
                        double ex1 = d->gt[0] + d->width * d->gt[1] +
                                     d->height * d->gt[2];
                        double ey0 = d->gt[3];
                        double ey1 = d->gt[3] + d->width * d->gt[4] +
                                     d->height * d->gt[5];
                        double rxmin = std::min(ex0, ex1);
                        double rxmax = std::max(ex0, ex1);
                        double rymin = std::min(ey0, ey1);
                        double rymax = std::max(ey0, ey1);
                        insideExtent = xmin >= rxmin && xmax <= rxmax &&
                                       ymin >= rymin && ymax <= rymax;
                    }
                    if (!insideExtent)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "clip: Clipping geometry is partially or "
                            "totally outside the extent of the raster. "
                            "You can set the "
                            "'allow-bbox-outside-source' argument to "
                            "proceed.");
                        return 1;
                    }
                }
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "clip: geometry cutline clipping is not "
                            "implemented in this build");
                return 1;
            }
            if (!p.geometryCrs.empty())
            {
                bool ok = false;
                cplPushQuietHandler();
                bboxSrs = Srs::fromCliInput(p.geometryCrs, ok);
                cplPopHandler();
                haveSrs = ok;
            }
        }
        else
        {
            int rc = clipLikeBounds(p, leafUsage, xmin, ymin, xmax, ymax,
                                    bboxSrs, haveSrs);
            if (rc >= 0)
                return rc;
        }
        if (p.addAlpha)
        {
            // the alpha channel comes from the warped-VRT cutline
            // machinery, which is not built
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "clip: --add-alpha is not implemented in this "
                        "build");
            return 1;
        }
        if (haveSrs)
        {
            if (!(d->hasSrs && d->srs.valid()))
                cplErrorStr(CE_Warning, CPLE_None,
                            "-projwin_srs ignored since the dataset has "
                            "no projection.");
            else if (bboxSrs.wkt2SingleLine() != d->srs.wkt2SingleLine())
                bboxSrs.transformBoundsTo(d->srs, xmin, ymin, xmax, ymax);
        }
        double det = d->gt[1] * d->gt[5] - d->gt[2] * d->gt[4];
        double inv1 = d->gt[5] / det, inv2 = -d->gt[2] / det;
        double inv4 = -d->gt[4] / det, inv5 = d->gt[1] / det;
        auto toPix = [&](double gx, double gy, double &px, double &py) {
            double ox = gx - d->gt[0], oy = gy - d->gt[3];
            px = inv1 * ox + inv2 * oy;
            py = inv4 * ox + inv5 * oy;
        };
        double px0, py0, px1, py1;
        toPix(xmin, ymax, px0, py0);
        toPix(xmax, ymin, px1, py1);
        double xoff = std::floor(px0 + 0.001);
        double yoff = std::floor(py0 + 0.001);
        double xsz = std::ceil(px1 - xoff - 0.001);
        double ysz = std::ceil(py1 - yoff - 0.001);
        if (!(xsz > 0) || !(ysz > 0))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Error: Computed source window "
                                  "(x,y)=(%g,%g), (width,height)=(%g,%g) "
                                  "has negative width and/or height.",
                                  xoff, yoff, xsz, ysz));
            return 1;
        }
        rx = (long long)xoff;
        ry = (long long)yoff;
        rw = (long long)xsz;
        rh = (long long)ysz;
    }
    bool inside = rx >= 0 && ry >= 0 && rx + rw <= d->width &&
                  ry + rh <= d->height;
    if (!inside)
    {
        bool overlap = rx < d->width && ry < d->height && rx + rw > 0 &&
                       ry + rh > 0;
        const char *kind = overlap ? "partially" : "completely";
        if (!p.allowOutside)
        {
            cplErrorStr(
                CE_Failure, CPLE_AppDefined,
                strPrintf("%s-srcwin %lld %lld %lld %lld falls %s "
                          "outside source raster extent.",
                          fromWindow ? "" : "Computed ", rx, ry, rw, rh,
                          kind));
            return 1;
        }
        if (fromWindow)
            cplErrorStr(
                CE_Warning, CPLE_AppDefined,
                strPrintf("-srcwin %lld %lld %lld %lld falls %s outside "
                          "source raster extent. Pixels outside the "
                          "source raster extent will be set to the "
                          "NoData value (if defined), or zero.",
                          rx, ry, rw, rh, kind));
    }
    d = std::make_unique<SubWindowDataset>(std::move(d), rx, ry, rw, rh);
    return 0;
}

// ------------------------------------------------------------------
// leaf handlers
// ------------------------------------------------------------------

int tailLeafHandler(const std::string &verb, ParseResult &r)
{
    PrefixScope prefix(verb.c_str());
    TailCommon tc;
    int rc = tailBegin(verb, r, tc);
    if (rc >= 0)
        return rc;
    TailParams p = tailFillParams(
        verb, [&](const std::string &n) -> const std::vector<std::string> * {
            const ArgValue *v = r.get(n);
            return v ? &v->values : nullptr;
        });
    std::string extra = tailArgsEcho(verb, p);
    auto materialize = [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        return tailWrap(verb, p, d, true);
    };
    return rasterConvertWriteOutput(tc.ds, r, tc.input, tc.output, tc.quiet,
                                    tc.overwrite, tc.append, tc.drv, extra,
                                    materialize);
}

int rasterScalePreValidator(const CmdSpec &, ParseResult &r)
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

int demLeafHandler(const std::string &verb, ParseResult &r)
{
    PrefixScope prefix(verb.c_str());
    TailCommon tc;
    int rc = tailBegin(verb, r, tc);
    if (rc >= 0)
        return rc;
    DemParams p = demFillParams(
        verb, [&](const std::string &n) -> const std::vector<std::string> * {
            const ArgValue *v = r.get(n);
            return v ? &v->values : nullptr;
        });
    std::string extra = demArgsEcho(p);
    auto materialize = [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        return demWrap(p, d, true);
    };
    int nb = (int)tc.ds->bands.size();
    auto preValidate = [&](const std::string &dg, bool &failed)
    {
        if (dg == "VRT")
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        verb + ": VRT output is not supported. Consider "
                               "using the GDALG driver instead (files "
                               "with .gdalg.json extension)");
            failed = true;
        }
        if (p.band > nb)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("%s: Value of 'band' should be greater "
                                  "or equal than 1 and less or equal "
                                  "than %d.",
                                  verb.c_str(), nb));
            failed = true;
        }
    };
    return rasterConvertWriteOutput(tc.ds, r, tc.input, tc.output, tc.quiet,
                                    tc.overwrite, tc.append, tc.drv, extra,
                                    materialize, preValidate);
}

int clipLeafHandler(ParseResult &r)
{
    PrefixScope prefix("clip");
    TailCommon tc;
    int rc = tailBegin("clip", r, tc);
    if (rc >= 0)
        return rc;
    ClipParams p = clipFillParams(
        [&](const std::string &n) -> const std::vector<std::string> * {
            const ArgValue *v = r.get(n);
            return v ? &v->values : nullptr;
        });
    std::string extra = clipArgsEcho(p);
    auto materialize = [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        return clipWrap(p, d, true);
    };
    return rasterConvertWriteOutput(tc.ds, r, tc.input, tc.output, tc.quiet,
                                    tc.overwrite, tc.append, tc.drv, extra,
                                    materialize);
}

int reprojectLeafHandler(ParseResult &r)
{
    PrefixScope prefix("reproject");
    TailCommon tc;
    int rc = tailBegin("reproject", r, tc);
    if (rc >= 0)
        return rc;
    WarpParams p = warpFillParams(
        [&](const std::string &n) -> const std::vector<std::string> * {
            const ArgValue *v = r.get(n);
            return v ? &v->values : nullptr;
        });
    std::string extra = warpArgsEcho(p);
    auto materialize = [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        return warpWrap(p, d, true);
    };
    return rasterConvertWriteOutput(tc.ds, r, tc.input, tc.output, tc.quiet,
                                    tc.overwrite, tc.append, tc.drv, extra,
                                    materialize);
}

int rasterReprojectArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName != "bbox")
        return 0;
    const ArgValue *bb = r.get("bbox");
    if (bb && bb->values.size() == 4)
    {
        double x0 = atof(bb->values[0].c_str());
        double y0 = atof(bb->values[1].c_str());
        double x1 = atof(bb->values[2].c_str());
        double y1 = atof(bb->values[3].c_str());
        if (x0 > x1 || y0 > y1)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Value of 'bbox' should be xmin,ymin,xmax,ymax "
                        "with xmin <= xmax and ymin <= ymax");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "reproject: Invalid bounding box specified");
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

int rasterClipArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName == "window")
    {
        const ArgValue *w = r.get("window");
        if (w && w->values.size() == 4)
        {
            long long a = strtoll(w->values[2].c_str(), nullptr, 10);
            long long b = strtoll(w->values[3].c_str(), nullptr, 10);
            if (a <= 0 || b <= 0)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Value of 'window' should be col,line,width,"
                            "height with width > 0 and height > 0");
                handlerPrintUsage();
                return 1;
            }
        }
    }
    else if (argName == "bbox")
    {
        const ArgValue *bb = r.get("bbox");
        if (bb && bb->values.size() == 4)
        {
            double x0 = atof(bb->values[0].c_str());
            double y0 = atof(bb->values[1].c_str());
            double x1 = atof(bb->values[2].c_str());
            double y1 = atof(bb->values[3].c_str());
            if (x0 > x1 || y0 > y1)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Value of 'bbox' should be xmin,ymin,xmax,"
                            "ymax with xmin <= xmax and ymin <= ymax");
                handlerPrintUsage();
                return 1;
            }
        }
    }
    return 0;
}

}  // namespace

double rasterFinishReal(double v, DType t)
{
    return finishReal(v, t);
}

bool clipGeometryParseText(const std::string &text, OgrGeometry &g)
{
    return clipParseGeometry(text, g);
}

bool clipGeometryEnvelope(const OgrGeometry &g, double &xmin, double &ymin,
                          double &xmax, double &ymax)
{
    return geomEnvelope(g, xmin, ymin, xmax, ymax);
}

void rasterEncodeReal(DType t, uint8_t *q, double re, double im)
{
    encodeReal(t, q, re, im);
}

// ------------------------------------------------------------------
// pipeline step interface
// ------------------------------------------------------------------

bool rasterDemStepName(const std::string &name)
{
    DemVerb kind;
    return demVerbName(name, kind);
}

bool rasterTailStepKnown(const std::string &name)
{
    return name == "set-type" || name == "scale" || name == "unscale" ||
           name == "clip" || name == "reproject" || name == "edit" ||
           name == "select" || rasterDemStepName(name);
}

std::string rasterTailStepEcho(const std::string &name,
                               const PipeStepArgs &args)
{
    auto get = [&](const std::string &n) -> const std::vector<std::string> * {
        auto it = args.find(n);
        return it == args.end() ? nullptr : &it->second;
    };
    if (name == "clip")
        return " ! clip" + clipArgsEcho(clipFillParams(get));
    if (name == "reproject")
        return " ! reproject" + warpArgsEcho(warpFillParams(get));
    if (name == "edit")
        return editStepEcho(args);
    if (name == "select")
    {
        std::string s = " ! select";
        if (const auto *b = get("band"))
        {
            s += " --band ";
            for (size_t i = 0; i < b->size(); ++i)
            {
                if (i)
                    s += ",";
                s += (*b)[i];
            }
        }
        if (get("exclude"))
            s += " --exclude";
        if (const auto *m = get("mask"))
            if (!m->empty())
                s += " --mask " + (*m)[0];
        return s;
    }
    if (rasterDemStepName(name))
        return " ! " + name + demArgsEcho(demFillParams(name, get));
    TailParams p = tailFillParams(name, get);
    return " ! " + name + tailArgsEcho(name, p);
}

int rasterTailApplyPipeStep(const std::string &name,
                            const PipeStepArgs &args,
                            std::unique_ptr<RasterDatasetBase> &ds)
{
    auto get = [&](const std::string &n) -> const std::vector<std::string> * {
        auto it = args.find(n);
        return it == args.end() ? nullptr : &it->second;
    };
    if (name == "clip")
        return clipWrap(clipFillParams(get), ds, false);
    if (name == "reproject")
    {
        PrefixScope prefix("reproject");
        return warpWrap(warpFillParams(get), ds, false);
    }
    if (name == "edit")
        return editApplyPipeStep(args, ds);
    if (name == "select")
        return rasterSelectApplyPipeStep(args, ds);
    if (rasterDemStepName(name))
        return demWrap(demFillParams(name, get), ds, false);
    TailParams p = tailFillParams(name, get);
    return tailWrap(name, p, ds, false);
}

void presentAsTranslatedVrt(RasterDatasetBase &ds)
{
    ds.driverShort = "VRT";
    ds.driverLong = "Virtual Raster";
    ds.path = "";
    // VRT file lists name the source file but never its PAM sidecar
    if (!ds.pamPath.empty())
        ds.files.erase(
            std::remove(ds.files.begin(), ds.files.end(), ds.pamPath),
            ds.files.end());
    if (std::find(ds.sortedDomains.begin(), ds.sortedDomains.end(), "") ==
        ds.sortedDomains.end())
        ds.sortedDomains.push_back("");
    // wrapper bands already carry the GDALTranslate block rule (source
    // block kept when the source window is block-aligned)
    for (Band &b : ds.bands)
    {
        if (std::find(b.sortedDomains.begin(), b.sortedDomains.end(),
                      "") == b.sortedDomains.end())
            b.sortedDomains.push_back("");
    }
    std::string interleave, compression;
    auto it = ds.metadata.find("IMAGE_STRUCTURE");
    if (it != ds.metadata.end())
        for (const auto &kv : it->second)
        {
            if (kv.first == "INTERLEAVE")
                interleave = kv.second;
            else if (kv.first == "COMPRESSION")
                compression = kv.second;
        }
    std::vector<std::pair<std::string, std::string>> is;
    if (!interleave.empty())
        is.push_back({"INTERLEAVE", interleave});
    if (!compression.empty())
        is.push_back({"COMPRESSION", compression});
    if (is.empty())
    {
        if (it != ds.metadata.end())
        {
            ds.metadata.erase("IMAGE_STRUCTURE");
            ds.domainOrder.erase(std::remove(ds.domainOrder.begin(),
                                             ds.domainOrder.end(),
                                             "IMAGE_STRUCTURE"),
                                 ds.domainOrder.end());
            ds.sortedDomains.erase(std::remove(ds.sortedDomains.begin(),
                                               ds.sortedDomains.end(),
                                               "IMAGE_STRUCTURE"),
                                   ds.sortedDomains.end());
        }
    }
    else
        ds.metadata["IMAGE_STRUCTURE"] = is;
}

void registerRasterTailHandlers()
{
    for (const char *verb : {"set-type", "scale", "unscale"})
        registerHandler(std::string("raster_") + verb,
                        [verb](const CmdSpec &, ParseResult &r) {
                            return tailLeafHandler(verb, r);
                        });
    registerPreValidator("raster_scale", rasterScalePreValidator);
    for (const char *verb : {"slope", "hillshade", "aspect", "roughness",
                             "tpi", "tri"})
    {
        registerHandler(std::string("raster_") + verb,
                        [verb](const CmdSpec &, ParseResult &r) {
                            return demLeafHandler(verb, r);
                        });
        registerPreValidator(std::string("raster_") + verb,
                             rasterScalePreValidator);
    }
    registerHandler("raster_clip", [](const CmdSpec &, ParseResult &r) {
        return clipLeafHandler(r);
    });
    registerHandler("raster_reproject", [](const CmdSpec &, ParseResult &r) {
        return reprojectLeafHandler(r);
    });
    registerArgValueCheck(
        "raster_reproject",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName == "src-crs" || argName == "dst-crs" ||
                argName == "bbox-crs")
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return "Invalid value for '" + argName + "' argument";
                return "";
            }
            if (argName == "num-threads" && !warpNumThreadsValid(value))
                return "\x05Invalid value for 'num-threads' argument";
            return "";
        });
    registerArgCheck("raster_reproject", rasterReprojectArgCheck);
    registerArgCheck("raster_clip", rasterClipArgCheck);
    registerArgValueCheck(
        "raster_clip",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName != "bbox-crs" && argName != "geometry-crs")
                return "";
            bool ok = false;
            Srs::fromCliInput(value, ok, true);
            if (!ok)
                return "Invalid value for '" + argName + "' argument";
            return "";
        });
}
