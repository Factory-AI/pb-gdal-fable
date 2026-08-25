#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "gtiff_write.h"
#include "ogr.h"
#include "progress.h"
#include "srs.h"
#include "tiff.h"
#include "util.h"
#include "vrt.h"
#include "vsi.h"
#include "xml_min.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <strings.h>
#include <memory>
#include <string>
#include <vector>

// CPLStrtod-flavored nodata number: leading spaces, Windows special
// spellings, no hexadecimal; the empty string parses as 0
bool editNodataParse(const std::string &s, double &v)
{
    size_t i = 0;
    while (i < s.size() && s[i] == ' ')
        i++;
    static const std::pair<const char *, double> kSpecials[] = {
        {"-1.#QNAN", std::numeric_limits<double>::quiet_NaN()},
        {"-1.#IND", std::numeric_limits<double>::quiet_NaN()},
        {"1.#QNAN", std::numeric_limits<double>::quiet_NaN()},
        {"1.#SNAN", std::numeric_limits<double>::quiet_NaN()},
        {"1.#INF", std::numeric_limits<double>::infinity()},
        {"-1.#INF", -std::numeric_limits<double>::infinity()}};
    for (const auto &sp : kSpecials)
    {
        size_t n = strlen(sp.first);
        if (s.size() - i >= n &&
            strncasecmp(s.c_str() + i, sp.first, n) == 0)
        {
            if (i + n != s.size())
                return false;
            v = sp.second;
            return true;
        }
    }
    if (numLooksHex(s))
        return false;
    char *end = nullptr;
    v = strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

namespace
{

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

std::string baseNm(const std::string &p)
{
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
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

void storeStats(RasterDatasetBase &ds, Band &b, const StatsResult &r)
{
    auto set = [&](const char *k, const std::string &v) {
        if (ds.driverShort == "GTiff")
            cplDebug("GTIFF", "GTiffRasterBand::SetMetadataItem() goes to "
                              "PAM instead of TIFF tags");
        b.setMd("", k, v);
        pamSetBandItem(ds, b.index, k, v);
    };
    if (r.ok)
    {
        if (r.subsampled)
            set("STATISTICS_APPROXIMATE", "YES");
        else
        {
            b.removeMd("", "STATISTICS_APPROXIMATE");
            pamRemoveBandItem(ds, b.index, "STATISTICS_APPROXIMATE");
        }
        set("STATISTICS_MINIMUM", strPrintf("%.14g", r.mn));
        set("STATISTICS_MAXIMUM", strPrintf("%.14g", r.mx));
        set("STATISTICS_MEAN", strPrintf("%.14g", r.mean));
        set("STATISTICS_STDDEV", strPrintf("%.14g", r.stddev));
    }
    set("STATISTICS_VALID_PERCENT", strPrintf("%.4g", r.validPct));
    ds.pamDirty = true;
}

const char *kInterpNames[] = {
    "Undefined", "Gray",       "Palette",   "Red",     "Green",
    "Blue",      "Alpha",      "Hue",       "Saturation", "Lightness",
    "Cyan",      "Magenta",    "Yellow",    "Black",   "YCbCr_Y",
    "YCbCr_Cb",  "YCbCr_Cr",   "Pan",       "Coastal", "RedEdge",
    "NIR",       "SWIR",       "MWIR",      "LWIR",    "TIR",
    "OtherIR",   "SAR_Ka",     "SAR_K",     "SAR_Ku",  "SAR_X",
    "SAR_C",     "SAR_S",      "SAR_L",     "SAR_P"};

const char *canonicalInterp(const std::string &v)
{
    for (const char *n : kInterpNames)
        if (strEqualNoCase(v, n))
            return n;
    return nullptr;
}

bool parseFullDouble(const std::string &s, double &v)
{
    if (s.empty())
        return false;
    char *end = nullptr;
    v = strtod(s.c_str(), &end);
    return end && *end == '\0';
}

struct BandValue
{
    int band = -1;  // 0-based; -1 = all bands
    double value = 0;
    std::string bandStr, orig;
};

// values validated by cplValueType still parse with plain strtod,
// so a d/D exponent contributes nothing ("1d5" reads as 1)
double numParsePrefix(const std::string &s)
{
    return strtod(s.c_str(), nullptr);
}

// entries look like <value> or <band>=<value>; the band form only
// engages when the prefix reads as an integer, otherwise the whole
// entry must read as a number; band bounds are checked after open
bool parseBandValues(const std::vector<std::string> &in, const char *argName,
                     std::vector<BandValue> &out)
{
    for (const auto &e : in)
    {
        BandValue bv;
        bv.orig = e;
        size_t eq = e.find('=');
        std::string valPart = e;
        if (eq != std::string::npos &&
            cplValueType(e.substr(0, eq)) == 1)
        {
            bv.bandStr = e.substr(0, eq);
            valPart = e.substr(eq + 1);
            bv.band = 0;  // resolved after open
        }
        if (cplValueType(valPart) == 0)
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        strPrintf("edit: Invalid value '%s' for '%s'",
                                  e.c_str(), argName));
            handlerPrintUsage();
            return false;
        }
        bv.value = numParsePrefix(valPart);
        out.push_back(bv);
    }
    return true;
}

struct Gcp
{
    double pixel = 0, line = 0, x = 0, y = 0, z = 0;
};

std::vector<std::string> splitCsv(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos)
        {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool parseGcps(const std::vector<std::string> &in, std::vector<Gcp> &out,
               bool report)
{
    for (const auto &e : in)
    {
        // a lone @file entry resolves against a vector dataset after
        // open; any other combination is validated literally
        if (in.size() == 1 && e.size() > 1 && e[0] == '@')
            continue;
        std::vector<std::string> parts = splitCsv(e, ',');
        Gcp g;
        bool ok = parts.size() == 4 || parts.size() == 5;
        for (size_t pi = 0; ok && pi < parts.size(); ++pi)
            if (cplValueType(parts[pi]) == 0)
                ok = false;
        if (ok)
        {
            g.pixel = numParsePrefix(parts[0]);
            g.line = numParsePrefix(parts[1]);
            g.x = numParsePrefix(parts[2]);
            g.y = numParsePrefix(parts[3]);
            if (parts.size() == 5)
                g.z = numParsePrefix(parts[4]);
        }
        if (!ok)
        {
            if (report)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "edit: Bad format for " + e);
                handlerPrintUsage();
            }
            return false;
        }
        out.push_back(g);
    }
    return true;
}

double gcpFieldNum(const OgrFeature &f, int idx)
{
    if (idx < 0 || idx >= (int)f.values.size() || !f.values[idx].set)
        return 0;
    const JVal &v = f.values[idx].v;
    switch (v.type)
    {
        case JVal::INT:
            return (double)v.i;
        case JVal::DOUBLE:
            return v.d;
        case JVal::BOOL:
            return v.b ? 1 : 0;
        case JVal::STRING:
            return strtod(v.s.c_str(), nullptr);
        default:
            return 0;
    }
}

std::string gcpFieldStr(const OgrFeature &f, int idx)
{
    if (idx < 0 || idx >= (int)f.values.size() || !f.values[idx].set)
        return "";
    const JVal &v = f.values[idx].v;
    switch (v.type)
    {
        case JVal::STRING:
            return v.s;
        case JVal::INT:
            return strPrintf("%lld", v.i);
        case JVal::DOUBLE:
            return strPrintf("%.15g", v.d);
        case JVal::BOOL:
            return v.b ? "1" : "0";
        default:
            return "";
    }
}

// literal entries were validated up front; a lone @file entry opens a
// vector dataset whose first layer must carry column/line/x/y fields
// (case-insensitive), with optional z/id/info; errors print without usage
bool resolveGcpEntries(const std::vector<std::string> &in,
                       std::vector<GcpEntry> &out)
{
    for (const auto &e : in)
    {
        if (in.size() == 1 && e.size() > 1 && e[0] == '@')
        {
            std::string fn = e.substr(1);
            std::string err;
            std::unique_ptr<OgrDataset> vds =
                openVectorDataset(fn, err, {});
            if (!vds)
            {
                if (err == "missing" || !vsiExists(fn))
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(fn));
                else if (err != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + fn +
                                    "' not recognized as being in a "
                                    "supported file format.");
                return false;
            }
            const OgrLayer *lyr =
                vds->layers.empty() ? nullptr : &vds->layers[0];
            auto fieldIdx = [&](const char *n) -> int {
                if (!lyr)
                    return -1;
                for (size_t i = 0; i < lyr->fields.size(); ++i)
                    if (strEqualNoCase(lyr->fields[i].name, n))
                        return (int)i;
                return -1;
            };
            int iCol = fieldIdx("column"), iLine = fieldIdx("line");
            int iX = fieldIdx("x"), iY = fieldIdx("y");
            int iZ = fieldIdx("z"), iId = fieldIdx("id");
            int iInfo = fieldIdx("info");
            const char *missingField = iCol < 0    ? "column"
                                       : iLine < 0 ? "line"
                                       : iX < 0    ? "x"
                                       : iY < 0    ? "y"
                                                   : nullptr;
            if (missingField)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("edit: Field '%s' cannot be found "
                                      "in '%s'",
                                      missingField, fn.c_str()));
                return false;
            }
            for (const auto &f : lyr->features)
            {
                GcpEntry g;
                g.id = gcpFieldStr(f, iId);
                g.info = gcpFieldStr(f, iInfo);
                g.pixel = gcpFieldNum(f, iCol);
                g.line = gcpFieldNum(f, iLine);
                g.x = gcpFieldNum(f, iX);
                g.y = gcpFieldNum(f, iY);
                g.z = gcpFieldNum(f, iZ);
                out.push_back(std::move(g));
            }
            continue;
        }
        std::vector<std::string> parts = splitCsv(e, ',');
        if (parts.size() != 4 && parts.size() != 5)
            continue;
        GcpEntry g;
        g.pixel = numParsePrefix(parts[0]);
        g.line = numParsePrefix(parts[1]);
        g.x = numParsePrefix(parts[2]);
        g.y = numParsePrefix(parts[3]);
        if (parts.size() == 5)
            g.z = numParsePrefix(parts[4]);
        out.push_back(std::move(g));
    }
    return true;
}

// canonical color-interpretation name per band ("" = untouched); false
// after reporting. Entries tokenize on '=' with empty tokens dropped:
// exactly ["all", X] is the all form, any other two-token entry is the
// band form (atoi band), everything else validates whole as a color
// name. A lone "all="-prefixed value bypasses the single-entry check.
// Entries stream: on a mid-list failure ciTargets keeps the entries
// already applied, and those mutations stick.
bool resolveCiTargets(const std::vector<std::string> &ciList, int nBands,
                      std::vector<std::string> &ciTargets,
                      const std::vector<std::string> *curCi = nullptr,
                      std::vector<bool> *assigned = nullptr)
{
    if (ciList.empty())
        return true;
    ciTargets.assign(nBands, "");
    // a transient value differing from the running current dirties the
    // band even when a later entry restores it
    std::vector<std::string> cur;
    if (curCi)
        cur = *curCi;
    if (assigned)
        assigned->assign(nBands, false);
    auto setBand = [&](int b, const std::string &c) {
        ciTargets[b] = c;
        if (curCi && cur[b] != c)
        {
            cur[b] = c;
            if (assigned)
                (*assigned)[b] = true;
        }
    };
    auto tokenize = [](const std::string &s) {
        std::vector<std::string> t;
        for (const auto &p : strSplit(s, '='))
            if (!p.empty())
                t.push_back(p);
        return t;
    };
    auto unsupported = [](const std::string &v) {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "edit: Unsupported color interpretation: " + v);
    };
    if (ciList.size() == 1 && strStartsWith(ciList[0], "all="))
    {
        std::vector<std::string> t = tokenize(ciList[0]);
        bool allForm = t.size() == 2 && t[0] == "all";
        const std::string &v = allForm ? t[1] : ciList[0];
        const char *c = canonicalInterp(v);
        if (!c)
        {
            unsupported(v);
            return false;
        }
        for (int b = 0; b < nBands; b++)
            setBand(b, c);
        return true;
    }
    if (nBands > 1 && ciList.size() == 1)
    {
        cplErrorStr(
            CE_Failure, CPLE_NotSupported,
            "edit: With several bands, specify as many color "
            "interpretation as bands, one or many values of the form "
            "<band_number>=<color> or a single value all=<color>");
        return false;
    }
    // entries stream in order like scale/offset: plain entries take
    // consecutive bands via their own counter (overflowing with the
    // "More values" error, running short with the "Less values" one),
    // band and all forms apply as reached; a kind mix only errors after
    // every entry landed, keeping the mutations
    std::set<int> kinds;
    int iPlain = 0;
    for (const std::string &e : ciList)
    {
        std::vector<std::string> t = tokenize(e);
        int k = 3;
        if (t.size() == 2 && t[0] == "all")
            k = 1;
        else if (t.size() == 2)
            k = 2;
        kinds.insert(k);
        if (k == 1)
        {
            const char *c = canonicalInterp(t[1]);
            if (!c)
            {
                unsupported(t[1]);
                return false;
            }
            for (int b = 0; b < nBands; b++)
                setBand(b, c);
        }
        else if (k == 2)
        {
            int b = atoi(t[0].c_str());
            if (b < 1 || b > nBands)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            strPrintf("edit: Invalid band number '%s' in "
                                      "'%s'",
                                      t[0].c_str(), e.c_str()));
                return false;
            }
            const char *c = canonicalInterp(t[1]);
            if (!c)
            {
                unsupported(t[1]);
                return false;
            }
            setBand(b - 1, c);
        }
        else
        {
            if (iPlain >= nBands)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "edit: More color interpretation values "
                            "specified than bands in the dataset");
                return false;
            }
            const char *c = canonicalInterp(e);
            if (!c)
            {
                unsupported(e);
                return false;
            }
            setBand(iPlain++, c);
        }
    }
    if (kinds.size() > 1)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "edit: Mix of different syntaxes to specify color "
                    "interpretation");
        return false;
    }
    if (kinds.count(3) && iPlain < nBands)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "edit: Less color interpretation values specified "
                    "than bands in the dataset");
        return false;
    }
    return true;
}

// entries stream in order: a lone plain value broadcasts to all bands,
// otherwise plain entries take consecutive bands via their own counter
// and overflow with the "More values" error; band-form entries range
// check as reached. A kind mix only errors once every entry landed. On
// failure vals keeps the already-applied prefix and those stick.
bool resolveBandValues(std::vector<BandValue> &vals, int nBands,
                       const char *argName)
{
    if (vals.empty())
        return true;
    if (vals.size() == 1 && vals[0].bandStr.empty())
    {
        vals[0].band = -1;
        return true;
    }
    bool sawBand = false, sawPlain = false;
    int plainIdx = 0;
    for (size_t i = 0; i < vals.size(); i++)
    {
        BandValue &bv = vals[i];
        if (bv.bandStr.empty())
        {
            sawPlain = true;
            if (plainIdx >= nBands)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("edit: More %s values specified "
                                      "than bands in the dataset",
                                      argName));
                vals.resize(i);
                return false;
            }
            bv.band = plainIdx++;
            continue;
        }
        sawBand = true;
        long b = atol(bv.bandStr.c_str());
        if (b < 1 || b > nBands)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        strPrintf("edit: Invalid band number '%s' in "
                                  "'%s'",
                                  bv.bandStr.c_str(), bv.orig.c_str()));
            vals.resize(i);
            return false;
        }
        bv.band = (int)b - 1;
    }
    if (sawBand && sawPlain)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    strPrintf("edit: Mix of different syntaxes to "
                              "specify %s",
                              argName));
        return false;
    }
    return true;
}

int editPreValidator(const CmdSpec &, ParseResult &r)
{
    // per-argument checks fire in command-line order
    for (const auto &name : r.order)
    {
        if (name == "nodata")
        {
            std::string v = r.str("nodata");
            double d;
            if (v != "none" && v != "nan" && v != "inf" && v != "-inf" &&
                !editNodataParse(v, d))
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "edit: Value of 'nodata' should be 'none', a "
                            "numeric value, 'nan', 'inf' or '-inf'");
                handlerPrintUsage();
                return 1;
            }
        }
        else if (name == "crs")
        {
            std::string c = r.str("crs");
            if (c != "none" && c != "null")
            {
                bool ok = false;
                Srs s = Srs::fromCliInput(c, ok, true);
                if (!ok)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "edit: Invalid value for 'crs' argument");
                    handlerPrintUsage();
                    return 1;
                }
            }
        }
    }
    const ArgValue *bb = r.get("bbox");
    if (bb && bb->values.size() != 4)
    {
        long long cnt = (long long)bb->values.size();
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("edit: %lld value%s been specified for "
                              "argument 'bbox', whereas exactly 4 were "
                              "expected.",
                              cnt, cnt == 1 ? " has" : "s have"));
        handlerPrintUsage();
        return 1;
    }
    if (bb)
    {
        double x0 = atof(bb->values[0].c_str());
        double y0 = atof(bb->values[1].c_str());
        double x1 = atof(bb->values[2].c_str());
        double y1 = atof(bb->values[3].c_str());
        // NaN corners fail these comparisons too
        if (!(x0 <= x1) || !(y0 <= y1))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Value of 'bbox' should be xmin,ymin,xmax,ymax "
                        "with xmin <= xmax and ymin <= ymax");
            handlerPrintUsage();
            return 1;
        }
    }
    {
        std::vector<BandValue> tmp;
        if (!parseBandValues(r.list("scale"), "scale", tmp))
            return 1;
    }
    {
        std::vector<BandValue> tmp;
        if (!parseBandValues(r.list("offset"), "offset", tmp))
            return 1;
    }
    {
        std::vector<Gcp> tmp;
        if (!parseGcps(r.list("gcp"), tmp, true))
            return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------
// GDALMetadata is regenerated from the dataset model on flush, matching
// the driver: raw XML quirks (entity truncation, item order) do not
// round-trip
// ---------------------------------------------------------------------
std::string xesc1(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '&': r += "&amp;"; break;
            case '"': r += "&quot;"; break;
            default: r += c;
        }
    }
    return r;
}

std::string serializeGmd(const std::vector<GmdItem> &items)
{
    std::string xml = "<GDALMetadata>\n";
    for (const auto &it : items)
    {
        xml += "  <Item name=\"" + xesc1(it.name) + "\"";
        if (it.sample >= 0)
            xml += strPrintf(" sample=\"%d\"", it.sample);
        if (!it.role.empty())
            xml += " role=\"" + it.role + "\"";
        if (!it.domain.empty())
            xml += " domain=\"" + xesc1(it.domain) + "\"";
        xml += ">" + xesc1(xesc1(it.value)) + "</Item>\n";
    }
    xml += "</GDALMetadata>\n";
    return xml;
}

// ---------------------------------------------------------------------
// raw IFD model for the in-place rewrite
// ---------------------------------------------------------------------
struct RawTag
{
    uint16_t id = 0, type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> data;
};

size_t tiffTypeSize(uint16_t t)
{
    switch (t)
    {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12:
        case 16: case 17: case 18: return 8;
        default: return 0;
    }
}

bool parseRawIfd(const std::vector<uint8_t> &f, uint64_t off,
                 std::vector<RawTag> &out, uint64_t &next,
                 bool bt = false)
{
    const size_t entSize = bt ? 20 : 12;
    const size_t cntSize = bt ? 8 : 2;
    const size_t ptrSize = bt ? 8 : 4;
    const size_t inlineMax = bt ? 8 : 4;
    if (off + cntSize > f.size())
        return false;
    uint64_t n = 0;
    memcpy(&n, &f[off], cntSize);
    size_t entPos = off + cntSize;
    if (entPos + n * entSize + ptrSize > f.size())
        return false;
    for (uint64_t i = 0; i < n; i++)
    {
        const uint8_t *e = &f[entPos + (size_t)i * entSize];
        RawTag t;
        memcpy(&t.id, e, 2);
        memcpy(&t.type, e + 2, 2);
        uint64_t cnt = 0;
        const size_t entCntSize = bt ? 8 : 4;
        memcpy(&cnt, e + 4, entCntSize);
        t.count = (uint32_t)cnt;
        size_t sz = tiffTypeSize(t.type) * (size_t)cnt;
        if (sz == 0 || sz <= inlineMax)
        {
            size_t keep = sz ? sz : inlineMax;
            t.data.assign(e + 4 + entCntSize, e + 4 + entCntSize + keep);
        }
        else
        {
            uint64_t voff = 0;
            memcpy(&voff, e + 4 + entCntSize, ptrSize);
            if (voff + sz > f.size())
                return false;
            t.data.assign(f.begin() + voff, f.begin() + voff + sz);
        }
        out.push_back(std::move(t));
    }
    next = 0;
    memcpy(&next, &f[entPos + (size_t)n * entSize], ptrSize);
    return true;
}

RawTag mkAsciiTag(uint16_t id, const std::string &v)
{
    RawTag t;
    t.id = id;
    t.type = 2;
    t.count = (uint32_t)v.size() + 1;
    t.data.assign(v.begin(), v.end());
    t.data.push_back(0);
    return t;
}

RawTag mkDoublesTag(uint16_t id, const std::vector<double> &v)
{
    RawTag t;
    t.id = id;
    t.type = 12;
    t.count = (uint32_t)v.size();
    t.data.resize(v.size() * 8);
    memcpy(t.data.data(), v.data(), t.data.size());
    return t;
}

RawTag mkShortsTag(uint16_t id, const std::vector<uint16_t> &v)
{
    RawTag t;
    t.id = id;
    t.type = 3;
    t.count = (uint32_t)v.size();
    t.data.resize(v.size() * 2);
    memcpy(t.data.data(), v.data(), t.data.size());
    return t;
}

// tags libtiff stores in TIFFDirectory proper; everything else lives in
// the custom-field list and keeps its TIFFSetField (= read) order
bool isCustomTag(uint16_t id)
{
    static const uint16_t real[] = {
        254, 255, 256, 257, 258, 259, 262, 263, 266, 273, 274, 277,
        278, 279, 280, 281, 282, 283, 284, 286, 287, 296, 297, 301,
        317, 320, 321, 322, 323, 324, 325, 330, 332, 333, 334, 336,
        338, 339, 340, 341, 347, 529, 530, 531, 532};
    for (uint16_t r : real)
        if (r == id)
            return false;
    return true;
}

// TIFFWriteDirectorySec emission sequence for real fields
int stdRank(uint16_t id)
{
    static const uint16_t order[] = {
        254, 255, 256, 257, 282, 283, 286, 287, 258, 259, 262,
        263, 266, 274, 277, 278, 280, 281, 284, 296, 297, 322,
        323, 325, 279, 324, 273, 320, 317, 338, 339, 340, 341,
        321, 529, 530, 531, 532, 301, 333, 334, 336, 330, 347};
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++)
        if (order[i] == id)
            return (int)i;
    return 1000 + id;
}

struct IfdModel
{
    std::vector<RawTag> stdTags;   // kept sorted by id
    std::vector<RawTag> customs;   // libtiff custom-list order
    uint64_t next = 0;

    RawTag *findStd(uint16_t id)
    {
        for (auto &t : stdTags)
            if (t.id == id)
                return &t;
        return nullptr;
    }
    RawTag *findCustom(uint16_t id)
    {
        for (auto &t : customs)
            if (t.id == id)
                return &t;
        return nullptr;
    }
    void unsetCustom(uint16_t id)
    {
        customs.erase(std::remove_if(customs.begin(), customs.end(),
                                     [&](const RawTag &t)
                                     { return t.id == id; }),
                      customs.end());
    }
    // libtiff keeps the slot of an existing custom tag and appends new ones
    void setCustom(RawTag t)
    {
        for (auto &c : customs)
        {
            if (c.id == t.id)
            {
                c = std::move(t);
                return;
            }
        }
        customs.push_back(std::move(t));
    }
};

bool loadIfdModel(const TiffFile &tf, uint64_t ifdOff, IfdModel &m)
{
    std::vector<RawTag> all;
    if (!parseRawIfd(tf.data, ifdOff, all, m.next, tf.bigTiff))
        return false;
    for (auto &t : all)
    {
        if (isCustomTag(t.id))
            m.customs.push_back(std::move(t));
        else
            m.stdTags.push_back(std::move(t));
    }
    return true;
}

void put16v(std::vector<uint8_t> &b, uint16_t v)
{
    b.push_back((uint8_t)(v & 0xff));
    b.push_back((uint8_t)(v >> 8));
}

void put32v(std::vector<uint8_t> &b, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b.push_back((uint8_t)((v >> (8 * i)) & 0xff));
}

void put64v(std::vector<uint8_t> &b, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        b.push_back((uint8_t)((v >> (8 * i)) & 0xff));
}

bool rewriteIfd(const std::string &path, const TiffFile &tf, uint64_t ifdOff,
                IfdModel &m)
{
    std::string existing;
    if (!readFileToString(path, existing))
        return false;
    const bool bt = tf.bigTiff;
    const size_t entSize = bt ? 20 : 12;
    const size_t cntSize = bt ? 8 : 2;
    const size_t ptrSize = bt ? 8 : 4;
    const size_t inlineMax = bt ? 8 : 4;
    uint64_t base = (existing.size() + 1) & ~1ull;

    // data blocks laid out in the write order: standard tags in the
    // canonical libtiff sequence, then custom tags in list order
    std::vector<const RawTag *> writeOrder;
    {
        std::vector<const RawTag *> stds;
        for (const auto &t : m.stdTags)
            stds.push_back(&t);
        std::stable_sort(stds.begin(), stds.end(),
                         [](const RawTag *a, const RawTag *b)
                         { return stdRank(a->id) < stdRank(b->id); });
        writeOrder = stds;
        for (const auto &t : m.customs)
            writeOrder.push_back(&t);
    }
    size_t n = writeOrder.size();
    uint64_t cur = base + cntSize + n * entSize + ptrSize;
    std::map<const RawTag *, uint64_t> dataOff;
    for (const RawTag *t : writeOrder)
    {
        if (t->data.size() > inlineMax)
        {
            if (cur & 1)
                ++cur;
            dataOff[t] = cur;
            cur += t->data.size();
        }
    }

    std::vector<const RawTag *> dirOrder = writeOrder;
    std::stable_sort(dirOrder.begin(), dirOrder.end(),
                     [](const RawTag *a, const RawTag *b)
                     { return a->id < b->id; });

    std::vector<uint8_t> blob;
    if (bt)
        put64v(blob, n);
    else
        put16v(blob, (uint16_t)n);
    for (const RawTag *t : dirOrder)
    {
        put16v(blob, t->id);
        put16v(blob, t->type);
        if (bt)
            put64v(blob, t->count);
        else
            put32v(blob, t->count);
        if (t->data.size() > inlineMax)
        {
            if (bt)
                put64v(blob, dataOff[t]);
            else
                put32v(blob, (uint32_t)dataOff[t]);
        }
        else
        {
            for (size_t i = 0; i < inlineMax; i++)
                blob.push_back(i < t->data.size() ? t->data[i] : 0);
        }
    }
    if (bt)
        put64v(blob, m.next);
    else
        put32v(blob, (uint32_t)m.next);
    for (const RawTag *t : writeOrder)
    {
        if (t->data.size() <= inlineMax)
            continue;
        while ((base + blob.size()) & 1)
            blob.push_back(0);
        blob.insert(blob.end(), t->data.begin(), t->data.end());
    }

    // find the pointer that references this IFD (header or chain)
    size_t patchAt = 0;
    uint64_t p0 = 0;
    memcpy(&p0, existing.data() + (bt ? 8 : 4), ptrSize);
    if (p0 == ifdOff)
        patchAt = bt ? 8 : 4;
    else
    {
        uint64_t off = p0;
        while (off && off + cntSize <= existing.size())
        {
            uint64_t cnt = 0;
            memcpy(&cnt, existing.data() + off, cntSize);
            size_t nextPos = (size_t)(off + cntSize + cnt * entSize);
            if (nextPos + ptrSize > existing.size())
                break;
            uint64_t nxt = 0;
            memcpy(&nxt, existing.data() + nextPos, ptrSize);
            if (nxt == ifdOff)
            {
                patchAt = nextPos;
                break;
            }
            off = nxt;
        }
        if (!patchAt)
            return false;
    }
    existing.resize(base, '\0');
    memcpy(&existing[patchAt], &base, ptrSize);
    existing.append(reinterpret_cast<const char *>(blob.data()), blob.size());
    return writeStringToFile(path, existing);
}

std::string fmtNodata(double v)
{
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v > 0 ? "inf" : "-inf";
    return strPrintf("%.17g", v);
}

// ---------------------------------------------------------------------
int rasterEditHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string input = r.str("dataset");
    bool auxiliary = r.flag("auxiliary");
    bool quiet = r.flag("quiet");
    std::string crsStr = r.str("crs");
    bool crsSet = r.get("crs") != nullptr;
    bool crsNone = crsStr == "none" || crsStr == "null";
    const ArgValue *bbox = r.get("bbox");
    std::string nodataStr = r.str("nodata");
    bool nodataSet = r.get("nodata") != nullptr;
    std::vector<BandValue> scales, offsets;
    parseBandValues(r.list("scale"), "scale", scales);
    parseBandValues(r.list("offset"), "offset", offsets);
    std::vector<std::string> ciList = r.list("color-interpretation");
    bool doStats = r.flag("stats") || r.flag("approx-stats");
    bool approx = r.flag("approx-stats");
    bool doHist = r.flag("hist");

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
        return 1;
    }
    // rewriting counts the directory chain, surfacing loop and truncation
    // diagnostics; they appear only when the rewrite actually happens
    std::vector<std::pair<bool, std::string>> walkDiags;
    bool scannedPages = false;
    if (TiffFile *wtf = ds->tiffFile())
    {
        if (!wtf->bigTiff && !wtf->bigEndian && wtf->data.size() >= 8)
        {
            const std::vector<uint8_t> &d = wtf->data;
            auto rd16 = [&](uint64_t o) {
                uint16_t v;
                memcpy(&v, &d[o], 2);
                return v;
            };
            auto rd32 = [&](uint64_t o) {
                uint32_t v;
                memcpy(&v, &d[o], 4);
                return v;
            };
            uint64_t off = rd32(4);
            std::map<uint64_t, int> seen;
            int cur = 0;
            for (int guard = 0; off && guard < 100; guard++)
            {
                if (off + 2 > d.size())
                {
                    if (cur > 0)
                        walkDiags.emplace_back(
                            false, "TIFFAdvanceDirectory:" + input +
                                       ": Error fetching directory count");
                    break;
                }
                uint64_t n = rd16(off);
                uint64_t nextPos = off + 2 + n * 12;
                if (nextPos + 4 > d.size())
                    break;
                uint64_t next = rd32(nextPos);
                if (!next)
                    break;
                int dirn = cur + 1;
                auto it = seen.find(next);
                if (it != seen.end())
                {
                    walkDiags.emplace_back(
                        true,
                        strPrintf("_TIFFCheckDirNumberAndOffset:TIFF "
                                  "directory %d has IFD looping to "
                                  "directory %d at offset 0x%llx (%llu)",
                                  dirn - 1, it->second,
                                  (unsigned long long)next,
                                  (unsigned long long)next));
                    walkDiags.emplace_back(
                        true,
                        strPrintf("TIFFAdvanceDirectory:the next directory "
                                  "%d at offset 0x%llx (%llu) might be an "
                                  "IFD loop. Treating directory %d as last "
                                  "directory",
                                  dirn, (unsigned long long)next,
                                  (unsigned long long)next, dirn - 1));
                    break;
                }
                seen[next] = dirn;
                off = next;
                cur++;
            }
        }
    }
    int nBands = (int)ds->bands.size();

    double bboxVals[4] = {0, 0, 0, 0};
    if (bbox)
        for (int i = 0; i < 4; i++)
            bboxVals[i] = atof(bbox->values[i].c_str());

    double newNodata = 0;
    bool nodataNone = nodataStr == "none";
    // an empty value validated fine but applies nothing
    if (nodataSet && nodataStr.empty())
        nodataSet = false;
    if (nodataSet && !nodataNone)
    {
        if (nodataStr == "nan")
            newNodata = std::nan("");
        else if (nodataStr == "inf")
            newNodata = INFINITY;
        else if (nodataStr == "-inf")
            newNodata = -INFINITY;
        else
            editNodataParse(nodataStr, newNodata);
    }

    Srs newSrs;
    if (crsSet && !crsNone)
    {
        bool ok = false;
        // the deprecation-swap warning already fired at validation
        cplPushQuietHandler();
        newSrs = Srs::fromCliInput(crsStr, ok, false);
        cplPopHandler();
    }

    std::vector<std::pair<std::string, std::string>> metaSets;
    for (const auto &kv : r.list("metadata"))
    {
        size_t eq = kv.find('=');
        if (eq == std::string::npos)
            metaSets.emplace_back(kv, "");
        else
            metaSets.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
    std::vector<std::string> metaUnsets = r.list("unset-metadata");
    std::vector<std::string> mdDomainUnsets =
        r.list("unset-metadata-domain");

    // georeferencing edits rewrite the geokeys, which decodes the
    // existing SRS up front
    if (crsSet || bbox || !r.list("gcp").empty())
        ds->replaySrsDecodeWarnings();

    VrtDataset *vds = dynamic_cast<VrtDataset *>(ds.get());
    bool nodataChanged = false;
    if (nodataSet && !nodataNone)
    {
        bool has = nBands > 0 && ds->bands[0].hasNodata;
        double old = has ? ds->bands[0].nodata : 0;
        bool same = has && ((std::isnan(old) && std::isnan(newNodata)) ||
                            old == newNodata);
        // int64-flavored nodata never compares equal to the incoming
        // double, so the reference always rewrites (the equal-value
        // multiband warning stays suppressed)
        nodataChanged = !same || ds->bands[0].nodataIsI64 ||
                        ds->bands[0].nodataIsU64;
        if (!same && nBands > 1 && has && !vds)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        baseNm(input) +
                            strPrintf(", band 1: Setting nodata to %s on "
                                      "band 1, but band 2 has nodata at %s. "
                                      "The TIFFTAG_GDAL_NODATA only support "
                                      "one value per dataset. This value of "
                                      "%s will be used for all bands on "
                                      "re-opening",
                                      fmtNodata(newNodata).c_str(),
                                      fmtNodata(old).c_str(),
                                      fmtNodata(newNodata).c_str()));
    }

    // ------------------------------------------------------------------
    // post-open argument resolution; mutations stream in the reference
    // order (crs, bbox, nodata, color-interp, scale, offset, metadata,
    // gcp, unsets, stats) and a failure keeps everything already
    // applied while dropping all later operations
    // ------------------------------------------------------------------
    std::vector<std::string> ciTargets;  // canonical name per band, "" = none
    std::vector<std::string> curCi;
    for (const auto &band : ds->bands)
        curCi.push_back(band.colorInterp);
    std::vector<bool> ciAssigned;
    bool ciFail =
        !resolveCiTargets(ciList, nBands, ciTargets, &curCi, &ciAssigned);
    bool soFail = false;
    if (ciFail)
    {
        scales.clear();
        offsets.clear();
    }
    else if (!resolveBandValues(scales, nBands, "scale"))
    {
        soFail = true;
        offsets.clear();
    }
    else if (!resolveBandValues(offsets, nBands, "offset"))
        soFail = true;
    bool editFail = ciFail || soFail;
    if (editFail)
        metaSets.clear();

    // per-band scale/offset targets
    std::vector<bool> touched(nBands, false);
    std::vector<double> newScale(nBands, 1.0), newOffset(nBands, 0.0);
    std::vector<bool> hasNewScale(nBands, false), hasNewOffset(nBands, false);
    for (const auto &bv : scales)
    {
        for (int b = 0; b < nBands; b++)
        {
            if (bv.band >= 0 && bv.band != b)
                continue;
            newScale[b] = bv.value;
            hasNewScale[b] = true;
            touched[b] = true;
        }
    }
    for (const auto &bv : offsets)
    {
        for (int b = 0; b < nBands; b++)
        {
            if (bv.band >= 0 && bv.band != b)
                continue;
            newOffset[b] = bv.value;
            hasNewOffset[b] = true;
            touched[b] = true;
        }
    }
    for (int b = 0; b < nBands; b++)
    {
        if (!touched[b])
            continue;
        if (!hasNewScale[b])
            newScale[b] = ds->bands[b].hasScale ? ds->bands[b].scale : 1.0;
        if (!hasNewOffset[b])
            newOffset[b] = ds->bands[b].hasOffset ? ds->bands[b].offset : 0.0;
    }

    // GCP resolution follows the mutation order: earlier edits stay
    // applied when a @file fails, later ones (metadata unsets, stats)
    // are dropped with the run failing
    std::vector<GcpEntry> gcps;
    bool gcpFail = false;
    if (editFail)
    {
        metaUnsets.clear();
        mdDomainUnsets.clear();
        doStats = doHist = false;
    }
    else if (!resolveGcpEntries(r.list("gcp"), gcps))
    {
        gcpFail = true;
        gcps.clear();
        metaUnsets.clear();
        mdDomainUnsets.clear();
        doStats = doHist = false;
    }

    // ------------------------------------------------------------------
    // VRT mode: edits mutate the model, then the dataset flush rewrites
    // the .vrt itself; --auxiliary makes no difference
    // ------------------------------------------------------------------
    if (vds)
    {
        if (crsSet && gcps.empty())
        {
            if (crsNone)
                ds->hasSrs = false;
            else
            {
                ds->srs = std::move(newSrs);
                ds->hasSrs = true;
            }
            vds->axisMapping.clear();
        }
        if (bbox)
        {
            ds->gt[0] = bboxVals[0];
            ds->gt[1] = (bboxVals[2] - bboxVals[0]) / ds->width;
            ds->gt[2] = 0;
            ds->gt[3] = bboxVals[3];
            ds->gt[4] = 0;
            ds->gt[5] = -(bboxVals[3] - bboxVals[1]) / ds->height;
            ds->hasGT = true;
        }
        if (!gcps.empty())
        {
            // replaces any previous list; SRS and geotransform stay
            ds->gcps = gcps;
            ds->gcpMapping.clear();
            if (crsSet && !crsNone)
            {
                ds->gcpSrs = std::move(newSrs);
                ds->hasGcpSrs = true;
            }
            else
                ds->hasGcpSrs = false;
        }
        if (nodataSet)
            for (auto &band : ds->bands)
            {
                band.hasNodata = !nodataNone;
                if (!nodataNone)
                    band.nodata = newNodata;
                band.nodataIsI64 = false;
                band.nodataIsU64 = false;
            }
        for (int b = 0; b < nBands; b++)
        {
            if (!touched[b])
                continue;
            // stored only while non-default, as a pair
            bool keep = newOffset[b] != 0 || newScale[b] != 1;
            ds->bands[b].hasOffset = keep;
            ds->bands[b].hasScale = keep;
            ds->bands[b].offset = newOffset[b];
            ds->bands[b].scale = newScale[b];
        }
        for (int b = 0; b < nBands && b < (int)ciTargets.size(); b++)
            if (!ciTargets[b].empty())
                ds->bands[b].colorInterp = ciTargets[b];
        for (const auto &kv : metaSets)
            ds->setMd("", kv.first, kv.second);
        for (const auto &k : metaUnsets)
            ds->removeMd("", k);
        for (const auto &dom : mdDomainUnsets)
        {
            ds->metadata.erase(dom);
            ds->domainOrder.erase(std::remove(ds->domainOrder.begin(),
                                              ds->domainOrder.end(), dom),
                                  ds->domainOrder.end());
            ds->sortedDomains.erase(
                std::remove(ds->sortedDomains.begin(),
                            ds->sortedDomains.end(), dom),
                ds->sortedDomains.end());
        }

        int rc = editFail || gcpFail ? 1 : 0;
        if (doStats || doHist)
        {
            ds->replayDeferred();
            TermProgress tp;
            bool progress = !quiet;
            if (progress && doStats)
                tp.update(0.0);
            int total = nBands * ((doStats ? 1 : 0) + (doHist ? 1 : 0));
            int opIdx = 0;
            bool aborted = false;
            if (doStats)
                for (int b = 0; b < nBands && !aborted; b++)
                {
                    Band &band = ds->bands[b];
                    double s0 = (double)opIdx / total;
                    double s1 = (double)(opIdx + 1) / total;
                    StatsResult sr = vrtAwareForcedStats(
                        *ds, band, approx, progress ? &tp : nullptr, s0,
                        s1);
                    ++opIdx;
                    if (!sr.ok)
                    {
                        statsFailureError(*ds, band);
                        aborted = true;
                    }
                }
            if (doHist)
                for (int b = 0; b < nBands && !aborted; b++)
                {
                    Band &band = ds->bands[b];
                    double hEnd = (double)(opIdx + 1) / total;
                    ++opIdx;
                    HistItem h;
                    bool fresh = false;
                    if (vrtAwareHistogram(*ds, band, h, fresh))
                    {
                        if (progress && fresh)
                            tp.update(hEnd);
                    }
                    else
                        aborted = true;
                }
        }

        bool anyArg = crsSet || bbox || nodataSet || !scales.empty() ||
                      !offsets.empty() || !ciList.empty() ||
                      !metaSets.empty() || !metaUnsets.empty() ||
                      !mdDomainUnsets.empty() || !gcps.empty() || doStats ||
                      doHist;
        if (anyArg || ds->pamDirty)
            vds->persistPam();
        vds->flushSourcePams();
        return rc;
    }

    // ------------------------------------------------------------------
    // auxiliary (.aux.xml) mode
    // ------------------------------------------------------------------
    if (auxiliary)
    {
        if (crsSet)
        {
            if (crsNone)
            {
                ds->pamSrsRaw.clear();
            }
            else
            {
                ds->pamSrsRaw = newSrs.wkt1Gdal();
                std::string mapping;
                for (int mi : newSrs.dataAxisToSRSAxisMapping())
                {
                    if (!mapping.empty())
                        mapping += ",";
                    mapping += strPrintf("%d", mi);
                }
                ds->pamSrsMapping = mapping;
            }
            ds->pamDirty = true;
        }
        if (bbox)
        {
            double gt[6];
            gt[0] = bboxVals[0];
            gt[1] = (bboxVals[2] - bboxVals[0]) / ds->width;
            gt[2] = 0;
            gt[3] = bboxVals[3];
            gt[4] = 0;
            gt[5] = -(bboxVals[3] - bboxVals[1]) / ds->height;
            std::string raw;
            for (int i = 0; i < 6; i++)
                raw += std::string(i ? "," : "") + strPrintf("%24.16e", gt[i]);
            ds->pamGtRaw = raw;
            ds->pamDirty = true;
        }
        if (nodataSet && !nodataNone && nodataChanged)
        {
            ds->pamBands[1].nodataRaw = strPrintf("%.14E", newNodata);
            ds->pamDirty = true;
        }
        for (int b = 0; b < nBands; b++)
        {
            if (!touched[b])
                continue;
            // the aux entries store independently, each only while
            // non-default
            auto &pb = ds->pamBands[b + 1];
            bool w = false;
            if (newOffset[b] != 0)
            {
                pb.offsetRaw = strPrintf("%.16g", newOffset[b]);
                w = true;
            }
            else if (!pb.offsetRaw.empty())
            {
                pb.offsetRaw.clear();
                w = true;
            }
            if (newScale[b] != 1)
            {
                pb.scaleRaw = strPrintf("%.16g", newScale[b]);
                w = true;
            }
            else if (!pb.scaleRaw.empty())
            {
                pb.scaleRaw.clear();
                w = true;
            }
            if (w)
                ds->pamDirty = true;
        }
        for (const auto &kv : metaSets)
        {
            bool found = false;
            for (auto &e : ds->pamMdi)
            {
                if (e[0].empty() && e[1] == kv.first)
                {
                    e[2] = kv.second;
                    found = true;
                }
            }
            if (!found)
                ds->pamMdi.push_back({"", kv.first, kv.second});
            ds->pamDirty = true;
        }
        for (const auto &k : metaUnsets)
        {
            ds->pamMdi.erase(std::remove_if(ds->pamMdi.begin(),
                                            ds->pamMdi.end(),
                                            [&](const std::array<std::string,
                                                                 3> &e) {
                                                return e[0].empty() &&
                                                       e[1] == k;
                                            }),
                             ds->pamMdi.end());
        }
    }

    // ------------------------------------------------------------------
    // in-place GTiff mode
    // ------------------------------------------------------------------
    IfdModel m;
    bool dirty = false;
    TiffFile *tf = ds->tiffFile();
    bool gmdDirty = false;
    int photometric = 1;
    std::vector<uint16_t> extras;
    std::vector<uint16_t> gmdExtras;

    if (!auxiliary)
    {
        if (!tf || tf->bigEndian)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "edit: editing this dataset is not implemented in "
                        "this build");
            return 1;
        }
        if (!loadIfdModel(*tf, ds->tiffIfdOffset(), m))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "edit: cannot re-read the TIFF directory");
            return 1;
        }
        // an out-of-range ResolutionUnit is rejected by _TIFFVSetField, so
        // the field never lands in memory and a rewrite drops the tag
        if (RawTag *ru = m.findStd(296))
        {
            uint32_t v = 0;
            if (ru->data.size() >= 2)
                memcpy(&v, ru->data.data(), 2);
            if (v < 1 || v > 3)
                m.stdTags.erase(
                    std::remove_if(m.stdTags.begin(), m.stdTags.end(),
                                   [](const RawTag &t)
                                   { return t.id == 296; }),
                    m.stdTags.end());
        }
        // libtiff holds ASCII values as C strings: bytes past an embedded
        // NUL are lost and a terminator is always present on write
        {
            auto normAscii = [](RawTag &t) {
                if (t.type != 2)
                    return;
                size_t p = 0;
                while (p < t.data.size() && t.data[p])
                    p++;
                t.data.resize(p + 1);
                t.data[p] = 0;
                t.count = (uint32_t)t.data.size();
            };
            for (RawTag &t : m.stdTags)
                normAscii(t);
            for (RawTag &t : m.customs)
                normAscii(t);
        }
        // libtiff always carries PlanarConfig in memory, so a rewrite
        // materializes it even when the parsed directory lacked the tag
        if (!m.findStd(284))
        {
            RawTag *sortedIns = nullptr;
            (void)sortedIns;
            RawTag pc;
            pc.id = 284;
            pc.type = 3;
            pc.count = 1;
            pc.data = {1, 0};
            m.stdTags.push_back(pc);
        }
        // the reader exposes a lone uncompressed strip as default-sized
        // blocks; a rewrite materializes that virtual layout
        {
            auto tagInt = [](const RawTag &t, size_t idx) -> uint64_t {
                size_t ts = tiffTypeSize(t.type);
                if (ts == 2 && (idx + 1) * 2 <= t.data.size())
                {
                    uint16_t v;
                    memcpy(&v, &t.data[idx * 2], 2);
                    return v;
                }
                if (ts == 4 && (idx + 1) * 4 <= t.data.size())
                {
                    uint32_t v;
                    memcpy(&v, &t.data[idx * 4], 4);
                    return v;
                }
                return 0;
            };
            // resolution lives in memory as float; the rewrite re-encodes
            // that float exactly as a reduced dyadic rational
            auto reduceRational = [](RawTag *t) {
                if (!t || t->type != 5 || t->data.size() < 8)
                    return;
                uint32_t num, den;
                memcpy(&num, t->data.data(), 4);
                memcpy(&den, t->data.data() + 4, 4);
                if (den == 0)
                    return;
                float f = (float)((double)num / (double)den);
                double d = (double)f;
                uint64_t outNum;
                uint32_t outDen;
                if (d < 0 || d >= 4294967296.0)
                    return;
                if (d == floor(d))
                {
                    outNum = (uint64_t)d;
                    outDen = 1;
                }
                else
                {
                    int k = 0;
                    while (d != floor(d) && k < 31)
                    {
                        d *= 2;
                        k++;
                    }
                    if (d != floor(d) || d >= 4294967296.0)
                        return;
                    outNum = (uint64_t)d;
                    outDen = 1u << k;
                    while (!(outNum & 1) && outDen > 1)
                    {
                        outNum >>= 1;
                        outDen >>= 1;
                    }
                }
                uint32_t n32 = (uint32_t)outNum;
                memcpy(t->data.data(), &n32, 4);
                memcpy(t->data.data() + 4, &outDen, 4);
            };
            reduceRational(m.findStd(282));
            reduceRational(m.findStd(283));
            RawTag *xr = m.findStd(282);
            RawTag *yr = m.findStd(283);
            if ((xr != nullptr) != (yr != nullptr))
            {
                RawTag r;
                r.id = xr ? 283 : 282;
                r.type = 5;
                r.count = 1;
                r.data.assign(8, 0);
                r.data[4] = 1;
                m.stdTags.push_back(r);
            }
            RawTag *so = m.findStd(273);
            RawTag *sc = m.findStd(279);
            RawTag *rpsT = m.findStd(278);
            uint32_t comp = 1;
            if (RawTag *c = m.findStd(259))
                comp = (uint32_t)tagInt(*c, 0);
            bool tiled = m.findStd(322) != nullptr;
            if (RawTag *tc = m.findStd(325))
            {
                if (tc->type == 4 && tc->count > 1)
                {
                    std::vector<uint32_t> vals(tc->count);
                    memcpy(vals.data(), tc->data.data(),
                           std::min(tc->data.size(), vals.size() * 4));
                    uint32_t mx = 0;
                    for (uint32_t v : vals)
                        mx = std::max(mx, v);
                    if (mx <= 0xffff)
                    {
                        std::vector<uint16_t> v16(vals.begin(), vals.end());
                        tc->type = 3;
                        tc->data.resize(v16.size() * 2);
                        memcpy(tc->data.data(), v16.data(),
                               tc->data.size());
                    }
                }
            }
            if (!tiled && comp == 1 && so && !sc &&
                !ds->bands.empty() && ds->bands[0].blockY > 0)
            {
                RawTag nsc;
                nsc.id = 279;
                nsc.type = 4;
                nsc.count = 0;
                m.stdTags.push_back(nsc);
                sc = m.findStd(279);
                so = m.findStd(273);
                rpsT = m.findStd(278);
            }
            if (!tiled && !rpsT && so)
            {
                uint32_t spp0 = 1;
                if (RawTag *sp = m.findStd(277))
                    spp0 = (uint32_t)tagInt(*sp, 0);
                uint32_t bps0 = 8;
                if (RawTag *bp = m.findStd(258))
                    bps0 = (uint32_t)tagInt(*bp, 0);
                uint32_t planar0 = 1;
                if (RawTag *pl = m.findStd(284))
                    planar0 = (uint32_t)tagInt(*pl, 0);
                size_t scan = planar0 == 2
                                  ? ((size_t)ds->width * bps0 + 7) / 8
                                  : ((size_t)ds->width * spp0 * bps0 + 7) /
                                        8;
                uint16_t def = (uint16_t)std::max<size_t>(
                    1, scan ? 8192 / scan : 1);
                RawTag rp;
                rp.id = 278;
                rp.type = 3;
                rp.count = 1;
                rp.data.resize(2);
                memcpy(rp.data.data(), &def, 2);
                m.stdTags.push_back(rp);
                rpsT = m.findStd(278);
            }
            uint64_t rpsTag =
                rpsT ? tagInt(*rpsT, 0) : (uint64_t)ds->height;
            int blockY = nBands ? ds->bands[0].blockY : 0;
            if (!tiled && comp == 1 && so && so->count == 1 && sc &&
                rpsTag >= (uint64_t)ds->height && blockY > 0 &&
                blockY < ds->height)
            {
                uint32_t spp = 1;
                if (RawTag *sp = m.findStd(277))
                    spp = (uint32_t)tagInt(*sp, 0);
                uint32_t bps = 8;
                if (RawTag *bp = m.findStd(258))
                    bps = (uint32_t)tagInt(*bp, 0);
                size_t scanline = ((size_t)ds->width * spp * bps + 7) / 8;
                uint32_t base0 = (uint32_t)tagInt(*so, 0);
                int nChunks = (ds->height + blockY - 1) / blockY;
                std::vector<uint32_t> offs, cnts;
                for (int k = 0; k < nChunks; k++)
                {
                    int rows = std::min(blockY, ds->height - k * blockY);
                    offs.push_back(base0 +
                                   (uint32_t)(scanline * (size_t)blockY * k));
                    cnts.push_back((uint32_t)(scanline * (size_t)rows));
                }
                so->type = 4;
                so->count = (uint32_t)offs.size();
                so->data.resize(offs.size() * 4);
                memcpy(so->data.data(), offs.data(), so->data.size());
                uint32_t mx = 0;
                for (uint32_t c : cnts)
                    mx = std::max(mx, c);
                if (nChunks > 1 && mx <= 0xffff)
                {
                    std::vector<uint16_t> sc16(cnts.begin(), cnts.end());
                    sc->type = 3;
                    sc->count = (uint32_t)sc16.size();
                    sc->data.resize(sc16.size() * 2);
                    memcpy(sc->data.data(), sc16.data(), sc->data.size());
                }
                else
                {
                    sc->type = 4;
                    sc->count = (uint32_t)cnts.size();
                    sc->data.resize(cnts.size() * 4);
                    memcpy(sc->data.data(), cnts.data(), sc->data.size());
                }
                if (rpsT)
                {
                    uint16_t v = (uint16_t)blockY;
                    if (rpsT->type == 3)
                        memcpy(rpsT->data.data(), &v, 2);
                    else if (rpsT->type == 4)
                    {
                        uint32_t v32 = (uint32_t)blockY;
                        memcpy(rpsT->data.data(), &v32, 4);
                    }
                }
            }
            // no strip chop: the estimated byte counts land in the
            // rewritten directory as-is
            if (sc && sc->count == 0 && so && !tiled)
            {
                uint32_t spp = 1;
                if (RawTag *sp = m.findStd(277))
                    spp = (uint32_t)tagInt(*sp, 0);
                uint32_t bps = 8;
                if (RawTag *bp = m.findStd(258))
                    bps = (uint32_t)tagInt(*bp, 0);
                uint32_t planar = 1;
                if (RawTag *pl = m.findStd(284))
                    planar = (uint32_t)tagInt(*pl, 0);
                size_t scanline =
                    planar == 2
                        ? ((size_t)ds->width * bps + 7) / 8
                        : ((size_t)ds->width * spp * bps + 7) / 8;
                uint64_t rps =
                    rpsT ? tagInt(*rpsT, 0) : (uint64_t)ds->height;
                if (rps > (uint64_t)ds->height)
                    rps = (uint64_t)ds->height;
                int perPlane =
                    rps ? (int)((ds->height + rps - 1) / rps) : 1;
                std::vector<uint32_t> cnts;
                for (uint32_t k = 0; k < so->count; k++)
                {
                    int row = (int)((k % perPlane) * rps);
                    int rows =
                        (int)std::min<uint64_t>(rps, ds->height - row);
                    cnts.push_back((uint32_t)(scanline * (size_t)rows));
                }
                sc->type = 4;
                sc->count = (uint32_t)cnts.size();
                sc->data.resize(cnts.size() * 4);
                if (!cnts.empty())
                    memcpy(sc->data.data(), cnts.data(), sc->data.size());
            }
        }
        if (RawTag *p = m.findStd(262))
            if (p->data.size() >= 2)
                memcpy(&photometric, p->data.data(), 2);
        if (RawTag *x = m.findStd(338))
        {
            extras.resize(x->count);
            memcpy(extras.data(), x->data.data(),
                   std::min(x->data.size(), extras.size() * 2));
            // the libtiff mismatch fixup grows an existing too-small
            // array in memory (appending unspecified slots), and any
            // later rewrite persists the grown count; a fully missing
            // tag stays missing
            uint16_t spp = 1;
            if (RawTag *sp = m.findStd(277))
                if (sp->type == 3 && sp->data.size() >= 2)
                    memcpy(&spp, sp->data.data(), 2);
            int cc = 0;
            switch (photometric)
            {
                case 0: case 1: case 3: case 4:
                    cc = 1;
                    break;
                case 2: case 6: case 8: case 9: case 10:
                case 32845:
                    cc = 3;
                    break;
                case 5:
                    cc = 4;
                    break;
            }
            if (cc && spp > cc + extras.size())
            {
                extras.resize((size_t)(spp - cc), 0);
                *x = mkShortsTag(338, extras);
            }
        }
        gmdExtras = extras;

        // --- metadata edits (flushed before the geo tags)
        for (const auto &kv : metaSets)
        {
            ds->setMd("", kv.first, kv.second);
            gmdDirty = true;
        }
        for (const auto &k : metaUnsets)
        {
            ds->removeMd("", k);
            gmdDirty = true;
        }

        // --- color interpretation (a value equal to the current one is a
        // per-band no-op, so it neither dirties metadata nor rewrites)
        std::vector<bool> ciChanged(nBands, false);
        bool anyCiChanged = false;
        for (int b = 0; b < nBands && b < (int)ciTargets.size(); b++)
        {
            if (ciTargets[b].empty() || !ciAssigned[b])
                continue;
            // gray onto the palette synthesized for 1-bit data (no real
            // color table) is a no-op
            uint32_t ciBps = 8;
            if (RawTag *bt = m.findStd(258))
                if (bt->type == 3 && bt->data.size() >= 2)
                {
                    uint16_t v;
                    memcpy(&v, bt->data.data(), 2);
                    ciBps = v;
                }
            if (ciTargets[b] == "Gray" &&
                ds->bands[b].colorInterp == "Palette" && photometric <= 1 &&
                ciBps == 1 && !m.findStd(320))
                continue;
            ciChanged[b] = true;
            anyCiChanged = true;
        }
        if (!ciTargets.empty() && anyCiChanged)
        {
            // bands apply in order like GDAL's per-band
            // SetColorInterpretation: photometric decisions and the
            // extra-samples array see the interps applied so far
            int newPhot = photometric;
            std::vector<std::string> cur(nBands);
            for (int b = 0; b < nBands; b++)
                cur[b] = ds->bands[b].colorInterp;
            std::vector<uint16_t> oldExtras = extras;
            bool extrasResized = false;
            // leaving RGB grows the extra-samples array to cover every
            // band past the first, old values keeping their bands; a
            // photometric that was already MINISBLACK never grows it
            auto growExtras = [&]() {
                size_t newCount = (size_t)(nBands - 1);
                if (extras.size() >= newCount)
                    return;
                std::vector<uint16_t> ne(newCount, 0);
                for (size_t i = 0; i < extras.size(); i++)
                    ne[newCount - extras.size() + i] = extras[i];
                extras = std::move(ne);
                extrasResized = true;
            };
            static const char *rgbNames[] = {"Red", "Green", "Blue"};
            for (int b = 0; b < nBands && b < (int)ciTargets.size(); b++)
            {
                if (!ciChanged[b])
                    continue;
                const std::string &ci = ciTargets[b];
                cur[b] = ci;
                if (b == 0 && ci == "Gray")
                {
                    if (newPhot == 2)
                        growExtras();
                    newPhot = 1;
                    continue;
                }
                if (b == 0 && ci == "Palette" && m.findStd(320))
                {
                    newPhot = 3;
                    continue;
                }
                int nBase = nBands - (int)extras.size();
                if (ci == "Alpha" && !extras.empty())
                {
                    // an alpha request first flags every other band that
                    // already reads as alpha, then marks its own slot
                    // when it actually is an extra sample
                    for (int i = 0; i < nBands; i++)
                        if (i != b && cur[i] == "Alpha")
                            cplErrorStr(
                                CE_Warning, CPLE_AppDefined,
                                input +
                                    strPrintf(", band %d: Band %d was "
                                              "already identified as alpha "
                                              "band, and band %d is now "
                                              "marked as alpha too",
                                              b + 1, i + 1, b + 1));
                    if (b >= nBase)
                    {
                        extras[b - nBase] = 2;
                        continue;
                    }
                }
                if ((ci == "Gray" || ci == "Undefined") &&
                    !extras.empty() && b >= nBase)
                {
                    extras[b - nBase] = 0;
                    continue;
                }
                if (b < 3 && nBands >= 3 && ci == rgbNames[b] &&
                    cur[0] == "Red" && cur[1] == "Green" &&
                    cur[2] == "Blue")
                {
                    newPhot = 2;
                    continue;
                }
                if (newPhot == 2)
                {
                    growExtras();
                    newPhot = 1;
                    int slot = b - 1;
                    if (slot >= 0 && slot < (int)extras.size())
                        extras[slot] = ci == "Alpha" ? 2 : 0;
                }
                // photometric already MINISBLACK: colors only land in
                // the COLORINTERP metadata items
            }
            if (newPhot != photometric)
            {
                RawTag *p = m.findStd(262);
                if (p)
                {
                    uint16_t v = (uint16_t)newPhot;
                    memcpy(p->data.data(), &v, 2);
                }
                photometric = newPhot;
            }
            if (extrasResized)
            {
                if (RawTag *x = m.findStd(338))
                    *x = mkShortsTag(338, extras);
                else
                    m.stdTags.push_back(mkShortsTag(338, extras));
            }
            else if (RawTag *x = m.findStd(338))
                memcpy(x->data.data(), extras.data(),
                       std::min(x->data.size(), extras.size() * 2));
            gmdExtras = oldExtras;
            for (int b = 0; b < nBands; b++)
                if (ciChanged[b])
                    ds->bands[b].colorInterp = ciTargets[b];
            gmdDirty = true;
            dirty = true;
        }

        // --- scale/offset
        for (int b = 0; b < nBands; b++)
        {
            if (!touched[b])
                continue;
            // stored only while non-default, as a pair
            bool keep = newOffset[b] != 0 || newScale[b] != 1;
            ds->bands[b].hasOffset = keep;
            ds->bands[b].hasScale = keep;
            ds->bands[b].offset = newOffset[b];
            ds->bands[b].scale = newScale[b];
            gmdDirty = true;
        }
    }

    // ------------------------------------------------------------------
    // statistics / histogram (shared by both modes)
    // ------------------------------------------------------------------
    int rc = editFail || gcpFail ? 1 : 0;
    if (doStats || doHist)
    {
        bool floatBand = false;
        if (!ds->bands.empty())
            switch (ds->bands[0].type)
            {
                case DType::Float16:
                case DType::Float32:
                case DType::Float64:
                case DType::CFloat32:
                case DType::CFloat64:
                    floatBand = true;
                    break;
                default:
                    break;
            }
        // the pixel pass triggers the directory scan, unless a nodata
        // value lets the mask lookup skip it
        bool statScans =
            ds->driverShort == "GTiff" &&
            !(!ds->bands.empty() && ds->bands[0].hasNodata);
        if (statScans)
            cplDebug("GTiff", "ScanDirectories()");
        for (const auto &w : ds->deferredWarnings)
        {
            if (w.debug)
            {
                if (statScans)
                    cplDebug("GTiff", w.text);
                continue;
            }
            if (!statScans && !w.mainPage)
                continue;
            if (w.text.compare(0, 19, "TIFFFetchNormalTag:") == 0 &&
                (w.text.find("tag \"GDAL") != std::string::npos ||
                 w.text.find("tag \"GeoASCIIParams\"") !=
                     std::string::npos) &&
                !(floatBand &&
                  w.text.find("tag \"GDALNoDataValue\"") !=
                      std::string::npos))
                continue;
            cplErrorStr(w.warning ? CE_Warning : CE_Failure,
                        CPLE_AppDefined, w.text);
        }
        ds->deferredWarnings.clear();
        // the directory-chain scan at first band access registers extra
        // pages, which then land in the rebuilt GDALMetadata
        if (!auxiliary)
            scannedPages = true;
        TermProgress tp;
        bool progress = !quiet;
        if (progress && doStats)
            tp.update(0.0);
        // ops run phase-major: all stats first, then all histograms, one
        // shared progress split across nBands * nPhases spans
        int total = nBands * ((doStats ? 1 : 0) + (doHist ? 1 : 0));
        int opIdx = 0;
        bool aborted = false;
        if (doStats)
            for (int b = 0; b < nBands && !aborted; b++)
            {
                Band &band = ds->bands[b];
                if (!auxiliary)
                {
                    auto pbIt = ds->pamBands.find(band.index);
                    if (pbIt != ds->pamBands.end())
                    {
                        auto &mdi = pbIt->second.mdi;
                        size_t before = mdi.size();
                        mdi.erase(
                            std::remove_if(
                                mdi.begin(), mdi.end(),
                                [](const std::pair<std::string,
                                                   std::string> &kv) {
                                    return kv.first.compare(
                                               0, 11, "STATISTICS_") == 0;
                                }),
                            mdi.end());
                        if (mdi.size() != before)
                            ds->pamDirty = true;
                    }
                }
                StatsResult sr = computeBandStats(*ds, band, approx);
                if (auxiliary)
                    storeStats(*ds, band, sr);
                else
                {
                    if (sr.ok)
                    {
                        if (sr.subsampled)
                            band.setMd("", "STATISTICS_APPROXIMATE", "YES");
                        else
                            band.removeMd("", "STATISTICS_APPROXIMATE");
                        band.setMd("", "STATISTICS_MINIMUM",
                                   strPrintf("%.14g", sr.mn));
                        band.setMd("", "STATISTICS_MAXIMUM",
                                   strPrintf("%.14g", sr.mx));
                        band.setMd("", "STATISTICS_MEAN",
                                   strPrintf("%.14g", sr.mean));
                        band.setMd("", "STATISTICS_STDDEV",
                                   strPrintf("%.14g", sr.stddev));
                    }
                    band.setMd("", "STATISTICS_VALID_PERCENT",
                               strPrintf("%.4g", sr.validPct));
                    gmdDirty = true;
                }
                ++opIdx;
                if (!sr.ok)
                {
                    statsFailureError(*ds, band);
                    aborted = true;
                }
                if (progress)
                    tp.update((double)opIdx / total);
            }
        if (doHist)
            for (int b = 0; b < nBands && !aborted; b++)
            {
                Band &band = ds->bands[b];
                double hEnd = (double)(opIdx + 1) / total;
                ++opIdx;
                // a histogram already present in PAM is returned as-is:
                // no stats lookup, no compute, no store, no progress
                if (!band.pamHists.empty())
                    continue;
                double mn = 0, mx = 0;
                long long buckets = 256;
                bool specOk = true;
                if (band.type == DType::Byte)
                {
                    mn = -0.5;
                    mx = 255.5;
                }
                else
                {
                    double hmn = 0, hmx = 0;
                    bool cachedSpec = false;
                    {
                        auto g = [&](const char *k, double &v) {
                            const std::string *s = band.getMd("", k);
                            if (!s)
                                return false;
                            v = strtod(s->c_str(), nullptr);
                            return true;
                        };
                        double dMean = 0, dStd = 0;
                        cachedSpec = g("STATISTICS_MINIMUM", hmn) &&
                                     g("STATISTICS_MAXIMUM", hmx) &&
                                     g("STATISTICS_MEAN", dMean) &&
                                     g("STATISTICS_STDDEV", dStd);
                    }
                    if (!cachedSpec)
                    {
                        StatsResult hr = computeBandStats(*ds, band, true);
                        if (hr.ok)
                        {
                            hmn = hr.mn;
                            hmx = hr.mx;
                        }
                        else
                        {
                            statsFailureError(*ds, band);
                            specOk = false;
                            aborted = true;
                        }
                        if (auxiliary)
                            storeStats(*ds, band, hr);
                        else
                        {
                            if (hr.ok)
                            {
                                if (hr.subsampled)
                                    band.setMd("", "STATISTICS_APPROXIMATE",
                                               "YES");
                                else
                                    band.removeMd("",
                                                  "STATISTICS_APPROXIMATE");
                                band.setMd("", "STATISTICS_MINIMUM",
                                           strPrintf("%.14g", hr.mn));
                                band.setMd("", "STATISTICS_MAXIMUM",
                                           strPrintf("%.14g", hr.mx));
                                band.setMd("", "STATISTICS_MEAN",
                                           strPrintf("%.14g", hr.mean));
                                band.setMd("", "STATISTICS_STDDEV",
                                           strPrintf("%.14g", hr.stddev));
                            }
                            band.setMd("", "STATISTICS_VALID_PERCENT",
                                       strPrintf("%.4g", hr.validPct));
                            gmdDirty = true;
                        }
                    }
                    if (specOk)
                    {
                        if (hmn == hmx)
                        {
                            buckets = 1;
                            mn = hmn - 0.5;
                            mx = hmx + 0.5;
                        }
                        else
                        {
                            double hb = (hmx - hmn) / (2 * (buckets - 1));
                            mn = hmn - hb;
                            mx = hmx + hb;
                        }
                    }
                }
                if (specOk)
                {
                    HistItem h;
                    h.mn = mn;
                    h.mx = mx;
                    h.buckets = buckets;
                    if (computeHistogram(*ds, band, h))
                    {
                        band.pamHists.push_back(h);
                        ds->pamBands[band.index].hists.push_back(h);
                        ds->pamDirty = true;
                        if (progress)
                            tp.update(hEnd);
                    }
                }
            }
    }

    if (!auxiliary)
    {
        // --- GMD flush (metadata precedes the geo tag rewrite)
        if (gmdDirty)
        {
            std::vector<GmdItem> items =
                buildGmdItems(*ds, photometric, gmdExtras, false,
                              scannedPages ? 2 : 1);
            if (items.empty())
                m.unsetCustom(42112);
            else
                m.setCustom(mkAsciiTag(42112, serializeGmd(items)));
            dirty = true;
        }

        if (crsSet && gcps.empty() && !ds->pamSrsRaw.empty())
        {
            ds->pamSrsRaw.clear();
            ds->pamSrsMapping.clear();
            ds->pamDirty = true;
        }
        if (bbox && !ds->pamGtRaw.empty())
        {
            ds->pamGtRaw.clear();
            ds->pamDirty = true;
        }

        // --- GCPs / geotransform / CRS
        if (!gcps.empty())
        {
            if (ds->hasGT)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            baseNm(input) +
                                ": A geotransform previously set is going "
                                "to be cleared due to the setting of GCPs.");
            m.unsetCustom(33550);
            m.unsetCustom(33922);
            m.unsetCustom(34264);
            bool wantKeys = crsSet && !crsNone;
            bool hadKeys = m.findCustom(34735) != nullptr;
            const std::string *aop = ds->getMd("", "AREA_OR_POINT");
            bool pointPix = aop && *aop == "Point";
            if (!wantKeys)
            {
                m.unsetCustom(34735);
                m.unsetCustom(34736);
                m.unsetCustom(34737);
            }
            double gcpShift = pointPix ? 0.5 : 0.0;
            std::vector<double> tps;
            for (const auto &g : gcps)
            {
                tps.push_back(g.pixel + gcpShift);
                tps.push_back(g.line + gcpShift);
                tps.push_back(0.0);
                tps.push_back(g.x);
                tps.push_back(g.y);
                tps.push_back(g.z);
            }
            m.setCustom(mkDoublesTag(33922, tps));
            if (!wantKeys && pointPix)
                m.setCustom(mkShortsTag(34735, {1, 1, 0, 1, 1025, 0, 1, 2}));
            if (wantKeys)
            {
                GeoTags geo;
                buildGeoTags(newSrs, geo, pointPix);
                if (hadKeys)
                {
                    for (size_t i = 4; i + 3 < geo.dir.size(); i += 4)
                        if (geo.dir[i + 1] == 34736)
                            geo.dir[i + 3] += 1;
                    geo.doubles.insert(geo.doubles.begin(), 0.0);
                }
                m.setCustom(mkShortsTag(34735, geo.dir));
                if (!geo.doubles.empty())
                    m.setCustom(mkDoublesTag(34736, geo.doubles));
                if (!geo.ascii.empty())
                    m.setCustom(mkAsciiTag(34737, geo.ascii));
                else if (hadKeys)
                    m.setCustom(mkAsciiTag(34737, ""));
            }
            dirty = true;
        }
        else if (crsSet || bbox)
        {
            bool hadKeys = m.findCustom(34735) != nullptr;
            bool hadGeoTags = hadKeys || m.findCustom(33550) ||
                              m.findCustom(33922) || m.findCustom(34264) ||
                              m.findCustom(34736) || m.findCustom(34737);
            const std::string *aop = ds->getMd("", "AREA_OR_POINT");
            bool pointPix = aop && *aop == "Point";
            m.unsetCustom(33550);
            m.unsetCustom(33922);
            m.unsetCustom(34264);
            double gt[6];
            bool haveGt = false;
            if (bbox)
            {
                gt[0] = bboxVals[0];
                gt[1] = (bboxVals[2] - bboxVals[0]) / ds->width;
                gt[2] = 0;
                gt[3] = bboxVals[3];
                gt[4] = 0;
                gt[5] = -(bboxVals[3] - bboxVals[1]) / ds->height;
                haveGt = true;
            }
            else if (ds->hasGT)
            {
                memcpy(gt, ds->gt, sizeof(gt));
                haveGt = true;
            }
            if (haveGt)
            {
                if (pointPix)
                {
                    gt[0] += gt[1] * 0.5 + gt[2] * 0.5;
                    gt[3] += gt[4] * 0.5 + gt[5] * 0.5;
                }
                if (gt[2] == 0.0 && gt[4] == 0.0 && gt[5] < 0.0)
                {
                    m.setCustom(mkDoublesTag(33550, {gt[1], -gt[5], 0.0}));
                    m.setCustom(
                        mkDoublesTag(33922, {0, 0, 0, gt[0], gt[3], 0.0}));
                }
                else
                    m.setCustom(mkDoublesTag(
                        34264, {gt[1], gt[2], 0.0, gt[0], gt[4], gt[5], 0.0,
                                gt[3], 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                                1.0}));
            }
            if (crsSet)
            {
                if (crsNone)
                {
                    m.unsetCustom(34735);
                    m.unsetCustom(34736);
                    m.unsetCustom(34737);
                }
                else
                {
                    GeoTags geo;
                    buildGeoTags(newSrs, geo, pointPix);
                    if (hadKeys)
                    {
                        for (size_t i = 4; i + 3 < geo.dir.size(); i += 4)
                            if (geo.dir[i + 1] == 34736)
                                geo.dir[i + 3] += 1;
                        geo.doubles.insert(geo.doubles.begin(), 0.0);
                    }
                    m.setCustom(mkShortsTag(34735, geo.dir));
                    if (!geo.doubles.empty())
                        m.setCustom(mkDoublesTag(34736, geo.doubles));
                    if (!geo.ascii.empty())
                        m.setCustom(mkAsciiTag(34737, geo.ascii));
                    else if (hadKeys)
                        m.setCustom(mkAsciiTag(34737, ""));
                    if (geo.zScaleOne && hadKeys && ds->bands.size() == 1)
                        if (RawTag *ps = m.findCustom(33550))
                            if (ps->count == 3 && ps->data.size() >= 24)
                            {
                                double one = 1.0;
                                memcpy(ps->data.data() + 16, &one, 8);
                            }
                }
            }
            else if (haveGt)
            {
                // geotransform-only rewrite: a CRS adopted from PAM is
                // persisted into the file (fresh keys, no quirk); with an
                // internal CRS the key directory stays untouched unless a
                // metadata rewrite also happens, which re-encodes the keys
                // with the existing-keys double-params quirk; PixelIsPoint
                // without a CRS forces a minimal rastertype-only
                // directory with empty double/ascii params
                if (ds->hasSrs && !ds->srsSynthetic &&
                    (ds->srsFromPam || gmdDirty))
                {
                    GeoTags geo;
                    if (buildGeoTags(ds->srs, geo, pointPix))
                    {
                        if (!ds->srsFromPam && hadKeys)
                        {
                            for (size_t i = 4; i + 3 < geo.dir.size();
                                 i += 4)
                                if (geo.dir[i + 1] == 34736)
                                    geo.dir[i + 3] += 1;
                            geo.doubles.insert(geo.doubles.begin(), 0.0);
                        }
                        m.setCustom(mkShortsTag(34735, geo.dir));
                        if (!geo.doubles.empty())
                            m.setCustom(mkDoublesTag(34736, geo.doubles));
                        if (!geo.ascii.empty())
                            m.setCustom(mkAsciiTag(34737, geo.ascii));
                        else if (!ds->srsFromPam && hadKeys)
                            m.setCustom(mkAsciiTag(34737, ""));
                    }
                }
                else if (pointPix)
                {
                    m.setCustom(
                        mkShortsTag(34735, {1, 1, 0, 1, 1025, 0, 1, 2}));
                    m.setCustom(mkDoublesTag(34736, {0.0}));
                    m.setCustom(mkAsciiTag(34737, ""));
                }
            }
            // dropping a CRS from a file that never carried geo tags is
            // a no-op that must not rewrite the directory
            if (!crsNone || haveGt || hadGeoTags)
                dirty = true;
        }

        // --- nodata (skipped when the value is unchanged)
        if (nodataSet)
        {
            if (nodataNone)
            {
                // int64-flavored nodata is invisible to the double
                // deletion path, so 'none' leaves the tag in place
                bool i64Flavor = !ds->bands.empty() &&
                                 (ds->bands[0].nodataIsI64 ||
                                  ds->bands[0].nodataIsU64);
                if (!i64Flavor && m.findCustom(42113))
                {
                    m.unsetCustom(42113);
                    dirty = true;
                }
            }
            else if (nodataChanged)
            {
                m.setCustom(mkAsciiTag(42113, fmtNodata(newNodata)));
                dirty = true;
            }
        }

        if (dirty)
        {
            // libtiff keeps bits-per-sample as a scalar and always writes
            // one value per sample
            if (RawTag *bp = m.findStd(258))
            {
                uint16_t spp = 1;
                if (RawTag *sp = m.findStd(277))
                    if (sp->type == 3 && sp->data.size() >= 2)
                        memcpy(&spp, sp->data.data(), 2);
                if (bp->type == 3 && spp > 1 && bp->count < spp &&
                    bp->count >= 1)
                {
                    uint16_t v = 0;
                    memcpy(&v, bp->data.data(), 2);
                    std::vector<uint16_t> vals(spp, v);
                    bp->count = spp;
                    bp->data.resize(spp * 2);
                    memcpy(bp->data.data(), vals.data(), bp->data.size());
                }
            }
            for (const auto &w : walkDiags)
                cplErrorStr(w.first ? CE_Warning : CE_Failure,
                            CPLE_AppDefined, w.second);
            // the extras-mismatch diagnostic replays on the rewrite
            // re-read only while the edited directory still disagrees
            bool extrasStillOff = true;
            {
                uint16_t spp = 1;
                if (RawTag *sp = m.findStd(277))
                    if (sp->type == 3 && sp->data.size() >= 2)
                        memcpy(&spp, sp->data.data(), 2);
                uint64_t extraCount = 0;
                if (RawTag *x = m.findStd(338))
                    extraCount = x->count;
                int cc = 0;
                switch (photometric)
                {
                    case 0: case 1: case 3: case 4:
                        cc = 1;
                        break;
                    case 2: case 6: case 8: case 9: case 10:
                    case 32845:
                        cc = 3;
                        break;
                    case 5:
                        cc = 4;
                        break;
                    default:
                        cc = 0;
                }
                extrasStillOff = cc && spp > (uint64_t)cc + extraCount;
            }
            if (extrasStillOff)
                for (const auto &w : ds->rewriteWarnings)
                    cplErrorStr(CE_Warning, CPLE_AppDefined, w);
            if (!rewriteIfd(input, *tf, ds->tiffIfdOffset(), m))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "edit: rewrite of '" + input + "' failed");
                return 1;
            }
        }
    }

    if (ds->pamDirty)
    {
        if (!auxiliary)
            for (auto &bp : ds->pamBands)
                bp.second.mdiSorted = true;
        writePam(*ds);
    }
    debugCloseDataset(*ds);
    return rc;
}

}  // namespace

// ---------------------------------------------------------------------
// pipeline step: edits an in-memory VRT copy of the incoming dataset
// ---------------------------------------------------------------------

std::string editStepEcho(const PipeStepArgs &args)
{
    auto get = [&](const char *n) -> const std::vector<std::string> * {
        auto it = args.find(n);
        return it == args.end() || it->second.empty() ? nullptr
                                                      : &it->second;
    };
    // values quote on space/tab/quote/comma; packed lists rejoin with
    // commas into a single canonical token
    auto quote = [](const std::string &s) -> std::string {
        if (!s.empty() && s.find_first_of(" \t\",") == std::string::npos)
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
    };
    auto packedJoin = [&](const char *n) -> std::string {
        const auto *v = get(n);
        if (!v)
            return "";
        std::string joined;
        for (const auto &raw : *v)
            for (const auto &p : splitCsv(raw, ','))
            {
                if (!joined.empty())
                    joined += ",";
                joined += quote(p);
            }
        return joined;
    };
    std::string e = " ! edit";
    if (const auto *v = get("crs"))
        e += " --crs " + quote((*v)[0]);
    if (const auto *v = get("bbox"))
    {
        std::string joined;
        for (const auto &raw : *v)
            for (const auto &p : splitCsv(raw, ','))
            {
                if (!joined.empty())
                    joined += ",";
                joined += strPrintf("%.17g", atof(p.c_str()));
            }
        e += " --bbox " + joined;
    }
    if (const auto *v = get("nodata"))
        e += " --nodata " + quote((*v)[0]);
    std::string s;
    if (!(s = packedJoin("color-interpretation")).empty())
        e += " --color-interpretation " + s;
    if (!(s = packedJoin("scale")).empty())
        e += " --scale " + s;
    if (!(s = packedJoin("offset")).empty())
        e += " --offset " + s;
    if (const auto *v = get("metadata"))
        for (const auto &m : *v)
            e += " --metadata " + quote(m);
    if (!(s = packedJoin("unset-metadata")).empty())
        e += " --unset-metadata " + s;
    if (!(s = packedJoin("unset-metadata-domain")).empty())
        e += " --unset-metadata-domain " + s;
    if (const auto *v = get("gcp"))
        for (const auto &g : *v)
            e += " --gcp " + quote(g);
    return e;
}

int editApplyPipeStep(const PipeStepArgs &args,
                      std::unique_ptr<RasterDatasetBase> &ds)
{
    auto get = [&](const char *n) -> const std::vector<std::string> * {
        auto it = args.find(n);
        return it == args.end() || it->second.empty() ? nullptr
                                                      : &it->second;
    };
    auto packedList = [&](const char *n) {
        std::vector<std::string> out;
        if (const auto *v = get(n))
            for (const auto &raw : *v)
                for (const auto &p : splitCsv(raw, ','))
                    out.push_back(p);
        return out;
    };
    std::string crsStr;
    bool crsSet = false, crsNone = false;
    if (const auto *v = get("crs"))
    {
        crsSet = true;
        crsStr = (*v)[0];
        crsNone = crsStr == "none" || crsStr == "null";
    }
    std::vector<std::string> bboxParts = packedList("bbox");
    bool bboxSet = !bboxParts.empty();
    double bboxVals[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < 4 && i < bboxParts.size(); ++i)
        bboxVals[i] = atof(bboxParts[i].c_str());
    std::string nodataStr;
    bool nodataSet = false;
    if (const auto *v = get("nodata"))
    {
        nodataSet = true;
        nodataStr = (*v)[0];
    }
    std::vector<BandValue> scales, offsets;
    parseBandValues(packedList("scale"), "scale", scales);
    parseBandValues(packedList("offset"), "offset", offsets);
    std::vector<std::string> ciList = packedList("color-interpretation");
    std::vector<std::pair<std::string, std::string>> metaSets;
    if (const auto *v = get("metadata"))
        for (const auto &kv : *v)
        {
            size_t eq = kv.find('=');
            if (eq == std::string::npos)
                metaSets.emplace_back(kv, "");
            else
                metaSets.emplace_back(kv.substr(0, eq),
                                      kv.substr(eq + 1));
        }
    std::vector<std::string> metaUnsets = packedList("unset-metadata");
    std::vector<std::string> mdDomainUnsets =
        packedList("unset-metadata-domain");
    std::vector<std::string> gcpArgs;
    if (const auto *v = get("gcp"))
        gcpArgs = *v;

    std::string xml = vrtSerializeXml(*ds, ds->path, "");
    std::string err;
    OpenOptions oo;
    std::unique_ptr<RasterDatasetBase> copy =
        openVrtContent("", xml, err, oo);
    if (!copy)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "edit: cannot copy input dataset");
        return 1;
    }
    VrtDataset *cvds = dynamic_cast<VrtDataset *>(copy.get());
    // the XML round trip loses CRS object identity (datum ensembles) and
    // can round numeric band items; restore them from the source model
    copy->hasSrs = ds->hasSrs;
    if (ds->hasSrs && ds->srs.valid())
        copy->srs = ds->srs.clone();
    copy->hasGcpSrs = ds->hasGcpSrs;
    if (ds->hasGcpSrs && ds->gcpSrs.valid())
        copy->gcpSrs = ds->gcpSrs.clone();
    for (size_t i = 0; i < copy->bands.size() && i < ds->bands.size(); ++i)
    {
        Band &nb = copy->bands[i];
        const Band &ob = ds->bands[i];
        nb.hasNodata = ob.hasNodata;
        if (ob.hasNodata)
        {
            nb.nodata = ob.nodata;
            nb.nodataIsI64 = ob.nodataIsI64;
            nb.nodataIsU64 = ob.nodataIsU64;
        }
        if (ob.hasScale)
            nb.scale = ob.scale;
        if (ob.hasOffset)
            nb.offset = ob.offset;
    }

    int nBands = (int)copy->bands.size();
    std::vector<std::string> ciTargets;
    if (!resolveCiTargets(ciList, nBands, ciTargets))
        return 1;
    if (!resolveBandValues(scales, nBands, "scale") ||
        !resolveBandValues(offsets, nBands, "offset"))
        return 1;
    std::vector<GcpEntry> gcps;
    if (!resolveGcpEntries(gcpArgs, gcps))
        return 1;

    std::vector<bool> touched(nBands, false);
    std::vector<double> newScale(nBands, 1.0), newOffset(nBands, 0.0);
    std::vector<bool> hasNewScale(nBands, false),
        hasNewOffset(nBands, false);
    for (const auto &bv : scales)
    {
        for (int b = 0; b < nBands; b++)
        {
            if (bv.band >= 0 && bv.band != b)
                continue;
            newScale[b] = bv.value;
            hasNewScale[b] = true;
            touched[b] = true;
        }
    }
    for (const auto &bv : offsets)
    {
        for (int b = 0; b < nBands; b++)
        {
            if (bv.band >= 0 && bv.band != b)
                continue;
            newOffset[b] = bv.value;
            hasNewOffset[b] = true;
            touched[b] = true;
        }
    }
    for (int b = 0; b < nBands; b++)
    {
        if (!touched[b])
            continue;
        if (!hasNewScale[b])
            newScale[b] =
                copy->bands[b].hasScale ? copy->bands[b].scale : 1.0;
        if (!hasNewOffset[b])
            newOffset[b] =
                copy->bands[b].hasOffset ? copy->bands[b].offset : 0.0;
    }

    double newNodata = 0;
    bool nodataNone = nodataStr == "none";
    // an empty value validated fine but applies nothing
    if (nodataSet && nodataStr.empty())
        nodataSet = false;
    if (nodataSet && !nodataNone)
    {
        if (nodataStr == "nan")
            newNodata = std::nan("");
        else if (nodataStr == "inf")
            newNodata = INFINITY;
        else if (nodataStr == "-inf")
            newNodata = -INFINITY;
        else
            editNodataParse(nodataStr, newNodata);
    }

    Srs newSrs;
    if (crsSet && !crsNone)
    {
        bool ok = false;
        // the deprecation-swap warning already fired at parse time
        cplPushQuietHandler();
        newSrs = Srs::fromUserInput(crsStr, ok);
        cplPopHandler();
    }

    if (crsSet && gcps.empty())
    {
        if (crsNone)
            copy->hasSrs = false;
        else
        {
            copy->srs = std::move(newSrs);
            copy->hasSrs = true;
        }
        if (cvds)
            cvds->axisMapping.clear();
    }
    if (bboxSet)
    {
        copy->gt[0] = bboxVals[0];
        copy->gt[1] = (bboxVals[2] - bboxVals[0]) / copy->width;
        copy->gt[2] = 0;
        copy->gt[3] = bboxVals[3];
        copy->gt[4] = 0;
        copy->gt[5] = -(bboxVals[3] - bboxVals[1]) / copy->height;
        copy->hasGT = true;
    }
    if (!gcps.empty())
    {
        // replaces any previous list; SRS and geotransform stay
        copy->gcps = gcps;
        copy->gcpMapping.clear();
        if (crsSet && !crsNone)
        {
            copy->gcpSrs = std::move(newSrs);
            copy->hasGcpSrs = true;
        }
        else
            copy->hasGcpSrs = false;
    }
    if (nodataSet)
        for (auto &band : copy->bands)
        {
            band.hasNodata = !nodataNone;
            if (!nodataNone)
                band.nodata = newNodata;
            band.nodataIsI64 = false;
            band.nodataIsU64 = false;
        }
    for (int b = 0; b < nBands; b++)
    {
        if (!touched[b])
            continue;
        // stored only while non-default, as a pair
        bool keep = newOffset[b] != 0 || newScale[b] != 1;
        copy->bands[b].hasOffset = keep;
        copy->bands[b].hasScale = keep;
        copy->bands[b].offset = newOffset[b];
        copy->bands[b].scale = newScale[b];
    }
    for (int b = 0; b < nBands && b < (int)ciTargets.size(); b++)
        if (!ciTargets[b].empty())
            copy->bands[b].colorInterp = ciTargets[b];
    for (const auto &kv : metaSets)
        copy->setMd("", kv.first, kv.second);
    for (const auto &k : metaUnsets)
        copy->removeMd("", k);
    for (const auto &dom : mdDomainUnsets)
    {
        copy->metadata.erase(dom);
        copy->domainOrder.erase(std::remove(copy->domainOrder.begin(),
                                            copy->domainOrder.end(), dom),
                                copy->domainOrder.end());
        copy->sortedDomains.erase(
            std::remove(copy->sortedDomains.begin(),
                        copy->sortedDomains.end(), dom),
            copy->sortedDomains.end());
    }

    copy->inMemoryVrtCopy = true;
    copy->pamSuppressItems = true;
    ds = std::move(copy);
    return 0;
}

void registerRasterEditHandlers()
{
    registerHandler("raster_edit", rasterEditHandler);
    registerPreValidator("raster_edit", editPreValidator);
}
