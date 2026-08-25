#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "progress.h"
#include "proj_min.h"
#include "srs.h"
#include "tiff.h"
#include "vrt.h"
#include "vsi.h"
#include <dirent.h>
#include <sys/stat.h>
#include "util.h"

#include <cmath>
#include <cstdio>
#include <cstring>

std::string g_infoFilesHide;
bool g_infoFilesHideDerived = false;
bool g_infoSrsOverrideSet = false;
Srs g_infoSrsOverride;

namespace
{

std::string fmtG16(double d)
{
    // GDAL json output: integral doubles as "<n>.0"; otherwise %.16f with
    // one truncation pass that removes a trailing run (>= 6) of '0'/'9'
    // digits, allowing a single rounding-artifact digit after the run.
    if (std::isfinite(d) && std::fabs(d) < 9.2e18 &&
        d == (double)(long long)d)
        return strPrintf("%lld.0", (long long)d);
    char buf[512];
    snprintf(buf, sizeof(buf), "%.16f", d);
    char *dot = strchr(buf, '.');
    if (dot)
    {
        char *dec = dot + 1;
        int n = (int)strlen(dec);
        int runEnd = -1;
        for (int cand = n; cand >= n - 1 && cand > 0; --cand)
        {
            char ch = dec[cand - 1];
            if (ch == '0' || ch == '9')
            {
                runEnd = cand;
                break;
            }
        }
        if (runEnd > 0)
        {
            char ch = dec[runEnd - 1];
            int start = runEnd;
            while (start > 0 && dec[start - 1] == ch)
                --start;
            if (runEnd - start >= 6)
                snprintf(buf, sizeof(buf), "%.*f", start, d);
        }
    }
    if (!strchr(buf, '.') && !strchr(buf, 'n') && !strchr(buf, 'i'))
        strcat(buf, ".0");
    return buf;
}

JVal jraw(const std::string &lit)
{
    JVal v;
    v.type = JVal::DOUBLE;
    v.s = lit;
    return v;
}

JVal jint(long long i)
{
    JVal v;
    v.type = JVal::INT;
    v.i = i;
    return v;
}

JVal jstr(const std::string &s)
{
    JVal v;
    v.type = JVal::STRING;
    v.s = s;
    return v;
}

JVal jobj()
{
    JVal v;
    v.type = JVal::OBJECT;
    return v;
}

JVal jarr()
{
    JVal v;
    v.type = JVal::ARRAY;
    return v;
}

std::string decToDms(double angle, bool isLong, int precision)
{
    const double epsilon = (0.5 / 3600.0) * pow(0.1, precision);
    const double absAngle = std::fabs(angle) + epsilon;
    if (absAngle > 361.0)
        return "Invalid angle";
    const int degrees = (int)absAngle;
    const int minutes = (int)((absAngle - degrees) * 60);
    double seconds = absAngle * 3600 - degrees * 3600 - minutes * 60;
    if (seconds > epsilon * 3600.0)
        seconds -= epsilon * 3600.0;
    const char *hemisphere;
    if (isLong)
        hemisphere = angle < 0.0 ? "W" : "E";
    else
        hemisphere = angle < 0.0 ? "S" : "N";
    return strPrintf("%3dd%2d'%*.*f\"%s", degrees, minutes, precision + 3,
                     precision, seconds, hemisphere);
}

// area-of-use display name: trailing '.' stripped; names longer than 40
// bytes get clipped at the first " - " (or, failing that, the first ", ")
std::string areaDisplayName(const std::string &raw)
{
    std::string s = raw;
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    if (s.size() > 40)
    {
        size_t cut = s.find(" - ");
        if (cut == std::string::npos)
            cut = s.find(", ");
        if (cut == std::string::npos)
            cut = s.find(' ');
        if (cut != std::string::npos)
            s = s.substr(0, cut) + "...";
    }
    return s;
}

struct InfoOpts
{
    bool mm = false, stats = false, approxStats = false, hist = false;
    bool checksum = false, noMd = false, noCt = false, noFl = false;
    bool noGcp = false, noNodata = false, noMask = false, listMdd = false;
    bool mddSet = false;
    std::string mdd;
    std::string crsFormat = "AUTO";
};

// json-c double serializer with significant figures: %.{sig}g, and when
// the decimals carry a 999999/000000 rounding artifact retry with up to
// three fewer digits; ".0" appended to bare integers
std::string fmtJsonSig(double d, int sig, bool zeroSuffix)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "%.*g", sig, d);
    const char *dot = strchr(buf, '.');
    if (dot && (strstr(dot, "999999") || strstr(dot, "000000")))
    {
        char alt[80];
        for (int i = 1; i <= 3; i++)
        {
            snprintf(alt, sizeof(alt), "%.*g", sig - i, d);
            const char *d2 = strchr(alt, '.');
            if (!d2 || (!strstr(d2, "999999") && !strstr(d2, "000000")))
            {
                memcpy(buf, alt, sizeof(alt));
                break;
            }
        }
    }
    if (zeroSuffix && !strchr(buf, '.') && !strchr(buf, 'e') &&
        !strchr(buf, 'E') && !strchr(buf, 'n') && !strchr(buf, 'i'))
        strcat(buf, ".0");
    return buf;
}

// minimal %g representation that round-trips (GDAL nodata display)
std::string fmtLadderG(double d, bool asFloat)
{
    char buf[64];
    for (int p = 1; p <= 17; ++p)
    {
        snprintf(buf, sizeof(buf), "%.*g", p, d);
        if (asFloat ? (strtof(buf, nullptr) == (float)d)
                    : (strtod(buf, nullptr) == d))
            return buf;
    }
    return buf;
}

std::string fmtNodataText(const Band &b)
{
    if (b.nodataIsI64)
        return strPrintf("%lld", b.nodataI64);
    if (b.nodataIsU64)
        return strPrintf("%llu", b.nodataU64);
    return fmtLadderG(b.nodata, b.type == DType::Float32);
}

std::string fmtNodataJson(const Band &b)
{
    if (b.nodataIsI64)
        return strPrintf("%lld", b.nodataI64);
    if (b.nodataIsU64)
    {
        // beyond int64 the JSON emitter falls back to a quoted string
        if (b.nodataU64 > 9223372036854775807ULL)
            return "\"" + strPrintf("%llu", b.nodataU64) + "\"";
        return strPrintf("%llu", b.nodataU64);
    }
    if (b.type == DType::Float32 || b.type == DType::Float64)
    {
        std::string s = fmtLadderG(b.nodata, b.type == DType::Float32);
        if (s.find_first_not_of("-0123456789") == std::string::npos)
            s += ".0";
        return s;
    }
    bool floating = b.type == DType::Float16 ||
                    b.type == DType::CFloat32 || b.type == DType::CFloat64;
    if (!floating && b.nodata == floor(b.nodata) &&
        fabs(b.nodata) < 9.2e18)
        return strPrintf("%lld", (long long)b.nodata);
    return fmtJsonSig(b.nodata, 17, true);
}

// default GDALRasterBand::GetMaskBand(): own nodata wins (suppressed in
// display); otherwise a trailing alpha band of a 2- or 4-band dataset
// gives the other bands a PER_DATASET|ALPHA mask
bool bandHasAlphaMask(const RasterDatasetBase &ds, const Band &b,
                      const InfoOpts &o)
{
    if (o.noMask || b.hasNodata)
        return false;
    size_t n = ds.bands.size();
    if (n != 2 && n != 4)
        return false;
    return ds.bands.back().colorInterp == "Alpha" && b.index != (int)n;
}

bool crsProjectionInfo(const Srs &srs, std::string &convName,
                       std::string &methodName)
{
    PJ *crs = srs.pj();
    PJ *sub = nullptr;
    if (crs && proj_get_type(crs) == PJ_TYPE_COMPOUND_CRS)
    {
        sub = proj_crs_get_sub_crs(projCtx(), crs, 0);
        if (sub)
            crs = sub;
    }
    PJ *op = proj_crs_get_coordoperation(projCtx(), crs);
    if (sub)
        proj_destroy(sub);
    if (!op)
        return false;
    const char *cn = proj_get_name(op);
    convName = cn ? cn : "";
    const char *mn = nullptr;
    proj_coordoperation_get_method_info(projCtx(), op, &mn, nullptr, nullptr);
    methodName = mn ? mn : "";
    proj_destroy(op);
    return true;
}

std::string crsUnitsName(const Srs &srs)
{
    PJ *crs = srs.pj();
    PJ *sub = nullptr;
    if (crs && proj_get_type(crs) == PJ_TYPE_COMPOUND_CRS)
    {
        sub = proj_crs_get_sub_crs(projCtx(), crs, 0);
        if (sub)
            crs = sub;
    }
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), crs);
    if (sub)
        proj_destroy(sub);
    if (!cs)
        return "";
    const char *unitName = nullptr;
    proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr, nullptr,
                          nullptr, &unitName, nullptr, nullptr);
    std::string r = unitName ? unitName : "";
    proj_destroy(cs);
    return r;
}

void corners(const RasterDatasetBase &ds,
             double out[5][2])
{
    const double pts[5][2] = {
        {0, 0},
        {0, (double)ds.height},
        {(double)ds.width, 0},
        {(double)ds.width, (double)ds.height},
        {ds.width / 2.0, ds.height / 2.0},
    };
    for (int i = 0; i < 5; ++i)
    {
        double x = pts[i][0], y = pts[i][1];
        out[i][0] = ds.gt[0] + ds.gt[1] * x + ds.gt[2] * y;
        out[i][1] = ds.gt[3] + ds.gt[4] * x + ds.gt[5] * y;
    }
}

std::string trim7(double v)
{
    std::string s = strPrintf("%.7f", v);
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s += '0';
    return s;
}

std::string trim3(double v)
{
    std::string s = strPrintf("%.3f", v);
    while (!s.empty() && s.back() == '0')
        s.pop_back();
    if (!s.empty() && s.back() == '.')
        s += '0';
    return s;
}

std::string fmt17(double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
        !strchr(buf, 'n') && !strchr(buf, 'i'))
        strcat(buf, ".0");
    return buf;
}

bool computeMinMax(RasterDatasetBase &ds, const Band &b, double &mn,
                   double &mx);

struct BandInfoCtx
{
    bool gotMin = false, gotMax = false;
    double mdMin = 0, mdMax = 0;
    bool computedOk = false, computedShowJson = false;
    double cmpMin = 0, cmpMax = 0;
    bool haveStats = false;
    double sMin = 0, sMax = 0, sMean = 0, sStd = 0;
    bool haveHist = false;
    HistItem hist;
    bool histFresh = false;
    bool histBrokenZero = false;
};

void pamSetBandItem(RasterDatasetBase &ds, int band, const std::string &k,
                    const std::string &v)
{
    PamBandState &pb = ds.pamBands[band];
    auto &mdi = pb.mdi;
    for (auto &kv : mdi)
    {
        if (kv.first == k)
        {
            kv.second = v;
            return;
        }
    }
    if (pb.mdiSorted)
    {
        auto it = mdi.begin();
        while (it != mdi.end() && it->first < k)
            ++it;
        mdi.insert(it, {k, v});
    }
    else
        mdi.emplace_back(k, v);
}

void pamRemoveBandItem(RasterDatasetBase &ds, int band, const std::string &k)
{
    auto &mdi = ds.pamBands[band].mdi;
    for (auto it = mdi.begin(); it != mdi.end(); ++it)
    {
        if (it->first == k)
        {
            mdi.erase(it);
            return;
        }
    }
}

bool statsFromCache(const Band &b, bool approxOK, BandInfoCtx &c)
{
    if (!approxOK && b.getMd("", "STATISTICS_APPROXIMATE"))
        return false;
    auto g = [&](const char *k, double &v) {
        const std::string *s = b.getMd("", k);
        if (!s)
            return false;
        v = strtod(s->c_str(), nullptr);
        return true;
    };
    return g("STATISTICS_MINIMUM", c.sMin) &&
           g("STATISTICS_MAXIMUM", c.sMax) && g("STATISTICS_MEAN", c.sMean) &&
           g("STATISTICS_STDDEV", c.sStd);
}

void storeStats(RasterDatasetBase &ds, Band &b, const StatsResult &r)
{
    auto set = [&](const char *k, const std::string &v) {
        if (ds.driverShort == "GTiff")
            cplDebug("GTIFF", "GTiffRasterBand::SetMetadataItem() goes to "
                              "PAM instead of TIFF tags");
        b.setMd("", k, v);
        if (!ds.pamSuppressItems)
            pamSetBandItem(ds, b.index, k, v);
    };
    if (r.ok)
    {
        if (r.subsampled)
            set("STATISTICS_APPROXIMATE", "YES");
        else
        {
            b.removeMd("", "STATISTICS_APPROXIMATE");
            if (!ds.pamSuppressItems)
                pamRemoveBandItem(ds, b.index, "STATISTICS_APPROXIMATE");
        }
        set("STATISTICS_MINIMUM", strPrintf("%.14g", r.mn));
        set("STATISTICS_MAXIMUM", strPrintf("%.14g", r.mx));
        set("STATISTICS_MEAN", strPrintf("%.14g", r.mean));
        set("STATISTICS_STDDEV", strPrintf("%.14g", r.stddev));
    }
    set("STATISTICS_VALID_PERCENT", strPrintf("%.4g", r.validPct));
    if (!ds.pamSuppressItems)
        ds.pamDirty = true;
}

bool vrtMosaicForcedStats(RasterDatasetBase &ds, Band &b, bool approx,
                          StatsResult &out)
{
    auto *vd = dynamic_cast<VrtDataset *>(&ds);
    if (!vd)
        return false;
    std::vector<VrtSource *> parts;
    if (!vd->mosaicStatsParts(b.index, parts))
        return false;
    long long n = 0;
    double mean = 0, m2 = 0, mn = 0, mx = 0, areas = 0;
    bool sub = false;
    for (VrtSource *s : parts)
    {
        RasterDatasetBase *sd = s->ds.get();
        Band &sb = sd->bands[(size_t)s->sourceBand - 1];
        StatsResult raw = computeBandStats(*sd, sb, approx);
        storeStats(*sd, sb, raw);
        StatsResult aware = raw;
        if (b.hasNodata)
        {
            // the merge honours the VRT band's nodata, which the source
            // does not carry; run the source scan with it applied
            bool oh = sb.hasNodata;
            double ov = sb.nodata;
            sb.hasNodata = true;
            sb.nodata = b.nodata;
            aware = computeBandStats(*sd, sb, approx);
            sb.hasNodata = oh;
            sb.nodata = ov;
        }
        areas += (double)sd->width * sd->height;
        if (aware.subsampled)
            sub = true;
        if (!aware.ok || aware.count <= 0)
            continue;
        // pairwise Welford merge of per-source aggregates
        long long nb = aware.count;
        double mb = aware.mean;
        double m2b = aware.stddev * aware.stddev * (double)nb;
        if (n == 0)
        {
            mn = aware.mn;
            mx = aware.mx;
            n = nb;
            mean = mb;
            m2 = m2b;
        }
        else
        {
            mn = mn < aware.mn ? mn : aware.mn;
            mx = mx > aware.mx ? mx : aware.mx;
            long long nt = n + nb;
            double delta = mb - mean;
            mean += delta * (double)nb / (double)nt;
            m2 += m2b + delta * delta * ((double)n * (double)nb / (double)nt);
            n = nt;
        }
    }
    out = StatsResult();
    out.subsampled = sub;
    if (n <= 0)
        return true;
    double denom = b.hasNodata ? (double)ds.width * ds.height : areas;
    out.ok = true;
    out.count = n;
    out.mn = mn;
    out.mx = mx;
    out.mean = mean;
    double var = m2 / (double)n;
    out.stddev = sqrt(var > 0 ? var : 0);
    out.validPct = denom > 0 ? 100.0 * (double)n / denom : 0;
    return true;
}

void statsFailureError(const RasterDatasetBase &ds, const Band &b)
{
    size_t slash = ds.path.find_last_of('/');
    std::string base =
        slash == std::string::npos ? ds.path : ds.path.substr(slash + 1);
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                strPrintf("%s, band %d: Failed to compute statistics, no "
                          "valid pixels found in sampling.",
                          base.c_str(), b.index));
}

void vrtBrokenReadError(const RasterDatasetBase &ds, const Band &b,
                        const std::string &suffix)
{
    size_t slash = ds.path.find_last_of('/');
    std::string base =
        slash == std::string::npos ? ds.path : ds.path.substr(slash + 1);
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                strPrintf("%s, band %d: IReadBlock failed at X offset 0, "
                          "Y offset 0%s",
                          base.c_str(), b.index, suffix.c_str()));
}

// runs GetStatistics/GetDefaultHistogram semantics, updating metadata + PAM
BandInfoCtx prepareBandInfo(RasterDatasetBase &ds, Band &b,
                            const InfoOpts &o)
{
    BandInfoCtx c;
    const std::string *s;
    if ((s = b.getMd("", "STATISTICS_MINIMUM")))
    {
        c.gotMin = true;
        c.mdMin = strtod(s->c_str(), nullptr);
    }
    if ((s = b.getMd("", "STATISTICS_MAXIMUM")))
    {
        c.gotMax = true;
        c.mdMax = strtod(s->c_str(), nullptr);
    }
    if (!c.gotMin && !c.gotMax)
    {
        double hmn = 0, hmx = 0;
        if (ds.bandMinMaxHint(b.index, hmn, hmx))
        {
            c.gotMin = c.gotMax = true;
            c.mdMin = hmn;
            c.mdMax = hmx;
        }
    }
    if (o.mm)
    {
        long seq = cplErrorSeq();
        c.computedOk = computeMinMax(ds, b, c.cmpMin, c.cmpMax);
        // gdalinfo json omits these if the compute raised an error/warning
        c.computedShowJson = c.computedOk && cplErrorSeq() == seq;
    }
    VrtDataset *vdBroken = dynamic_cast<VrtDataset *>(&ds);
    VrtSource *badSrc =
        vdBroken ? vdBroken->firstFailing(b.index) : nullptr;
    bool statsForce = o.stats || o.approxStats;
    bool approxOK = o.approxStats;
    if (statsFromCache(b, approxOK, c))
        c.haveStats = true;
    else if (statsForce)
    {
        if (badSrc)
        {
            // broken sources: read fails without storing anything
            auto &srcs = vdBroken->bandSources[(size_t)b.index - 1];
            bool single = srcs.size() == 1;
            bool badFirst = badSrc == &srcs[0];
            int pre = single ? (approxOK ? 5 : 1) : (badFirst ? 2 : 1);
            for (int i = 0; i < pre; i++)
                vdBroken->sourceAttempt(*badSrc);
            vrtBrokenReadError(ds, b, "");
            if (single)
            {
                vdBroken->sourceAttempt(*badSrc);
                vdBroken->sourceAttempt(*badSrc);
            }
        }
        else
        {
            StatsResult r = vrtAwareForcedStats(ds, b, approxOK);
            if (r.ok)
            {
                c.haveStats = true;
                c.sMin = r.mn;
                c.sMax = r.mx;
                c.sMean = r.mean;
                c.sStd = r.stddev;
            }
            else
                statsFailureError(ds, b);
        }
    }
    if (o.hist && badSrc)
    {
        // empirically pinned attempt/suffix table for failed reads
        auto &srcs = vdBroken->bandSources[(size_t)b.index - 1];
        bool single = srcs.size() == 1;
        bool badFirst = badSrc == &srcs[0];
        int pre;
        bool suffixed;
        if (single)
        {
            pre = statsForce ? 0 : 1;
            suffixed = false;
        }
        else if (badFirst)
        {
            pre = 1;
            suffixed = statsForce;
        }
        else
        {
            pre = statsForce ? 0 : 1;
            suffixed = !statsForce;
        }
        for (int i = 0; i < pre; i++)
            vdBroken->sourceAttempt(*badSrc);
        vrtBrokenReadError(ds, b,
                           suffixed ? ": " + badSrc->failMsg : "");
        if (single)
        {
            vdBroken->sourceAttempt(*badSrc);
            vdBroken->sourceAttempt(*badSrc);
        }
        c.histBrokenZero = true;
    }
    else if (o.hist)
        c.haveHist = vrtAwareHistogram(ds, b, c.hist, c.histFresh);
    return c;
}

bool computeMinMax(RasterDatasetBase &ds, const Band &b, double &mn,
                   double &mx);

void printMetaItems(std::string &out, const MetaDomain &items,
                    const char *itemIndent)
{
    for (auto &kv : items)
        out += strPrintf("%s%s=%s\n", itemIndent, kv.first.c_str(),
                         kv.second.c_str());
}

std::vector<std::string> buildDomainList(
    const std::map<std::string, MetaDomain> &md,
    const std::map<std::string, std::string> &xmlD,
    const std::vector<std::string> &order, bool dsLevel)
{
    std::vector<std::string> out;
    for (const auto &d : order)
    {
        if (dsLevel && d == "SUBDATASETS")
            continue;
        auto it = md.find(d);
        bool nonEmpty =
            (it != md.end() && !it->second.empty()) || xmlD.count(d) > 0;
        if (nonEmpty)
            out.push_back(d);
    }
    if (dsLevel)
    {
        out.push_back("DERIVED_SUBDATASETS");
        auto sd = md.find("SUBDATASETS");
        if (sd != md.end() && !sd->second.empty())
            out.push_back("SUBDATASETS");
    }
    return out;
}

std::vector<std::string> expandExtraDomains(
    const InfoOpts &o, const std::vector<std::string> &list)
{
    static const char *skipList[] = {"",
                                     "IMAGE_STRUCTURE",
                                     "TILING_SCHEME",
                                     "SUBDATASETS",
                                     "GEOLOCATION",
                                     "RPC",
                                     "DERIVED_SUBDATASETS"};
    std::vector<std::string> out;
    if (!o.mddSet)
        return out;
    if (strEqualNoCase(o.mdd, "all"))
    {
        for (const auto &d : list)
        {
            bool skip = false;
            for (const char *s : skipList)
                if (strEqualNoCase(d, s))
                    skip = true;
            if (!skip)
                out.push_back(d);
        }
    }
    else
        out.push_back(o.mdd);
    return out;
}

bool resolveDomain(const std::vector<std::string> &order,
                   const std::string &req, std::string &actual)
{
    for (const auto &d : order)
    {
        if (strEqualNoCase(d, req))
        {
            actual = d;
            return true;
        }
    }
    return false;
}

void renderExtraDomainsText(std::string &out, const InfoOpts &o,
                            const std::map<std::string, MetaDomain> &md,
                            const std::map<std::string, std::string> &xmlD,
                            const std::vector<std::string> &order,
                            bool dsLevel, const char *indent)
{
    for (const auto &req :
         expandExtraDomains(o, buildDomainList(md, xmlD, order, dsLevel)))
    {
        std::string actual;
        if (!resolveDomain(order, req, actual))
            continue;
        auto xd = xmlD.find(actual);
        if (xd != xmlD.end())
        {
            out += strPrintf("%sMetadata (%s):\n", indent, req.c_str());
            out += indent + xd->second + "\n";
            continue;
        }
        auto it = md.find(actual);
        if (it == md.end() || it->second.empty())
            continue;
        out += strPrintf("%sMetadata (%s):\n", indent, req.c_str());
        printMetaItems(out, it->second,
                       (std::string(indent) + "  ").c_str());
    }
}

bool infoFilesHidden(const RasterDatasetBase &ds)
{
    if (g_infoFilesHide.empty())
        return false;
    if (ds.path == g_infoFilesHide)
        return true;
    // a lone warp wrapper around the live temp keeps it in the file
    // list; the reference shows none there too
    return g_infoFilesHideDerived && !ds.files.empty() &&
           ds.files[0] == g_infoFilesHide;
}

// the reference's rasterize transition hands the live output dataset to
// the info terminal: no on-disk implicit metadata, and the SRS is the
// one the verb set rather than the geokey round-trip
void applyLiveInfoOverrides(RasterDatasetBase &ds)
{
    if (g_infoFilesHide.empty())
        return;
    bool direct = ds.path == g_infoFilesHide;
    bool derived = !direct && !ds.files.empty() &&
                   ds.files[0] == g_infoFilesHide;
    if (!direct && !derived)
        return;
    auto stripKey = [](MetaDomain &d, const char *k) {
        for (auto it = d.begin(); it != d.end(); ++it)
            if (it->first == k)
            {
                d.erase(it);
                break;
            }
    };
    auto dit = ds.metadata.find("");
    if (dit != ds.metadata.end())
        stripKey(dit->second, "AREA_OR_POINT");
    dit = ds.metadata.find("IMAGE_STRUCTURE");
    if (dit != ds.metadata.end())
        stripKey(dit->second, "LAYOUT");
    if (g_infoSrsOverrideSet)
    {
        ds.srs = g_infoSrsOverride;
        ds.hasSrs = true;
    }
}

std::string renderText(RasterDatasetBase &ds, const InfoOpts &o)
{
    std::string out;
    // computed pipeline datasets (dem steps) have no driver at all
    if (!ds.driverShort.empty() || !ds.driverLong.empty())
        out += strPrintf("Driver: %s/%s\n", ds.driverShort.c_str(),
                         ds.driverLong.c_str());
    if (!o.noFl)
    {
        if (ds.files.empty() || infoFilesHidden(ds))
            out += "Files: none associated\n";
        else
        {
            out += strPrintf("Files: %s\n", ds.files[0].c_str());
            for (size_t i = 1; i < ds.files.size(); ++i)
                out += strPrintf("       %s\n", ds.files[i].c_str());
        }
    }
    out += strPrintf("Size is %d, %d\n", ds.width, ds.height);

    if (ds.hasSrs)
    {
        std::string auth = ds.srs.authName(), code = ds.srs.code();
        bool asProjjson = strEqualNoCase(o.crsFormat, "PROJJSON");
        bool asWkt = strEqualNoCase(o.crsFormat, "WKT2") ||
                     (!asProjjson && (auth.empty() || code.empty() ||
                                      !ds.srs.idTypeMatchesDb() ||
                                      !ds.srs.matchesDbDefinition()));
        if (asProjjson)
            out += "Coordinate Reference System PROJJSON:\n" +
                   ds.srs.projjson() + "\n";
        else if (asWkt)
            out += "Coordinate Reference System WKT:\n" +
                   ds.srs.wkt2_2019() + "\n";
        else
        {
            out += "Coordinate Reference System:\n";
            out += strPrintf("  - name: %s\n", ds.srs.name().c_str());
            if (!auth.empty() && !code.empty())
                out += strPrintf("  - ID: %s:%s\n", auth.c_str(),
                                 code.c_str());
            out += strPrintf("  - type: %s\n", ds.srs.typeString().c_str());
            if (ds.srs.isProjected())
            {
                std::string conv, method;
                if (crsProjectionInfo(ds.srs, conv, method))
                {
                    std::string convNorm = conv;
                    for (char &c : convNorm)
                        if (c == '-')
                            c = ' ';
                    if (convNorm == method)
                        out += strPrintf("  - projection type: %s\n",
                                         conv.c_str());
                    else
                        out += strPrintf("  - projection type: %s, %s\n",
                                         conv.c_str(), method.c_str());
                }
                std::string units = crsUnitsName(ds.srs);
                if (!units.empty())
                    out += strPrintf("  - units: %s\n", units.c_str());
            }
            double w, s, e, n;
            std::string areaName;
            if (ds.srs.areaOfUse(w, s, e, n, areaName))
                out += strPrintf(
                    "  - area of use: %s, west %.2f, south %.2f, east "
                    "%.2f, north %.2f\n",
                    areaDisplayName(areaName).c_str(), w, s, e, n);
        }
        auto mapping = ds.srs.dataAxisToSRSAxisMapping();
        std::string m;
        for (size_t i = 0; i < mapping.size(); ++i)
        {
            if (i)
                m += ",";
            m += strPrintf("%d", mapping[i]);
        }
        out += strPrintf("Data axis to CRS axis mapping: %s\n", m.c_str());
    }

    if (ds.hasGT)
    {
        if (ds.gt[2] == 0.0 && ds.gt[4] == 0.0)
        {
            out += strPrintf("Origin = (%.15f,%.15f)\n", ds.gt[0], ds.gt[3]);
            out += strPrintf("Pixel Size = (%.15f,%.15f)\n", ds.gt[1],
                             ds.gt[5]);
        }
        else
        {
            out += "GeoTransform =\n";
            out += strPrintf("  %.16g, %.16g, %.16g\n", ds.gt[0], ds.gt[1],
                             ds.gt[2]);
            out += strPrintf("  %.16g, %.16g, %.16g\n", ds.gt[3], ds.gt[4],
                             ds.gt[5]);
        }
    }

    if (!ds.gcps.empty() && !o.noGcp)
    {
        if (ds.hasGcpSrs && ds.gcpSrs.valid())
        {
            out += "GCP Projection = \n";
            out += ds.gcpSrs.wkt2_2019() + "\n";
            std::vector<int> mapv =
                !ds.gcpMapping.empty()
                    ? ds.gcpMapping
                    : ds.gcpSrs.dataAxisToSRSAxisMapping();
            std::string m;
            for (size_t i = 0; i < mapv.size(); ++i)
            {
                if (i)
                    m += ",";
                m += strPrintf("%d", mapv[i]);
            }
            out += strPrintf("Data axis to CRS axis mapping: %s\n",
                             m.c_str());
        }
        int n = 0;
        for (const auto &g : ds.gcps)
        {
            out += strPrintf("GCP[%3d]: Id=%s, Info=%s\n", n,
                             g.id.c_str(), g.info.c_str());
            out += strPrintf("          (%.15g,%.15g) -> "
                             "(%.15g,%.15g,%.15g)\n",
                             g.pixel, g.line, g.x, g.y, g.z);
            ++n;
        }
    }

    if (o.listMdd)
    {
        out += "Metadata domains:\n";
        for (const auto &d : buildDomainList(ds.metadata, ds.xmlDomains,
                                             ds.domainOrder, true))
            out += strPrintf("  %s\n", d.empty() ? "(default)" : d.c_str());
    }

    if (!o.noMd)
    {
        auto it = ds.metadata.find("");
        if (it != ds.metadata.end() && !it->second.empty())
        {
            out += "Metadata:\n";
            printMetaItems(out, it->second, "  ");
        }
        renderExtraDomainsText(out, o, ds.metadata, ds.xmlDomains,
                               ds.domainOrder, true, "");
        auto is = ds.metadata.find("IMAGE_STRUCTURE");
        if (is != ds.metadata.end() && !is->second.empty())
        {
            out += "Image Structure Metadata:\n";
            printMetaItems(out, is->second, "  ");
        }
    }

    if (!o.noMd)
    {
        auto sd = ds.metadata.find("SUBDATASETS");
        if (sd != ds.metadata.end() && !sd->second.empty())
        {
            out += "Subdatasets:\n";
            printMetaItems(out, sd->second, "  ");
        }
    }

    out += "Corner Coordinates:\n";
    static const char *cornerNames[5] = {"Upper Left", "Lower Left",
                                         "Upper Right", "Lower Right",
                                         "Center"};
    const double pts[5][2] = {
        {0, 0},
        {0, (double)ds.height},
        {(double)ds.width, 0},
        {(double)ds.width, (double)ds.height},
        {ds.width / 2.0, ds.height / 2.0},
    };
    for (int i = 0; i < 5; ++i)
    {
        out += strPrintf("%-11s ", cornerNames[i]);
        if (!ds.hasGT)
        {
            out += strPrintf("(%7.1f,%7.1f)\n", pts[i][0], pts[i][1]);
            continue;
        }
        double geoX = ds.gt[0] + ds.gt[1] * pts[i][0] + ds.gt[2] * pts[i][1];
        double geoY = ds.gt[3] + ds.gt[4] * pts[i][0] + ds.gt[5] * pts[i][1];
        if (std::fabs(geoX) < 181 && std::fabs(geoY) < 91)
            out += strPrintf("(%12.7f,%12.7f) ", geoX, geoY);
        else
            out += strPrintf("(%12.3f,%12.3f) ", geoX, geoY);
        double lon, lat;
        if (ds.hasSrs && ds.srs.toGeodetic(geoX, geoY, lon, lat))
        {
            out += strPrintf("(%s,", decToDms(lon, true, 2).c_str());
            out += strPrintf("%s)", decToDms(lat, false, 2).c_str());
        }
        out += "\n";
    }

    for (auto &b : ds.bands)
    {
        ds.infoBandTouch(b.index);
        out += strPrintf("Band %d Block=%dx%d Type=%s, ColorInterp=%s\n",
                         b.index, b.blockX, b.blockY, dtypeName(b.type),
                         b.colorInterp.c_str());
        if (!b.description.empty())
            out += strPrintf("  Description = %s\n", b.description.c_str());
        BandInfoCtx c = prepareBandInfo(ds, b, o);
        if (c.gotMin || c.gotMax || o.mm)
        {
            out += "  ";
            if (c.gotMin)
                out += strPrintf("Min=%.3f ", c.mdMin);
            if (c.gotMax)
                out += strPrintf("Max=%.3f ", c.mdMax);
            if (o.mm && c.computedOk)
                out += strPrintf("  Computed Min/Max=%.3f,%.3f", c.cmpMin,
                                 c.cmpMax);
            out += "\n";
        }
        if (c.haveStats)
            out += strPrintf(
                "  Minimum=%.3f, Maximum=%.3f, Mean=%.3f, StdDev=%.3f\n",
                c.sMin, c.sMax, c.sMean, c.sStd);
        if (c.haveHist)
        {
            if (c.histFresh)
                out += "0...10...20...30...40...50...60...70...80...90...100 "
                       "- done.\n";
            out += strPrintf("  %lld buckets from %g to %g:\n  ",
                             c.hist.buckets, c.hist.mn, c.hist.mx);
            for (long long v : c.hist.counts)
                out += strPrintf("%lld ", v);
            out += "\n";
        }
        else if (c.histBrokenZero)
            out += "0";
        if (o.checksum)
            out += strPrintf("  Checksum=%d\n", checksumBand(ds, b.index));
        if (b.hasNodata && !o.noNodata)
            out += strPrintf("  NoData Value=%s\n",
                             fmtNodataText(b).c_str());
        if (!ds.dispOverviews().empty())
        {
            const auto &ovl = ds.dispOverviews();
            out += "  Overviews: ";
            for (size_t i = 0; i < ovl.size(); ++i)
                out += strPrintf("%s%dx%d", i ? ", " : "", ovl[i].w,
                                 ovl[i].h);
            out += "\n";
            if (o.checksum)
            {
                out += "  Overviews checksum: ";
                for (size_t i = 0; i < ovl.size(); ++i)
                {
                    auto ovr = ds.openOverviewEntry(ovl[i]);
                    out += strPrintf(
                        "%s%d", i ? ", " : "",
                        ovr ? checksumBand(*ovr, b.index) : 0);
                }
                out += "\n";
            }
        }
        if (bandHasAlphaMask(ds, b, o))
            out += "  Mask Flags: PER_DATASET ALPHA \n";
        if (!b.unitType.empty())
            out += strPrintf("  Unit Type: %s\n", b.unitType.c_str());
        if (b.hasOffset || b.hasScale)
            out += strPrintf("  Offset: %.15g,   Scale:%.15g\n", b.offset,
                             b.scale);
        if (o.listMdd)
        {
            auto bl = buildDomainList(b.metadata, b.xmlDomains,
                                      b.domainOrder, false);
            if (!bl.empty())
            {
                out += "  Metadata domains:\n";
                for (const auto &d : bl)
                    out += strPrintf("    %s\n",
                                     d.empty() ? "(default)" : d.c_str());
            }
        }
        if (!o.noMd)
        {
            auto it = b.metadata.find("");
            if (it != b.metadata.end() && !it->second.empty())
            {
                out += "  Metadata:\n";
                printMetaItems(out, it->second, "    ");
            }
            renderExtraDomainsText(out, o, b.metadata, b.xmlDomains,
                                   b.domainOrder, false, "  ");
            auto is = b.metadata.find("IMAGE_STRUCTURE");
            if (is != b.metadata.end() && !is->second.empty())
            {
                out += "  Image Structure Metadata:\n";
                printMetaItems(out, is->second, "    ");
            }
        }
        if (!b.colorTable.empty() && b.colorInterp == "Palette")
        {
            out += strPrintf(
                "  Color Table (RGB with %d entries)\n",
                (int)b.colorTable.size());
            if (!o.noCt)
            {
                for (size_t i = 0; i < b.colorTable.size(); ++i)
                {
                    const ColorEntry &e = b.colorTable[i];
                    out += strPrintf("  %3d: %d,%d,%d,%d\n", (int)i, e.c1,
                                     e.c2, e.c3, e.c4);
                }
            }
        }
    }
    return out;
}

bool computeMinMax(RasterDatasetBase &ds, const Band &b, double &mn,
                   double &mx);

JVal metaToJson(const std::map<std::string, MetaDomain> &metadata,
                const std::map<std::string, std::string> &xmlD,
                const std::vector<std::string> &order, const InfoOpts &o,
                bool dsLevel)
{
    JVal m = jobj();
    auto has = [&](const std::string &key) {
        for (auto &kv : m.obj)
            if (kv.first == key)
                return true;
        return false;
    };
    auto add = [&](const std::string &domain) {
        if (has(domain))
            return;
        auto it = metadata.find(domain);
        if (it == metadata.end() || it->second.empty())
            return;
        JVal d = jobj();
        for (auto &kv : it->second)
            d.obj.emplace_back(kv.first, jstr(kv.second));
        m.obj.emplace_back(domain, std::move(d));
    };
    if (o.listMdd)
    {
        JVal arr = jarr();
        for (const auto &d : buildDomainList(metadata, xmlD, order, dsLevel))
            arr.arr.push_back(jstr(d));
        m.obj.emplace_back("metadataDomains", std::move(arr));
    }
    add("");
    for (const auto &req :
         expandExtraDomains(o, buildDomainList(metadata, xmlD, order,
                                               dsLevel)))
    {
        std::string actual;
        if (!resolveDomain(order, req, actual))
            continue;
        if (has(req))
            continue;
        auto xd = xmlD.find(actual);
        if (xd != xmlD.end())
        {
            m.obj.emplace_back(req, jstr(xd->second));
            continue;
        }
        auto it = metadata.find(actual);
        if (it == metadata.end() || it->second.empty())
            continue;
        JVal d = jobj();
        for (auto &kv : it->second)
            d.obj.emplace_back(kv.first, jstr(kv.second));
        m.obj.emplace_back(req, std::move(d));
    }
    add("IMAGE_STRUCTURE");
    if (dsLevel)
        add("SUBDATASETS");
    return m;
}

std::string renderJson(RasterDatasetBase &ds, const InfoOpts &o)
{
    JVal root = jobj();
    root.obj.emplace_back("description", jstr(ds.path));
    if (!ds.driverShort.empty() || !ds.driverLong.empty())
    {
        root.obj.emplace_back("driverShortName", jstr(ds.driverShort));
        root.obj.emplace_back("driverLongName", jstr(ds.driverLong));
    }
    if (!o.noFl)
    {
        JVal files = jarr();
        if (!infoFilesHidden(ds))
            for (auto &f : ds.files)
                files.arr.push_back(jstr(f));
        root.obj.emplace_back("files", std::move(files));
    }
    JVal size = jarr();
    size.arr.push_back(jint(ds.width));
    size.arr.push_back(jint(ds.height));
    root.obj.emplace_back("size", std::move(size));

    if (ds.hasSrs)
    {
        JVal cs = jobj();
        cs.obj.emplace_back("wkt", jstr(ds.srs.wkt2_2019()));
        JVal mapping = jarr();
        for (int v : ds.srs.dataAxisToSRSAxisMapping())
            mapping.arr.push_back(jint(v));
        cs.obj.emplace_back("dataAxisToSRSAxisMapping", std::move(mapping));
        root.obj.emplace_back("coordinateSystem", std::move(cs));
    }

    if (ds.hasGT)
    {
        JVal gtj = jarr();
        for (int i = 0; i < 6; ++i)
            gtj.arr.push_back(jraw(fmtG16(ds.gt[i])));
        root.obj.emplace_back("geoTransform", std::move(gtj));
    }

    if (!ds.gcps.empty() && !o.noGcp)
    {
        auto fmtP15 = [](double d) -> std::string
        {
            char buf[256];
            snprintf(buf, sizeof(buf), "%.15f", d);
            char *dot = strchr(buf, '.');
            if (dot)
            {
                char *p = buf + strlen(buf) - 1;
                while (p > dot + 1 && *p == '0')
                    --p;
                *(p + 1) = 0;
            }
            return buf;
        };
        JVal gj = jobj();
        if (ds.hasGcpSrs && ds.gcpSrs.valid())
        {
            JVal cs = jobj();
            cs.obj.emplace_back("wkt", jstr(ds.gcpSrs.wkt2_2019()));
            JVal mapping = jarr();
            std::vector<int> mapv =
                !ds.gcpMapping.empty()
                    ? ds.gcpMapping
                    : ds.gcpSrs.dataAxisToSRSAxisMapping();
            for (int v : mapv)
                mapping.arr.push_back(jint(v));
            cs.obj.emplace_back("dataAxisToSRSAxisMapping",
                                std::move(mapping));
            gj.obj.emplace_back("coordinateSystem", std::move(cs));
        }
        JVal lst = jarr();
        for (const auto &g : ds.gcps)
        {
            JVal e = jobj();
            e.obj.emplace_back("id", jstr(g.id));
            e.obj.emplace_back("info", jstr(g.info));
            e.obj.emplace_back("pixel", jraw(fmtP15(g.pixel)));
            e.obj.emplace_back("line", jraw(fmtP15(g.line)));
            e.obj.emplace_back("x", jraw(fmtP15(g.x)));
            e.obj.emplace_back("y", jraw(fmtP15(g.y)));
            e.obj.emplace_back("z", jraw(fmtP15(g.z)));
            lst.arr.push_back(std::move(e));
        }
        gj.obj.emplace_back("gcpList", std::move(lst));
        root.obj.emplace_back("gcps", std::move(gj));
    }

    if (!o.noMd)
        root.obj.emplace_back("metadata",
                              metaToJson(ds.metadata, ds.xmlDomains,
                                         ds.domainOrder, o, true));

    // corner coordinates use the geotransform (default if absent)
    {
        JVal cc = jobj();
        static const char *names[5] = {"upperLeft", "lowerLeft",
                                       "lowerRight", "upperRight", "center"};
        const double pts[5][2] = {
            {0, 0},
            {0, (double)ds.height},
            {(double)ds.width, (double)ds.height},
            {(double)ds.width, 0},
            {ds.width / 2.0, ds.height / 2.0},
        };
        for (int i = 0; i < 5; ++i)
        {
            double geoX =
                ds.gt[0] + ds.gt[1] * pts[i][0] + ds.gt[2] * pts[i][1];
            double geoY =
                ds.gt[3] + ds.gt[4] * pts[i][0] + ds.gt[5] * pts[i][1];
            JVal pt = jarr();
            bool deg = std::fabs(geoX) < 181 && std::fabs(geoY) < 91;
            pt.arr.push_back(jraw(deg ? trim7(geoX) : trim3(geoX)));
            pt.arr.push_back(jraw(deg ? trim7(geoY) : trim3(geoY)));
            cc.obj.emplace_back(names[i], std::move(pt));
        }
        root.obj.emplace_back("cornerCoordinates", std::move(cc));
    }

    if (ds.hasSrs && ds.hasGT)
    {
        double c[4][2] = {
            {ds.gt[0], ds.gt[3]},
            {ds.gt[0] + ds.gt[2] * ds.height,
             ds.gt[3] + ds.gt[5] * ds.height},
            {ds.gt[0] + ds.gt[1] * ds.width + ds.gt[2] * ds.height,
             ds.gt[3] + ds.gt[4] * ds.width + ds.gt[5] * ds.height},
            {ds.gt[0] + ds.gt[1] * ds.width, ds.gt[3] + ds.gt[4] * ds.width},
        };
        bool ok = true;
        double ll[4][2];
        for (int i = 0; i < 4 && ok; ++i)
            ok = ds.srs.toWgs84(c[i][0], c[i][1], ll[i][0], ll[i][1]);
        if (ok)
        {
            JVal ext = jobj();
            ext.obj.emplace_back("type", jstr("Polygon"));
            JVal coords = jarr();
            JVal ring = jarr();
            for (int i = 0; i < 5; ++i)
            {
                int k = i % 4;
                JVal pt = jarr();
                pt.arr.push_back(jraw(trim7(ll[k][0])));
                pt.arr.push_back(jraw(trim7(ll[k][1])));
                ring.arr.push_back(std::move(pt));
            }
            coords.arr.push_back(std::move(ring));
            ext.obj.emplace_back("coordinates", std::move(coords));
            root.obj.emplace_back("wgs84Extent", std::move(ext));
        }
    }

    JVal bandsJ = jarr();
    std::vector<BandInfoCtx> ctxs;
    for (auto &b : ds.bands)
    {
        ds.infoBandTouch(b.index);
        JVal bj = jobj();
        bj.obj.emplace_back("band", jint(b.index));
        JVal block = jarr();
        block.arr.push_back(jint(b.blockX));
        block.arr.push_back(jint(b.blockY));
        bj.obj.emplace_back("block", std::move(block));
        bj.obj.emplace_back("type", jstr(dtypeName(b.type)));
        bj.obj.emplace_back("colorInterpretation", jstr(b.colorInterp));
        if (!b.description.empty())
            bj.obj.emplace_back("description", jstr(b.description));
        ctxs.push_back(prepareBandInfo(ds, b, o));
        const BandInfoCtx &c = ctxs.back();
        if (c.gotMin)
            bj.obj.emplace_back("min", jraw(trim3(c.mdMin)));
        if (c.gotMax)
            bj.obj.emplace_back("max", jraw(trim3(c.mdMax)));
        if (o.mm && c.computedShowJson)
        {
            bj.obj.emplace_back("computedMin", jraw(trim3(c.cmpMin)));
            bj.obj.emplace_back("computedMax", jraw(trim3(c.cmpMax)));
        }
        if (c.haveStats)
        {
            bj.obj.emplace_back("minimum", jraw(trim3(c.sMin)));
            bj.obj.emplace_back("maximum", jraw(trim3(c.sMax)));
            bj.obj.emplace_back("mean", jraw(trim3(c.sMean)));
            bj.obj.emplace_back("stdDev", jraw(trim3(c.sStd)));
        }
        if (c.haveHist)
        {
            JVal hj = jobj();
            hj.obj.emplace_back("count", jint((long long)c.hist.buckets));
            hj.obj.emplace_back("min", jraw(fmt17(c.hist.mn)));
            hj.obj.emplace_back("max", jraw(fmt17(c.hist.mx)));
            JVal bk = jarr();
            for (long long v : c.hist.counts)
                bk.arr.push_back(jint(v));
            hj.obj.emplace_back("buckets", std::move(bk));
            bj.obj.emplace_back("histogram", std::move(hj));
        }
        if (o.checksum)
            bj.obj.emplace_back("checksum", jint(checksumBand(ds, b.index)));
        if (b.hasNodata && !o.noNodata)
        {
            if (std::isnan(b.nodata))
                bj.obj.emplace_back("noDataValue", jstr("NaN"));
            else
                bj.obj.emplace_back("noDataValue",
                                    jraw(fmtNodataJson(b)));
        }
        if (!ds.dispOverviews().empty())
        {
            JVal ovs = jarr();
            for (const auto &ov : ds.dispOverviews())
            {
                JVal oj = jobj();
                JVal sz = jarr();
                sz.arr.push_back(jint(ov.w));
                sz.arr.push_back(jint(ov.h));
                oj.obj.emplace_back("size", std::move(sz));
                if (o.checksum)
                {
                    auto ovr = ds.openOverviewEntry(ov);
                    oj.obj.emplace_back(
                        "checksum",
                        jint(ovr ? checksumBand(*ovr, b.index) : 0));
                }
                ovs.arr.push_back(std::move(oj));
            }
            bj.obj.emplace_back("overviews", std::move(ovs));
        }
        if (bandHasAlphaMask(ds, b, o))
        {
            JVal mask = jobj();
            JVal fl = jarr();
            fl.arr.push_back(jstr("PER_DATASET"));
            fl.arr.push_back(jstr("ALPHA"));
            mask.obj.emplace_back("flags", std::move(fl));
            mask.obj.emplace_back("overviews", jarr());
            bj.obj.emplace_back("mask", std::move(mask));
        }
        if (!b.unitType.empty())
            bj.obj.emplace_back("unit", jstr(b.unitType));
        if (b.hasOffset || b.hasScale)
        {
            bj.obj.emplace_back("offset",
                                jraw(fmtJsonSig(b.offset, 15, true)));
            bj.obj.emplace_back("scale",
                                jraw(fmtJsonSig(b.scale, 15, true)));
        }
        if (!o.noMd)
            bj.obj.emplace_back("metadata",
                                metaToJson(b.metadata, b.xmlDomains,
                                           b.domainOrder, o, false));
        if (!b.colorTable.empty() && b.colorInterp == "Palette" && !o.noCt)
        {
            JVal ct = jobj();
            ct.obj.emplace_back("palette", jstr("RGB"));
            ct.obj.emplace_back("count", jint((long long)b.colorTable.size()));
            JVal entries = jarr();
            for (const ColorEntry &e : b.colorTable)
            {
                JVal ent = jarr();
                ent.arr.push_back(jint(e.c1));
                ent.arr.push_back(jint(e.c2));
                ent.arr.push_back(jint(e.c3));
                ent.arr.push_back(jint(e.c4));
                entries.arr.push_back(std::move(ent));
            }
            ct.obj.emplace_back("entries", std::move(entries));
            bj.obj.emplace_back("colorTable", std::move(ct));
        }
        bandsJ.arr.push_back(std::move(bj));
    }
    root.obj.emplace_back("bands", std::move(bandsJ));

    // stac
    {
        JVal stac = jobj();
        JVal shape = jarr();
        shape.arr.push_back(jint(ds.height));
        shape.arr.push_back(jint(ds.width));
        stac.obj.emplace_back("proj:shape", std::move(shape));
        if (ds.hasSrs)
        {
            stac.obj.emplace_back("proj:wkt2", jstr(ds.srs.wkt2_2019()));
            int epsg = ds.srs.epsgCode();
            if (epsg > 0)
                stac.obj.emplace_back("proj:epsg", jint(epsg));
            else
                stac.obj.emplace_back("proj:epsg", JVal());
            std::string pjson = ds.srs.projjson();
            if (!pjson.empty())
            {
                bool ok = false;
                JVal parsed = JVal::parse(pjson, &ok);
                if (ok)
                    stac.obj.emplace_back("proj:projjson", std::move(parsed));
            }
        }
        if (ds.hasGT)
        {
            JVal tr = jarr();
            const int order[6] = {1, 2, 0, 4, 5, 3};
            for (int i = 0; i < 6; ++i)
                tr.arr.push_back(jraw(fmtG16(ds.gt[order[i]])));
            stac.obj.emplace_back("proj:transform", std::move(tr));
        }
        JVal rbands = jarr();
        for (auto &b : ds.bands)
        {
            JVal rb = jobj();
            rb.obj.emplace_back("data_type", jstr(dtypeStacName(b.type)));
            size_t bi = (size_t)(b.index - 1);
            if (bi < ctxs.size() && ctxs[bi].haveStats)
            {
                const BandInfoCtx &sc = ctxs[bi];
                JVal st = jobj();
                st.obj.emplace_back("minimum", jraw(trim3(sc.sMin)));
                st.obj.emplace_back("maximum", jraw(trim3(sc.sMax)));
                st.obj.emplace_back("mean", jraw(trim3(sc.sMean)));
                st.obj.emplace_back("stddev", jraw(trim3(sc.sStd)));
                rb.obj.emplace_back("stats", std::move(st));
            }
            if (bi < ctxs.size() && ctxs[bi].haveHist)
            {
                const HistItem &h = ctxs[bi].hist;
                JVal hj = jobj();
                hj.obj.emplace_back("count", jint((long long)h.buckets));
                hj.obj.emplace_back("min", jraw(fmt17(h.mn)));
                hj.obj.emplace_back("max", jraw(fmt17(h.mx)));
                JVal bk = jarr();
                for (long long v : h.counts)
                    bk.arr.push_back(jint(v));
                hj.obj.emplace_back("buckets", std::move(bk));
                rb.obj.emplace_back("histogram", std::move(hj));
            }
            // STAC omits nodata that only fits as a quoted string
            // (UInt64 beyond int64)
            if (b.hasNodata && !o.noNodata &&
                !(b.nodataIsU64 && b.nodataU64 > 9223372036854775807ULL))
            {
                if (std::isnan(b.nodata))
                    rb.obj.emplace_back("nodata", jstr("NaN"));
                else
                    rb.obj.emplace_back("nodata",
                                        jraw(fmtNodataJson(b)));
            }
            if (!b.unitType.empty())
                rb.obj.emplace_back("unit", jstr(b.unitType));
            if (b.hasScale || b.hasOffset)
            {
                rb.obj.emplace_back("scale", JVal());
                rb.obj.emplace_back("offset", JVal());
            }
            rbands.arr.push_back(std::move(rb));
        }
        stac.obj.emplace_back("raster:bands", std::move(rbands));
        JVal ebands = jarr();
        for (auto &b : ds.bands)
        {
            JVal eb = jobj();
            eb.obj.emplace_back("name", jstr(strPrintf("b%d", b.index)));
            eb.obj.emplace_back(
                "description",
                jstr(b.description.empty() ? b.colorInterp : b.description));
            if (b.colorInterp == "Red")
                eb.obj.emplace_back("common_name", jstr("red"));
            else if (b.colorInterp == "Green")
                eb.obj.emplace_back("common_name", jstr("green"));
            else if (b.colorInterp == "Blue")
                eb.obj.emplace_back("common_name", jstr("blue"));
            ebands.arr.push_back(std::move(eb));
        }
        stac.obj.emplace_back("eo:bands", std::move(ebands));
        root.obj.emplace_back("stac", std::move(stac));
    }

    return jsoncSerialize(root, false) + "\n";
}

bool computeMinMax(RasterDatasetBase &ds, const Band &b, double &mn,
                   double &mx)
{
    std::vector<double> vals;
    if (!readBandValues(ds, b.index, vals))
        return false;
    bool found = false;
    for (double v : vals)
    {
        if (std::isnan(v))
            continue;
        if (b.hasNodata)
        {
            if (std::isnan(b.nodata) ? std::isnan(v) : v == b.nodata)
                continue;
        }
        if (!found)
        {
            mn = mx = v;
            found = true;
        }
        else
        {
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
    }
    if (!found)
    {
        size_t slash = ds.path.find_last_of('/');
        std::string base =
            slash == std::string::npos ? ds.path : ds.path.substr(slash + 1);
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s, band %d: Failed to compute min/max, no "
                              "valid pixels found in sampling.",
                              base.c_str(), b.index));
        return false;
    }
    return true;
}

const char *kDriverNames[] = {"GTiff",          "COG",        "VRT",
                              "MEM",            "GNMFile",    "GNMDatabase",
                              "ESRI Shapefile", "GeoJSON",    "GeoJSONSeq",
                              "ESRIJSON",       "TopoJSON"};

int rasterInfoHandler(const CmdSpec &, ParseResult &r)
{
    std::string path = r.str("input");

    for (const auto &d : r.list("input-format"))
    {
        std::string err = inputFormatCapError(false, d);
        if (!err.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "info: " + err);
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
        if (strEqualNoCase(key, "COLOR_TABLE_MULTIPLIER"))
            oo.ctMult = atol(val.c_str());
        else if (strEqualNoCase(key, "GEOREF_SOURCES"))
        {
            oo.georefSet = true;
            oo.georefSources = strSplit(val, ',');
        }
    }

    std::string err;
    auto ds = openRaster(path, err, oo);
    if (!ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + path +
                            "' not recognized as being in a supported file "
                            "format.");
        handlerPrintUsage();
        return 1;
    }

    if (r.get("subdataset"))
    {
        long long want = atoll(r.str("subdataset").c_str());
        long long count = 0;
        auto sd = ds->metadata.find("SUBDATASETS");
        if (sd != ds->metadata.end())
            count = (long long)sd->second.size() / 2;
        if (want < 1 || want > count)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Invalid value for 'subdataset' argument. "
                                  "Should be between 1 and %lld",
                                  count));
            return 1;
        }
        std::string subPath =
            strPrintf("GTIFF_DIR:%lld:%s", want, path.c_str());
        ds = openRaster(subPath, err, oo);
        if (!ds)
        {
            if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + subPath +
                                "' not recognized as being in a supported "
                                "file format.");
            handlerPrintUsage();
            return 1;
        }
    }

    if (g_pipelineTailMaterialize && g_pipelineTailMaterialize(ds))
        return 1;

    std::vector<std::pair<std::string, std::string>> dbgSrcs;

    InfoOpts o;
    o.mm = r.flag("min-max");
    o.stats = r.flag("stats");
    o.approxStats = r.flag("approx-stats");
    o.hist = r.flag("hist");
    o.checksum = r.flag("checksum");
    o.noMd = r.flag("no-md");
    o.noCt = r.flag("no-ct");
    o.noFl = r.flag("no-fl");
    o.noGcp = r.flag("no-gcp");
    o.noNodata = r.flag("no-nodata");
    o.noMask = r.flag("no-mask");
    o.listMdd = r.flag("list-mdd");
    o.mddSet = r.get("metadata-domain") != nullptr;
    o.mdd = r.str("metadata-domain");
    o.crsFormat = r.str("crs-format", "AUTO");

    std::string format = r.str("output-format", "text");
    if (format == "json" && r.get("crs-format") &&
        !strEqualNoCase(o.crsFormat, "AUTO"))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "info: 'crs-format' cannot be set when 'format' is set "
                    "to 'json'");
        handlerPrintUsage();
        debugCloseDataset(*ds);
        return 1;
    }
    if (ds->driverShort == "GTiff")
    {
        if (!ds->isSubdataset)
            ds->debugOverviewScan();
        cplDebug("GTiff", "ScanDirectories()");
    }
    else if (ds->driverShort == "VRT" && cplDebugEnabled("GDAL"))
    {
        cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
        gdalDebugCacheMaxOnce();
        for (size_t i = 1; i < ds->files.size(); ++i)
        {
            const std::string &src = ds->files[i];
            if (!TiffFile::identify(src))
                continue;
            std::string ptr = cplDebugPtr();
            cplDebug("GDAL", "GDALOpen(" + src + ", this=" + ptr +
                                 ") succeeds as GTiff.");
            cplDebug("GTiff", "ScanDirectories()");
            cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
            dbgSrcs.emplace_back(src, ptr);
        }
    }
    ds->replayDeferred();
    applyLiveInfoOverrides(*ds);
    if (format == "json")
        fputs(renderJson(*ds, o).c_str(), stdout);
    else
        fputs(renderText(*ds, o).c_str(), stdout);
    if (ds->pamDirty)
        ds->persistPam();
    ds->flushSourcePams();
    debugCloseDataset(*ds);
    for (const auto &s : dbgSrcs)
        cplDebug("GDAL", "GDALClose(" + s.first + ", this=" + s.second + ")");
    return 0;
}

}  // namespace

// forwarded computes run on the source band (its PAM gets the result);
// the outcome is adopted as the VRT band's own only when nodata agrees
// or the VRT band's nodata falls outside the source's computed range
StatsResult vrtAwareForcedStats(RasterDatasetBase &ds, Band &b, bool approx,
                                TermProgress *tp, double p0, double p1)
{
    auto drive = [&]()
    {
        if (tp)
        {
            tp->update(p0);
            tp->update(p1);
        }
    };
    int dgBand = b.index;
    RasterDatasetBase *dg = ds.statsDelegate(b.index, dgBand);
    Band &db = (dg == &ds) ? b : dg->bands[(size_t)dgBand - 1];
    bool adopt = ds.statsAdopt(b.index);
    bool isVrt = dynamic_cast<VrtDataset *>(&ds) != nullptr;
    if (dg != &ds)
    {
        StatsResult rs = computeBandStats(*dg, db, approx);
        storeStats(*dg, db, rs);
        drive();
        if (!adopt && rs.ok && b.hasNodata && !db.hasNodata &&
            !std::isnan(b.nodata) &&
            (b.nodata < rs.mn || b.nodata > rs.mx))
            adopt = true;
        if (adopt)
        {
            if (rs.ok)
            {
                // an adopted approx result is flagged APPROXIMATE even
                // when the source compute never subsampled
                StatsResult ra = rs;
                ra.subsampled = approx;
                storeStats(ds, b, ra);
            }
            else
                storeStats(ds, b, rs);
            return rs;
        }
    }
    StatsResult r;
    if (dg == &ds && vrtMosaicForcedStats(ds, b, approx, r))
    {
        drive();
        if (r.ok)
        {
            StatsResult ra = r;
            ra.subsampled = approx;
            storeStats(ds, b, ra);
        }
        else
            storeStats(ds, b, r);
        return r;
    }
    r = computeBandStats(ds, b, approx);
    drive();
    bool scaled = false;
    if (auto *vd = dynamic_cast<VrtDataset *>(&ds))
        for (const auto &s : vd->bandSources[(size_t)b.index - 1])
            if (s.complex && (s.hasScaleRatio || s.hasScaleOffset ||
                              s.hasExponent))
                scaled = true;
    if (r.ok && approx && isVrt && !scaled)
    {
        // a VRT band computing its own forced approx stats is flagged
        // APPROXIMATE even when the compute never subsampled - except
        // when scaled sources push it down the base-band path
        StatsResult ra = r;
        ra.subsampled = true;
        storeStats(ds, b, ra);
    }
    else
        storeStats(ds, b, r);
    return r;
}

// forced GetDefaultHistogram: spec from cached/approx stats (256 buckets,
// Byte fixed), VRT lone-source forwarding, PAM cache reuse and embedding
bool vrtAwareHistogram(RasterDatasetBase &ds, Band &b, HistItem &out,
                       bool &fresh)
{
    fresh = false;
    // PAM answers GetDefaultHistogram with its first stored histogram
    // whatever its shape, before any range derivation
    if (!b.pamHists.empty())
    {
        out = b.pamHists[0];
        return true;
    }
    double mn = 0, mx = 0;
    long long buckets = 256;
    if (b.type == DType::Byte)
    {
        mn = -0.5;
        mx = 255.5;
    }
    else
    {
        double hmin = 0, hmax = 0;
        BandInfoCtx hc;
        if (statsFromCache(b, true, hc))
        {
            hmin = hc.sMin;
            hmax = hc.sMax;
        }
        else
        {
            StatsResult r = vrtAwareForcedStats(ds, b, true);
            if (r.ok)
            {
                hmin = r.mn;
                hmax = r.mx;
            }
            else
            {
                statsFailureError(ds, b);
                return false;
            }
        }
        if (hmin == hmax)
        {
            buckets = 1;
            mn = hmin - 0.5;
            mx = hmax + 0.5;
        }
        else
        {
            double hb = (hmax - hmin) / (2 * (buckets - 1));
            mn = hmin - hb;
            mx = hmax + hb;
        }
    }
    VrtDataset *vd = dynamic_cast<VrtDataset *>(&ds);
    bool cached = false;
    for (const auto &h : b.pamHists)
    {
        if (h.buckets == buckets && h.mn == mn && h.mx == mx)
        {
            out = h;
            cached = true;
            break;
        }
    }
    RasterDatasetBase *hg = &ds;
    int hgBand = b.index;
    Band *hb = &b;
    // the band's own cache answers before the delegate is even resolved,
    // so a cached rerun never opens the source
    if (!cached && vd)
    {
        if (VrtSource *hs = vd->histDelegate(b.index))
        {
            hg = hs->ds.get();
            hgBand = hs->sourceBand;
            hb = &hg->bands[(size_t)hgBand - 1];
        }
    }
    if (!cached && !vd)
    {
        if (RasterDatasetBase *wd = ds.histDelegateWrap(b.index))
        {
            hg = wd;
            hgBand = b.index;
            hb = &hg->bands[(size_t)b.index - 1];
        }
    }
    bool fwd = hg != &ds;
    bool loneSource =
        vd && vd->bandSources[(size_t)b.index - 1].size() == 1;
    auto storeVrtHist = [&]()
    {
        b.pamHists.push_back(out);
        ds.pamBands[b.index].hists.push_back(out);
        ds.pamDirty = true;
    };
    if (!cached && fwd)
    {
        for (const auto &h : hb->pamHists)
        {
            if (h.buckets == buckets && h.mn == mn && h.mx == mx)
            {
                out = h;
                cached = true;
                storeVrtHist();
                break;
            }
        }
    }
    if (!cached)
    {
        out.mn = mn;
        out.mx = mx;
        out.buckets = buckets;
        if (!computeHistogram(*hg, *hb, out))
            return false;
        fresh = true;
        int wrapMode = ds.histPamMode(b.index);
        if (fwd)
        {
            hb->pamHists.push_back(out);
            hg->pamBands[hgBand].hists.push_back(out);
            hg->pamDirty = true;
            storeVrtHist();
        }
        else if (wrapMode >= 0 ? wrapMode == 1 : !loneSource)
            storeVrtHist();
    }
    return true;
}

std::string crsAreaDisplayName(const std::string &raw)
{
    return areaDisplayName(raw);
}

bool crsProjectionInfoShared(const Srs &srs, std::string &convName,
                             std::string &methodName)
{
    return crsProjectionInfo(srs, convName, methodName);
}

std::string crsUnitsNameShared(const Srs &srs)
{
    return crsUnitsName(srs);
}

bool infoDispatchRaster(const std::string &path)
{
    if (TiffFile::identify(path))
        return true;
    std::string content;
    if (readFileToString(path, content) &&
        strStartsWith(content.substr(0, 6000), "<VRTDataset"))
        return true;
    return false;
}

bool infoDispatchVector(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    if (S_ISDIR(st.st_mode))
    {
        bool found = false;
        DIR *dir = opendir(path.c_str());
        if (dir)
        {
            struct dirent *de;
            while (!found && (de = readdir(dir)) != nullptr)
            {
                std::string n = de->d_name;
                if (n.size() > 4 &&
                    (strEqualNoCase(n.substr(n.size() - 4), ".shp") ||
                     strEqualNoCase(n.substr(n.size() - 4), ".dbf")))
                    found = true;
            }
            closedir(dir);
        }
        return found;
    }
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    if (strEqualNoCase(ext, "shp") || strEqualNoCase(ext, "dbf"))
        return true;
    std::string content;
    if (readFileToString(path, content))
    {
        std::string head = content.substr(0, 6000);
        size_t b = head.find_first_not_of(" \t\r\n");
        if (b != std::string::npos && (head[b] == '{' || head[b] == '[') &&
            jsonVectorIdentify(head))
            return true;
    }
    return false;
}

bool datasetIdentify(const std::string &path,
                     const std::vector<std::string> &types)
{
    bool vectorOk = types.empty();
    for (const auto &t : types)
        if (t == "vector")
            vectorOk = true;
    if (vectorOk && !gdalSkipHas("ESRI Shapefile"))
    {
        if (vsiIsVirtual(path) && vsiIsDir(path))
        {
            std::vector<std::string> names;
            vsiListDir(path, names);
            for (const auto &n : names)
                if (n.size() > 4 &&
                    (strEqualNoCase(n.substr(n.size() - 4), ".shp") ||
                     strEqualNoCase(n.substr(n.size() - 4), ".dbf")))
                    return true;
            return false;
        }
        struct stat st;
        if (!vsiIsVirtual(path) && stat(path.c_str(), &st) == 0 &&
            S_ISDIR(st.st_mode))
        {
            bool found = false;
            DIR *dir = opendir(path.c_str());
            if (dir)
            {
                struct dirent *de;
                while (!found && (de = readdir(dir)) != nullptr)
                {
                    std::string n = de->d_name;
                    if (n.size() > 4 &&
                        (strEqualNoCase(n.substr(n.size() - 4), ".shp") ||
                         strEqualNoCase(n.substr(n.size() - 4), ".dbf")))
                        found = true;
                }
                closedir(dir);
            }
            return found;
        }
        size_t dot = path.find_last_of('.');
        std::string ext =
            dot == std::string::npos ? "" : path.substr(dot + 1);
        if (strEqualNoCase(ext, "shp") || strEqualNoCase(ext, "dbf"))
            return true;
    }
    if (TiffFile::identify(path) &&
        (!gdalSkipHas("GTiff") || !gdalSkipHas("COG")))
        return true;
    std::string content;
    if (readFileToString(path, content))
    {
        std::string head = content.substr(0, 6000);
        if (strStartsWith(head, "<VRTDataset") && !gdalSkipHas("VRT"))
            return true;
        size_t b = head.find_first_not_of(" \t\r\n\x1e");
        if (b != std::string::npos && (head[b] == '{' || head[b] == '['))
        {
            if (jsonVectorIdentify(head))
            {
                std::string ext;
                size_t dot = path.find_last_of('.');
                if (dot != std::string::npos)
                    ext = strToLower(path.substr(dot + 1));
                std::string drv = "GeoJSON";
                if (ext == "geojsonl" || ext == "geojsons" ||
                    head.find('\x1e') != std::string::npos)
                    drv = "GeoJSONSeq";
                else if (head.find("\"fieldAliases\"") !=
                             std::string::npos ||
                         head.find("\"esriFieldType") !=
                             std::string::npos ||
                         head.find("\"geometryType\":\"esriGeometry") !=
                             std::string::npos ||
                         head.rfind("{\"spatialReference\":{\"wkid\"", 0) ==
                             0)
                    drv = "ESRIJSON";
                else if (head.find("\"Topology\"") != std::string::npos)
                    drv = "TopoJSON";
                if (!gdalSkipHas(drv))
                    return true;
            }
        }
        if (head.size() >= 4 && (unsigned char)head[0] == 0 &&
            (unsigned char)head[1] == 0 && (unsigned char)head[2] == 0x27 &&
            (unsigned char)head[3] == 0x0a &&
            !gdalSkipHas("ESRI Shapefile"))
            return true;  // shapefile .shp
    }
    return false;
}

void registerRasterHandlers()
{
    registerHandler("raster_info", rasterInfoHandler);
    registerHandler("info", rasterInfoHandler);
}
