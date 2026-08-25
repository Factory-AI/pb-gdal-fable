#include "cpl.h"
#include "dataset.h"
#include "util.h"
#include "xml_min.h"
#include "progress.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

const char *dtypeName(DType t)
{
    switch (t)
    {
        case DType::Byte:
            return "Byte";
        case DType::Int8:
            return "Int8";
        case DType::UInt16:
            return "UInt16";
        case DType::Int16:
            return "Int16";
        case DType::UInt32:
            return "UInt32";
        case DType::Int32:
            return "Int32";
        case DType::UInt64:
            return "UInt64";
        case DType::Int64:
            return "Int64";
        case DType::Float16:
            return "Float16";
        case DType::Float32:
            return "Float32";
        case DType::Float64:
            return "Float64";
        case DType::CInt16:
            return "CInt16";
        case DType::CInt32:
            return "CInt32";
        case DType::CFloat32:
            return "CFloat32";
        case DType::CFloat64:
            return "CFloat64";
        default:
            return "Unknown";
    }
}

DType dtypeFromName(const std::string &name)
{
    static const struct
    {
        const char *n;
        DType t;
    } table[] = {
        {"Byte", DType::Byte},       {"UInt8", DType::Byte},
        {"Int8", DType::Int8},       {"UInt16", DType::UInt16},
        {"Int16", DType::Int16},     {"UInt32", DType::UInt32},
        {"Int32", DType::Int32},     {"UInt64", DType::UInt64},
        {"Int64", DType::Int64},     {"Float16", DType::Float16},
        {"Float32", DType::Float32}, {"Float64", DType::Float64},
        {"CInt16", DType::CInt16},   {"CInt32", DType::CInt32},
        {"CFloat32", DType::CFloat32}, {"CFloat64", DType::CFloat64},
    };
    for (auto &e : table)
        if (strEqualNoCase(name, e.n))
            return e.t;
    return DType::Unknown;
}

int dtypeSizeBytes(DType t)
{
    switch (t)
    {
        case DType::Byte:
        case DType::Int8:
            return 1;
        case DType::UInt16:
        case DType::Int16:
        case DType::Float16:
            return 2;
        case DType::UInt32:
        case DType::Int32:
        case DType::Float32:
        case DType::CInt16:
            return 4;
        case DType::UInt64:
        case DType::Int64:
        case DType::Float64:
        case DType::CInt32:
        case DType::CFloat32:
            return 8;
        case DType::CFloat64:
            return 16;
        default:
            return 1;
    }
}

bool dtypeIsComplex(DType t)
{
    return t == DType::CInt16 || t == DType::CInt32 ||
           t == DType::CFloat32 || t == DType::CFloat64;
}

const char *dtypeStacName(DType t)
{
    switch (t)
    {
        case DType::Byte:
            return "uint8";
        case DType::Int8:
            return "int8";
        case DType::UInt16:
            return "uint16";
        case DType::Int16:
            return "int16";
        case DType::UInt32:
            return "uint32";
        case DType::Int32:
            return "int32";
        case DType::UInt64:
            return "uint64";
        case DType::Int64:
            return "int64";
        case DType::Float16:
            return "float16";
        case DType::Float32:
            return "float32";
        case DType::Float64:
            return "float64";
        case DType::CInt16:
            return "cint16";
        case DType::CInt32:
            return "cint32";
        case DType::CFloat32:
            return "cfloat32";
        case DType::CFloat64:
            return "cfloat64";
        default:
            return "unknown";
    }
}

// CSLSetNameValue semantics: keys match case-insensitively, a replace
// keeps the slot but takes the new key spelling, sorted domains insert
// by case-insensitive order
static int mdKeyCmp(const std::string &a, const std::string &b)
{
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i)
    {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

static void setInDomain(MetaDomain &d, const std::string &k,
                        const std::string &v, bool sorted)
{
    for (auto &kv : d)
    {
        if (mdKeyCmp(kv.first, k) == 0)
        {
            kv.first = k;
            kv.second = v;
            return;
        }
    }
    if (sorted)
    {
        auto it = d.begin();
        while (it != d.end() && mdKeyCmp(it->first, k) < 0)
            ++it;
        d.insert(it, {k, v});
    }
    else
        d.emplace_back(k, v);
}

static void noteDomainIn(std::vector<std::string> &order,
                         const std::string &domain)
{
    for (const auto &d : order)
        if (d == domain)
            return;
    order.push_back(domain);
}

static bool isSortedDomain(const std::vector<std::string> &sortedDomains,
                           const std::string &domain)
{
    for (const auto &d : sortedDomains)
        if (d == domain)
            return true;
    return false;
}

static void markPamSortedIn(std::map<std::string, MetaDomain> &metadata,
                            std::vector<std::string> &sortedDomains,
                            const std::string &domain)
{
    if (!isSortedDomain(sortedDomains, domain))
        sortedDomains.push_back(domain);
    auto it = metadata.find(domain);
    if (it != metadata.end())
        std::stable_sort(it->second.begin(), it->second.end(),
                         [](const auto &a, const auto &b) {
                             return a.first < b.first;
                         });
}

static const std::string *getInDomain(const MetaDomain &d,
                                      const std::string &k)
{
    for (auto &kv : d)
        if (kv.first == k)
            return &kv.second;
    return nullptr;
}

void Band::setMd(const std::string &domain, const std::string &k,
                 const std::string &v)
{
    noteDomainIn(domainOrder, domain);
    setInDomain(metadata[domain], k, v,
                isSortedDomain(sortedDomains, domain));
}

void Band::noteDomain(const std::string &domain)
{
    noteDomainIn(domainOrder, domain);
}

void Band::markPamSorted(const std::string &domain)
{
    markPamSortedIn(metadata, sortedDomains, domain);
}

const std::string *Band::getMd(const std::string &domain,
                               const std::string &k) const
{
    auto it = metadata.find(domain);
    if (it == metadata.end())
        return nullptr;
    return getInDomain(it->second, k);
}

void Band::removeMd(const std::string &domain, const std::string &k)
{
    auto it = metadata.find(domain);
    if (it == metadata.end())
        return;
    auto &dom = it->second;
    for (auto e = dom.begin(); e != dom.end(); ++e)
    {
        if (mdKeyCmp(e->first, k) == 0)
        {
            dom.erase(e);
            break;
        }
    }
    if (dom.empty())
        metadata.erase(it);
}

void RasterDatasetBase::setMd(const std::string &domain, const std::string &k,
                              const std::string &v)
{
    noteDomainIn(domainOrder, domain);
    setInDomain(metadata[domain], k, v,
                isSortedDomain(sortedDomains, domain));
}

void RasterDatasetBase::removeMd(const std::string &domain,
                                 const std::string &k)
{
    auto it = metadata.find(domain);
    if (it == metadata.end())
        return;
    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                    [&](const std::pair<std::string,
                                                        std::string> &kv) {
                                        return mdKeyCmp(kv.first, k) == 0;
                                    }),
                     it->second.end());
}

void RasterDatasetBase::noteDomain(const std::string &domain)
{
    noteDomainIn(domainOrder, domain);
}

void RasterDatasetBase::markPamSorted(const std::string &domain)
{
    markPamSortedIn(metadata, sortedDomains, domain);
}

const std::string *RasterDatasetBase::getMd(const std::string &domain,
                                            const std::string &k) const
{
    auto it = metadata.find(domain);
    if (it == metadata.end())
        return nullptr;
    return getInDomain(it->second, k);
}

// integer bands materialize through the band type before analysis
// (stats/checksum/histogram/min-max), the way block-cached reads do:
// scaled VRT sources may compute fractional doubles that the band
// rounds away
bool readBandValues(RasterDatasetBase &ds, int band,
                    std::vector<double> &vals)
{
    DType t = ds.bands[(size_t)band - 1].type;
    bool intType = t == DType::Byte || t == DType::Int8 ||
                   t == DType::UInt16 || t == DType::Int16 ||
                   t == DType::UInt32 || t == DType::Int32 ||
                   t == DType::UInt64 || t == DType::Int64;
    if (!intType)
        return ds.readBand(band, vals);
    std::vector<uint8_t> raw;
    if (!ds.readBandRaw(band, raw))
        return false;
    size_t sz = (size_t)dtypeSizeBytes(t);
    size_t n = raw.size() / sz;
    vals.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t *p = raw.data() + i * sz;
        switch (t)
        {
            case DType::Byte:
                vals[i] = *p;
                break;
            case DType::Int8:
                vals[i] = *(const int8_t *)p;
                break;
            case DType::UInt16:
                vals[i] = *(const uint16_t *)p;
                break;
            case DType::Int16:
                vals[i] = *(const int16_t *)p;
                break;
            case DType::UInt32:
                vals[i] = *(const uint32_t *)p;
                break;
            case DType::Int32:
                vals[i] = *(const int32_t *)p;
                break;
            case DType::UInt64:
                vals[i] = (double)*(const uint64_t *)p;
                break;
            default:
                vals[i] = (double)*(const int64_t *)p;
                break;
        }
    }
    return true;
}

int checksumBand(RasterDatasetBase &ds, int band)
{
    static const int primes[11] = {7,  11, 13, 17, 19, 23,
                                   29, 31, 37, 41, 43};
    int hook = ds.checksumHook(band);
    if (hook != -2)
        return hook;
    std::vector<double> vals;
    DType t = ds.bands[(size_t)band - 1].type;
    if (dtypeIsComplex(t))
    {
        // the checksum runs over interleaved re/im components, each
        // advancing the prime cycle
        std::vector<uint8_t> raw;
        if (!ds.readBandRaw(band, raw))
            return -1;
        int half = dtypeSizeBytes(t) / 2;
        size_t n = raw.size() / (size_t)half;
        vals.resize(n);
        for (size_t k = 0; k < n; ++k)
        {
            const uint8_t *p = raw.data() + (size_t)k * half;
            switch (t)
            {
                case DType::CInt16:
                    vals[k] = *(const int16_t *)p;
                    break;
                case DType::CInt32:
                    vals[k] = *(const int32_t *)p;
                    break;
                case DType::CFloat32:
                    vals[k] = *(const float *)p;
                    break;
                default:
                    vals[k] = *(const double *)p;
                    break;
            }
        }
    }
    else if (!readBandValues(ds, band, vals))
        return -1;
    bool floatPath = false;
    switch (t)
    {
        case DType::Float16:
        case DType::Float32:
        case DType::Float64:
        case DType::CInt16:
        case DType::CInt32:
        case DType::CFloat32:
        case DType::CFloat64:
            floatPath = true;
            break;
        default:
            break;
    }
    long long checksum = 0;
    size_t i = 0;
    for (double d : vals)
    {
        long long v;
        if (floatPath)
        {
            // GDAL rounds half-up; NaN/Inf cast to INT32_MIN on x86 but
            // finite overflow clamps to +/-INT32_MAX (asymmetric)
            if (!std::isfinite(d))
                v = -2147483648LL;
            else
            {
                double r = std::floor(d + 0.5);
                if (r > 2147483647.0)
                    v = 2147483647LL;
                else if (r < -2147483647.0)
                    v = -2147483647LL;
                else
                    v = (long long)r;
            }
        }
        else
        {
            // integer data is read through Int32, which rounds and clamps
            if (d > 2147483647.0)
                v = 2147483647LL;
            else if (d < -2147483648.0)
                v = -2147483648LL;
            else
                v = (long long)(d >= 0 ? d + 0.5 : d - 0.5);
        }
        checksum += v % primes[i % 11];
        checksum &= 0xffff;
        ++i;
    }
    return (int)checksum;
}

namespace
{

bool dtypeIsFloat(DType t)
{
    switch (t)
    {
        case DType::Float16:
        case DType::Float32:
        case DType::Float64:
        case DType::CFloat32:
        case DType::CFloat64:
            return true;
        default:
            return false;
    }
}

bool skipValue(double v, bool isFloat, bool hasNodata, double nodata)
{
    if (isFloat && std::isnan(v))
        return true;
    if (hasNodata)
    {
        if (std::isnan(nodata))
            return std::isnan(v);
        return v == nodata;
    }
    return false;
}

}  // namespace

StatsResult computeBandStats(RasterDatasetBase &ds, const Band &b,
                             bool approxOK)
{
    StatsResult r;
    std::vector<double> vals;
    if (!readBandValues(ds, b.index, vals))
        return r;
    bool isFloat = dtypeIsFloat(b.type);
    bool intPath = b.type == DType::Byte || b.type == DType::UInt16;
    int W = ds.width, H = ds.height;
    long long total = 0;
    long long count = 0;
    double mn = 0, mx = 0;
    unsigned long long nSum = 0, nSumSquare = 0;
    double dfMean = 0, dfM2 = 0;
    auto addValue = [&](double v) {
        if (skipValue(v, isFloat, b.hasNodata, b.nodata))
            return;
        if (count == 0)
        {
            mn = mx = v;
        }
        else
        {
            if (v < mn)
                mn = v;
            if (v > mx)
                mx = v;
        }
        ++count;
        if (intPath)
        {
            unsigned long long uv = (unsigned long long)v;
            nSum += uv;
            nSumSquare += uv * uv;
        }
        else
        {
            const double delta = v - dfMean;
            dfMean += delta / count;
            dfM2 += delta * (v - dfMean);
        }
    };
    int bw = b.blockX > 0 ? b.blockX : W;
    int bh = b.blockY > 0 ? b.blockY : H;
    long long nbx = (W + bw - 1) / bw;
    long long nby = (H + bh - 1) / bh;
    long long nBlocks = nbx * nby;
    long long sampleRate = 1;
    if (approxOK)
    {
        sampleRate = (long long)sqrt((double)nBlocks);
        if (sampleRate < 1)
            sampleRate = 1;
    }
    r.subsampled = sampleRate > 1;
    for (long long ib = 0; ib < nBlocks; ib += sampleRate)
    {
        int bx = (int)(ib % nbx) * bw;
        int by = (int)(ib / nbx) * bh;
        int ye = by + bh > H ? H : by + bh;
        int xe = bx + bw > W ? W : bx + bw;
        total += (long long)(ye - by) * (xe - bx);
        for (int y = by; y < ye; ++y)
            for (int x = bx; x < xe; ++x)
                addValue(vals[(size_t)y * W + x]);
    }
    r.validPct = total > 0 ? 100.0 * (double)count / (double)total : 0.0;
    r.count = count;
    if (count == 0)
        return r;
    r.ok = true;
    r.mn = mn;
    r.mx = mx;
    if (intPath)
    {
        r.mean = (double)nSum / (double)count;
        r.stddev =
            sqrt((double)nSumSquare / (double)count - r.mean * r.mean);
    }
    else
    {
        r.mean = dfMean;
        r.stddev = sqrt(dfM2 / (double)count);
    }
    return r;
}

static void emitXmlDomain(std::string &pam, const std::string &content,
                          int depth)
{
    XmlNode wrap;
    if (xmlParse("<W>" + content + "</W>", wrap))
        for (const auto &c : wrap.children)
            xmlSerialize(c, pam, depth);
}

static std::string pamBody(RasterDatasetBase &ds, int depth)
{
    std::string ind(depth * 2, ' ');
    std::string pam;
    if (!ds.pamSrsRaw.empty())
        pam += ind + "<SRS dataAxisToSRSAxisMapping=\"" + ds.pamSrsMapping +
               "\">" + xmlEsc(ds.pamSrsRaw) + "</SRS>\n";
    if (!ds.pamGtRaw.empty())
    {
        std::vector<double> g;
        std::string cur;
        for (char c : ds.pamGtRaw + ",")
        {
            if (c == ',')
            {
                if (!cur.empty())
                    g.push_back(strtod(cur.c_str(), nullptr));
                cur.clear();
            }
            else
                cur += c;
        }
        std::string gtxt = ds.pamGtRaw;
        if (g.size() == 6)
        {
            gtxt.clear();
            for (int i = 0; i < 6; ++i)
                gtxt += strPrintf(i ? ",%24.16e" : "%24.16e", g[i]);
        }
        pam += ind + "<GeoTransform>" + gtxt + "</GeoTransform>\n";
    }
    std::vector<std::string> domains;
    for (const auto &e : ds.pamMdi)
        noteDomainIn(domains, e[0]);
    for (const auto &x : ds.pamXmlDomains)
        noteDomainIn(domains, x.first);
    std::sort(domains.begin(), domains.end());
    for (const auto &dom : domains)
    {
        const std::string *xml = nullptr;
        for (const auto &x : ds.pamXmlDomains)
            if (x.first == dom)
                xml = &x.second;
        if (xml)
        {
            pam += ind + "<Metadata domain=\"" + xmlEsc(dom, true) +
                   "\" format=\"xml\">\n";
            emitXmlDomain(pam, *xml, depth + 1);
            pam += ind + "</Metadata>\n";
            continue;
        }
        pam += dom.empty() ? ind + "<Metadata>\n"
                           : ind + "<Metadata domain=\"" + xmlEsc(dom, true) +
                                 "\">\n";
        for (const auto &e : ds.pamMdi)
            if (e[0] == dom)
                pam += ind + "  <MDI key=\"" + xmlEsc(e[1], true) + "\">" +
                       xmlEsc(e[2]) + "</MDI>\n";
        pam += ind + "</Metadata>\n";
    }
    for (const auto &bp : ds.pamBands)
    {
        const PamBandState &pb = bp.second;
        if (pb.mdi.empty() && pb.hists.empty() && pb.extraMdi.empty() &&
            pb.xmlDomains.empty() && pb.nodataRaw.empty() &&
            pb.scaleRaw.empty() && pb.offsetRaw.empty() &&
            pb.description.empty())
            continue;
        pam += ind + strPrintf("<PAMRasterBand band=\"%d\">\n", bp.first);
        if (!pb.description.empty())
            pam += ind + "  <Description>" + xmlEsc(pb.description) +
                   "</Description>\n";
        if (!pb.nodataRaw.empty())
            pam += ind + "  <NoDataValue>" + pb.nodataRaw +
                   "</NoDataValue>\n";
        if (!pb.offsetRaw.empty())
            pam += ind + "  <Offset>" + pb.offsetRaw + "</Offset>\n";
        if (!pb.scaleRaw.empty())
            pam += ind + "  <Scale>" + pb.scaleRaw + "</Scale>\n";
        if (!pb.hists.empty())
            emitHistogramsXml(pam, pb.hists, ind + "  ");
        std::vector<std::string> bdoms;
        if (!pb.mdi.empty())
            bdoms.push_back("");
        for (const auto &e : pb.extraMdi)
            noteDomainIn(bdoms, e[0]);
        for (const auto &x : pb.xmlDomains)
            noteDomainIn(bdoms, x.first);
        std::sort(bdoms.begin(), bdoms.end());
        for (const auto &dom : bdoms)
        {
            const std::string *xml = nullptr;
            for (const auto &x : pb.xmlDomains)
                if (x.first == dom)
                    xml = &x.second;
            if (xml)
            {
                pam += ind + "  <Metadata domain=\"" + xmlEsc(dom, true) +
                       "\" format=\"xml\">\n";
                emitXmlDomain(pam, *xml, depth + 2);
                pam += ind + "  </Metadata>\n";
                continue;
            }
            if (dom.empty())
            {
                pam += ind + "  <Metadata>\n";
                for (const auto &kv : pb.mdi)
                    pam += ind + "    <MDI key=\"" + xmlEsc(kv.first, true) +
                           "\">" + xmlEsc(kv.second) + "</MDI>\n";
                pam += ind + "  </Metadata>\n";
                continue;
            }
            pam += ind + "  <Metadata domain=\"" + xmlEsc(dom, true) +
                   "\">\n";
            for (const auto &e : pb.extraMdi)
                if (e[0] == dom)
                    pam += ind + "    <MDI key=\"" + xmlEsc(e[1], true) +
                           "\">" + xmlEsc(e[2]) + "</MDI>\n";
            pam += ind + "  </Metadata>\n";
        }
        pam += ind + "</PAMRasterBand>\n";
    }
    return pam;
}

void RasterDatasetBase::persistPam()
{
    writePam(*this);
}

void emitHistogramsXml(std::string &out, const std::vector<HistItem> &hists,
                       const std::string &ind)
{
    out += ind + "<Histograms>\n";
    for (const auto &h : hists)
    {
        out += ind + "  <HistItem>\n";
        out += ind + strPrintf("    <HistMin>%.16g</HistMin>\n", h.mn);
        out += ind + strPrintf("    <HistMax>%.16g</HistMax>\n", h.mx);
        out += ind + strPrintf("    <BucketCount>%lld</BucketCount>\n",
                               h.buckets);
        out += ind + "    <IncludeOutOfRange>1</IncludeOutOfRange>\n";
        out += ind + strPrintf("    <Approximate>%d</Approximate>\n",
                               h.approx ? 1 : 0);
        out += ind + "    <HistCounts>";
        for (size_t i = 0; i < h.counts.size(); ++i)
        {
            if (i)
                out += "|";
            out += strPrintf("%lld", h.counts[i]);
        }
        out += "</HistCounts>\n";
        out += ind + "  </HistItem>\n";
    }
    out += ind + "</Histograms>\n";
}

void RasterDatasetBase::replayDeferred()
{
    for (const auto &w : deferredWarnings)
    {
        if (w.debug)
            cplDebug("GTiff", w.text);
        else
            cplErrorStr(w.warning ? CE_Warning : CE_Failure, CPLE_AppDefined,
                        w.text);
    }
    deferredWarnings.clear();
}

void RasterDatasetBase::replaySrsDecodeWarnings()
{
    for (auto it = deferredWarnings.begin(); it != deferredWarnings.end();)
    {
        if (it->srsDecode)
        {
            cplErrorStr(it->warning ? CE_Warning : CE_Failure,
                        CPLE_AppDefined, it->text);
            it = deferredWarnings.erase(it);
        }
        else
            ++it;
    }
}

void RasterDatasetBase::debugOverviewScan()
{
    cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
    if (extOvrPath.empty())
        return;
    extOvrOpened = true;
    extOvrDebugPtr = cplDebugPtr();
    cplDebug("GDAL", "GDALOpen(" + extOvrPath + ", this=" + extOvrDebugPtr +
                         ") succeeds as GTiff.");
    cplDebug("GTiff", "ScanDirectories()");
    for (size_t i = 1; i < extOverviews.size(); ++i)
        cplDebug("GTiff", strPrintf("Opened %dx%d overview.",
                                    extOverviews[i].w, extOverviews[i].h));
    cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
}

void debugCloseDataset(const RasterDatasetBase &ds)
{
    if (ds.debugPtr.empty())
        return;
    cplDebug("GDAL", "GDALClose(" + ds.path + ", this=" + ds.debugPtr + ")");
    if (ds.extOvrOpened)
        cplDebug("GDAL", "GDALClose(" + ds.extOvrPath + ", this=" +
                             ds.extOvrDebugPtr + ")");
}

void writePam(RasterDatasetBase &ds)
{
    if (!gdalPamEnabled())
    {
        ds.pamDirty = false;
        return;
    }
    for (const Band &b : ds.bands)
        if (!b.description.empty())
            ds.pamBands[b.index].description = b.description;
    if (!ds.isSubdataset)
    {
        std::string body = pamBody(ds, 1);
        if (body.empty())
        {
            remove(ds.pamPath.c_str());
            ds.pamDirty = false;
            return;
        }
    }
    std::string pam = "<PAMDataset>\n";
    if (ds.isSubdataset)
    {
        std::string existing;
        XmlNode root;
        if (readFileToString(ds.pamPath, existing) &&
            xmlParse(existing, root) && root.name == "PAMDataset")
        {
            for (const auto &c : root.children)
            {
                if (c.name == "Subdataset" && c.attr("name") == ds.subName)
                    continue;
                xmlSerialize(c, pam, 1);
            }
        }
        pam += "  <Subdataset name=\"" + xmlEsc(ds.subName, true) + "\">\n";
        pam += "    <PAMDataset>\n";
        pam += pamBody(ds, 3);
        pam += "    </PAMDataset>\n";
        pam += "  </Subdataset>\n";
    }
    else
        pam += pamBody(ds, 1);
    pam += "</PAMDataset>\n";
    writeStringToFile(ds.pamPath, pam);
    ds.pamDirty = false;
}

bool computeHistogram(RasterDatasetBase &ds, const Band &b, HistItem &h)
{
    std::vector<double> vals;
    if (!readBandValues(ds, b.index, vals))
        return false;
    bool isFloat = dtypeIsFloat(b.type);
    h.counts.assign((size_t)h.buckets, 0);
    const double scale =
        h.mx > h.mn ? (double)h.buckets / (h.mx - h.mn) : 0.0;
    for (double v : vals)
    {
        if (skipValue(v, isFloat, b.hasNodata, b.nodata))
            continue;
        long long i = (long long)floor((v - h.mn) * scale);
        if (i < 0)
            i = 0;
        else if (i >= h.buckets)
            i = h.buckets - 1;
        h.counts[(size_t)i]++;
    }
    return true;
}

bool RasterDatasetBase::readAllBands(std::vector<std::vector<uint8_t>> &out,
                                     TermProgress *tp, bool strict)
{
    (void)strict;
    out.resize(bands.size());
    if (tp)
        tp->update(0.0);
    for (size_t i = 0; i < bands.size(); i++)
    {
        if (!readBandRaw((int)i + 1, out[i]))
            return false;
        if (tp)
            tp->update(0.5 * (double)(i + 1) / (double)bands.size());
    }
    return true;
}
