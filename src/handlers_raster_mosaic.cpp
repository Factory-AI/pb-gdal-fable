// gdal raster mosaic / gdal raster stack: virtual (VRT), materialized
// (GTiff/MEM/stream) and GDALG outputs composed from a source-window
// model that mirrors the reference's buildvrt-style pipeline
#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "progress.h"
#include "spec.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

bool fileExistsMs2(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

struct PrefixScopeMs
{
    bool active;
    explicit PrefixScopeMs(const char *name)
    {
        active = g_pipelineStepPrefix.empty();
        if (active)
            g_pipelineStepPrefix = name;
    }
    ~PrefixScopeMs()
    {
        if (active)
            g_pipelineStepPrefix.clear();
    }
};

std::string fmt18Ms(double d)
{
    if (std::isnan(d))
        return "nan";
    return strPrintf("%.18g", d);
}

std::string fmt17Ms(double d)
{
    if (std::isnan(d))
        return "nan";
    return strPrintf("%.17g", d);
}

std::string fmtRectMs(double d)
{
    return strPrintf("%.15g", d);
}

std::string xmlEscTextMs(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '&':
                r += "&amp;";
                break;
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            default:
                r += c;
        }
    }
    return r;
}

std::string xmlEscAttrMs(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '&':
                r += "&amp;";
                break;
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            case '"':
                r += "&quot;";
                break;
            default:
                r += c;
        }
    }
    return r;
}

std::string gdalgQuoteMs(const std::string &tok)
{
    if (tok.find_first_of(" \",\\") == std::string::npos)
        return tok;
    std::string r = "\"";
    for (char c : tok)
    {
        if (c == '"' || c == '\\')
            r += '\\';
        r += c;
    }
    r += '"';
    return r;
}

std::string baseNameOfMs(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string dirNameMs(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

// CPLStrtod-flavoured full-token scan (copied from the calc unit)
bool scanDoubleMs(const char *&p, double &out)
{
    const char *q = p;
    while (*q == ' ' || *q == '\t')
        q++;
    const char *s = q;
    if (*q == '+' || *q == '-')
        q++;
    auto ciMatch = [&](const char *word) -> bool
    {
        const char *a = q;
        for (const char *w = word; *w; ++w, ++a)
            if (tolower((unsigned char)*a) != *w)
                return false;
        q = a;
        return true;
    };
    if (ciMatch("nan"))
    {
        out = std::nan("");
        p = q;
        return true;
    }
    if (ciMatch("infinity") || ciMatch("inf"))
    {
        out = (*s == '-') ? -HUGE_VAL : HUGE_VAL;
        p = q;
        return true;
    }
    bool any = false;
    while (isdigit((unsigned char)*q))
    {
        q++;
        any = true;
    }
    if (*q == '.')
    {
        q++;
        while (isdigit((unsigned char)*q))
        {
            q++;
            any = true;
        }
    }
    if (!any)
        return false;
    if (*q == 'e' || *q == 'E')
    {
        const char *e = q + 1;
        if (*e == '+' || *e == '-')
            e++;
        if (isdigit((unsigned char)*e))
        {
            q = e;
            while (isdigit((unsigned char)*q))
                q++;
        }
    }
    out = strtod(std::string(s, (size_t)(q - s)).c_str(), nullptr);
    p = q;
    return true;
}

bool parseFullDoubleMs(const std::string &tok, double &out)
{
    const char *p = tok.c_str();
    if (!scanDoubleMs(p, out))
        return false;
    return *p == '\0';
}

bool valueIsNodataMs(double v, bool hasNd, double nd)
{
    if (!hasNd)
        return false;
    return v == nd || (std::isnan(v) && std::isnan(nd));
}

bool dtypeIntegerMs(DType t)
{
    switch (t)
    {
        case DType::Byte:
        case DType::Int8:
        case DType::UInt16:
        case DType::Int16:
        case DType::UInt32:
        case DType::Int32:
        case DType::UInt64:
        case DType::Int64:
        case DType::CInt16:
        case DType::CInt32:
            return true;
        default:
            return false;
    }
}

bool nodataRepresentableMs(double v, DType t)
{
    if (!dtypeIntegerMs(t))
        return true;
    if (std::isnan(v) || v != std::floor(v))
        return false;
    switch (t)
    {
        case DType::Byte:
            return v >= 0 && v <= 255;
        case DType::Int8:
            return v >= -128 && v <= 127;
        case DType::UInt16:
            return v >= 0 && v <= 65535;
        case DType::Int16:
        case DType::CInt16:
            return v >= -32768 && v <= 32767;
        case DType::UInt32:
            return v >= 0 && v <= 4294967295.0;
        case DType::Int32:
        case DType::CInt32:
            return v >= -2147483648.0 && v <= 2147483647.0;
        case DType::UInt64:
            return v >= 0 && v < 18446744073709551616.0;
        case DType::Int64:
            return v >= -9223372036854775808.0 && v < 9223372036854775808.0;
        default:
            return false;
    }
}

// ------------------------------------------------------------------
// glob / @file input expansion
// ------------------------------------------------------------------

bool wildMatchMs(const char *p, const char *s)
{
    while (*p)
    {
        if (*p == '*')
        {
            while (*p == '*')
                p++;
            if (!*p)
                return true;
            for (const char *t = s;; t++)
            {
                if (wildMatchMs(p, t))
                    return true;
                if (!*t)
                    break;
            }
            return false;
        }
        if (*p == '?')
        {
            if (!*s)
                return false;
            p++;
            s++;
            continue;
        }
        if (*p == '[')
        {
            const char *q = p + 1;
            bool neg = false;
            if (*q == '!' || *q == '^')
            {
                neg = true;
                q++;
            }
            bool matched = false;
            bool first = true;
            char c = *s;
            while (*q && (*q != ']' || first))
            {
                first = false;
                char lo = *q;
                if (q[1] == '-' && q[2] && q[2] != ']')
                {
                    char hi = q[2];
                    if (c >= lo && c <= hi)
                        matched = true;
                    q += 3;
                }
                else
                {
                    if (c == lo)
                        matched = true;
                    q++;
                }
            }
            if (!*q)  // unterminated set: literal '['
            {
                if (*s != '[')
                    return false;
                p++;
                s++;
                continue;
            }
            if (!*s || matched == neg)
                return false;
            p = q + 1;
            s++;
            continue;
        }
        if (*p != *s)
            return false;
        p++;
        s++;
    }
    return !*s;
}

bool hasWildMs(const std::string &s)
{
    return s.find_first_of("*?[") != std::string::npos;
}

std::string joinPathMs(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    if (dir == "/")
        return "/" + name;
    return dir + "/" + name;
}

std::vector<std::string> globExpandMs(const std::string &pattern)
{
    std::vector<std::string> comps;
    {
        std::string cur;
        for (char c : pattern)
        {
            if (c == '/')
            {
                if (!cur.empty())
                    comps.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty())
            comps.push_back(cur);
    }
    bool absolute = !pattern.empty() && pattern[0] == '/';
    std::vector<std::string> cands{absolute ? std::string("/")
                                            : std::string()};
    for (const std::string &comp : comps)
    {
        std::vector<std::string> next;
        if (!hasWildMs(comp))
        {
            for (const std::string &c : cands)
                next.push_back(joinPathMs(c, comp));
        }
        else
        {
            for (const std::string &c : cands)
            {
                DIR *d = opendir(c.empty() ? "." : c.c_str());
                if (!d)
                    continue;
                std::vector<std::string> names;
                while (struct dirent *de = readdir(d))
                {
                    const char *n = de->d_name;
                    if (!strcmp(n, ".") || !strcmp(n, ".."))
                        continue;
                    if (wildMatchMs(comp.c_str(), n))
                        names.push_back(n);
                }
                closedir(d);
                // VSIGlob surfaces directory entries in reversed
                // readdir order
                std::reverse(names.begin(), names.end());
                for (const std::string &n : names)
                    next.push_back(joinPathMs(c, n));
            }
        }
        cands = std::move(next);
    }
    std::vector<std::string> out;
    for (const std::string &c : cands)
    {
        struct stat st;
        if (stat(c.c_str(), &st) == 0)
            out.push_back(c);
    }
    return out;
}

// ------------------------------------------------------------------
// resolution strategies
// ------------------------------------------------------------------

enum ResMode
{
    RES_SAME,
    RES_AVERAGE,
    RES_COMMON,
    RES_HIGHEST,
    RES_LOWEST,
    RES_EXPLICIT
};

bool resolutionNumericMs(const std::string &v, double &x, double &y)
{
    std::vector<std::string> toks;
    {
        std::string cur;
        for (char c : v)
        {
            if (c == ',')
            {
                if (!cur.empty())
                    toks.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty())
            toks.push_back(cur);
    }
    if (toks.size() != 2)
        return false;
    for (const std::string &t : toks)
        if (cplValueType(t) == 0 || numLooksHex(t))
            return false;
    x = strtod(toks[0].c_str(), nullptr);
    y = strtod(toks[1].c_str(), nullptr);
    return x > 0 && y > 0;
}

// rational reconstruction via continued fractions, used by the
// 'common' strategy the way CPLGreatestCommonDivisor resolves the GCD
// of two fractional resolutions
bool ratFromDoubleMs(double v, long long &num, long long &den)
{
    if (!(v > 0) || !std::isfinite(v))
        return false;
    double x = v;
    long long p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    for (int it = 0; it < 64; ++it)
    {
        double fl = std::floor(x);
        if (fl > 9e17 || fl < -9e17)
            return false;
        long long a = (long long)fl;
        long long p2 = a * p1 + p0;
        long long q2 = a * q1 + q0;
        if (q2 == 0)
            return false;
        double approx = (double)p2 / (double)q2;
        if (approx == v)
        {
            num = p2;
            den = q2;
            return true;
        }
        double frac = x - fl;
        if (frac < 1e-14)
        {
            num = p2;
            den = q2;
            return true;
        }
        x = 1.0 / frac;
        p0 = p1;
        q0 = q1;
        p1 = p2;
        q1 = q2;
    }
    num = p1;
    den = q1;
    return q1 != 0;
}

long long gcdMs(long long a, long long b)
{
    while (b)
    {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

bool ratGcdMs(double a, double b, double &out)
{
    long long n1, d1, n2, d2;
    if (!ratFromDoubleMs(a, n1, d1) || !ratFromDoubleMs(b, n2, d2))
        return false;
    __int128 num = (__int128)n1 * d2;
    __int128 num2 = (__int128)n2 * d1;
    __int128 den = (__int128)d1 * d2;
    long long g = gcdMs((long long)num, (long long)num2);
    if (g == 0 || den == 0)
        return false;
    __int128 rn = g;
    long long rg = gcdMs((long long)rn, (long long)den);
    long long fn = (long long)(rn / rg);
    long long fd = (long long)(den / rg);
    if (fd == 0)
        return false;
    out = (double)fn / (double)fd;
    return out > 0;
}

// ------------------------------------------------------------------
// pixel functions (mosaic --pixel-function), adapted from the calc unit
// ------------------------------------------------------------------

enum MosFuncId
{
    MF_SUM,
    MF_MUL,
    MF_MIN,
    MF_MAX,
    MF_MEAN,
    MF_MEDIAN,
    MF_MODE,
    MF_GEOMEAN,
    MF_HARMEAN,
    MF_QUANTILE,
    MF_SQRT,
    MF_EXP,
    MF_LOG10,
    MF_ROUND,
    MF_INV,
    MF_DB,
    MF_DB2AMP,
    MF_DB2POW,
    MF_POW,
    MF_INTERP_LIN,
    MF_INTERP_EXP,
    MF_SCALE,
    MF_NORMDIFF,
    MF_DIFF,
    MF_DIV,
    MF_CMUL,
    MF_COMPLEX,
    MF_POLAR,
    MF_REAL,
    MF_IMAG,
    MF_MOD,
    MF_PHASE,
    MF_CONJ,
    MF_INTENSITY,
    MF_REPLACE_ND,
    MF_RECLASSIFY,
    MF_EXPRESSION
};

enum MosSrcClass
{
    MSC_ANY,
    MSC_ONE,
    MSC_TWO,
    MSC_MUL,
    MSC_INTERP
};

enum MosNdMode
{
    MND_SKIP,
    MND_PROP,
    MND_RAW
};

struct MosFuncInfo
{
    const char *name;
    MosFuncId id;
    MosSrcClass srcClass;
    MosNdMode ndMode;
    bool preserveType;  // keeps the band type as SourceTransferType
};

const MosFuncInfo kMosFuncs[] = {
    {"sum", MF_SUM, MSC_ANY, MND_SKIP, false},
    {"mul", MF_MUL, MSC_MUL, MND_SKIP, false},
    {"min", MF_MIN, MSC_ANY, MND_SKIP, true},
    {"max", MF_MAX, MSC_ANY, MND_SKIP, true},
    {"mean", MF_MEAN, MSC_ANY, MND_SKIP, true},
    {"median", MF_MEDIAN, MSC_ANY, MND_SKIP, false},
    {"mode", MF_MODE, MSC_ANY, MND_SKIP, false},
    {"geometric_mean", MF_GEOMEAN, MSC_ANY, MND_SKIP, false},
    {"harmonic_mean", MF_HARMEAN, MSC_ANY, MND_SKIP, false},
    {"quantile", MF_QUANTILE, MSC_ANY, MND_SKIP, false},
    {"sqrt", MF_SQRT, MSC_ONE, MND_PROP, false},
    {"exp", MF_EXP, MSC_ONE, MND_PROP, false},
    {"log10", MF_LOG10, MSC_ONE, MND_PROP, false},
    {"round", MF_ROUND, MSC_ONE, MND_PROP, false},
    {"inv", MF_INV, MSC_ONE, MND_PROP, false},
    {"dB", MF_DB, MSC_ONE, MND_PROP, false},
    {"dB2amp", MF_DB2AMP, MSC_ONE, MND_PROP, false},
    {"dB2pow", MF_DB2POW, MSC_ONE, MND_PROP, false},
    {"pow", MF_POW, MSC_ONE, MND_PROP, false},
    {"interpolate_linear", MF_INTERP_LIN, MSC_INTERP, MND_PROP, false},
    {"interpolate_exp", MF_INTERP_EXP, MSC_INTERP, MND_PROP, false},
    {"scale", MF_SCALE, MSC_ONE, MND_PROP, false},
    {"norm_diff", MF_NORMDIFF, MSC_TWO, MND_PROP, false},
    {"diff", MF_DIFF, MSC_TWO, MND_PROP, false},
    {"div", MF_DIV, MSC_TWO, MND_PROP, false},
    {"cmul", MF_CMUL, MSC_TWO, MND_RAW, false},
    {"complex", MF_COMPLEX, MSC_TWO, MND_PROP, false},
    {"polar", MF_POLAR, MSC_TWO, MND_RAW, false},
    {"real", MF_REAL, MSC_ONE, MND_PROP, false},
    {"imag", MF_IMAG, MSC_ONE, MND_PROP, false},
    {"mod", MF_MOD, MSC_ONE, MND_PROP, false},
    {"phase", MF_PHASE, MSC_ONE, MND_PROP, false},
    {"conj", MF_CONJ, MSC_ONE, MND_PROP, false},
    {"intensity", MF_INTENSITY, MSC_ONE, MND_PROP, false},
    {"replace_nodata", MF_REPLACE_ND, MSC_ONE, MND_RAW, false},
    {"reclassify", MF_RECLASSIFY, MSC_ONE, MND_RAW, false},
    {"expression", MF_EXPRESSION, MSC_ANY, MND_RAW, false},
};

const MosFuncInfo *mosFuncLookup(const std::string &name)
{
    for (const MosFuncInfo &f : kMosFuncs)
        if (name == f.name)
            return &f;
    return nullptr;
}

struct MosFail
{
    bool silent = false;
    int errNum = CPLE_AppDefined;
    std::string msg;
    int failBand = 1;

    void set(int num, const std::string &m)
    {
        silent = false;
        errNum = num;
        msg = m;
    }
    void setSilent() { silent = true; }
};

void mosFailBar(bool quiet, int failBand, int totalBands,
                bool skipZero = false)
{
    if (quiet)
        return;
    int thisTick =
        (int)((double)failBand / (2.0 * (double)totalBands) * 40.0);
    if (thisTick > 40)
        thisTick = 40;
    for (int t = skipZero ? 1 : 0; t <= thisTick; ++t)
    {
        if (t % 4 == 0)
            printf("%d", t / 4 * 10);
        else
            printf(".");
    }
    fflush(stdout);
}

// ------------------------------------------------------------------
// dataset model
// ------------------------------------------------------------------

struct MosGeom
{
    bool valid = false;
    double sx = 0, sy = 0, sw = 0, sh = 0;
    double dx = 0, dy = 0, dw = 0, dh = 0;
    std::string xmlPath;  // SourceFilename payload
    int relative = 0;
};

struct MosBandSrc
{
    int srcIdx = 0;
    int srcBand = 1;
    bool ndSet = false;
    double nd = 0;
    bool alpha = false;
};

struct MosOutBand
{
    bool ndSet = false;  // band-level NoDataValue (background fill)
    double nd = 0;
    bool hide = false;
    std::string interp = "Undefined";
    bool derived = false;
    bool alpha = false;
    std::vector<MosBandSrc> srcs;
};

class MosaicDataset final : public RasterDatasetBase
{
  public:
    std::vector<std::unique_ptr<RasterDatasetBase>> srcs;
    std::vector<MosGeom> geoms;
    std::vector<MosOutBand> model;
    std::string pfName;
    std::vector<std::pair<std::string, std::string>> pfArgs;
    std::string vrtXml;

    std::vector<std::vector<double>> data;
    bool evaluated = false;
    // late-opened (wildcard) inputs printed a progress tick already;
    // the write bar continues without restarting at zero
    bool earlyTick = false;

    bool suppressWriteBar() const override { return earlyTick; }

    std::string customVrtXml(const std::string &,
                             const std::string &) override
    {
        return vrtXml;
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        if (!evaluated)
            return false;
        out = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        for (double &v : out)
            v = rasterFinishReal(v, t);
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        if (!evaluated)
            return false;
        const std::vector<double> &src = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        size_t sz = (size_t)dtypeSizeBytes(t);
        out.assign(src.size() * sz, 0);
        for (size_t i = 0; i < src.size(); ++i)
            rasterEncodeReal(t, out.data() + i * sz,
                             rasterFinishReal(src[i], t), 0);
        return true;
    }

    bool evalAll(MosFail &fail);

  private:
    std::map<std::pair<int, int>, std::vector<double>> srcCache;
    const std::vector<double> &srcPlane(int srcIdx, int band);
    void paintInto(std::vector<double> &out, const MosBandSrc &s);
    bool evalDerived(int outBand, MosFail &fail);
};

const std::vector<double> &MosaicDataset::srcPlane(int srcIdx, int band)
{
    auto key = std::make_pair(srcIdx, band);
    auto it = srcCache.find(key);
    if (it != srcCache.end())
        return it->second;
    std::vector<double> v;
    srcs[(size_t)srcIdx]->readBand(band, v);
    return srcCache.emplace(key, std::move(v)).first->second;
}

// VRTSimpleSource-style window paint (math mirrored from the VRT unit)
void MosaicDataset::paintInto(std::vector<double> &out, const MosBandSrc &s)
{
    const MosGeom &g = geoms[(size_t)s.srcIdx];
    if (!g.valid)
        return;
    RasterDatasetBase *src = srcs[(size_t)s.srcIdx].get();
    const std::vector<double> *sv = nullptr;
    if (!s.alpha)
        sv = &srcPlane(s.srcIdx, s.srcBand);
    int srcW = src->width, srcH = src->height;
    double sx = g.sx, sy = g.sy, sw = g.sw, sh = g.sh;
    double dx = g.dx, dy = g.dy, dw = g.dw, dh = g.dh;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    if (sx < 0)
    {
        dx += -sx * dw / sw;
        dw -= -sx * dw / sw;
        sw += sx;
        sx = 0;
    }
    if (sy < 0)
    {
        dy += -sy * dh / sh;
        dh -= -sy * dh / sh;
        sh += sy;
        sy = 0;
    }
    if (sx + sw > srcW)
    {
        double cut = sx + sw - srcW;
        dw -= cut * dw / sw;
        sw -= cut;
    }
    if (sy + sh > srcH)
    {
        double cut = sy + sh - srcH;
        dh -= cut * dh / sh;
        sh -= cut;
    }
    if (sw <= 0.5 || sh <= 0.5 || dw <= 0 || dh <= 0)
        return;
    // VRTSimpleSource dst-window rounding (+-0.001 fuzz, ceil trail)
    int x0 = (int)(dx + 0.001);
    int y0 = (int)(dy + 0.001);
    int x1 = (int)std::ceil(dx + dw - 0.001);
    int y1 = (int)std::ceil(dy + dh - 0.001);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > width)
        x1 = width;
    if (y1 > height)
        y1 = height;
    for (int py = y0; py < y1; py++)
    {
        double sYf = sy + (py + 0.5 - dy) * sh / dh;
        int sY = (int)std::floor(sYf);
        if (sY < 0)
            sY = 0;
        if (sY >= srcH)
            sY = srcH - 1;
        for (int px = x0; px < x1; px++)
        {
            if (s.alpha)
            {
                out[(size_t)py * width + px] = 255;
                continue;
            }
            double sXf = sx + (px + 0.5 - dx) * sw / dw;
            int sX = (int)std::floor(sXf);
            if (sX < 0)
                sX = 0;
            if (sX >= srcW)
                sX = srcW - 1;
            double v = (*sv)[(size_t)sY * srcW + sX];
            if (valueIsNodataMs(v, s.ndSet, s.nd))
                continue;
            out[(size_t)py * width + px] = v;
        }
    }
}

bool MosaicDataset::evalAll(MosFail &fail)
{
    data.assign(bands.size(), {});
    for (size_t b = 0; b < bands.size(); ++b)
    {
        MosOutBand &ob = model[b];
        if (!ob.derived)
        {
            std::vector<double> &out = data[b];
            out.assign((size_t)width * height,
                       ob.alpha ? 0.0 : (ob.ndSet ? ob.nd : 0.0));
            for (const MosBandSrc &s : ob.srcs)
                paintInto(out, s);
        }
        else if (!evalDerived((int)b + 1, fail))
        {
            fail.failBand = (int)b + 1;
            return false;
        }
    }
    evaluated = true;
    return true;
}

const std::string *mosFindArg(
    const std::vector<std::pair<std::string, std::string>> &args,
    const char *key)
{
    const std::string *hit = nullptr;
    for (const auto &kv : args)
        if (kv.first == key)
            hit = &kv.second;  // last one wins
    return hit;
}

bool MosaicDataset::evalDerived(int outBand, MosFail &fail)
{
    MosOutBand &ob = model[(size_t)outBand - 1];
    const MosFuncInfo *f = mosFuncLookup(pfName);
    size_t n = ob.srcs.size();
    if (!f)
    {
        // area/argmax/argmin are not implemented in this build
        fail.setSilent();
        return false;
    }

    double kVal = 0;
    bool kSet = false;
    int polarMode = 0;

    auto parseNamed = [&](const char *key, double &out2, bool &set,
                          bool required) -> bool
    {
        const std::string *a = mosFindArg(pfArgs, key);
        if (!a)
        {
            if (required)
            {
                fail.set(CPLE_AppDefined,
                         std::string("Missing pixel function argument: ") +
                             key);
                return false;
            }
            return true;
        }
        if (!parseFullDoubleMs(*a, out2))
        {
            fail.set(CPLE_AppDefined,
                     std::string("Failed to parse pixel function "
                                 "argument: ") +
                         key);
            return false;
        }
        set = true;
        return true;
    };

    switch (f->srcClass)
    {
        case MSC_ONE:
            if (n != 1)
            {
                fail.setSilent();
                return false;
            }
            break;
        case MSC_TWO:
            if (n != 2)
            {
                fail.setSilent();
                return false;
            }
            break;
        default:
            break;
    }

    bool propagate = false;
    if (const std::string *pa = mosFindArg(pfArgs, "propagateNoData"))
        propagate = !(strEqualNoCase(*pa, "no") ||
                      strEqualNoCase(*pa, "false") ||
                      strEqualNoCase(*pa, "off") || *pa == "0");

    double base = M_E, factExp = 1, digits = 0, invK = 1, dbFact = 20,
           powVal = 0, t0 = 0, dt = 0, tVal = 0, toVal = std::nan(""),
           quantQ = 0;
    bool powSet = false, t0Set = false, dtSet = false, tSet = false,
         toSet = false;

    switch (f->id)
    {
        case MF_SUM:
        case MF_MIN:
        case MF_MAX:
            if (!parseNamed("k", kVal, kSet, false))
                return false;
            break;
        case MF_MUL:
            if (!parseNamed("k", kVal, kSet, false))
                return false;
            if (n < 2 && !kSet)
            {
                fail.set(CPLE_AppDefined,
                         "mul requires at least two sources or a "
                         "specified constant k");
                return false;
            }
            break;
        case MF_QUANTILE:
        {
            const std::string *qa = mosFindArg(pfArgs, "q");
            if (!qa)
            {
                fail.set(CPLE_AppDefined, "quantile: q must be specified");
                return false;
            }
            if (!parseFullDoubleMs(*qa, quantQ) || quantQ < 0 ||
                quantQ > 1)
            {
                fail.set(CPLE_AppDefined,
                         "quantile: q must be between 0 and 1");
                return false;
            }
            break;
        }
        case MF_EXP:
        {
            bool baseSet = false, factSet = false;
            if (!parseNamed("base", base, baseSet, false))
                return false;
            if (!parseNamed("fact", factExp, factSet, false))
                return false;
            break;
        }
        case MF_ROUND:
        {
            bool ds = false;
            if (!parseNamed("digits", digits, ds, false))
                return false;
            break;
        }
        case MF_INV:
        {
            bool ks = false;
            if (!parseNamed("k", invK, ks, false))
                return false;
            break;
        }
        case MF_DB:
        {
            bool fs = false;
            if (!parseNamed("fact", dbFact, fs, false))
                return false;
            break;
        }
        case MF_POW:
            if (!parseNamed("power", powVal, powSet, true))
                return false;
            break;
        case MF_INTERP_LIN:
        case MF_INTERP_EXP:
        {
            if (!parseNamed("t0", t0, t0Set, true))
                return false;
            if (!parseNamed("t", tVal, tSet, true))
                return false;
            if (!parseNamed("dt", dt, dtSet, true))
                return false;
            // the reference validates t here but names dt in the message
            if (tVal == 0 || !std::isfinite(tVal))
            {
                fail.set(CPLE_AppDefined, "dt must be finite and non-zero");
                return false;
            }
            if (n < 2)
            {
                fail.set(CPLE_AppDefined,
                         "At least two sources required for "
                         "interpolation.");
                return false;
            }
            break;
        }
        case MF_POLAR:
        {
            const std::string *pa = mosFindArg(pfArgs, "amplitude_type");
            if (pa)
            {
                if (*pa == "AMPLITUDE")
                    polarMode = 0;
                else if (*pa == "INTENSITY")
                    polarMode = 1;
                else if (*pa == "dB")
                    polarMode = 2;
                else
                {
                    fail.set(CPLE_AppDefined,
                             "Invalid value for pixel function argument "
                             "'amplitude_type': " +
                                 *pa);
                    return false;
                }
            }
            break;
        }
        case MF_REPLACE_ND:
        {
            if (!ob.ndSet)
            {
                fail.set(CPLE_AppDefined, "Raster has no NoData");
                return false;
            }
            if (!parseNamed("to", toVal, toSet, false))
                return false;
            DType ot = bands[(size_t)outBand - 1].type;
            if (std::isnan(toVal) && dtypeIntegerMs(ot))
            {
                fail.set(CPLE_AppDefined,
                         "Using nan requires a floating point type "
                         "output buffer");
                return false;
            }
            break;
        }
        case MF_RECLASSIFY:
        {
            const std::string *ma = mosFindArg(pfArgs, "mapping");
            if (!ma)
            {
                fail.set(CPLE_AppDefined,
                         "reclassify must be called with 'mapping' "
                         "argument");
                return false;
            }
            // full mapping evaluation is not wired for mosaic
            fail.setSilent();
            return false;
        }
        case MF_EXPRESSION:
        {
            const std::string *xa = mosFindArg(pfArgs, "expression");
            if (!xa)
            {
                fail.set(CPLE_AppDefined,
                         "Missing 'expression' pixel function argument");
                return false;
            }
            const std::string *da = mosFindArg(pfArgs, "dialect");
            std::string dia = da ? *da : "muparser";
            if (dia == "muparser")
                fail.set(CPLE_IllegalArg,
                         "Dialect 'muparser' is not supported by this "
                         "GDAL build. A GDAL build with muparser is "
                         "needed.");
            else
                fail.set(CPLE_IllegalArg,
                         "Unknown expression dialect: " + dia);
            return false;
        }
        default:
            break;
    }

    // per-source buffers: background fill, then the painted window;
    // the derived function then treats the band nodata as each buffer's
    // nodata marker
    size_t pixels = (size_t)width * (size_t)height;
    double outNd = ob.ndSet ? ob.nd : 0.0;
    std::vector<std::vector<double>> bufs(n);
    for (size_t i = 0; i < n; ++i)
    {
        bufs[i].assign(pixels, outNd);
        paintInto(bufs[i], ob.srcs[i]);
    }
    bool bandNd = ob.ndSet;
    double bandNdVal = ob.nd;

    std::vector<double> &out = data[(size_t)outBand - 1];
    out.resize(pixels);

    double scScale = 1, scOffset = 0;
    if (f->id == MF_SCALE && n >= 1)
    {
        const Band &sb =
            srcs[(size_t)ob.srcs[0].srcIdx]
                ->bands[(size_t)ob.srcs[0].srcBand - 1];
        if (sb.hasScale)
            scScale = sb.scale;
        if (sb.hasOffset)
            scOffset = sb.offset;
    }

    std::vector<double> vals;
    vals.reserve(n);
    for (size_t px = 0; px < pixels; ++px)
    {
        bool anyNd = false;
        for (size_t i = 0; i < n && !anyNd; ++i)
            if (valueIsNodataMs(bufs[i][px], bandNd, bandNdVal))
                anyNd = true;
        if (anyNd && (propagate || f->ndMode == MND_PROP))
        {
            out[px] = outNd;
            continue;
        }

        double r = 0;
        if (f->ndMode == MND_SKIP)
        {
            vals.clear();
            for (size_t i = 0; i < n; ++i)
            {
                double v = bufs[i][px];
                if (!valueIsNodataMs(v, bandNd, bandNdVal))
                    vals.push_back(v);
            }
            size_t m = vals.size();
            switch (f->id)
            {
                case MF_SUM:
                {
                    double acc = kSet ? kVal : 0;
                    for (double v : vals)
                        acc += v;
                    r = acc;
                    break;
                }
                case MF_MUL:
                {
                    double acc = kSet ? kVal : 1;
                    for (double v : vals)
                        acc *= v;
                    r = acc;
                    break;
                }
                case MF_MIN:
                case MF_MAX:
                {
                    bool isMin = f->id == MF_MIN;
                    bool have = false;
                    double m0 = 0;
                    for (double v : vals)
                    {
                        if (!have || (isMin ? v < m0 : v > m0))
                            m0 = v;
                        have = true;
                    }
                    if (kSet && (!have || (isMin ? kVal < m0 : kVal > m0)))
                    {
                        m0 = kVal;
                        have = true;
                    }
                    r = have ? m0 : outNd;
                    break;
                }
                case MF_MEAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    double s = 0;
                    for (double v : vals)
                        s += v;
                    r = s * (1.0 / (double)m);
                    break;
                }
                case MF_MEDIAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    std::sort(vals.begin(), vals.end());
                    if (m % 2)
                        r = vals[m / 2];
                    else
                        r = 0.5 * (vals[m / 2 - 1] + vals[m / 2]);
                    break;
                }
                case MF_MODE:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    size_t bestCount = 0;
                    double best = 0;
                    for (size_t i = 0; i < m; ++i)
                    {
                        size_t c = 0;
                        for (size_t j = 0; j < m; ++j)
                            if (vals[j] == vals[i] ||
                                (std::isnan(vals[j]) &&
                                 std::isnan(vals[i])))
                                c++;
                        if (c > bestCount)
                        {
                            bestCount = c;
                            best = vals[i];
                        }
                    }
                    r = best;
                    break;
                }
                case MF_GEOMEAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    double prod = 1;
                    for (double v : vals)
                        prod *= v;
                    r = std::pow(prod, 1.0 / (double)m);
                    break;
                }
                case MF_HARMEAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    double s = 0;
                    for (double v : vals)
                        s += 1.0 / v;
                    r = s > 0 ? (double)m / s : 0.0;
                    break;
                }
                case MF_QUANTILE:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    std::sort(vals.begin(), vals.end());
                    double pos = quantQ * (double)(m - 1);
                    size_t lo = (size_t)std::floor(pos);
                    if (lo >= m - 1)
                        r = vals[m - 1];
                    else
                    {
                        double frac = pos - (double)lo;
                        r = vals[lo] + (vals[lo + 1] - vals[lo]) * frac;
                    }
                    break;
                }
                default:
                    r = 0;
                    break;
            }
        }
        else
        {
            double a = bufs[0][px];
            double b = n > 1 ? bufs[1][px] : 0;
            switch (f->id)
            {
                case MF_SQRT:
                    r = std::sqrt(a);
                    break;
                case MF_EXP:
                    r = std::pow(base, factExp * a);
                    break;
                case MF_LOG10:
                    r = std::log10(std::fabs(a));
                    break;
                case MF_ROUND:
                    r = std::round(a * std::pow(10.0, digits)) *
                        std::pow(10.0, -digits);
                    break;
                case MF_INV:
                    r = invK / a;
                    break;
                case MF_DB:
                    r = dbFact * std::log10(std::fabs(a));
                    break;
                case MF_DB2AMP:
                    r = std::pow(10.0, a * 0.05);
                    break;
                case MF_DB2POW:
                    r = std::pow(10.0, a * 0.1);
                    break;
                case MF_POW:
                    r = std::pow(a, powVal);
                    break;
                case MF_SCALE:
                    r = a * scScale + scOffset;
                    break;
                case MF_NORMDIFF:
                    r = (a - b) / (a + b);
                    break;
                case MF_DIFF:
                    r = a - b;
                    break;
                case MF_DIV:
                    r = a / b;
                    break;
                case MF_CMUL:
                    r = a * b;
                    break;
                case MF_COMPLEX:
                    r = a;
                    break;
                case MF_POLAR:
                {
                    double amp = a;
                    if (polarMode == 1)
                        amp = std::sqrt(a);
                    else if (polarMode == 2)
                        amp = std::pow(10.0, a / 20.0);
                    r = amp * std::cos(b);
                    break;
                }
                case MF_REAL:
                case MF_CONJ:
                    r = a;
                    break;
                case MF_IMAG:
                    r = 0;
                    break;
                case MF_MOD:
                    r = std::fabs(a);
                    break;
                case MF_PHASE:
                    r = a < 0 ? M_PI : 0;
                    break;
                case MF_INTENSITY:
                    r = a * a;
                    break;
                case MF_REPLACE_ND:
                    r = valueIsNodataMs(a, bandNd, bandNdVal) ? toVal : a;
                    break;
                case MF_INTERP_LIN:
                case MF_INTERP_EXP:
                {
                    double q = (tVal - t0) / dt;
                    long i = 0;
                    if (std::isnan(q) || q < 0)
                        i = 0;
                    else if (q >= (double)(n - 1))
                        i = (long)n - 2;
                    else
                        i = (long)std::floor(q);
                    double xi = t0 + (double)i * dt;
                    double va = bufs[(size_t)i][px];
                    double vb = bufs[(size_t)i + 1][px];
                    if (tVal == xi)
                        r = va;
                    else if (f->id == MF_INTERP_LIN)
                        r = va + (tVal - xi) * ((vb - va) / dt);
                    else
                        r = va * std::exp(std::log(vb / va) / dt *
                                          (tVal - xi));
                    break;
                }
                default:
                    r = 0;
                    break;
            }
        }
        out[px] = r;
    }
    (void)toSet;
    return true;
}

// ------------------------------------------------------------------
// stashed typed inputs (kept away from the generic validation opens)
// ------------------------------------------------------------------

std::vector<std::string> g_msInputsTyped;
bool g_msStashed = false;

// relativeToVRT resolution: as-typed path relative to the VRT directory
std::string relToOutputMs(const std::string &input,
                          const std::string &output, int &relative)
{
    std::string outDir = dirNameMs(output);
    if (outDir.empty())
    {
        relative = !input.empty() && input[0] != '/' &&
                           input.find(':') == std::string::npos
                       ? 1
                       : 0;
        return input;
    }
    if (input.size() > outDir.size() + 1 &&
        input.compare(0, outDir.size(), outDir) == 0 &&
        input[outDir.size()] == '/')
    {
        relative = 1;
        return input.substr(outDir.size() + 1);
    }
    relative = 0;
    return input;
}

std::string vrtXmlBuild(const MosaicDataset &ds)
{
    std::string x = strPrintf(
        "<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\">\n", ds.width,
        ds.height);
    if (ds.hasSrs && ds.srs.valid())
    {
        std::vector<int> mapv = ds.srs.dataAxisToSRSAxisMapping();
        std::string mapping;
        for (int m : mapv)
        {
            if (!mapping.empty())
                mapping += ",";
            mapping += strPrintf("%d", m);
        }
        std::string wkt = ds.srs.wkt1Gdal();
        if (wkt.empty())
            wkt = ds.srs.wkt2SingleLine();
        x += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             xmlEscTextMs(wkt) + "</SRS>\n";
    }
    if (ds.hasGT)
    {
        x += "  <GeoTransform>";
        for (int i = 0; i < 6; i++)
        {
            if (i)
                x += ",";
            x += strPrintf("%24.16e", ds.gt[i]);
        }
        x += "</GeoTransform>\n";
    }
    for (size_t b = 0; b < ds.bands.size(); ++b)
    {
        const MosOutBand &ob = ds.model[b];
        const Band &bb = ds.bands[b];
        x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\"%s>\n",
                       dtypeName(bb.type), (int)b + 1,
                       ob.derived ? " subClass=\"VRTDerivedRasterBand\""
                                  : "");
        if (ob.ndSet)
            x += "    <NoDataValue>" + fmt18Ms(ob.nd) + "</NoDataValue>\n";
        if (ob.hide)
            x += "    <HideNoDataValue>1</HideNoDataValue>\n";
        if (ob.interp != "Undefined")
            x += "    <ColorInterp>" + ob.interp + "</ColorInterp>\n";
        for (const MosBandSrc &s : ob.srcs)
        {
            const MosGeom &g = ds.geoms[(size_t)s.srcIdx];
            RasterDatasetBase *src = ds.srcs[(size_t)s.srcIdx].get();
            const Band &sb = src->bands[(size_t)s.srcBand - 1];
            bool complexTag = s.alpha || s.ndSet;
            const char *tag = complexTag ? "ComplexSource" : "SimpleSource";
            x += strPrintf("    <%s>\n", tag);
            x += strPrintf("      <SourceFilename relativeToVRT=\"%d\">%s"
                           "</SourceFilename>\n",
                           g.relative, xmlEscTextMs(g.xmlPath).c_str());
            x += strPrintf("      <SourceBand>%d</SourceBand>\n",
                           s.srcBand);
            int bw = 0, bh = 0;
            src->realBlockDims(bw, bh);
            x += strPrintf("      <SourceProperties RasterXSize=\"%d\" "
                           "RasterYSize=\"%d\" DataType=\"%s\" "
                           "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                           src->width, src->height, dtypeName(sb.type), bw,
                           bh);
            x += "      <SrcRect xOff=\"" + fmtRectMs(g.sx) + "\" yOff=\"" +
                 fmtRectMs(g.sy) + "\" xSize=\"" + fmtRectMs(g.sw) +
                 "\" ySize=\"" + fmtRectMs(g.sh) + "\" />\n";
            x += "      <DstRect xOff=\"" + fmtRectMs(g.dx) + "\" yOff=\"" +
                 fmtRectMs(g.dy) + "\" xSize=\"" + fmtRectMs(g.dw) +
                 "\" ySize=\"" + fmtRectMs(g.dh) + "\" />\n";
            if (s.alpha)
                x += "      <ScaleOffset>255</ScaleOffset>\n"
                     "      <ScaleRatio>0</ScaleRatio>\n";
            else if (s.ndSet)
                x += "      <NODATA>" + fmt18Ms(s.nd) + "</NODATA>\n";
            x += strPrintf("    </%s>\n", tag);
        }
        if (ob.derived)
        {
            x += "    <PixelFunctionType>" + xmlEscTextMs(ds.pfName) +
                 "</PixelFunctionType>\n";
            if (!ds.pfArgs.empty())
            {
                x += "    <PixelFunctionArguments";
                for (const auto &kv : ds.pfArgs)
                    x += " " + kv.first + "=\"" + xmlEscAttrMs(kv.second) +
                         "\"";
                x += " />\n";
            }
            const MosFuncInfo *f = mosFuncLookup(ds.pfName);
            DType stt = f && f->preserveType ? bb.type : DType::Float64;
            x += strPrintf("    <SourceTransferType>%s"
                           "</SourceTransferType>\n",
                           dtypeName(stt));
            x += "    <SkipNonContributingSources>true"
                 "</SkipNonContributingSources>\n";
        }
        x += "  </VRTRasterBand>\n";
    }
    x += "</VRTDataset>\n";
    return x;
}

}  // namespace

// ------------------------------------------------------------------
// entry points shared by raster mosaic and raster stack
// ------------------------------------------------------------------

std::string rasterMosaicStackArgValueCheckEntry(const std::string &name,
                                                const std::string &value)
{
    if (name == "output-format")
    {
        std::string canon;
        return rasterOutFormatIssue(value, canon);
    }
    if (name == "input-format")
        return inputFormatCapError(false, value);
    if (name == "resolution")
    {
        if (value == "same" || value == "average" || value == "common" ||
            value == "highest" || value == "lowest")
            return "";
        double x = 0, y = 0;
        if (resolutionNumericMs(value, x, y))
            return "";
        return "resolution: two comma separated positive values should be "
               "provided, or 'same', 'average', 'common', 'highest' or "
               "'lowest'";
    }
    return "";
}

int rasterMosaicStackArgCheckEntry(const std::string &name, ParseResult &r)
{
    if (name == "band")
    {
        for (const auto &sv : r.list("band"))
            if (atoi(sv.c_str()) < 1)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Value of 'band' should greater or equal "
                            "to 1.");
                handlerPrintUsage();
                return 1;
            }
    }
    else if (name == "bbox")
    {
        auto v = r.list("bbox");
        if (v.size() == 4)
        {
            double x0 = strtod(v[0].c_str(), nullptr);
            double y0 = strtod(v[1].c_str(), nullptr);
            double x1 = strtod(v[2].c_str(), nullptr);
            double y1 = strtod(v[3].c_str(), nullptr);
            if (!(x0 <= x1 && y0 <= y1))
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

int rasterMosaicStackPreValidatorEntry(const CmdSpec &, ParseResult &r)
{
    // the handler expands globs/@files itself; hide the raw values from
    // the generic dataset-open validation pass
    auto it = r.byName.find("input");
    if (it != r.byName.end() && it->second.set)
    {
        g_msInputsTyped = it->second.values;
        g_msStashed = true;
        it->second.values.clear();
    }
    return 0;
}

bool rasterMosaicStackPostValidatorEntry(const CmdSpec &spec,
                                         ParseResult &r, bool)
{
    bool isStack = spec.id == "raster_stack";
    const char *pfx = isStack ? "stack" : "mosaic";
    bool ow = r.flag("overwrite");
    bool ap = r.flag("append");
    if (ow && ap)
        return false;  // the mutex report suppresses output processing
    bool failed = false;
    std::string output = r.str("output");
    std::string of = r.str("output-format");
    if (r.get("output") && !output.empty() &&
        !strEqualNoCase(of, "MEM") && !strEqualNoCase(of, "Memory") &&
        !strEqualNoCase(of, "stream") && fileExistsMs2(output))
    {
        std::string kind = outputExistsKind(output);
        if (!ow && !ap)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("%s: %s '%s' already exists. You may "
                                  "specify the --overwrite/--append "
                                  "option.",
                                  pfx, kind.c_str(), output.c_str()));
            failed = true;
        }
        else if (ow)
        {
            if (kind == "Directory")
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("%s: Directory '%s' already exists, "
                                      "but is not recognized as a valid "
                                      "GDAL dataset. Please manually "
                                      "delete it before retrying",
                                      pfx, output.c_str()));
                failed = true;
            }
            else if (kind == "Dataset")
                overwriteDeleteFileset(output);
        }
    }
    if (r.flag("target-aligned-pixels") && !r.get("resolution"))
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    strPrintf("%s: Argument 'target-aligned-pixels' can "
                              "only be specified if argument 'resolution' "
                              "is also specified.",
                              pfx));
        failed = true;
    }
    return failed;
}

int rasterMosaicStackHandlerEntry(const CmdSpec &spec, ParseResult &r)
{
    bool isStack = spec.id == "raster_stack";
    const char *pfx = isStack ? "stack" : "mosaic";
    PrefixScopeMs prefix(pfx);
    std::string algName =
        isStack ? "gdal raster stack" : "gdal raster mosaic";

    std::string output = r.str("output");
    std::string of = r.str("output-format");
    std::string drv;
    rasterOutFormatIssue(of, drv);
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");

    std::string dg = drv;
    if (dg.empty())
    {
        std::string base = baseNameOfMs(output);
        std::string lbase = strToLower(base);
        size_t dot = base.find_last_of('.');
        std::string ext =
            dot == std::string::npos ? ""
                                     : strToLower(base.substr(dot + 1));
        if (lbase.size() > 11 &&
            lbase.compare(lbase.size() - 11, 11, ".gdalg.json") == 0)
            dg = "GDALG";
        else if (ext.empty() || ext == "tif" || ext == "tiff")
            dg = "GTiff";
        else if (ext == "vrt")
            dg = "VRT";
    }
    bool vrtOut = dg == "VRT";
    bool gdalgOut = dg == "GDALG";
    bool streamOut = drv == "stream";

    std::vector<std::string> inputsTyped =
        g_msStashed ? g_msInputsTyped : r.list("input");

    // ---- canonical argument echo for GDALG serialization ----
    std::string gdalgExtra;
    {
        auto bandVals = r.list("band");
        if (!bandVals.empty())
        {
            gdalgExtra += " --band ";
            for (size_t i = 0; i < bandVals.size(); ++i)
            {
                if (i)
                    gdalgExtra += ",";
                gdalgExtra += strPrintf("%d", atoi(bandVals[i].c_str()));
            }
        }
        if (r.flag("absolute-path"))
            gdalgExtra += " --absolute-path";
        if (r.get("resolution"))
            gdalgExtra +=
                " --resolution " + gdalgQuoteMs(r.str("resolution"));
        auto bboxVals = r.list("bbox");
        if (bboxVals.size() == 4)
        {
            gdalgExtra += " --bbox ";
            for (size_t i = 0; i < 4; ++i)
            {
                if (i)
                    gdalgExtra += ",";
                gdalgExtra +=
                    fmt17Ms(strtod(bboxVals[i].c_str(), nullptr));
            }
        }
        if (r.flag("target-aligned-pixels"))
            gdalgExtra += " --target-aligned-pixels";
        auto ndEcho = [&](const char *arg, const char *flagName)
        {
            auto vals = r.list(arg);
            if (vals.empty())
                return;
            gdalgExtra += std::string(" ") + flagName + " ";
            for (size_t i = 0; i < vals.size(); ++i)
            {
                if (i)
                    gdalgExtra += ",";
                gdalgExtra += fmt17Ms(strtod(vals[i].c_str(), nullptr));
            }
        };
        ndEcho("src-nodata", "--src-nodata");
        ndEcho("dst-nodata", "--dst-nodata");
        if (r.flag("hide-nodata"))
            gdalgExtra += " --hide-nodata";
        if (!isStack)
        {
            if (r.flag("add-alpha"))
                gdalgExtra += " --add-alpha";
            if (r.get("pixel-function"))
                gdalgExtra +=
                    " --pixel-function " + r.str("pixel-function");
            auto pfa = r.list("pixel-function-arg");
            if (!pfa.empty())
                gdalgExtra +=
                    " --pixel-function-arg " + strJoin(pfa, ",");
        }
    }
    std::string inputEcho = strJoin(inputsTyped, " --input ");

    // ---- GDALG serialization: no input expansion or validation ----
    if (gdalgOut)
    {
        if (!fileExistsMs2(output) && output.compare(0, 4, "/vsi") != 0)
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
            std::make_unique<MosaicDataset>();
        auto noop = [](std::unique_ptr<RasterDatasetBase> &) -> int
        { return 0; };
        return rasterConvertWriteOutput(base, r, inputEcho, output, true,
                                        overwrite, append, drv, gdalgExtra,
                                        noop, nullptr);
    }

    // ---- input expansion (globs, @filelist) ----
    // wildcard inputs are opened late, inside the run: a progress tick
    // lands on stdout before any open/validation output, but only when
    // the pattern's directory scan visits at least one entry
    bool lateOpen = false;
    for (const std::string &tok : inputsTyped)
    {
        if (tok.empty() || tok[0] == '@' || !hasWildMs(tok))
            continue;
        size_t slash = tok.find_last_of('/');
        std::string dir =
            slash == std::string::npos ? "." : tok.substr(0, slash);
        if (DIR *d = opendir(dir.empty() ? "/" : dir.c_str()))
        {
            while (struct dirent *de = readdir(d))
            {
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                {
                    lateOpen = true;
                    break;
                }
            }
            closedir(d);
        }
        if (lateOpen)
            break;
    }
    if (lateOpen && !quiet)
    {
        printf("0");
        fflush(stdout);
    }
    std::vector<std::string> expanded;
    for (const std::string &tok : inputsTyped)
    {
        if (!tok.empty() && tok[0] == '@')
        {
            std::string content;
            if (!readFileToString(tok.substr(1), content))
            {
                cplErrorStr(CE_Failure, CPLE_FileIO,
                            std::string(pfx) + ": Cannot open " +
                                tok.substr(1));
                return 1;
            }
            std::vector<std::string> lines = strSplit(content, '\n');
            if (!lines.empty() && lines.back().empty())
                lines.pop_back();
            for (std::string &ln : lines)
            {
                while (!ln.empty() && ln.back() == '\r')
                    ln.pop_back();
                expanded.push_back(ln);
            }
        }
        else if (hasWildMs(tok))
        {
            for (const std::string &m : globExpandMs(tok))
                expanded.push_back(m);
        }
        else
            expanded.push_back(tok);
    }
    if (expanded.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "No input dataset specified.");
        return 1;
    }

    // ---- resolution strategy ----
    int resMode = RES_SAME;
    double explXres = 0, explYres = 0;
    if (r.get("resolution"))
    {
        std::string rs = r.str("resolution");
        if (rs == "average")
            resMode = RES_AVERAGE;
        else if (rs == "common")
            resMode = RES_COMMON;
        else if (rs == "highest")
            resMode = RES_HIGHEST;
        else if (rs == "lowest")
            resMode = RES_LOWEST;
        else if (rs == "same")
            resMode = RES_SAME;
        else if (resolutionNumericMs(rs, explXres, explYres))
            resMode = RES_EXPLICIT;
    }
    if (r.flag("target-aligned-pixels") && resMode != RES_EXPLICIT)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "-tap option cannot be used without using -tr");
        return 1;
    }

    // ---- open the sources with homogeneity checks ----
    OpenOptions oo;
    oo.allowedDrivers = r.list("input-format");
    for (const auto &kv : r.list("open-option"))
    {
        size_t eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        std::string val =
            eq == std::string::npos ? "" : kv.substr(eq + 1);
        oo.raw.emplace_back(key, val);
        if (strEqualNoCase(key, "COLOR_TABLE_MULTIPLIER"))
            oo.ctMult = atol(val.c_str());
        else if (strEqualNoCase(key, "GEOREF_SOURCES"))
        {
            oo.georefSet = true;
            oo.georefSources = strSplit(val, ',');
        }
    }

    auto md = std::make_unique<MosaicDataset>();
    bool firstGeo = false;
    for (size_t i = 0; i < expanded.size(); ++i)
    {
        const std::string &name = expanded[i];
        cplPushQuietHandler();
        std::string err;
        auto ds = openRaster(name, err, oo);
        cplPopHandler();
        if (!ds)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Can't open " + name + ".");
            return 1;
        }
        bool geo = ds->hasGT;
        if (!isStack && !geo)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        algName +
                            " does not support ungeoreferenced image.");
            return 1;
        }
        if (i == 0)
            firstGeo = geo;
        else
        {
            RasterDatasetBase *f0 = md->srcs[0].get();
            if (isStack && geo != firstGeo)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            algName + " cannot stack ungeoreferenced and "
                                      "georeferenced images.");
                return 1;
            }
            if (firstGeo)
            {
                std::string w0 = f0->hasSrs && f0->srs.valid()
                                     ? f0->srs.wkt1Gdal()
                                     : "";
                std::string wi = ds->hasSrs && ds->srs.valid()
                                     ? ds->srs.wkt1Gdal()
                                     : "";
                if (w0 != wi)
                {
                    std::string n0 = f0->hasSrs && f0->srs.valid()
                                         ? f0->srs.name()
                                         : "(null)";
                    std::string ni = ds->hasSrs && ds->srs.valid()
                                         ? ds->srs.name()
                                         : "(null)";
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                algName +
                                    " does not support heterogeneous "
                                    "projection: expected " +
                                    n0 + ", got " + ni + ".");
                    return 1;
                }
            }
            if (!isStack)
            {
                if (ds->bands.size() != f0->bands.size())
                {
                    cplErrorStr(
                        CE_Failure, CPLE_AppDefined,
                        algName +
                            strPrintf(" does not support heterogeneous "
                                      "band numbers: expected %d, "
                                      "got %d.",
                                      (int)f0->bands.size(),
                                      (int)ds->bands.size()));
                    return 1;
                }
                for (size_t j = 0; j < ds->bands.size(); ++j)
                    if (ds->bands[j].type != f0->bands[j].type)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            algName +
                                strPrintf(" does not support "
                                          "heterogeneous band data type: "
                                          "expected %s, got %s.",
                                          dtypeName(f0->bands[j].type),
                                          dtypeName(ds->bands[j].type)));
                        return 1;
                    }
            }
            if (firstGeo && resMode == RES_SAME &&
                (ds->gt[1] != f0->gt[1] || ds->gt[5] != f0->gt[5]))
            {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("Dataset %s has resolution %.17g x %.17g, "
                              "whereas previous sources have resolution "
                              "%.17g x %.17g. To mosaic these data "
                              "sources, a different resolution strategy "
                              "should be specified.",
                              name.c_str(), ds->gt[1], ds->gt[5],
                              f0->gt[1], f0->gt[5]));
                return 1;
            }
            if (isStack && !firstGeo &&
                (ds->width != f0->width || ds->height != f0->height))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            algName +
                                " cannot stack ungeoreferenced images "
                                "that have not the same dimensions.");
                return 1;
            }
        }
        md->srcs.push_back(std::move(ds));
    }

    // ---- band selection ----
    std::vector<int> bandSel;
    for (const auto &sv : r.list("band"))
        bandSel.push_back(atoi(sv.c_str()));
    if (!isStack)
    {
        for (int b : bandSel)
            if (b > (int)md->srcs[0]->bands.size())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("Invalid band number: %d", b));
                return 1;
            }
    }
    else
    {
        for (size_t i = 0; i < md->srcs.size(); ++i)
            for (int b : bandSel)
                if (b > (int)md->srcs[i]->bands.size())
                {
                    cplErrorStr(
                        CE_Failure, CPLE_AppDefined,
                        strPrintf("%s has %d bands, but %d is requested",
                                  expanded[i].c_str(),
                                  (int)md->srcs[i]->bands.size(), b));
                    return 1;
                }
    }

    // ---- nodata lists ----
    std::vector<double> snList, dnList;
    for (const auto &sv : r.list("src-nodata"))
        snList.push_back(strtod(sv.c_str(), nullptr));
    for (const auto &sv : r.list("dst-nodata"))
        dnList.push_back(strtod(sv.c_str(), nullptr));
    bool hide = r.flag("hide-nodata");
    auto listVal = [](const std::vector<double> &l, size_t idx) -> double
    { return l[idx < l.size() ? idx : l.size() - 1]; };

    std::string pfName = r.str("pixel-function");
    bool pfSet = !isStack && r.get("pixel-function") != nullptr;
    bool addAlpha = !isStack && r.flag("add-alpha");

    // ---- output band model ----
    for (size_t bi = 0;; ++bi)
    {
        if (!isStack)
        {
            size_t nSel = bandSel.empty() ? md->srcs[0]->bands.size()
                                          : bandSel.size();
            if (bi >= nSel)
                break;
            int srcBand = bandSel.empty() ? (int)bi + 1 : bandSel[bi];
            const Band &fb = md->srcs[0]->bands[(size_t)srcBand - 1];
            MosOutBand ob;
            ob.derived = pfSet;
            ob.interp = fb.colorInterp;
            if (!dnList.empty())
            {
                ob.ndSet = true;
                ob.nd = listVal(dnList, bi);
            }
            else if (!snList.empty())
            {
                ob.ndSet = true;
                ob.nd = listVal(snList, bi);
            }
            else if (fb.hasNodata)
            {
                ob.ndSet = true;
                ob.nd = fb.nodata;
            }
            ob.hide = hide;
            for (size_t si = 0; si < md->srcs.size(); ++si)
            {
                MosBandSrc s;
                s.srcIdx = (int)si;
                s.srcBand = srcBand;
                const Band &sb =
                    md->srcs[si]->bands[(size_t)srcBand - 1];
                if (!snList.empty())
                {
                    s.ndSet = true;
                    s.nd = listVal(snList, bi);
                }
                else if (sb.hasNodata)
                {
                    s.ndSet = true;
                    s.nd = sb.nodata;
                }
                ob.srcs.push_back(s);
            }
            Band b;
            b.index = (int)bi + 1;
            b.type = fb.type;
            b.colorInterp = ob.interp;
            b.hasNodata = ob.ndSet && !ob.hide;
            b.nodata = ob.nd;
            md->model.push_back(std::move(ob));
            md->bands.push_back(std::move(b));
        }
        else
            break;
    }
    if (isStack)
    {
        size_t outIdx = 0;
        for (size_t si = 0; si < md->srcs.size(); ++si)
        {
            size_t nSel = bandSel.empty() ? md->srcs[si]->bands.size()
                                          : bandSel.size();
            for (size_t k = 0; k < nSel; ++k)
            {
                int srcBand = bandSel.empty() ? (int)k + 1 : bandSel[k];
                const Band &sb =
                    md->srcs[si]->bands[(size_t)srcBand - 1];
                MosOutBand ob;
                if (!dnList.empty())
                {
                    ob.ndSet = true;
                    ob.nd = listVal(dnList, outIdx);
                }
                else if (!snList.empty())
                {
                    ob.ndSet = true;
                    ob.nd = listVal(snList, outIdx);
                }
                else if (sb.hasNodata)
                {
                    ob.ndSet = true;
                    ob.nd = sb.nodata;
                }
                ob.hide = hide;
                MosBandSrc s;
                s.srcIdx = (int)si;
                s.srcBand = srcBand;
                if (!snList.empty())
                {
                    s.ndSet = true;
                    s.nd = listVal(snList, outIdx);
                }
                else if (sb.hasNodata)
                {
                    s.ndSet = true;
                    s.nd = sb.nodata;
                }
                ob.srcs.push_back(s);
                Band b;
                b.index = (int)outIdx + 1;
                b.type = sb.type;
                b.hasNodata = ob.ndSet && !ob.hide;
                b.nodata = ob.nd;
                md->model.push_back(std::move(ob));
                md->bands.push_back(std::move(b));
                outIdx++;
            }
        }
    }
    if (addAlpha)
    {
        MosOutBand ob;
        ob.alpha = true;
        ob.interp = "Alpha";
        for (size_t si = 0; si < md->srcs.size(); ++si)
        {
            MosBandSrc s;
            s.srcIdx = (int)si;
            s.srcBand = 1;
            s.alpha = true;
            ob.srcs.push_back(s);
        }
        Band b;
        b.index = (int)md->bands.size() + 1;
        b.type = DType::Byte;
        b.colorInterp = "Alpha";
        md->model.push_back(std::move(ob));
        md->bands.push_back(std::move(b));
    }
    if (pfSet)
    {
        md->pfName = pfName;
        for (const auto &kv : r.list("pixel-function-arg"))
        {
            size_t eq = kv.find('=');
            if (eq == std::string::npos)
                md->pfArgs.emplace_back(kv, "");
            else
                md->pfArgs.emplace_back(kv.substr(0, eq),
                                        kv.substr(eq + 1));
        }
    }

    // ---- nodata representability warnings (not muted by --quiet) ----
    // mosaic-only: the effective band value is set once per source, so
    // the warning repeats per source; stack never warns
    if (!isStack)
        for (size_t b = 0; b < md->bands.size(); ++b)
        {
            const MosOutBand &ob = md->model[b];
            if (ob.alpha || !ob.ndSet)
                continue;
            DType t = md->bands[b].type;
            if (!nodataRepresentableMs(ob.nd, t))
                for (size_t si = 0; si < ob.srcs.size(); ++si)
                    cplErrorStr(
                        CE_Warning, CPLE_NotSupported,
                        strPrintf("Band data type of %s cannot "
                                  "represent the specified NoData "
                                  "value of %g",
                                  dtypeName(t), ob.nd));
        }

    // ---- grid geometry ----
    RasterDatasetBase *f0 = md->srcs[0].get();
    if (firstGeo)
    {
        double xres = 0, yres = 0;
        switch (resMode)
        {
            case RES_SAME:
                xres = f0->gt[1];
                yres = std::fabs(f0->gt[5]);
                break;
            case RES_AVERAGE:
            {
                double sx = 0, sy = 0;
                for (const auto &s : md->srcs)
                {
                    sx += s->gt[1];
                    sy += std::fabs(s->gt[5]);
                }
                xres = sx / (double)md->srcs.size();
                yres = sy / (double)md->srcs.size();
                break;
            }
            case RES_HIGHEST:
            case RES_LOWEST:
            {
                bool hi = resMode == RES_HIGHEST;
                xres = f0->gt[1];
                yres = std::fabs(f0->gt[5]);
                for (const auto &s : md->srcs)
                {
                    double xr = s->gt[1], yr = std::fabs(s->gt[5]);
                    if (hi ? xr < xres : xr > xres)
                        xres = xr;
                    if (hi ? yr < yres : yr > yres)
                        yres = yr;
                }
                break;
            }
            case RES_COMMON:
            {
                xres = f0->gt[1];
                yres = std::fabs(f0->gt[5]);
                for (size_t i = 1; i < md->srcs.size(); ++i)
                {
                    ratGcdMs(xres, md->srcs[i]->gt[1], xres);
                    ratGcdMs(yres, std::fabs(md->srcs[i]->gt[5]), yres);
                }
                break;
            }
            case RES_EXPLICIT:
                xres = explXres;
                yres = explYres;
                break;
        }

        double minX = 0, maxX = 0, minY = 0, maxY = 0;
        for (size_t i = 0; i < md->srcs.size(); ++i)
        {
            RasterDatasetBase *s = md->srcs[i].get();
            double x0 = s->gt[0];
            double x1 = s->gt[0] + s->gt[1] * s->width;
            double y1 = s->gt[3];
            double y0 = s->gt[3] + s->gt[5] * s->height;
            if (i == 0)
            {
                minX = x0;
                maxX = x1;
                minY = y0;
                maxY = y1;
            }
            else
            {
                minX = std::min(minX, x0);
                maxX = std::max(maxX, x1);
                minY = std::min(minY, y0);
                maxY = std::max(maxY, y1);
            }
        }
        auto bboxVals = r.list("bbox");
        if (bboxVals.size() == 4)
        {
            minX = strtod(bboxVals[0].c_str(), nullptr);
            minY = strtod(bboxVals[1].c_str(), nullptr);
            maxX = strtod(bboxVals[2].c_str(), nullptr);
            maxY = strtod(bboxVals[3].c_str(), nullptr);
        }
        if (r.flag("target-aligned-pixels"))
        {
            minX = std::floor(minX / xres) * xres;
            maxX = std::ceil(maxX / xres) * xres;
            minY = std::floor(minY / yres) * yres;
            maxY = std::ceil(maxY / yres) * yres;
        }
        int W = (int)(0.5 + (maxX - minX) / xres);
        int H = (int)(0.5 + (maxY - minY) / yres);
        if (W <= 0 || H <= 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Computed VRT dimension is invalid. You've "
                        "probably specified inappropriate resolution.");
            return 1;
        }
        md->width = W;
        md->height = H;
        md->hasGT = true;
        md->gt[0] = minX;
        md->gt[1] = xres;
        md->gt[2] = 0;
        md->gt[3] = maxY;
        md->gt[4] = 0;
        md->gt[5] = -yres;
        if (f0->hasSrs && f0->srs.valid())
        {
            md->hasSrs = true;
            md->srs = f0->srs;
        }

        // per-source windows, GetSrcDstWin-style: leading edges clip in
        // georeferenced space, trailing edges as a dst-pixel cut, then
        // AddSimpleSource's near-integer snapping (1e-3)
        auto snapInt = [](double v)
        {
            double c = std::floor(v + 0.5);
            return std::fabs(v - c) < 1e-3 ? c : v;
        };
        for (size_t i = 0; i < md->srcs.size(); ++i)
        {
            RasterDatasetBase *s = md->srcs[i].get();
            double sXres = s->gt[1];
            double sYres = std::fabs(s->gt[5]);
            double x0 = s->gt[0];
            double y0top = s->gt[3];
            MosGeom g;
            // strict intersection pre-test against the raw extent:
            // touching edges are dropped
            if (x0 + s->width * sXres <= minX || x0 >= maxX ||
                y0top - s->height * sYres >= maxY || y0top <= minY)
            {
                md->geoms.push_back(g);
                continue;
            }
            if (x0 < minX)
            {
                g.sx = (minX - x0) / sXres;
                g.dx = 0.0;
            }
            else
            {
                g.sx = 0.0;
                g.dx = (x0 - minX) / xres;
            }
            if (maxY < y0top)
            {
                g.sy = (y0top - maxY) / sYres;
                g.dy = 0.0;
            }
            else
            {
                g.sy = 0.0;
                g.dy = (maxY - y0top) / yres;
            }
            g.sw = s->width;
            g.sh = s->height;
            if (g.sx > 0)
                g.sw -= g.sx;
            if (g.sy > 0)
                g.sh -= g.sy;
            double rx = sXres / xres;
            double ry = sYres / yres;
            g.dw = g.sw * rx;
            g.dh = g.sh * ry;
            if (g.dx + g.dw > (double)W)
            {
                double cut = g.dx + g.dw - (double)W;
                g.dw -= cut;
                g.sw -= cut / rx;
            }
            if (g.dy + g.dh > (double)H)
            {
                double cut = g.dy + g.dh - (double)H;
                g.dh -= cut;
                g.sh -= cut / ry;
            }
            g.sx = snapInt(g.sx);
            g.sy = snapInt(g.sy);
            g.sw = snapInt(g.sw);
            g.sh = snapInt(g.sh);
            g.dx = snapInt(g.dx);
            g.dy = snapInt(g.dy);
            g.dw = snapInt(g.dw);
            g.dh = snapInt(g.dh);
            g.valid = g.sw > 0 && g.sh > 0 && g.dw > 0 && g.dh > 0;
            md->geoms.push_back(g);
        }
    }
    else
    {
        md->width = f0->width;
        md->height = f0->height;
        for (size_t i = 0; i < md->srcs.size(); ++i)
        {
            RasterDatasetBase *s = md->srcs[i].get();
            MosGeom g;
            g.valid = true;
            g.sx = 0;
            g.sy = 0;
            g.sw = s->width;
            g.sh = s->height;
            g.dx = 0;
            g.dy = 0;
            g.dw = s->width;
            g.dh = s->height;
            md->geoms.push_back(g);
        }
    }

    // drop sources that do not intersect the grid
    for (auto &ob : md->model)
    {
        std::vector<MosBandSrc> kept;
        for (const MosBandSrc &s : ob.srcs)
            if (md->geoms[(size_t)s.srcIdx].valid)
                kept.push_back(s);
        ob.srcs = std::move(kept);
    }
    if (isStack)
    {
        // stacked bands of non-intersecting sources disappear entirely
        std::vector<MosOutBand> keptM;
        std::vector<Band> keptB;
        for (size_t b = 0; b < md->model.size(); ++b)
        {
            if (md->model[b].srcs.empty())
                continue;
            keptM.push_back(std::move(md->model[b]));
            Band bb = md->bands[b];
            bb.index = (int)keptB.size() + 1;
            keptB.push_back(std::move(bb));
        }
        md->model = std::move(keptM);
        md->bands = std::move(keptB);
    }

    // ---- source paths in the serialized VRT ----
    bool absPath = r.flag("absolute-path");
    std::string cwd;
    {
        char buf[4096];
        if (getcwd(buf, sizeof(buf)))
            cwd = buf;
    }
    for (size_t i = 0; i < md->srcs.size(); ++i)
    {
        MosGeom &g = md->geoms[i];
        const std::string &name = expanded[i];
        if (absPath)
        {
            g.relative = 0;
            g.xmlPath = (!name.empty() && name[0] == '/')
                            ? name
                            : joinPathMs(cwd, name);
        }
        else
            g.xmlPath = relToOutputMs(name, output, g.relative);
    }

    md->driverShort = "VRT";
    md->driverLong = "Virtual Raster";
    md->path = output;
    md->vrtXml = vrtXmlBuild(*md);

    // VRT output: the reference opens the target for writing before
    // serializing; failures report "Cannot create" without progress
    if (vrtOut && !fileExistsMs2(output) && output.compare(0, 4, "/vsi") != 0)
    {
        FILE *f = fopen(output.c_str(), "wb");
        if (!f)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot create " + output);
            return 0;
        }
        fclose(f);
        remove(output.c_str());
    }

    // a stack whose bands were all dropped (bbox outside every source)
    // still serializes as VRT (with a reopen error) but refuses other
    // writers before any pixel work
    if (md->bands.empty() && !vrtOut)
    {
        if (streamOut)
            return 0;
        if (dg == "GTiff")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        baseNameOfMs(output) +
                            ": Unable to export GeoTIFF files with "
                            "zero bands.");
            return 0;
        }
        if (!quiet && !lateOpen)
        {
            printf("0");
            fflush(stdout);
        }
        return 0;
    }

    // the GDAL_NODATA tag carries band 1 only; other bands land in a
    // PAM sidecar (no per-band rewrite warnings), so hide them from the
    // GTiff writer and serialize the sidecar ourselves
    struct PamNd
    {
        int band;
        double v;
    };
    std::vector<PamNd> pamNds;
    if (dg == "GTiff")
    {
        bool firstHas =
            !md->bands.empty() && md->bands[0].hasNodata;
        double firstNd = firstHas ? md->bands[0].nodata : 0;
        for (size_t b = 1; b < md->bands.size(); ++b)
        {
            Band &bb = md->bands[b];
            if (!bb.hasNodata)
                continue;
            if (!firstHas || bb.nodata != firstNd)
                pamNds.push_back({(int)b + 1, bb.nodata});
            bb.hasNodata = false;
        }
    }

    md->earlyTick = lateOpen && !quiet;
    MosaicDataset *mp = md.get();
    auto mat = [mp, quiet, vrtOut,
                streamOut](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        if (vrtOut)
            return 0;
        MosFail fail;
        if (!mp->evalAll(fail))
        {
            if (!streamOut)
                mosFailBar(quiet, fail.failBand, (int)mp->bands.size(),
                           mp->earlyTick);
            if (!fail.silent)
                cplErrorStr(CE_Failure, fail.errNum, fail.msg);
            return 1;
        }
        return 0;
    };

    bool remBar = lateOpen && !quiet && !vrtOut && !streamOut;
    std::unique_ptr<RasterDatasetBase> base = std::move(md);
    int wrc = rasterConvertWriteOutput(base, r, inputEcho, output,
                                       quiet || vrtOut || streamOut,
                                       overwrite, append, drv, gdalgExtra,
                                       mat, nullptr);
    if (wrc == 0 && remBar)
    {
        // continuation of the tick printed before the late input open
        printf("...10...20...30...40...50...60...70...80...90...100 "
               "- done.\n");
        fflush(stdout);
    }
    if (wrc == 0 && vrtOut && mp->bands.empty())
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Missing one of rasterXSize, rasterYSize or bands "
                    "on VRTDataset.");
    if (wrc == 0 && !pamNds.empty() && gdalPamEnabled())
    {
        std::string pam = "<PAMDataset>\n";
        for (const PamNd &pn : pamNds)
        {
            pam += strPrintf("  <PAMRasterBand band=\"%d\">\n", pn.band);
            std::string txt;
            std::string attr;
            if (std::isnan(pn.v))
                txt = "nan";
            else
                txt = strPrintf("%.14E", pn.v);
            if (pn.v != std::floor(pn.v) ||
                strtod(txt.c_str(), nullptr) != pn.v)
            {
                double le = pn.v;
                unsigned char bytes[8];
                memcpy(bytes, &le, 8);
                std::string hex;
                for (int i = 0; i < 8; ++i)
                    hex += strPrintf("%02X", bytes[i]);
                attr = " le_hex_equiv=\"" + hex + "\"";
            }
            pam += "    <NoDataValue" + attr + ">" + txt +
                   "</NoDataValue>\n";
            pam += "  </PAMRasterBand>\n";
        }
        pam += "</PAMDataset>\n";
        writeStringToFile(output + ".aux.xml", pam);
    }
    return 0;
}

void registerRasterMosaicHandler()
{
    registerHandler("raster_mosaic", rasterMosaicStackHandlerEntry);
    registerArgValueCheck("raster_mosaic",
                          rasterMosaicStackArgValueCheckEntry);
    registerArgCheck("raster_mosaic", rasterMosaicStackArgCheckEntry);
    registerPreValidator("raster_mosaic",
                         rasterMosaicStackPreValidatorEntry);
    registerPostValidator("raster_mosaic",
                          rasterMosaicStackPostValidatorEntry);
}
