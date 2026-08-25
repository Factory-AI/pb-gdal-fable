#include "cpl.h"
#include "recode.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "progress.h"
#include "proj_min.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

std::unique_ptr<OgrDataset> g_convertSourceOverride;
bool g_pipelineTransCapture = false;
std::unique_ptr<OgrDataset> g_pipelineTransCaptured;
bool g_transZWarnEnable = false;
bool g_convertLayerWriteFailed = false;
bool g_gjForceFidIds = false;
std::function<int(const OgrLayer &)> g_convertLayerGate;
std::function<void(const OgrLayer &, OgrFeature &)> g_convertFeatureHook;
std::function<int(OgrDataset &)> g_convertDatasetMutate;
std::function<int(OgrDataset &)> g_convertDatasetPreCheck;
int g_convertWritePulls = 1;
bool g_pipelineMutateSilentFail = false;
bool g_convertMutateBarOk = false;
ConvertClipPending g_convertClipPending;
void convertClipEmitLayerErrors(const ConvertClipPending::L &e)
{
    for (const auto &p : e.errors)
        cplErrorStr(CE_Failure,
                    p.first == CPLE_NotSupported ? CPLE_NotSupported
                                                 : CPLE_AppDefined,
                    p.second);
}
// per-feature replay of streamed-reader events so they interleave with
// the writer's progress ticks and warnings
static std::function<void(const OgrLayer &)> g_seqFeatureTick;
static void seqTick(const OgrLayer &lyr)
{
    if (g_seqFeatureTick)
        g_seqFeatureTick(lyr);
}
bool g_convertCaptureWritten = false;
std::unique_ptr<OgrDataset> g_convertWrittenDs;

std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver)
{
    if (format.empty())
        return "";
    // GDAL_SKIP unregisters the named driver: an explicit -f naming it
    // fails the generic name lookup ("Memory" resolves through the
    // deprecated-alias path and dodges the skip)
    static const char *kSkippable[] = {"GeoJSON",  "GeoJSONSeq",
                                       "ESRIJSON", "TopoJSON",
                                       "MEM",      "GTiff",
                                       "COG",      "VRT",
                                       "ESRI Shapefile"};
    for (const char *n : kSkippable)
        if (strEqualNoCase(format, n) && gdalSkipHas(n))
            return "Invalid value for argument 'output-format'. Driver '" +
                   format + "' does not exist.";
    if (strEqualNoCase(format, "GeoJSON"))
        driver = "GeoJSON";
    else if (strEqualNoCase(format, "GeoJSONSeq"))
        driver = "GeoJSONSeq";
    else if (strEqualNoCase(format, "ESRIJSON") ||
             strEqualNoCase(format, "TopoJSON"))
        return "Invalid value for argument 'output-format'. Driver '" +
               format + "' does not have write support.";
    else if (strEqualNoCase(format, "ESRI Shapefile"))
        driver = "ESRI Shapefile";
    else if (strEqualNoCase(format, "MEM") ||
             strEqualNoCase(format, "Memory"))
    {
        if (strEqualNoCase(format, "Memory"))
            memoryDriverDeprecationWarnOnce();
        driver = "MEM";
    }
    else if (strEqualNoCase(format, "GDALG"))
        driver = "GDALG";
    else if (strEqualNoCase(format, "stream"))
        driver = "stream";
    else if (strEqualNoCase(format, "GTiff") ||
             strEqualNoCase(format, "COG") || strEqualNoCase(format, "VRT"))
        return "Invalid value for argument 'output-format'. Driver '" +
               format + "' does not expose the required 'DCAP_VECTOR' "
                        "capability.";
    else
        return "Invalid value for argument 'output-format'. Driver '" +
               format + "' does not exist.";
    return "";
}

namespace
{

void incrementDecimalString(std::string &s)
{
    int i = (int)s.size() - 1;
    while (i >= 0)
    {
        if (s[i] == '.' || s[i] == '-')
        {
            --i;
            continue;
        }
        if (s[i] != '9')
        {
            s[i]++;
            return;
        }
        s[i] = '0';
        --i;
    }
    size_t at = s[0] == '-' ? 1 : 0;
    s.insert(at, "1");
}

// %.15f followed by roundoff-run trimming, derived from a 17.8k-sample
// differential fit. Maximal runs of '0'/'9' (len >= 2) in the decimals:
// scanning runs right-to-left while they touch the last three decimals
// (gap g = 15-runEnd <= 2), a run fires when g==0 && len>=6, g==1 &&
// len>=5, or g==2 && runStart<=7. Failing that, the leftmost run with
// runStart<=7 whose end reaches max(8, 17-intDigits) fires (that depth
// is where sub-ulp garbage begins, so the cut always round-trips).
// Zero runs truncate at runStart-1 decimals; nine runs also round up.
std::string fmtCoord(double v)
{
    if (std::isnan(v) || std::isinf(v) || fabs(v) > 1e50)
        return strPrintf("%.17g", v);
    char buf[512];
    snprintf(buf, sizeof(buf), "%.15f", v);
    std::string s = buf;
    size_t dot = s.find('.');
    struct Run
    {
        char c;
        int start;
        int len;
    };
    Run runs[8];
    int nRuns = 0;
    const char *dec = s.c_str() + dot + 1;
    for (int i = 0; i < 15;)
    {
        char c = dec[i];
        int j = i;
        while (j < 15 && dec[j] == c)
            ++j;
        if ((c == '0' || c == '9') && j - i >= 2 && nRuns < 8)
            runs[nRuns++] = {c, i + 1, j - i};
        i = j;
    }
    const int intd = static_cast<int>(dot) - (s[0] == '-' ? 1 : 0);
    char rc = 0;
    int rp = -1;
    for (int r = nRuns - 1; r >= 0; --r)
    {
        const int end = runs[r].start + runs[r].len - 1;
        const int g = 15 - end;
        if (g > 2)
            break;
        if ((g == 0 && runs[r].len >= 6) || (g == 1 && runs[r].len >= 5) ||
            (g == 2 && runs[r].start <= 7))
        {
            rc = runs[r].c;
            rp = runs[r].start - 1;
            break;
        }
    }
    if (rp < 0)
        for (int r = 0; r < nRuns; ++r)
        {
            const int end = runs[r].start + runs[r].len - 1;
            const int need = intd >= 9 ? 8 : 17 - intd;
            if (runs[r].start <= 7 && end >= need)
            {
                rc = runs[r].c;
                rp = runs[r].start - 1;
                break;
            }
        }
    if (rp >= 0)
    {
        s.erase(rp ? dot + 1 + rp : dot);
        if (rc == '9')
            incrementDecimalString(s);
    }
    dot = s.find('.');
    if (dot == std::string::npos)
        s += ".0";
    else
    {
        size_t lastNz = s.find_last_not_of('0');
        if (lastNz == dot)
            lastNz = dot + 1;
        if (lastNz + 1 < s.size())
            s.erase(lastNz + 1);
        if (s.size() == dot + 1)
            s += '0';
    }
    return s;
}

// json-c significant-figures serializer semantics: %.17g, and when the
// part from the decimal point on shows a "999999" or "000000" rounding
// artifact, retry with 16..14 significant digits and keep the first
// rendering that still has a decimal point but no artifact, else the
// full 17
std::string fmtPropDouble(double d)
{
    if (std::isnan(d))
        return "NaN";
    if (std::isinf(d))
        return d > 0 ? "Infinity" : "-Infinity";
    char buf[80];
    snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos &&
        (s.find("999999", dot) != std::string::npos ||
         s.find("000000", dot) != std::string::npos))
    {
        for (int i = 1; i <= 3; ++i)
        {
            snprintf(buf, sizeof(buf), "%.*g", 17 - i, d);
            std::string s2 = buf;
            size_t dot2 = s2.find('.');
            if (dot2 != std::string::npos &&
                s2.find("999999", dot2) == std::string::npos &&
                s2.find("000000", dot2) == std::string::npos)
            {
                s = s2;
                break;
            }
        }
    }
    if (s.find('.') == std::string::npos &&
        s.find_first_of("eE") == std::string::npos)
        s += ".0";
    return s;
}

// Float32 fields serialize with 8 significant figures, json-c style
std::string fmtPropFloat32(double d)
{
    if (std::isnan(d))
        return "NaN";
    if (std::isinf(d))
        return d > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.8g", d);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos)
        s += ".0";
    return s;
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20)
                    out += strPrintf("\\u%04x", c);
                else
                    out += static_cast<char>(c);
        }
    }
    return out;
}

void serializeCompact(std::string &out, const JVal &v)
{
    switch (v.type)
    {
        case JVal::NUL:
            out += "null";
            break;
        case JVal::BOOL:
            out += v.b ? "true" : "false";
            break;
        case JVal::INT:
            out += strPrintf("%lld", v.i);
            break;
        case JVal::DOUBLE:
            out += fmtPropDouble(v.d);
            break;
        case JVal::STRING:
            out += '"' + jsonEscape(v.s) + '"';
            break;
        case JVal::ARRAY:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i)
            {
                if (i)
                    out += ',';
                serializeCompact(out, v.arr[i]);
            }
            out += ']';
            break;
        case JVal::OBJECT:
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i)
            {
                if (i)
                    out += ',';
                out += '"' + jsonEscape(v.obj[i].first) + "\":";
                serializeCompact(out, v.obj[i].second);
            }
            out += '}';
            break;
    }
}

std::string isoDateTime(const OgrDateTime &dt, int fieldType)
{
    std::string s;
    if (fieldType == OFTDate)
        return strPrintf("%04d-%02d-%02d", dt.year, dt.month, dt.day);
    if (fieldType == OFTTime)
    {
        s = strPrintf("%02d:%02d:%02d", dt.hour, dt.minute, (int)dt.sec);
        double frac = dt.sec - (int)dt.sec;
        if (frac > 0)
            s += strPrintf(".%03d", (int)(frac * 1000 + 0.5));
        return s;
    }
    s = strPrintf("%04d-%02d-%02dT%02d:%02d:%02d", dt.year, dt.month,
                  dt.day, dt.hour, dt.minute, (int)dt.sec);
    double frac = dt.sec - (int)dt.sec;
    if (frac > 0)
        s += strPrintf(".%03d", (int)(frac * 1000 + 0.5));
    if (dt.tzFlag == 100)
        s += "Z";
    else if (dt.tzFlag > 1)
    {
        int off = (dt.tzFlag - 100) * 15;
        s += strPrintf("%c%02d:%02d", off < 0 ? '-' : '+', abs(off) / 60,
                       abs(off) % 60);
    }
    return s;
}

struct GeoJsonOpts
{
    bool bbox = false;
    bool rfc = false;
    bool skipErrors = false;
    bool writeName = true;
    int prec = -1;       // effective coordinate precision
    bool precSet = false;  // COORDINATE_PRECISION explicitly given
    // unknown-feature-count pipelines render the whole bar after the
    // last feature, before the document footer
    bool barAtEnd = false;
};

int g_gjPrec = -1;

std::string gjCoord(double v)
{
    if (g_gjPrec < 0)
        return fmtCoord(v);
    std::string s = strPrintf("%.*f", g_gjPrec, v);
    size_t dot = s.find('.');
    if (dot != std::string::npos)
    {
        size_t last = s.find_last_not_of('0');
        if (last == dot)
            ++last;
        s.erase(last + 1);
    }
    return s;
}

void tupleJson(std::string &out, const double *c, bool hasZ)
{
    out += '[' + gjCoord(c[0]) + ',' + gjCoord(c[1]);
    if (hasZ)
        out += ',' + gjCoord(c[2]);
    out += ']';
}

void lineJson(std::string &out, const OgrGeometry &g)
{
    out += '[';
    size_t n = g.coords.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        if (i)
            out += ',';
        tupleJson(out, &g.coords[i * 3], g.hasZ);
    }
    out += ']';
}

void polyCoordsJson(std::string &out, const OgrGeometry &g)
{
    out += '[';
    for (size_t i = 0; i < g.parts.size(); ++i)
    {
        if (i)
            out += ',';
        lineJson(out, g.parts[i]);
    }
    out += ']';
}

void geomJson(std::string &out, const OgrGeometry &g)
{
    static const char *names[] = {"",           "Point",
                                  "LineString", "Polygon",
                                  "MultiPoint", "MultiLineString",
                                  "MultiPolygon"};
    if (g.type == 7)
    {
        out += "{\"type\":\"GeometryCollection\",\"geometries\":[";
        for (size_t i = 0; i < g.parts.size(); ++i)
        {
            if (i)
                out += ',';
            geomJson(out, g.parts[i]);
        }
        out += "]}";
        return;
    }
    out += strPrintf("{\"type\":\"%s\",\"coordinates\":", names[g.type]);
    switch (g.type)
    {
        case 1:
            if (!g.empty && g.coords.size() >= 3)
                tupleJson(out, &g.coords[0], g.hasZ);
            else
                out += "[]";
            break;
        case 2:
            lineJson(out, g);
            break;
        case 3:
            polyCoordsJson(out, g);
            break;
        case 4:
            out += '[';
            for (size_t i = 0; i < g.parts.size(); ++i)
            {
                if (i)
                    out += ',';
                if (g.parts[i].coords.size() >= 3)
                    tupleJson(out, &g.parts[i].coords[0], g.parts[i].hasZ);
                else
                    out += "[]";
            }
            out += ']';
            break;
        case 5:
            out += '[';
            for (size_t i = 0; i < g.parts.size(); ++i)
            {
                if (i)
                    out += ',';
                lineJson(out, g.parts[i]);
            }
            out += ']';
            break;
        case 6:
            out += '[';
            for (size_t i = 0; i < g.parts.size(); ++i)
            {
                if (i)
                    out += ',';
                polyCoordsJson(out, g.parts[i]);
            }
            out += ']';
            break;
    }
    out += '}';
}

std::string propValueJson(const OgrFieldDefn &f, const JVal &v)
{
    if (v.type == JVal::NUL)
        return "null";
    auto elemInt = [](const JVal &e) -> std::string {
        if (e.type == JVal::BOOL)
            return e.b ? "1" : "0";
        if (e.type == JVal::INT)
            return strPrintf("%lld", e.i);
        if (e.type == JVal::DOUBLE)
            return strPrintf("%lld", (long long)e.d);
        return "0";
    };
    auto elemReal = [](const JVal &e) -> std::string {
        double d = e.type == JVal::INT      ? (double)e.i
                   : e.type == JVal::DOUBLE ? e.d
                   : e.type == JVal::BOOL   ? (e.b ? 1 : 0)
                                            : 0;
        return fmtPropDouble(d);
    };
    if (f.type == OFTIntegerList || f.type == OFTInteger64List ||
        f.type == OFTRealList)
    {
        bool real = f.type == OFTRealList;
        auto elemBool = [](const JVal &e) -> std::string {
            bool b = e.type == JVal::BOOL     ? e.b
                     : e.type == JVal::INT    ? e.i != 0
                     : e.type == JVal::DOUBLE ? e.d != 0
                                              : false;
            return b ? "true" : "false";
        };
        bool asBool =
            f.type == OFTIntegerList && f.subType == OFSTBoolean;
        std::string out = "[";
        if (v.type == JVal::ARRAY)
        {
            for (size_t i = 0; i < v.arr.size(); ++i)
            {
                if (i)
                    out += ',';
                out += asBool ? elemBool(v.arr[i])
                       : real ? elemReal(v.arr[i])
                              : elemInt(v.arr[i]);
            }
        }
        else
            out += asBool ? elemBool(v)
                   : real ? elemReal(v)
                          : elemInt(v);
        return out + "]";
    }
    switch (f.type)
    {
        case OFTInteger:
        case OFTInteger64:
            if (f.subType == OFSTBoolean)
            {
                if (v.type == JVal::BOOL)
                    return v.b ? "true" : "false";
                return (v.type == JVal::INT && v.i != 0) ? "true" : "false";
            }
            if (v.type == JVal::BOOL)
                return v.b ? "1" : "0";
            if (v.type == JVal::INT)
                return strPrintf("%lld", v.i);
            if (v.type == JVal::DOUBLE)
                return strPrintf("%lld", (long long)v.d);
            break;
        case OFTReal:
        {
            double d = 0;
            if (v.type == JVal::INT)
                d = (double)v.i;
            else if (v.type == JVal::DOUBLE)
                d = v.d;
            else if (v.type == JVal::BOOL)
                d = v.b ? 1 : 0;
            else
                break;
            if (f.subType == OFSTFloat32)
                return fmtPropFloat32((double)(float)d);
            return fmtPropDouble(d);
        }
        case OFTDate:
        case OFTDateTime:
        case OFTTime:
        {
            if (v.type == JVal::STRING)
            {
                OgrDateTime dt;
                if (ogrParseDate(v.s, dt))
                    return '"' + jsonEscape(isoDateTime(dt, f.type)) + '"';
                return '"' + jsonEscape(v.s) + '"';
            }
            break;
        }
        case OFTString:
            // AUTODETECT_JSON_STRINGS: a string value that looks like a
            // JSON object or array and parses cleanly embeds raw
            if (v.type == JVal::STRING && !v.s.empty() &&
                (v.s[0] == '{' || v.s[0] == '['))
            {
                bool ok = false;
                JVal parsed = JVal::parse(v.s, &ok);
                if (ok)
                {
                    std::string raw;
                    serializeCompact(raw, parsed);
                    return raw;
                }
            }
            // scalars stored raw in a string field (String(JSON) from
            // mixed-type attributes) write their json-c text form quoted;
            // objects and arrays keep their raw serialization
            if (v.type == JVal::BOOL)
                return v.b ? "\"true\"" : "\"false\"";
            if (v.type == JVal::INT)
                return '"' + strPrintf("%lld", v.i) + '"';
            if (v.type == JVal::DOUBLE)
                return '"' +
                       jsonEscape(v.s.empty() ? ogrJsonDouble(v.d) : v.s) +
                       '"';
            break;
        default:
            break;
    }
    std::string out;
    serializeCompact(out, v);
    return out;
}

std::string crsJsonLine(const OgrLayer &lyr)
{
    if (!lyr.hasSrs)
        return "";
    int code = lyr.srs.epsgCode();
    if (code == 4326)
        return "\"crs\": { \"type\": \"name\", \"properties\": { \"name\": "
               "\"urn:ogc:def:crs:OGC:1.3:CRS84\" } },\n";
    if (code > 0)
        return strPrintf("\"crs\": { \"type\": \"name\", \"properties\": { "
                         "\"name\": \"urn:ogc:def:crs:EPSG::%d\" } },\n",
                         code);
    std::string auth = lyr.srs.authName();
    std::string c = lyr.srs.code();
    if (!auth.empty() && !c.empty())
        return strPrintf("\"crs\": { \"type\": \"name\", \"properties\": { "
                         "\"name\": \"urn:ogc:def:crs:%s::%s\" } },\n",
                         auth.c_str(), c.c_str());
    return "";
}

bool geomHasZDeep(const OgrGeometry &g)
{
    if (g.hasZ)
        return true;
    for (const auto &p : g.parts)
        if (geomHasZDeep(p))
            return true;
    return false;
}

bool geomHasMDeep(const OgrGeometry &g)
{
    if (g.hasM)
        return true;
    for (const auto &p : g.parts)
        if (geomHasMDeep(p))
            return true;
    return false;
}

struct ProgressSpan
{
    TermProgress *tp = nullptr;
    double base = 0;
    double span = 1;

    void update(double f) const
    {
        if (tp)
            tp->update(base + f * span);
    }
};

double ringSignedArea(const std::vector<double> &c)
{
    double a = 0;
    size_t n = c.size() / 3;
    for (size_t i = 0; i + 1 < n; ++i)
        a += c[i * 3] * c[(i + 1) * 3 + 1] - c[(i + 1) * 3] * c[i * 3 + 1];
    return a / 2;
}

void reverseRing(std::vector<double> &c)
{
    size_t n = c.size() / 3;
    if (n < 4)
        return;
    for (size_t i = 1, j = n - 2; i < j; ++i, --j)
        for (int k = 0; k < 3; ++k)
            std::swap(c[i * 3 + k], c[j * 3 + k]);
}

void rfcOrient(OgrGeometry &g)
{
    if (g.type == 3)
    {
        for (size_t i = 0; i < g.parts.size(); ++i)
        {
            double a = ringSignedArea(g.parts[i].coords);
            if ((i == 0 && a < 0) || (i > 0 && a > 0))
                reverseRing(g.parts[i].coords);
        }
        return;
    }
    if (g.type == 6 || g.type == 7)
        for (auto &p : g.parts)
            rfcOrient(p);
}

void gjLonEnvelope(const OgrGeometry &g, double &mn, double &mx, bool &any)
{
    size_t n = g.coords.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        double x = g.coords[i * 3];
        if (!any)
        {
            mn = mx = x;
            any = true;
        }
        else
        {
            mn = std::min(mn, x);
            mx = std::max(mx, x);
        }
    }
    for (const auto &p : g.parts)
        gjLonEnvelope(p, mn, mx, any);
}

void gjAddLonOffset(OgrGeometry &g, double off)
{
    for (size_t i = 0; i * 3 < g.coords.size(); ++i)
        g.coords[i * 3] += off;
    for (auto &p : g.parts)
        gjAddLonOffset(p, off);
}

void gjWrapPointLon(OgrGeometry &g)
{
    if (g.empty || g.coords.size() < 3)
        return;
    double &x = g.coords[0];
    while (x > 180)
        x -= 360;
    while (x < -180)
        x += 360;
}

// mirrors OGRGeometryFactory's dateline split with its quirks: a border
// vertex at exactly +/-180 absorbs the crossing via lookahead, the
// inserted boundary vertex carries the interpolated Y in its Z slot, and
// only a boundary point equal to the current piece tail is deduplicated
std::vector<std::vector<double>> gjSplitLineAtDateline(
    const std::vector<double> &c)
{
    std::vector<std::vector<double>> pieces(1);
    size_t n = c.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        double x = c[i * 3];
        if (i > 0 && fabs(x - c[(i - 1) * 3]) > 350.0)
        {
            double x1 = c[(i - 1) * 3], y1 = c[(i - 1) * 3 + 1];
            double x2 = x, y2 = c[i * 3 + 1];
            if (x1 > -180 && x1 < -170 && x2 == 180 && i + 1 < n &&
                c[(i + 1) * 3] > -180 && c[(i + 1) * 3] < -170)
            {
                auto &pc = pieces.back();
                pc.insert(pc.end(), {-180.0, c[i * 3 + 1], c[i * 3 + 2]});
                ++i;
                pc.insert(pc.end(),
                          {c[i * 3], c[i * 3 + 1], c[i * 3 + 2]});
                continue;
            }
            if (x1 < 180 && x1 > 170 && x2 == -180 && i + 1 < n &&
                c[(i + 1) * 3] > 170 && c[(i + 1) * 3] < 180)
            {
                auto &pc = pieces.back();
                pc.insert(pc.end(), {180.0, c[i * 3 + 1], c[i * 3 + 2]});
                ++i;
                pc.insert(pc.end(),
                          {c[i * 3], c[i * 3 + 1], c[i * 3 + 2]});
                continue;
            }
            if (x1 > 170 && x2 < -170)
                x2 += 360;
            else if (x1 < -170 && x2 > 170)
            {
                x1 += 360;
                std::swap(x1, x2);
                std::swap(y1, y2);
            }
            if (x1 <= 180 && x2 >= 180 && x1 < x2)
            {
                double r = (180 - x1) / (x2 - x1);
                double yi = r * y2 + (1 - r) * y1;
                double bx = c[(i - 1) * 3] > 170 ? 180.0 : -180.0;
                auto &pc = pieces.back();
                size_t m = pc.size();
                if (m == 0 || pc[m - 3] != bx || pc[m - 2] != yi)
                    pc.insert(pc.end(), {bx, yi, yi});
                pieces.emplace_back();
                pieces.back().insert(pieces.back().end(), {-bx, yi, yi});
            }
            else
                pieces.emplace_back();
        }
        pieces.back().insert(
            pieces.back().end(),
            {x > 180 ? x - 360 : x, c[i * 3 + 1], c[i * 3 + 2]});
    }
    return pieces;
}

void gjCutGeometry(OgrGeometry &g, bool &geosErr)
{
    if (g.type == 1)
    {
        gjWrapPointLon(g);
        return;
    }
    if (g.type == 2)
    {
        double mn = 0, mx = 0;
        bool any = false;
        gjLonEnvelope(g, mn, mx, any);
        if (!any)
            return;
        if (mn < -170 && mx > 170)
        {
            size_t n = g.coords.size() / 3;
            bool cross = false;
            for (size_t i = 1; i < n; ++i)
                if (fabs(g.coords[i * 3] - g.coords[(i - 1) * 3]) > 350.0)
                {
                    cross = true;
                    break;
                }
            if (!cross)
                return;
            auto pieces = gjSplitLineAtDateline(g.coords);
            if (pieces.size() == 1)
            {
                g.coords = std::move(pieces[0]);
                return;
            }
            OgrGeometry ml;
            ml.type = 5;
            ml.hasZ = g.hasZ;
            ml.hasM = g.hasM;
            for (auto &pc : pieces)
            {
                OgrGeometry ls;
                ls.type = 2;
                ls.hasZ = g.hasZ;
                ls.coords = std::move(pc);
                ml.parts.push_back(std::move(ls));
            }
            g = std::move(ml);
        }
        else if (mn < -180 || mx > 180)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "GEOS support not enabled.");
            geosErr = true;
        }
        return;
    }
    if (g.type == 3)
    {
        double mn = 0, mx = 0;
        bool any = false;
        gjLonEnvelope(g, mn, mx, any);
        bool cross = false;
        for (const auto &ring : g.parts)
        {
            size_t n = ring.coords.size() / 3;
            for (size_t i = 1; i < n && !cross; ++i)
                if (fabs(ring.coords[i * 3] - ring.coords[(i - 1) * 3]) >
                    350.0)
                    cross = true;
        }
        // polygon splitting needs GEOS, so both a crossing ring and an
        // out-of-range envelope leave the geometry untouched with an error
        if (cross || (any && (mn < -180 || mx > 180)))
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "GEOS support not enabled.");
            geosErr = true;
        }
        return;
    }
    if (g.type >= 4 && g.type <= 7)
    {
        std::vector<OgrGeometry> flat;
        for (auto &p : g.parts)
        {
            // the reader leaves homogeneous multi members untyped
            if (g.type != 7)
                p.type = g.type - 3;
            bool wasLine = p.type == 2;
            gjCutGeometry(p, geosErr);
            if (wasLine && p.type == 5)
                for (auto &sub : p.parts)
                    flat.push_back(std::move(sub));
            else
                flat.push_back(std::move(p));
        }
        g.parts = std::move(flat);
    }
}

void gjWrapDateline(OgrGeometry &g, bool &geosErr)
{
    if (g.type == 1)
    {
        gjWrapPointLon(g);
        return;
    }
    double mn = 0, mx = 0;
    bool any = false;
    gjLonEnvelope(g, mn, mx, any);
    if (any && mn >= -360 && mx <= -180)
    {
        gjAddLonOffset(g, 360);
        return;
    }
    if (any && mn >= 180 && mx <= 360)
    {
        gjAddLonOffset(g, -360);
        return;
    }
    gjCutGeometry(g, geosErr);
}

void envAccumGeom(const OgrGeometry &g, double *env, bool &any)
{
    size_t n = g.coords.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        double x = g.coords[i * 3], y = g.coords[i * 3 + 1];
        if (!any)
        {
            env[0] = env[2] = x;
            env[1] = env[3] = y;
            any = true;
        }
        else
        {
            env[0] = std::min(env[0], x);
            env[1] = std::min(env[1], y);
            env[2] = std::max(env[2], x);
            env[3] = std::max(env[3], y);
        }
    }
    for (const auto &p : g.parts)
        envAccumGeom(p, env, any);
}

struct GjBboxInfo
{
    bool any = false;
    bool hasZ = false;
    double minx = 0, miny = 0, minz = 0;
    double maxx = 0, maxy = 0, maxz = 0;
    // RFC7946 antimeridian bbox: west > east spanning the date line
    bool swapped = false;
    double west = 0, east = 0;
};

void gjAccumXyz(const OgrGeometry &g, GjBboxInfo &b)
{
    size_t n = g.coords.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        double x = g.coords[i * 3], y = g.coords[i * 3 + 1],
               z = g.coords[i * 3 + 2];
        if (!b.any)
        {
            b.minx = b.maxx = x;
            b.miny = b.maxy = y;
            b.minz = b.maxz = z;
            b.any = true;
        }
        else
        {
            b.minx = std::min(b.minx, x);
            b.miny = std::min(b.miny, y);
            b.minz = std::min(b.minz, z);
            b.maxx = std::max(b.maxx, x);
            b.maxy = std::max(b.maxy, y);
            b.maxz = std::max(b.maxz, z);
        }
    }
    for (const auto &p : g.parts)
        gjAccumXyz(p, b);
}

bool geomHasZDeep(const OgrGeometry &g);
void shpLinearizeCurve(OgrGeometry &g);

GjBboxInfo gjGeomBbox(const OgrGeometry &g, bool rfc)
{
    GjBboxInfo b;
    gjAccumXyz(g, b);
    b.hasZ = geomHasZDeep(g);
    // a multi-part geometry takes the RFC7946 west>east antimeridian
    // representation when every part envelope hugs the date line (lon
    // >= 120 or <= -120) or when the full envelope touches both -180
    // and +180 (an actually split geometry); parts group by lon sign
    if (rfc && g.type >= 4 && g.type <= 7 && g.parts.size() >= 2)
    {
        bool allZoned = true, wAny = false, eAny = false;
        double west = 0, east = 0;
        for (const auto &p : g.parts)
        {
            double mn = 0, mx = 0;
            bool any = false;
            gjLonEnvelope(p, mn, mx, any);
            if (!any)
                continue;
            if (!(mn >= 120 || mx <= -120))
                allZoned = false;
            if (mn >= 0)
            {
                if (!wAny || mn < west)
                    west = mn;
                wAny = true;
            }
            else
            {
                if (!eAny || mx > east)
                    east = mx;
                eAny = true;
            }
        }
        const double eps = 1e-7;
        bool touchBoth = b.any && fabs(b.minx + 180) < eps &&
                         fabs(b.maxx - 180) < eps;
        if ((allZoned || touchBoth) && wAny && eAny)
        {
            b.swapped = true;
            b.west = west;
            b.east = east;
        }
    }
    return b;
}

bool gjNonFiniteReal(const OgrFieldDefn &f, const JVal &v)
{
    // Float32 fields serialize their non-finite values (json-c Infinity)
    return f.type == OFTReal && f.subType != OFSTFloat32 &&
           v.type == JVal::DOUBLE && !std::isfinite(v.d);
}

void gjWarnNonFiniteOnce()
{
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "NaN of Infinity value found. Skipped. Further "
                    "messages of this type will be suppressed.");
    }
}

void geoJsonFeatureLine(std::string &out, const OgrLayer &lyr,
                        const OgrFeature &feat,
                        const GeoJsonOpts &opts = GeoJsonOpts())
{
    out += "{\"type\":\"Feature\"";
    if (g_gjForceFidIds)
        out += strPrintf(",\"id\":%lld", feat.fid);
    out += ",\"properties\":{";
    bool first = true;
    for (size_t k = 0; k < lyr.fields.size(); ++k)
    {
        if (k >= feat.values.size() || !feat.values[k].set)
            continue;
        if (gjNonFiniteReal(lyr.fields[k], feat.values[k].v))
        {
            gjWarnNonFiniteOnce();
            continue;
        }
        if (!first)
            out += ',';
        first = false;
        out += '"' + jsonEscape(lyr.fields[k].name) + "\":";
        out += propValueJson(lyr.fields[k], feat.values[k].v);
    }
    out += "}";
    OgrGeometry oriented;
    const OgrGeometry *geom = &feat.geom;
    if (feat.hasGeom && opts.rfc)
    {
        oriented = feat.geom;
        rfcOrient(oriented);
        geom = &oriented;
    }
    if (opts.bbox && feat.hasGeom)
    {
        GjBboxInfo b = gjGeomBbox(*geom, opts.rfc);
        if (b.any)
        {
            double w = b.swapped ? b.west : b.minx;
            double e = b.swapped ? b.east : b.maxx;
            out += ",\"bbox\":[" + gjCoord(w) + ',' + gjCoord(b.miny);
            if (b.hasZ)
                out += ',' + gjCoord(b.minz);
            out += ',' + gjCoord(e) + ',' + gjCoord(b.maxy);
            if (b.hasZ)
                out += ',' + gjCoord(b.maxz);
            out += ']';
        }
    }
    out += ",\"geometry\":";
    int gfail = feat.hasGeom ? geomJsonExportFail(*geom) : 0;
    if (gfail == 2)
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "Infinite or NaN coordinate encountered");
    if (feat.hasGeom && !gfail)
        geomJson(out, *geom);
    else if (feat.hasGeom && geom->type == 7)
        out += "{\"type\":\"GeometryCollection\",\"geometries\":null}";
    else
        out += "null";
    out += '}';
}

void reprojWgs84(OgrGeometry &g, const Srs &srs);

// OGRFeature::SetFrom value coercion used by in-place appends: values
// are converted to the target field type up front, with the reference's
// warning choreography
JVal appendCoerceValue(const std::string &lyrName, const OgrFieldDefn &tf,
                       const OgrFieldDefn &sf, const JVal &v)
{
    if (v.type == JVal::NUL)
        return v;
    auto fieldRef = [&]() { return lyrName + "." + tf.name; };
    if ((tf.type == OFTInteger || tf.type == OFTInteger64) &&
        tf.subType == OFSTBoolean)
    {
        JVal r;
        r.type = JVal::BOOL;
        if (v.type == JVal::BOOL)
            r.b = v.b;
        else if (v.type == JVal::INT)
            r.b = v.i != 0;
        else if (v.type == JVal::DOUBLE)
            r.b = v.d != 0;
        else if (v.type == JVal::STRING)
        {
            if (strEqualNoCase(v.s, "1") || strEqualNoCase(v.s, "true") ||
                strEqualNoCase(v.s, "yes") || strEqualNoCase(v.s, "on"))
                r.b = true;
            else if (strEqualNoCase(v.s, "0") ||
                     strEqualNoCase(v.s, "false") ||
                     strEqualNoCase(v.s, "no") || strEqualNoCase(v.s, "off"))
                r.b = false;
            else
            {
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Invalid value '" + v.s +
                                "' for boolean field " + fieldRef() +
                                ". Assuming it to be false.");
                r.b = false;
            }
        }
        else
            return v;
        return r;
    }
    if (tf.type == OFTInteger || tf.type == OFTInteger64)
    {
        bool is32 = tf.type == OFTInteger;
        JVal r;
        r.type = JVal::INT;
        if (v.type == JVal::INT)
        {
            r.i = v.i;
            if (is32 && (v.i > 2147483647LL || v.i < -2147483648LL))
            {
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("Field %s: integer overflow occurred "
                                      "when trying to set %lld as 32 bit "
                                      "integer.",
                                      fieldRef().c_str(), v.i));
                r.i = v.i > 0 ? 2147483647LL : -2147483648LL;
            }
            return r;
        }
        if (v.type == JVal::BOOL)
        {
            r.i = v.b ? 1 : 0;
            return r;
        }
        if (v.type == JVal::DOUBLE)
        {
            double lim = is32 ? 2147483647.0 : 9223372036854775807.0;
            double lo = is32 ? -2147483648.0 : -9223372036854775808.0;
            bool lossy;
            if (v.d >= lim)
            {
                r.i = is32 ? 2147483647LL : 9223372036854775807LL;
                lossy = true;
            }
            else if (v.d <= lo)
            {
                r.i = is32 ? -2147483648LL : (-9223372036854775807LL - 1);
                lossy = true;
            }
            else
            {
                r.i = (long long)v.d;
                lossy = (double)r.i != v.d;
            }
            if (lossy)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("Field %s: Lossy conversion occurred "
                                      "when trying to set %s bit integer "
                                      "field from real value %.17g.",
                                      fieldRef().c_str(), is32 ? "32" : "64",
                                      v.d));
            return r;
        }
        if (v.type == JVal::STRING)
        {
            errno = 0;
            char *end = nullptr;
            long long ll = strtoll(v.s.c_str(), &end, 10);
            bool full = end && end != v.s.c_str() && *end == '\0' &&
                        errno != ERANGE;
            if (is32)
            {
                if (ll > 2147483647LL)
                    ll = 2147483647LL;
                else if (ll < -2147483648LL)
                    ll = -2147483648LL;
                else if (full)
                {
                    r.i = ll;
                    return r;
                }
                r.i = ll;
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("Value '%s' of field %s parsed "
                                      "incompletely to integer %lld.",
                                      v.s.c_str(), fieldRef().c_str(), ll));
                return r;
            }
            r.i = ll;
            return r;
        }
        return v;
    }
    if (tf.type == OFTReal)
    {
        if (v.type == JVal::STRING)
        {
            char *end = nullptr;
            double d = strtod(v.s.c_str(), &end);
            bool full = end && end != v.s.c_str() && *end == '\0';
            if (!full)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("Value '%s' of field %s parsed "
                                      "incompletely to real %g.",
                                      v.s.c_str(), fieldRef().c_str(), d));
            JVal r;
            r.type = JVal::DOUBLE;
            r.d = d;
            return r;
        }
        return v;
    }
    if (tf.type == OFTString && tf.subType != OFSTJSON)
    {
        JVal r;
        r.type = JVal::STRING;
        if (v.type == JVal::DOUBLE)
        {
            r.s = strPrintf("%.15g", v.d);
            return r;
        }
        if (v.type == JVal::BOOL &&
            (sf.type == OFTInteger || sf.type == OFTInteger64))
        {
            r.s = v.b ? "1" : "0";
            return r;
        }
        if (v.type == JVal::ARRAY)
        {
            r.s = strPrintf("(%d:", (int)v.arr.size());
            for (size_t k = 0; k < v.arr.size(); ++k)
            {
                const JVal &e = v.arr[k];
                if (k)
                    r.s += ',';
                if (e.type == JVal::BOOL)
                    r.s += e.b ? "1" : "0";
                else if (e.type == JVal::INT)
                    r.s += strPrintf("%lld", e.i);
                else if (e.type == JVal::DOUBLE)
                    r.s += strPrintf("%.16g", e.d);
                else if (e.type == JVal::STRING)
                    r.s += e.s;
            }
            r.s += ')';
            return r;
        }
        return v;
    }
    return v;
}

// SetFrom semantics: the map is built per SOURCE field (exact target
// name scan, then case-insensitive) and applied in source order, so a
// later source field resolving to the same target slot overwrites it
OgrFeature appendMapFeature(const OgrLayer &tgt, const OgrLayer &src,
                            const OgrFeature &f)
{
    OgrFeature m;
    m.hasGeom = f.hasGeom;
    m.geom = f.geom;
    m.values.resize(tgt.fields.size());
    for (size_t si = 0; si < src.fields.size() && si < f.values.size();
         ++si)
    {
        int ti = -1;
        for (size_t k = 0; k < tgt.fields.size(); ++k)
            if (tgt.fields[k].name == src.fields[si].name)
            {
                ti = (int)k;
                break;
            }
        if (ti < 0)
            for (size_t k = 0; k < tgt.fields.size(); ++k)
                if (strEqualNoCase(tgt.fields[k].name,
                                   src.fields[si].name))
                {
                    ti = (int)k;
                    break;
                }
        if (ti < 0 || !f.values[si].set)
            continue;
        m.values[ti].set = true;
        m.values[ti].v = appendCoerceValue(tgt.name, tgt.fields[ti],
                                           src.fields[si], f.values[si].v);
    }
    return m;
}

// CreateField renames a duplicate field name with the first numeric
// suffix free among the prior fields and every original name (so a
// pending later field can bump the suffix), warning per rename;
// GeoJSON compares exactly, the shapefile DBF case-insensitively
bool renameDupFields(const OgrLayer &lyr, OgrLayer &out, bool ci)
{
    bool any = false;
    std::vector<std::string> names;
    auto eq = [&](const std::string &a, const std::string &b)
    { return ci ? strEqualNoCase(a, b) : a == b; };
    auto taken = [&](const std::string &n)
    {
        for (const auto &t : names)
            if (eq(t, n))
                return true;
        return false;
    };
    auto original = [&](const std::string &n)
    {
        for (const auto &f : lyr.fields)
            if (eq(f.name, n))
                return true;
        return false;
    };
    for (const auto &f : lyr.fields)
    {
        std::string nm = f.name;
        if (taken(nm))
        {
            int n = 2;
            std::string cand;
            do
                cand = nm + strPrintf("%d", n++);
            while (taken(cand) || original(cand));
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Field '" + nm + "' already exists. Renaming it "
                                         "as '" +
                            cand + "'");
            nm = cand;
            any = true;
        }
        names.push_back(nm);
    }
    if (!any)
        return false;
    out = lyr;
    for (size_t i = 0; i < names.size(); ++i)
        out.fields[i].name = names[i];
    return true;
}

bool gjRenameDupFields(const OgrLayer &lyr, OgrLayer &out)
{
    return renameDupFields(lyr, out, false);
}

void gjNativeFeatureLine(std::string &out, const OgrLayer &lyr,
                         const OgrFeature &feat);

bool writeGeoJson(const OgrLayer &lyr, const std::string &outLayerName,
                  const std::string &path, bool quiet,
                  const ProgressSpan &ps, const OgrLayer *appendFrom,
                  const GeoJsonOpts &opts = GeoJsonOpts())
{
    // an update step's reopened output flows its native document to the
    // writer instead of the standard serialization
    if (lyr.gjUpdateFlow && lyr.gjRoot && !appendFrom)
    {
        std::string doc;
        if (outLayerName != lyr.name)
        {
            OgrLayer named = lyr;
            named.name = outLayerName;
            doc = geoJsonUpdateRewriteDoc(named, lyr.gjRoot.get());
        }
        else
            doc = geoJsonUpdateRewriteDoc(lyr, lyr.gjRoot.get());
        if (path == "/vsistdout/")
            fwrite(doc.data(), 1, doc.size(), stdout);
        else if (!writeStringToFile(path, doc))
            return false;
        ps.update(1.0);
        return true;
    }
    {
        OgrLayer renamed;
        if (gjRenameDupFields(lyr, renamed))
            return writeGeoJson(renamed, outLayerName, path, quiet, ps,
                                appendFrom, opts);
    }
    g_gjPrec = opts.prec;
    const OgrLayer &head = appendFrom ? *appendFrom : lyr;
    std::string name = appendFrom ? appendFrom->name : outLayerName;
    bool streamOut = path == "/vsistdout/";
    // appends splice compact features into the existing file's bytes:
    // everything before the feature array close is left untouched
    bool inPlace = false;
    bool any = false;
    std::string out;
    if (appendFrom && !streamOut)
    {
        std::string orig;
        if (readFileToString(path, orig))
        {
            auto ws = [](char c)
            { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            size_t e = orig.size();
            while (e && ws(orig[e - 1]))
                --e;
            if (e && orig[e - 1] == '}')
            {
                size_t p = e - 1;
                while (p && ws(orig[p - 1]))
                    --p;
                if (p && orig[p - 1] == ']')
                {
                    --p;
                    while (p && ws(orig[p - 1]))
                        --p;
                    out = orig.substr(0, p);
                    inPlace = true;
                    any = p && orig[p - 1] != '[';
                    if (!any)
                        out += "\n";
                }
            }
        }
    }
    if (!inPlace)
    {
    out = "{\n\"type\": \"FeatureCollection\",\n";
    if (opts.writeName)
    {
        // the header is serialized with json-c's default slash escaping,
        // unlike the per-feature writer
        std::string esc = jsonEscape(name);
        std::string slashed;
        for (char ch : esc)
        {
            if (ch == '/')
                slashed += "\\/";
            else
                slashed += ch;
        }
        out += "\"name\": \"" + slashed + "\",\n";
    }
    if (!opts.rfc)
        out += crsJsonLine(head);
    if (opts.precSet)
    {
        out += strPrintf("\"xy_coordinate_resolution\": %g,\n",
                         pow(10.0, -opts.prec));
        if (head.geomHasZ)
            out += strPrintf("\"z_coordinate_resolution\": %g,\n",
                             pow(10.0, -opts.prec));
    }
    }
    // layer bbox accumulated over written features; swapped feature
    // envelopes merge in a continuous lon space past +180
    GjBboxInfo lb;
    auto lbMerge = [&](const GjBboxInfo &fb) {
        if (!fb.any)
            return;
        double cmin = fb.swapped ? fb.west : fb.minx;
        double cmax = fb.swapped ? fb.east + 360 : fb.maxx;
        if (!lb.any)
        {
            lb.any = true;
            lb.minx = cmin;
            lb.maxx = cmax;
            lb.miny = fb.miny;
            lb.maxy = fb.maxy;
            lb.minz = fb.minz;
            lb.maxz = fb.maxz;
        }
        else
        {
            lb.minx = std::min(lb.minx, cmin);
            lb.maxx = std::max(lb.maxx, cmax);
            lb.miny = std::min(lb.miny, fb.miny);
            lb.maxy = std::max(lb.maxy, fb.maxy);
            // the RFC7946 writer keeps only the first feature's Z range
            if (!opts.rfc)
            {
                lb.minz = std::min(lb.minz, fb.minz);
                lb.maxz = std::max(lb.maxz, fb.maxz);
            }
        }
        lb.hasZ = lb.hasZ || fb.hasZ;
    };
    auto bboxVals = [&]() {
        double e = opts.rfc && lb.maxx > 180 ? lb.maxx - 360 : lb.maxx;
        double vals[6] = {lb.minx, lb.miny, lb.minz, e, lb.maxy, lb.maxz};
        std::string s = "\"bbox\": [ ";
        int idx = 0;
        for (int k = 0; k < 6; ++k)
        {
            if (!lb.hasZ && (k == 2 || k == 5))
                continue;
            if (idx++)
                s += ", ";
            s += opts.prec >= 0 ? strPrintf("%.*f", opts.prec, vals[k])
                                : strPrintf("%.15g", vals[k]);
        }
        return s + " ]";
    };
    auto bboxLine = [&]() {
        std::string line = lb.any ? bboxVals() + "," : std::string();
        while (line.size() < 131)
            line += ' ';
        return line + "\n";
    };
    size_t bboxAt = std::string::npos;
    if (opts.bbox && !streamOut && !inPlace)
        bboxAt = out.size();
    // a non-seekable target appends its bbox after the feature array
    auto closeDoc = [&]() {
        if (streamOut && opts.bbox && lb.any)
            out += "\n],\n" + bboxVals() + "\n}\n";
        else
            out += "\n]\n}\n";
        if (bboxAt != std::string::npos)
            out.insert(bboxAt, bboxLine());
    };
    if (!inPlace)
        out += "\"features\": [\n";
    if (appendFrom && !inPlace)
        for (const auto &feat : appendFrom->features)
        {
            if (any)
                out += ",\n";
            any = true;
            geoJsonFeatureLine(out, *appendFrom, feat);
        }
    size_t total = lyr.features.size();
    bool rfcReproj = false;
    if (opts.rfc && lyr.hasSrs)
    {
        int code = lyr.srs.epsgCode();
        rfcReproj = !(code == 4326 || code == 4979 ||
                      (lyr.srs.authName() == "OGC" &&
                       lyr.srs.code() == "CRS84"));
    }
    bool zWarned = false;
    bool mWarned = false;
    bool curveWarned = false;
    // whether the most recently processed feature left a failure raised;
    // a clean later feature resets it, mirroring the per-feature error
    // state the reference consults after the loop
    bool rfcLastErr = false;
    size_t flushed = 0;
    // stdio buffering is kept: syscall-level interleave with stderr and
    // progress ticks (which do flush) mirrors the reference
    auto flush = [&]() {
        if (out.size() > flushed)
        {
            fwrite(out.data() + flushed, 1, out.size() - flushed, stdout);
            flushed = out.size();
        }
    };
    for (size_t i = 0; i < total; ++i)
    {
        const OgrFeature &feat0 = lyr.features[i];
        seqTick(lyr);
        if (g_convertFeatureHook)
            g_convertFeatureHook(lyr,
                                 const_cast<OgrFeature &>(feat0));
        OgrFeature gjWork;
        const OgrFeature *pf = &feat0;
        bool unsupGeom = false;
        if (feat0.hasGeom && feat0.geom.type >= 8 &&
            feat0.geom.type <= 12)
        {
            if (!curveWarned && !quiet &&
                !(streamOut && g_pipelineMode))
            {
                curveWarned = true;
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Attempt to write curve geometries to layer " +
                                name +
                                " that does not support them. They will "
                                "be linearized");
            }
            gjWork = feat0;
            shpLinearizeCurve(gjWork.geom);
            pf = &gjWork;
        }
        else if (feat0.hasGeom && feat0.geom.type >= 15 &&
                 feat0.geom.type <= 17)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "OGR geometry type unsupported as a GeoJSON "
                        "geometry detected. Feature gets NULL geometry "
                        "assigned.");
            gjWork = feat0;
            gjWork.hasGeom = false;
            gjWork.geom = OgrGeometry();
            pf = &gjWork;
            unsupGeom = true;
        }
        const OgrFeature &feat = *pf;
        if ((!quiet || g_transZWarnEnable) && !streamOut && !zWarned &&
            feat.hasGeom && geomHasZDeep(feat.geom))
        {
            zWarned = true;
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Attempt to write Z geometries to layer " + name +
                            " that does not support them. Z component will "
                            "be discarded");
        }
        if (!quiet && !streamOut && !mWarned && feat.hasGeom &&
            geomHasMDeep(feat.geom))
        {
            mWarned = true;
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Attempt to write M geometries to layer " + name +
                            " that does not support them. M component will "
                            "be discarded");
        }
        OgrFeature rfcWork;
        const OgrFeature *wf = &feat;
        bool skipFeat = false;
        rfcLastErr = unsupGeom;
        if (opts.rfc && feat.hasGeom)
        {
            rfcWork = feat;
            if (rfcReproj)
                reprojWgs84(rfcWork.geom, lyr.srs);
            gjWrapDateline(rfcWork.geom, rfcLastErr);
            double env[4] = {0, 0, 0, 0};
            bool anyPt = false;
            envAccumGeom(rfcWork.geom, env, anyPt);
            if (anyPt && (env[0] < -180 || env[2] > 180 || env[1] < -90 ||
                          env[3] > 90))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Geometry extent outside of "
                            "[-180.0,180.0]x[-90.0,90.0] bounds");
                if (!opts.skipErrors)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                strPrintf("Unable to write feature %lld "
                                          "from layer %s.",
                                          feat.fid, lyr.name.c_str()));
                    closeDoc();
                    g_gjPrec = -1;
                    if (streamOut)
                        flush();
                    else
                        writeStringToFile(path, out);
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Failed to write layer '" + lyr.name +
                                    "'. Use --skip-errors to ignore "
                                    "errors and continue writing.");
                    return false;
                }
                skipFeat = true;
            }
            wf = &rfcWork;
        }
        OgrFeature mappedFeat;
        if (inPlace && !skipFeat)
        {
            mappedFeat = appendMapFeature(*appendFrom, lyr, *wf);
            wf = &mappedFeat;
        }
        if (!skipFeat)
        {
            if (opts.bbox && wf->hasGeom)
                lbMerge(gjGeomBbox(wf->geom, opts.rfc));
            if (any)
                out += ",\n";
            any = true;
            if (lyr.gjNativeMerge && wf->gjNative && !inPlace)
                gjNativeFeatureLine(out, lyr, *wf);
            else
                geoJsonFeatureLine(out, inPlace ? *appendFrom : lyr, *wf,
                                   opts);
            if (streamOut)
                flush();
        }
        if (!opts.barAtEnd && i + 1 < total)
            ps.update((double)(i + 1) / (double)total);
    }
    // the close-time layer failure precedes the final bar tick; the
    // footer bytes still land after the tick on streams
    bool rfcFailed = !opts.skipErrors && rfcLastErr;
    if (rfcFailed)
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to write layer '" + lyr.name +
                        "'. Use --skip-errors to ignore errors and "
                        "continue writing.");
    if (ps.tp && streamOut)
    {
        flush();
        ps.update(1.0);
    }
    closeDoc();
    g_gjPrec = -1;
    if (streamOut)
        flush();
    else if (!writeStringToFile(path, out))
        return false;
    if (ps.tp && !streamOut)
        ps.update(1.0);
    return !rfcFailed;
}

struct GeoJsonSeqOpts
{
    bool rs = false;
    bool bbox = false;
    int prec = 7;
    std::string idField;
    std::string idType;
    bool barAtEnd = false;
    bool skipErrors = false;
    // appending to the found layer bypasses the RFC7946 processing of
    // fresh layers (no reprojection, no ring re-orientation); growing a
    // new layer onto the file keeps the full treatment
    bool rawAppend = false;
};

// the sequence writer always outputs WGS84 coordinates
void reprojWgs84(OgrGeometry &g, const Srs &srs)
{
    size_t n = g.coords.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        double lon = 0, lat = 0;
        if (srs.toWgs84(g.coords[i * 3], g.coords[i * 3 + 1], lon, lat))
        {
            g.coords[i * 3] = lon;
            g.coords[i * 3 + 1] = lat;
        }
    }
    for (auto &p : g.parts)
        reprojWgs84(p, srs);
}

bool writeGeoJsonSeq(const OgrLayer &lyr, const std::string &path,
                     const ProgressSpan &ps, bool append,
                     const GeoJsonSeqOpts &opts, bool quiet = false)
{
    {
        OgrLayer renamed;
        if (gjRenameDupFields(lyr, renamed))
            return writeGeoJsonSeq(renamed, path, ps, append, opts, quiet);
    }
    g_gjPrec = opts.prec;
    int idIdx = -1;
    if (!opts.idField.empty())
        for (size_t i = 0; i < lyr.fields.size(); ++i)
            if (strEqualNoCase(lyr.fields[i].name, opts.idField))
            {
                idIdx = (int)i;
                break;
            }
    bool reproj = false;
    if (lyr.hasSrs)
    {
        int code = lyr.srs.epsgCode();
        reproj = !(code == 4326 || code == 4979 ||
                   (lyr.srs.authName() == "OGC" &&
                    lyr.srs.code() == "CRS84"));
    }
    if (append && opts.rawAppend)
        reproj = false;
    std::string out;
    if (append)
        readFileToString(path, out);
    bool streamOut = path == "/vsistdout/";
    size_t flushed = 0;
    auto flush = [&]() {
        if (out.size() > flushed)
        {
            fwrite(out.data() + flushed, 1, out.size() - flushed, stdout);
            flushed = out.size();
        }
    };
    size_t total = lyr.features.size();
    bool zWarned = false;
    bool curveWarned = false;
    bool lastUnsup = false;
    for (size_t i = 0; i < total; ++i)
    {
        const OgrFeature &feat0 = lyr.features[i];
        seqTick(lyr);
        if (g_convertFeatureHook)
            g_convertFeatureHook(lyr,
                                 const_cast<OgrFeature &>(feat0));
        OgrFeature gjWork;
        const OgrFeature *pf = &feat0;
        lastUnsup = false;
        if (feat0.hasGeom && feat0.geom.type >= 8 &&
            feat0.geom.type <= 12)
        {
            if (!curveWarned && !quiet &&
                !(streamOut && g_pipelineMode))
            {
                curveWarned = true;
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Attempt to write curve geometries to layer " +
                                lyr.name +
                                " that does not support them. They will "
                                "be linearized");
            }
            gjWork = feat0;
            shpLinearizeCurve(gjWork.geom);
            pf = &gjWork;
        }
        else if (feat0.hasGeom && feat0.geom.type >= 15 &&
                 feat0.geom.type <= 17)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "OGR geometry type unsupported as a GeoJSON "
                        "geometry detected. Feature gets NULL geometry "
                        "assigned.");
            gjWork = feat0;
            gjWork.hasGeom = false;
            gjWork.geom = OgrGeometry();
            pf = &gjWork;
            lastUnsup = true;
        }
        const OgrFeature &feat = *pf;
        // the seq output layer is always declared 2D, so any Z-bearing
        // geometry draws the one-shot warning (z still gets written);
        // stdout targets keep the stream clean of warnings
        if ((!quiet || g_transZWarnEnable) && !streamOut && feat.hasGeom &&
            feat.geom.hasZ && !zWarned)
        {
            zWarned = true;
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Attempt to write Z geometries to layer " +
                            lyr.name +
                            " that does not support them. Z component "
                            "will be discarded");
        }
        const OgrGeometry *geom = &feat.geom;
        OgrGeometry rg;
        if (feat.hasGeom)
        {
            rg = feat.geom;
            if (reproj)
                reprojWgs84(rg, lyr.srs);
            if (!(append && opts.rawAppend))
                rfcOrient(rg);
            geom = &rg;
        }
        if (opts.rs)
            out += '\x1e';
        if (lyr.gjNativeMerge && feat.gjNative)
        {
            OgrFeature nf = feat;
            if (feat.hasGeom)
                nf.geom = *geom;
            gjNativeFeatureLine(out, lyr, nf);
            out += '\n';
            if (streamOut)
                flush();
            if (!opts.barAtEnd && i + 1 < total)
                ps.update((double)(i + 1) / (double)total);
            continue;
        }
        out += "{\"type\":\"Feature\"";
        if (g_gjForceFidIds && idIdx < 0)
            out += strPrintf(",\"id\":%lld", feat.fid);
        if (idIdx >= 0 && (size_t)idIdx < feat.values.size() &&
            feat.values[idIdx].set)
        {
            std::string val =
                propValueJson(lyr.fields[idIdx], feat.values[idIdx].v);
            if (strEqualNoCase(opts.idType, "String"))
            {
                if (val.empty() || val[0] != '"')
                    val = '"' + jsonEscape(val) + '"';
            }
            else if (strEqualNoCase(opts.idType, "Integer"))
            {
                if (!val.empty() && val[0] == '"')
                    val = strPrintf("%lld", atoll(val.c_str() + 1));
                else
                    val = strPrintf("%lld", atoll(val.c_str()));
            }
            out += ",\"id\":" + val;
        }
        out += ",\"properties\":{";
        bool first = true;
        for (size_t k = 0; k < lyr.fields.size(); ++k)
        {
            if ((int)k == idIdx)
                continue;
            if (k >= feat.values.size() || !feat.values[k].set)
                continue;
            if (gjNonFiniteReal(lyr.fields[k], feat.values[k].v))
            {
                gjWarnNonFiniteOnce();
                continue;
            }
            if (!first)
                out += ',';
            first = false;
            out += '"' + jsonEscape(lyr.fields[k].name) + "\":";
            out += propValueJson(lyr.fields[k], feat.values[k].v);
        }
        out += "}";
        if (opts.bbox && feat.hasGeom)
        {
            double env[4] = {0, 0, 0, 0};
            bool anyPt = false;
            envAccumGeom(*geom, env, anyPt);
            if (anyPt)
                out += ",\"bbox\":[" + gjCoord(env[0]) + ',' +
                       gjCoord(env[1]) + ',' + gjCoord(env[2]) + ',' +
                       gjCoord(env[3]) + ']';
        }
        out += ",\"geometry\":";
        int gfail = feat.hasGeom ? geomJsonExportFail(*geom) : 0;
        if (gfail == 2)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Infinite or NaN coordinate encountered");
        if (feat.hasGeom && !gfail)
            geomJson(out, *geom);
        else if (feat.hasGeom && geom->type == 7)
            out += "{\"type\":\"GeometryCollection\",\"geometries\":null}";
        else
            out += "null";
        out += "}\n";
        if (streamOut)
            flush();
        if (!opts.barAtEnd && i + 1 < total)
            ps.update((double)(i + 1) / (double)total);
    }
    g_gjPrec = -1;
    bool lateFail = lastUnsup && !opts.skipErrors;
    if (lateFail)
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to write layer '" + lyr.name +
                        "'. Use --skip-errors to ignore errors and "
                        "continue writing.");
    if (ps.tp && streamOut)
    {
        flush();
        ps.update(1.0);
    }
    if (streamOut)
        flush();
    else if (!writeStringToFile(path, out))
        return false;
    if (ps.tp && !streamOut)
        ps.update(1.0);
    return !lateFail;
}

// ---------------- shapefile .qix spatial index (shapelib SQT) --------

void put32(std::string &s, unsigned v);
void putD(std::string &s, double d);

struct QixNode
{
    double bmin[2], bmax[2];
    std::vector<int> ids;
    std::vector<std::unique_ptr<QixNode>> sub;
};

void qixSplitBounds(const double *inMin, const double *inMax,
                    double *min1, double *max1, double *min2, double *max2)
{
    const double ratio = 0.55;
    for (int k = 0; k < 2; ++k)
    {
        min1[k] = min2[k] = inMin[k];
        max1[k] = max2[k] = inMax[k];
    }
    if (inMax[0] - inMin[0] > inMax[1] - inMin[1])
    {
        double range = inMax[0] - inMin[0];
        max1[0] = inMin[0] + range * ratio;
        min2[0] = inMax[0] - range * ratio;
    }
    else
    {
        double range = inMax[1] - inMin[1];
        max1[1] = inMin[1] + range * ratio;
        min2[1] = inMax[1] - range * ratio;
    }
}

bool qixContained(const double *smin, const double *smax,
                  const double *nmin, const double *nmax)
{
    for (int k = 0; k < 2; ++k)
        if (smin[k] < nmin[k] || smax[k] > nmax[k])
            return false;
    return true;
}

void qixAddShape(QixNode *n, const double *smin, const double *smax,
                 int id, int maxDepth)
{
    if (!n->sub.empty())
    {
        for (auto &c : n->sub)
            if (qixContained(smin, smax, c->bmin, c->bmax))
            {
                qixAddShape(c.get(), smin, smax, id, maxDepth - 1);
                return;
            }
    }
    else if (maxDepth > 1)
    {
        double h1min[2], h1max[2], h2min[2], h2max[2];
        double qmin[4][2], qmax[4][2];
        qixSplitBounds(n->bmin, n->bmax, h1min, h1max, h2min, h2max);
        qixSplitBounds(h1min, h1max, qmin[0], qmax[0], qmin[1], qmax[1]);
        qixSplitBounds(h2min, h2max, qmin[2], qmax[2], qmin[3], qmax[3]);
        bool fits = false;
        for (int q = 0; q < 4 && !fits; ++q)
            fits = qixContained(smin, smax, qmin[q], qmax[q]);
        if (fits)
        {
            for (int q = 0; q < 4; ++q)
            {
                auto c = std::make_unique<QixNode>();
                memcpy(c->bmin, qmin[q], sizeof c->bmin);
                memcpy(c->bmax, qmax[q], sizeof c->bmax);
                n->sub.push_back(std::move(c));
            }
            qixAddShape(n, smin, smax, id, maxDepth);
            return;
        }
    }
    n->ids.push_back(id);
}

bool qixTrim(QixNode *n)
{
    for (size_t i = 0; i < n->sub.size(); ++i)
        if (qixTrim(n->sub[i].get()))
        {
            n->sub[i] = std::move(n->sub.back());
            n->sub.pop_back();
            --i;
        }
    if (n->sub.size() == 1 && n->ids.empty())
    {
        std::unique_ptr<QixNode> child = std::move(n->sub[0]);
        n->sub.clear();
        memcpy(n->bmin, child->bmin, sizeof n->bmin);
        memcpy(n->bmax, child->bmax, sizeof n->bmax);
        n->ids = std::move(child->ids);
        n->sub = std::move(child->sub);
    }
    return n->sub.empty() && n->ids.empty();
}

size_t qixNodeSize(const QixNode *n)
{
    size_t s = 4 + 32 + 4 + 4 * n->ids.size() + 4;
    for (const auto &c : n->sub)
        s += qixNodeSize(c.get());
    return s;
}

void qixWriteNode(std::string &out, const QixNode *n)
{
    size_t offset = 0;
    for (const auto &c : n->sub)
        offset += qixNodeSize(c.get());
    put32(out, (unsigned)offset);
    putD(out, n->bmin[0]);
    putD(out, n->bmin[1]);
    putD(out, n->bmax[0]);
    putD(out, n->bmax[1]);
    put32(out, (unsigned)n->ids.size());
    for (int id : n->ids)
        put32(out, (unsigned)id);
    put32(out, (unsigned)n->sub.size());
    for (const auto &c : n->sub)
        qixWriteNode(out, c.get());
}

struct QixShape
{
    int id;
    double env[4];
};

struct QixData
{
    unsigned total = 0;
    double fileBox[4] = {0, 0, 0, 0};
    bool filesWritten = false;
    std::vector<QixShape> shapes;
};

bool writeQix(const QixData &qd, const std::string &path)
{
    int maxDepth = 0;
    {
        size_t maxNodeCount = 1;
        while (maxNodeCount * 4 < qd.total)
        {
            ++maxDepth;
            maxNodeCount *= 2;
        }
        if (maxDepth > 12)
            maxDepth = 12;
    }
    QixNode root;
    root.bmin[0] = qd.fileBox[0];
    root.bmin[1] = qd.fileBox[1];
    root.bmax[0] = qd.fileBox[2];
    root.bmax[1] = qd.fileBox[3];
    for (const auto &sh : qd.shapes)
    {
        double smin[2] = {sh.env[0], sh.env[1]};
        double smax[2] = {sh.env[2], sh.env[3]};
        qixAddShape(&root, smin, smax, sh.id, maxDepth);
    }
    qixTrim(&root);
    std::string out;
    out += "SQT";
    out += (char)1;
    put32(out, 1);
    put32(out, qd.total);
    put32(out, (unsigned)maxDepth);
    qixWriteNode(out, &root);
    return writeStringToFile(path, out);
}

// ---------------- shapefile writing ----------------

void put16(std::string &s, unsigned v)
{
    s += (char)(v & 0xff);
    s += (char)((v >> 8) & 0xff);
}

void put32(std::string &s, unsigned v)
{
    for (int i = 0; i < 4; ++i)
        s += (char)((v >> (8 * i)) & 0xff);
}

void put32be(std::string &s, unsigned v)
{
    for (int i = 3; i >= 0; --i)
        s += (char)((v >> (8 * i)) & 0xff);
}

void putD(std::string &s, double d)
{
    char b[8];
    memcpy(b, &d, 8);
    s.append(b, 8);
}

struct ShpFieldOut
{
    std::string name;
    char type = 'C';
    int width = 0;
    int decimals = 0;
    int srcIndex = -1;
};

struct ShpWriteCtx
{
    int shpType = 0;  // 0 = not yet resolved (layer type unknown)
    bool typeResolved = false;
    std::string recs;      // shp records
    std::string shxRecs;   // shx records
    std::string dbfRecs;   // dbf records
    std::vector<ShpFieldOut> fields;
    int nRecords = 0;
    double bbox[4] = {0, 0, 0, 0};
    double zmin = 0, zmax = 0;
    bool boundsInit = false;
    bool appendMode = false;
    bool appendHasShp = false;
    bool hasM = false;
    bool geomless = false;
    double mmin = 0, mmax = 0;
    std::string dbfPrefix;  // existing header incl. descriptors
    std::string encoding;   // recode target for names/values ("" = none)
    bool haveCpg = false;   // create-time ENCODING lco given
    std::string cpgContent;
    bool curveWarned = false;
};

struct ShpAppendData
{
    bool hasShp = false;
    int shpType = 0;
    int nRecords = 0;
    double bbox[4] = {0, 0, 0, 0};
    double zmin = 0, zmax = 0;
    double mmin = 0, mmax = 0;
    bool hasM = false;
    std::string shpRecs;
    std::string shxRecs;
    std::string dbfPrefix;
    std::string dbfRecs;
    std::vector<ShpFieldOut> fields;
    std::string encoding;
};

// update-mode opens run the same FIRST_SHAPE measured-ness adjust as the
// reader: appended records only carry an M section when the target
// layer's first shape has a valid (>-1e38) measure
bool shpFirstRecordMValid(const std::string &recs, int shpType)
{
    bool z = shpType > 10 && shpType < 20;
    bool mCap = z || shpType == 21 || shpType == 23 || shpType == 25 ||
                shpType == 28;
    if (!mCap || recs.size() < 12)
        return false;
    const unsigned char *p = (const unsigned char *)recs.data();
    auto rbe32 = [&](size_t o) {
        return ((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) |
               ((uint32_t)p[o + 2] << 8) | p[o + 3];
    };
    auto rle32 = [&](size_t o) {
        return ((uint32_t)p[o + 3] << 24) | ((uint32_t)p[o + 2] << 16) |
               ((uint32_t)p[o + 1] << 8) | p[o];
    };
    auto rleD = [&](size_t o) {
        double d;
        memcpy(&d, p + o, 8);
        return d;
    };
    size_t len = (size_t)rbe32(4) * 2;
    if (8 + len > recs.size() || len < 4)
        return false;
    const size_t c = 8;
    int t = (int)rle32(c);
    if (t != shpType)
        return false;
    size_t mOff = 0, n = 0;
    if (t == 21 || t == 11)
    {
        mOff = t == 21 ? 20 : 28;
        n = 1;
        if (len < mOff + 8)
            return false;
        return rleD(c + mOff) > -1e38;
    }
    if (t == 18 || t == 28)
    {
        if (len < 40)
            return false;
        n = rle32(c + 36);
        size_t need = 40 + n * 16;
        mOff = need + (z ? 16 + n * 8 : 0) + 16;
    }
    else
    {
        if (len < 44)
            return false;
        size_t nParts = rle32(c + 36);
        n = rle32(c + 40);
        size_t need = 44 + nParts * 4 + n * 16;
        mOff = need + (z ? 16 + n * 8 : 0) + 16;
    }
    if (len < mOff + n * 8)
        return false;
    for (size_t i = 0; i < n; i++)
        if (rleD(c + mOff + i * 8) > -1e38)
            return true;
    return false;
}

bool loadShpAppendData(const std::string &base, const OgrLayer &input,
                       ShpAppendData &ad)
{
    std::string dbf;
    if (!readFileToString(base + ".dbf", dbf) || dbf.size() < 33)
        return false;
    auto u16 = [&](const std::string &s, size_t o) {
        return (unsigned)(unsigned char)s[o] |
               ((unsigned)(unsigned char)s[o + 1] << 8);
    };
    auto u32 = [&](const std::string &s, size_t o) {
        unsigned v = 0;
        for (int i = 3; i >= 0; --i)
            v = (v << 8) | (unsigned char)s[o + i];
        return v;
    };
    unsigned hs = u16(dbf, 8);
    unsigned rs = u16(dbf, 10);
    unsigned nrec = u32(dbf, 4);
    if (hs < 33 || hs > dbf.size())
        return false;
    ad.nRecords = (int)nrec;
    ad.dbfPrefix = dbf.substr(0, hs);
    ad.dbfRecs = dbf.substr(hs, (size_t)nrec * rs);

    // the update-mode open derives the layer encoding like the reader
    {
        std::string codePage;
        std::string cpg;
        if (readFileToString(base + ".cpg", cpg) ||
            readFileToString(base + ".CPG", cpg))
        {
            if (cpg.size() > 31)
                cpg.resize(31);
            codePage = cpg.substr(0, cpg.find_first_of("\r\n"));
        }
        else if ((unsigned char)dbf[29] != 0)
            codePage = strPrintf("LDID/%d", (unsigned char)dbf[29]);
        if (!codePage.empty())
        {
            if (codePage.size() >= 5 &&
                strEqualNoCase(codePage.substr(0, 5), "LDID/"))
                ad.encoding = recodeFromLdid((unsigned char)dbf[29]);
            else
                ad.encoding = recodeFromCpg(codePage);
        }
        if (configIsSet("SHAPE_ENCODING"))
            ad.encoding = configGet("SHAPE_ENCODING");
        if (!ad.encoding.empty() && !recodeSupported(ad.encoding))
            ad.encoding.clear();
    }

    size_t nf = (hs - 33) / 32;
    for (size_t i = 0; i < nf; ++i)
    {
        size_t o = 32 + 32 * i;
        ShpFieldOut fo;
        fo.name = dbf.substr(o, 11).c_str();
        fo.type = dbf[o + 11];
        fo.width = (unsigned char)dbf[o + 16];
        fo.decimals = (unsigned char)dbf[o + 17];
        fo.srcIndex = -1;
        for (size_t k = 0; k < input.fields.size(); ++k)
        {
            std::string n = input.fields[k].name;
            if (!ad.encoding.empty())
                n = cplRecodeSilent(n, "UTF-8", ad.encoding);
            if (n.size() > 10)
                n = n.substr(0, 10);
            if (n == fo.name)
            {
                fo.srcIndex = (int)k;
                break;
            }
        }
        ad.fields.push_back(std::move(fo));
    }
    std::string shp;
    if (readFileToString(base + ".shp", shp) && shp.size() >= 100)
    {
        ad.hasShp = true;
        ad.shpType = (int)u32(shp, 32);
        memcpy(ad.bbox, shp.data() + 36, 32);
        memcpy(&ad.zmin, shp.data() + 68, 8);
        memcpy(&ad.zmax, shp.data() + 76, 8);
        memcpy(&ad.mmin, shp.data() + 84, 8);
        memcpy(&ad.mmax, shp.data() + 92, 8);
        ad.shpRecs = shp.substr(100);
        ad.hasM = shpFirstRecordMValid(ad.shpRecs, ad.shpType);
        std::string shx;
        if (readFileToString(base + ".shx", shx) && shx.size() >= 100)
            ad.shxRecs = shx.substr(100);
    }
    return true;
}

int shpTypeForGeom(int geomType, bool hasZ, bool hasM = false)
{
    int base = 0;
    switch (geomType)
    {
        case 1:
            base = 1;
            break;
        case 2:
        case 5:
        case 8:   // circularstring
        case 9:   // compoundcurve
        case 11:  // multicurve
        case 13:  // curve
            base = 3;
            break;
        case 3:
        case 6:
        case 10:  // curvepolygon
        case 12:  // multisurface
        case 14:  // surface
        case 17:  // triangle
            base = 5;
            break;
        case 4:
            base = 8;
            break;
        case 15:  // polyhedralsurface
        case 16:  // tin
            return 31;
        default:
            return -1;
    }
    return hasZ ? base + 10 : (hasM ? base + 20 : base);
}

// curve containers reaching the shapefile writer only ever hold
// straight-line members (force wrapping), so linearizing is a relabel
// into the matching linear family
void shpLinearizeCurve(OgrGeometry &g)
{
    switch (g.type)
    {
        case 8:
            g.type = 2;
            break;
        case 9:
            if (g.parts.size() == 1)
            {
                bool z = g.hasZ, m = g.hasM;
                OgrGeometry mem = std::move(g.parts[0]);
                mem.type = 2;
                mem.hasZ = z;
                mem.hasM = m;
                g = std::move(mem);
            }
            else
                g.type = 2;
            break;
        case 10:
            g.type = 3;
            break;
        case 11:
            g.type = 5;
            for (OgrGeometry &p : g.parts)
                shpLinearizeCurve(p);
            break;
        case 12:
            g.type = 6;
            for (OgrGeometry &p : g.parts)
                shpLinearizeCurve(p);
            break;
        default:
            break;
    }
}

// container position determines meaning: MultiLineString parts are lines,
// Polygon parts are rings, MultiPolygon parts are polygons
void collectLines(const OgrGeometry &g,
                  std::vector<const OgrGeometry *> &lines)
{
    switch (g.type)
    {
        case 2:
            lines.push_back(&g);
            break;
        case 3:
        case 5:
            for (const auto &p : g.parts)
                lines.push_back(&p);
            break;
        case 6:
        case 7:
            for (const auto &p : g.parts)
                collectLines(p, lines);
            break;
    }
}

void collectPolys(const OgrGeometry &g,
                  std::vector<const OgrGeometry *> &polys)
{
    switch (g.type)
    {
        case 3:
        case 17:
            polys.push_back(&g);
            break;
        case 6:
        case 7:
        case 15:
        case 16:
            for (const auto &p : g.parts)
                collectPolys(p, polys);
            break;
    }
}

bool geomStructEmpty(const OgrGeometry &g)
{
    if (g.empty)
        return true;
    if (g.type == 1)
        return g.coords.size() < 3;
    return g.coords.empty() && g.parts.empty();
}

bool geomAcceptable(const OgrGeometry &g, int kind)
{
    switch (kind)
    {
        case 1:
            return g.type == 1;
        case 8:
            return g.type == 4;
        case 3:
            if (g.type == 2 || g.type == 3 || g.type == 5 || g.type == 6)
                return true;
            if (g.type == 7)
            {
                for (const auto &p : g.parts)
                    if (p.type != 2)
                        return false;
                return true;
            }
            return false;
        case 5:
            if (g.type == 3 || g.type == 6 || g.type >= 15)
                return true;
            if (g.type == 7)
            {
                for (const auto &p : g.parts)
                    if (p.type != 3 && p.type != 6)
                        return false;
                return true;
            }
            return false;
    }
    return false;
}

double ringArea2(const std::vector<double> &c)
{
    double a = 0;
    size_t n = c.size() / 3;
    for (size_t i = 0; i + 1 < n; ++i)
        a += c[i * 3] * c[(i + 1) * 3 + 1] - c[(i + 1) * 3] * c[i * 3 + 1];
    return a;
}

// content payload for one geometry, already validated against shpType
const double kShpNoDataM = -std::numeric_limits<double>::max();

std::string shpRecordContent(const OgrGeometry &g, int shpType,
                             double *bbox, double *zr, bool hasM = false,
                             double *mr = nullptr)
{
    std::string c;
    bool patch = shpType == 31;
    bool z = (shpType > 10 && shpType < 20) || patch;
    std::vector<double> pts;  // packed x,y,z
    std::vector<double> ms;   // parallel per-vertex measures
    std::vector<int> partStarts;
    std::vector<int> partTypes;
    if (shpType % 10 == 1 && !patch)
    {
        put32(c, shpType);
        putD(c, g.coords[0]);
        putD(c, g.coords[1]);
        if (z)
            putD(c, g.hasZ ? g.coords[2] : 0.0);
        double m = g.hasM && !g.m.empty() ? g.m[0] : kShpNoDataM;
        if (hasM)
            putD(c, m);
        bbox[0] = bbox[2] = g.coords[0];
        bbox[1] = bbox[3] = g.coords[1];
        zr[0] = zr[1] = z && g.hasZ ? g.coords[2] : 0.0;
        if (mr)
            mr[0] = mr[1] = hasM ? m : 0.0;
        return c;
    }
    if (shpType % 10 == 8)
    {
        for (const OgrGeometry &l : g.parts)
            if (!l.empty && l.coords.size() >= 3)
            {
                pts.push_back(l.coords[0]);
                pts.push_back(l.coords[1]);
                pts.push_back(l.hasZ ? l.coords[2] : 0.0);
                ms.push_back(l.hasM && !l.m.empty() ? l.m[0]
                                                    : kShpNoDataM);
            }
    }
    else if (shpType % 10 == 3)
    {
        std::vector<const OgrGeometry *> lines;
        collectLines(g, lines);
        for (const OgrGeometry *l : lines)
        {
            if (l->coords.empty())
                continue;
            partStarts.push_back((int)(pts.size() / 3));
            size_t n = l->coords.size() / 3;
            for (size_t i = 0; i < n; ++i)
            {
                pts.push_back(l->coords[i * 3]);
                pts.push_back(l->coords[i * 3 + 1]);
                pts.push_back(l->hasZ ? l->coords[i * 3 + 2] : 0.0);
                ms.push_back(l->hasM && i < l->m.size() ? l->m[i]
                                                        : kShpNoDataM);
            }
        }
    }
    else if (shpType % 10 == 5 || patch)
    {
        // rings from polygon(s): outer CW, holes CCW; close rings
        std::vector<const OgrGeometry *> polys;
        collectPolys(g, polys);
        if (patch && polys.size() == 1 && polyIsTriangle(*polys[0]))
        {
            // a lone closed 4-point ring becomes a 3-point triangle fan
            const OgrGeometry &ring0 = polys[0]->parts[0];
            partStarts.push_back(0);
            partTypes.push_back(1);
            for (size_t i = 0; i < 3; ++i)
            {
                pts.push_back(ring0.coords[i * 3]);
                pts.push_back(ring0.coords[i * 3 + 1]);
                pts.push_back(ring0.hasZ ? ring0.coords[i * 3 + 2] : 0.0);
                ms.push_back(ring0.hasM && i < ring0.m.size()
                                 ? ring0.m[i]
                                 : kShpNoDataM);
            }
            polys.clear();
        }
        for (const OgrGeometry *poly : polys)
        {
            for (size_t r = 0; r < poly->parts.size(); ++r)
            {
                std::vector<double> ring = poly->parts[r].coords;
                size_t n = ring.size() / 3;
                if (n == 0)
                    continue;
                std::vector<double> rm = poly->parts[r].hasM
                                             ? poly->parts[r].m
                                             : std::vector<double>();
                rm.resize(n, kShpNoDataM);
                bool closed = n > 1 && ring[0] == ring[(n - 1) * 3] &&
                              ring[1] == ring[(n - 1) * 3 + 1];
                if (!closed)
                {
                    ring.push_back(ring[0]);
                    ring.push_back(ring[1]);
                    ring.push_back(ring[2]);
                    rm.push_back(rm[0]);
                    n++;
                }
                double a2 = ringArea2(ring);
                bool wantCw = r == 0;
                bool isCw = a2 < 0;
                if (!patch && isCw != wantCw)
                {
                    std::vector<double> rev;
                    for (size_t i = n; i > 0; --i)
                    {
                        rev.push_back(ring[(i - 1) * 3]);
                        rev.push_back(ring[(i - 1) * 3 + 1]);
                        rev.push_back(ring[(i - 1) * 3 + 2]);
                    }
                    ring = rev;
                    std::reverse(rm.begin(), rm.end());
                }
                partStarts.push_back((int)(pts.size() / 3));
                partTypes.push_back(r == 0 ? 2 : 3);
                pts.insert(pts.end(), ring.begin(), ring.end());
                ms.insert(ms.end(), rm.begin(), rm.end());
            }
        }
    }
    size_t npts = pts.size() / 3;
    bbox[0] = bbox[1] = 1e400;
    bbox[2] = bbox[3] = -1e400;
    zr[0] = 1e400;
    zr[1] = -1e400;
    for (size_t i = 0; i < npts; ++i)
    {
        bbox[0] = std::min(bbox[0], pts[i * 3]);
        bbox[1] = std::min(bbox[1], pts[i * 3 + 1]);
        bbox[2] = std::max(bbox[2], pts[i * 3]);
        bbox[3] = std::max(bbox[3], pts[i * 3 + 1]);
        zr[0] = std::min(zr[0], pts[i * 3 + 2]);
        zr[1] = std::max(zr[1], pts[i * 3 + 2]);
    }
    if (npts == 0)
    {
        bbox[0] = bbox[1] = bbox[2] = bbox[3] = 0;
        zr[0] = zr[1] = 0;
    }
    if (!z)
        zr[0] = zr[1] = 0;
    put32(c, shpType);
    putD(c, bbox[0]);
    putD(c, bbox[1]);
    putD(c, bbox[2]);
    putD(c, bbox[3]);
    if (shpType % 10 == 8)
    {
        put32(c, (unsigned)npts);
    }
    else
    {
        put32(c, (unsigned)partStarts.size());
        put32(c, (unsigned)npts);
        for (int p : partStarts)
            put32(c, (unsigned)p);
        if (patch)
            for (int t : partTypes)
                put32(c, (unsigned)t);
    }
    for (size_t i = 0; i < npts; ++i)
    {
        putD(c, pts[i * 3]);
        putD(c, pts[i * 3 + 1]);
    }
    if (z)
    {
        putD(c, zr[0]);
        putD(c, zr[1]);
        for (size_t i = 0; i < npts; ++i)
            putD(c, pts[i * 3 + 2]);
    }
    double mmin = kShpNoDataM, mmax = kShpNoDataM;
    for (size_t i = 0; i < npts; ++i)
    {
        if (i == 0)
            mmin = mmax = ms[i];
        else
        {
            mmin = std::min(mmin, ms[i]);
            mmax = std::max(mmax, ms[i]);
        }
    }
    if (hasM)
    {
        putD(c, mmin);
        putD(c, mmax);
        for (size_t i = 0; i < npts; ++i)
            putD(c, ms[i]);
    }
    if (mr)
    {
        if (!hasM)
            mr[0] = mr[1] = 0.0;
        else
        {
            mr[0] = mmin;
            mr[1] = mmax;
        }
    }
    return c;
}

std::string dbfCellText(const JVal &v, const OgrFieldDefn &src,
                        ShpWriteCtx &ctx)
{
    std::string sval;
    if (v.type == JVal::STRING)
    {
        if (src.type == OFTDateTime)
        {
            OgrDateTime dt;
            if (ogrParseDate(v.s, dt))
                sval = isoDateTime(dt, OFTDateTime);
            else
                sval = v.s;
        }
        else
            sval = v.s;
    }
    else if (v.type == JVal::OBJECT || v.type == JVal::ARRAY)
        sval = ogrJsonSpacedSerialize(v);
    else if (v.type == JVal::BOOL)
        sval = v.b ? "1" : "0";
    else
        serializeCompact(sval, v);
    if (!ctx.encoding.empty())
        sval = cplRecode(sval, "UTF-8", ctx.encoding);
    return sval;
}

long long dbfCellInt(const JVal &v)
{
    return v.type == JVal::INT      ? v.i
           : v.type == JVal::DOUBLE ? (long long)v.d
           : v.type == JVal::BOOL   ? (v.b ? 1 : 0)
                                    : atoll(v.type == JVal::STRING
                                                ? v.s.c_str()
                                                : "0");
}

std::string dbfCell(const ShpFieldOut &f, const OgrFieldValue &fv,
                    ShpWriteCtx &ctx, const OgrFieldDefn &src,
                    long long fid)
{
    std::string cell;
    bool isNull = !fv.set || fv.v.type == JVal::NUL;
    const JVal &v = fv.v;
    switch (f.type)
    {
        case 'N':
            if (isNull)
                return std::string((size_t)f.width, '*');
            if (f.decimals > 0)
            {
                double d = v.type == JVal::INT      ? (double)v.i
                           : v.type == JVal::DOUBLE ? v.d
                           : v.type == JVal::BOOL   ? (v.b ? 1 : 0)
                           : atof(v.type == JVal::STRING ? v.s.c_str()
                                                         : "0");
                cell = strPrintf("%*.*f", f.width, f.decimals, d);
                if ((int)cell.size() > f.width)
                {
                    // xbase writer drops decimals to make the value fit
                    // and only reports failure when even the integer
                    // part overflows
                    int intLen = (int)strPrintf("%.0f", d).size();
                    if (intLen <= f.width)
                    {
                        int newDec = f.width - intLen - 1;
                        if (newDec > f.decimals)
                            newDec = f.decimals;
                        if (newDec < 0)
                            newDec = 0;
                        cell = strPrintf("%*.*f", f.width, newDec, d);
                    }
                    else
                        cplErrorStr(
                            CE_Warning, CPLE_AppDefined,
                            "Value " + strPrintf("%.17g", d) +
                                " of field " + src.name + " of feature " +
                                std::to_string(fid) +
                                " not successfully written. Possibly due "
                                "to too larger number with respect to "
                                "field width");
                }
            }
            else
                cell = strPrintf("%*lld", f.width, dbfCellInt(v));
            if ((int)cell.size() > f.width)
                cell = cell.substr(0, (size_t)f.width);
            return cell;
        case 'L':
            if (isNull)
                return "?";
            if (v.type == JVal::BOOL)
                return v.b ? "T" : "F";
            if (v.type == JVal::INT)
                return v.i ? "T" : "F";
            return "?";
        case 'D':
        {
            if (isNull)
                return "00000000";
            OgrDateTime dt;
            std::string raw = v.type == JVal::STRING ? v.s : "";
            if (ogrParseDate(raw, dt))
                return strPrintf("%04d%02d%02d", dt.year, dt.month, dt.day);
            return "00000000";
        }
        default:
        {
            if (isNull)
                return std::string((size_t)f.width, ' ');
            std::string sval = dbfCellText(v, src, ctx);
            if ((int)sval.size() > f.width)
                sval = sval.substr(0, (size_t)f.width);
            sval.resize((size_t)f.width, ' ');
            return sval;
        }
    }
}

// the xbase writer widens integer and string columns to fit the widest
// value it is asked to store (readers then see e.g. a 10-wide N column
// as Integer64), so field widths resolve against the data up front
void shpGrowFieldWidths(const OgrLayer &lyr, ShpWriteCtx &ctx)
{
    for (ShpFieldOut &fo : ctx.fields)
    {
        if (fo.srcIndex < 0 ||
            (size_t)fo.srcIndex >= lyr.fields.size())
            continue;
        bool intCol = fo.type == 'N' && fo.decimals == 0;
        bool strCol = fo.type == 'C';
        if (!intCol && !strCol)
            continue;
        const OgrFieldDefn &src = lyr.fields[fo.srcIndex];
        for (const OgrFeature &ft : lyr.features)
        {
            if ((size_t)fo.srcIndex >= ft.values.size())
                continue;
            const OgrFieldValue &fv = ft.values[fo.srcIndex];
            if (!fv.set || fv.v.type == JVal::NUL)
                continue;
            int len = intCol
                          ? (int)strPrintf("%lld", dbfCellInt(fv.v))
                                .size()
                          : (int)dbfCellText(fv.v, src, ctx).size();
            if (len > fo.width)
                fo.width = len;
        }
    }
}

// 0: convert wording; 1: create wording, '--field' defs (trailing period);
// 2: create wording, schema/like defs (no trailing period)
int g_shpCreateErrMode = 0;
std::string g_shpFailedField;

bool shpCreateFields(const OgrLayer &lyr, ShpWriteCtx &ctx, bool quiet,
                     bool widenReal)
{
    for (size_t i = 0; i < lyr.fields.size(); ++i)
    {
        const OgrFieldDefn &f = lyr.fields[i];
        ShpFieldOut o;
        o.srcIndex = (int)i;
        o.name = f.name;
        if (!ctx.encoding.empty())
        {
            recodeClearWarnFlags();
            bool failed = false;
            cplPushQuietHandler();
            std::string rec = cplRecode(f.name, "UTF-8", ctx.encoding,
                                        &failed);
            cplPopHandler();
            if (failed)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to create field name '" + f.name +
                                "': cannot convert to " + ctx.encoding);
                return false;
            }
            o.name = rec;
        }
        if (o.name.size() > 10)
            o.name = o.name.substr(0, 10);
        switch (f.type)
        {
            case OFTInteger:
                if (f.subType == OFSTBoolean)
                {
                    o.type = 'L';
                    o.width = 1;
                }
                else
                {
                    o.type = 'N';
                    o.width = f.width > 0 ? f.width : 9;
                }
                break;
            case OFTInteger64:
                o.type = 'N';
                o.width = f.width > 0 ? f.width : 18;
                break;
            case OFTReal:
                o.type = 'N';
                if (f.width > 0)
                {
                    // a layer filter routes the pipeline through a
                    // non-shapefile intermediate, and the translator then
                    // grows explicit real widths by two
                    o.width = f.width + (widenReal ? 2 : 0);
                    o.decimals = f.precision;
                }
                else
                {
                    o.width = 24;
                    o.decimals = 15;
                }
                break;
            case OFTString:
                o.type = 'C';
                o.width = f.width > 0 ? f.width : 80;
                break;
            case OFTDate:
                o.type = 'D';
                o.width = 8;
                break;
            case OFTDateTime:
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "Field " + f.name +
                                " created as String field, though DateTime "
                                "requested.");
                o.type = 'C';
                o.width = 29;
                break;
            default:
            {
                std::string tn = ogrFieldTypeName(f.type);
                if (!quiet)
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        "The output driver does not natively support " +
                            tn + " type for field " + f.name +
                            ". Misconversion can happen. -mapFieldType "
                            "can be used to control field type "
                            "conversion.");
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "Can't create fields of type " + tn +
                                " on shapefile layers.");
                g_shpFailedField = f.name;
                return false;
            }
        }
        ctx.fields.push_back(std::move(o));
    }
    return true;
}

void shpFinalize(const std::string &base, const OgrLayer &lyr,
                 ShpWriteCtx &ctx)
{
    bool geomless = lyr.geomType == 101 || ctx.geomless;
    // unresolved layers keep the driver's arc default
    int headerType = ctx.typeResolved ? ctx.shpType : 3;
    double *bb = ctx.bbox;
    double zmin = ctx.zmin;
    double zmax = ctx.zmax;
    auto header = [&](size_t totalBytes) {
        std::string h;
        put32be(h, 9994);
        for (int i = 0; i < 5; ++i)
            put32be(h, 0);
        put32be(h, (unsigned)(totalBytes / 2));
        put32(h, 1000);
        put32(h, (unsigned)headerType);
        putD(h, bb[0]);
        putD(h, bb[1]);
        putD(h, bb[2]);
        putD(h, bb[3]);
        putD(h, zmin);
        putD(h, zmax);
        putD(h, ctx.mmin);
        putD(h, ctx.mmax);
        return h;
    };
    if (ctx.appendMode)
    {
        if (ctx.appendHasShp)
        {
            writeStringToFile(base + ".shp",
                              header(100 + ctx.recs.size()) + ctx.recs);
            writeStringToFile(base + ".shx",
                              header(100 + ctx.shxRecs.size()) +
                                  ctx.shxRecs);
        }
        std::string dbf = ctx.dbfPrefix;
        time_t at = time(nullptr);
        struct tm atm;
        localtime_r(&at, &atm);
        dbf[1] = (char)(atm.tm_year);
        dbf[2] = (char)(atm.tm_mon + 1);
        dbf[3] = (char)(atm.tm_mday);
        dbf[4] = (char)(ctx.nRecords & 0xff);
        dbf[5] = (char)((ctx.nRecords >> 8) & 0xff);
        dbf[6] = (char)((ctx.nRecords >> 16) & 0xff);
        dbf[7] = (char)((ctx.nRecords >> 24) & 0xff);
        dbf += ctx.dbfRecs;
        dbf += (char)0x1A;
        writeStringToFile(base + ".dbf", dbf);
        return;
    }
    if (!geomless)
    {
        writeStringToFile(base + ".shp",
                          header(100 + ctx.recs.size()) + ctx.recs);
        writeStringToFile(base + ".shx",
                          header(100 + ctx.shxRecs.size()) + ctx.shxRecs);
    }

    // dbf
    std::string dbf;
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    dbf += (char)0x03;
    dbf += (char)(tmv.tm_year);
    dbf += (char)(tmv.tm_mon + 1);
    dbf += (char)(tmv.tm_mday);
    put32(dbf, (unsigned)ctx.nRecords);
    unsigned hs = 32 + 32 * (unsigned)ctx.fields.size() + 1;
    unsigned rs = 1;
    for (const auto &f : ctx.fields)
        rs += (unsigned)f.width;
    put16(dbf, hs);
    put16(dbf, rs);
    for (int i = 0; i < 17; ++i)
        dbf += '\0';
    // an explicit ENCODING lco zeroes the LDID and lands in a .cpg
    dbf += ctx.haveCpg ? '\0' : (char)0x57;
    dbf += '\0';
    dbf += '\0';
    for (const auto &f : ctx.fields)
    {
        std::string d = f.name;
        d.resize(11, '\0');
        d += f.type;
        d.append(4, '\0');
        d += (char)f.width;
        d += (char)f.decimals;
        d.append(14, '\0');
        dbf += d;
    }
    dbf += (char)0x0D;
    dbf += ctx.dbfRecs;
    dbf += (char)0x1A;
    writeStringToFile(base + ".dbf", dbf);
    if (ctx.haveCpg)
        writeStringToFile(base + ".cpg", ctx.cpgContent);

    if (lyr.hasSrs)
    {
        std::string wkt = lyr.srs.wkt1Esri();
        if (!wkt.empty())
            writeStringToFile(base + ".prj", wkt);
    }
    else
        remove((base + ".prj").c_str());
}

const char *kOgrGeomUpper[] = {
    "UNKNOWN",        "POINT",         "LINESTRING",
    "POLYGON",        "MULTIPOINT",    "MULTILINESTRING",
    "MULTIPOLYGON",   "GEOMETRYCOLLECTION",
    "CIRCULARSTRING", "COMPOUNDCURVE", "CURVEPOLYGON",
    "MULTICURVE",     "MULTISURFACE",  "CURVE",
    "SURFACE",        "POLYHEDRALSURFACE",
    "TIN",            "TRIANGLE"};

bool writeShapefile(const OgrLayer &lyr, const std::string &outPath,
                    bool quiet, const ProgressSpan &ps, bool &layerFailed,
                    const ShpAppendData *ap = nullptr,
                    bool widenReal = false, QixData *qixOut = nullptr,
                    int shptType = -1, bool shptM = false,
                    bool shptNone = false,
                    const std::string &encoding = "ISO-8859-1",
                    const std::string *cpg = nullptr,
                    bool skipErrors = false)
{
    layerFailed = false;
    std::string base = outPath;
    size_t dot = base.find_last_of('.');
    size_t slash = base.find_last_of('/');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash))
        base = base.substr(0, dot);

    if (!ap && lyr.geomType == 7 && shptType < 0 && !shptNone)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Geometry type of `" +
                        ogrGeomTypeName(lyr.geomType, lyr.geomHasZ) +
                        "' not supported in shapefiles.  Type can be "
                        "overridden with a layer creation option of "
                        "SHPT=POINT/ARC/POLYGON/MULTIPOINT/POINTZ/ARCZ/"
                        "POLYGONZ/MULTIPOINTZ/MULTIPATCH.");
        // --skip-errors drops the layer quietly: no follow-up error,
        // no files, the run completes
        if (skipErrors)
            return true;
        if (g_shpCreateErrMode != 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot create layer '" + lyr.name +
                            "'.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot create layer '" + lyr.name +
                            "'" + (g_shpCreateErrMode == 1 ? "." : ""));
        }
        else
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + lyr.name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
        layerFailed = true;
        return false;
    }

    if (!ap && strncmp(outPath.c_str(), "/vsizip/", 8) == 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Read-write random access not supported for /vsizip");
        const int probe = vsiZipWriteProbe(outPath);
        if (probe == 1)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Random access not supported for writable file in "
                        "/vsizip");
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to create file " + base +
                        (lyr.geomType == 101 || shptNone ? ".dbf"
                                                         : ".shp") +
                        ": Success");
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to write layer '" + lyr.name +
                        "'. Use --skip-errors to ignore errors and "
                        "continue writing.");
        if (probe == 1)
            vsiZipTouchArchive(outPath);
        layerFailed = true;
        return false;
    }

    if (!ap && strncmp(outPath.c_str(), "/vsitar/", 8) == 0)
    {
        // one refused write-open per shapelib file handle (.shp+.shx,
        // or the lone .dbf for geometryless layers)
        const bool dbfOnly = lyr.geomType == 101 || shptNone;
        for (int i = 0; i < (dbfOnly ? 1 : 2); ++i)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Only read-only mode is supported for /vsitar");
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to create file " + base +
                        (dbfOnly ? ".dbf" : ".shp") + ": " +
                        (vsiTarWriteArchiveMissing(outPath)
                             ? "No such file or directory"
                             : "Success"));
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Failed to write layer '" + lyr.name +
                        "'. Use --skip-errors to ignore errors and "
                        "continue writing.");
        layerFailed = true;
        return false;
    }

    if (!ap)
    {
        std::string dir = base;
        size_t sl = dir.find_last_of('/');
        dir = sl == std::string::npos ? "." : dir.substr(0, sl);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to create file " + base +
                            (lyr.geomType == 101 || shptNone ? ".dbf"
                                                             : ".shp") +
                            ": No such file or directory");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + lyr.name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            layerFailed = true;
            return false;
        }
    }

    if (!ap)
    {
        OgrLayer renamed;
        if (renameDupFields(lyr, renamed, true))
            return writeShapefile(renamed, outPath, quiet, ps,
                                  layerFailed, ap, widenReal, qixOut,
                                  shptType, shptM, shptNone, encoding,
                                  cpg, skipErrors);
    }

    ShpWriteCtx ctx;
    ctx.encoding = ap ? ap->encoding : encoding;
    if (!ap && cpg)
    {
        ctx.haveCpg = true;
        ctx.cpgContent = *cpg;
    }
    if (ap)
    {
        ctx.appendMode = true;
        ctx.appendHasShp = ap->hasShp;
        ctx.dbfPrefix = ap->dbfPrefix;
        ctx.dbfRecs = ap->dbfRecs;
        ctx.nRecords = ap->nRecords;
        ctx.fields = ap->fields;
        if (ap->hasShp)
        {
            ctx.shpType = ap->shpType;
            ctx.typeResolved = true;
            ctx.hasM = ap->hasM;
            ctx.recs = ap->shpRecs;
            ctx.shxRecs = ap->shxRecs;
            if (ap->nRecords > 0)
            {
                ctx.boundsInit = true;
                memcpy(ctx.bbox, ap->bbox, sizeof ctx.bbox);
                ctx.zmin = ap->zmin;
                ctx.zmax = ap->zmax;
                ctx.mmin = ap->mmin;
                ctx.mmax = ap->mmax;
            }
        }
    }
    else if (shptNone)
        ctx.geomless = true;
    else if (shptType >= 0)
    {
        ctx.shpType = shptType;
        ctx.typeResolved = true;
        ctx.hasM = shptM;
    }
    else if (shpTypeForGeom(lyr.geomType, lyr.geomHasZ, lyr.geomHasM) >= 0)
    {
        ctx.shpType =
            shpTypeForGeom(lyr.geomType, lyr.geomHasZ, lyr.geomHasM);
        ctx.typeResolved = true;
        ctx.hasM = lyr.geomHasM;
    }
    else if (lyr.shpPinType && lyr.geomType == 0)
    {
        ctx.shpType = lyr.geomHasZ ? 13 : 3;
        ctx.typeResolved = true;
        ctx.hasM = lyr.geomHasM;
    }
    if (!ap && !shpCreateFields(lyr, ctx, quiet, widenReal))
    {
        shpFinalize(base, lyr, ctx);
        if (qixOut && lyr.geomType != 101 && !ctx.geomless)
            qixOut->filesWritten = true;
        if (g_shpCreateErrMode)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot create field '" +
                            g_shpFailedField + "' in layer '" + lyr.name +
                            "'.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot create layer '" + lyr.name + "'" +
                            (g_shpCreateErrMode == 1 ? "." : ""));
        }
        else
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + lyr.name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
        layerFailed = true;
        return false;
    }
    if (!ap)
        shpGrowFieldWidths(lyr, ctx);

    size_t total = lyr.features.size();
    for (size_t i = 0; i < total; ++i)
    {
        const OgrFeature &feat = lyr.features[i];
        seqTick(lyr);
        if (g_convertFeatureHook)
            g_convertFeatureHook(lyr,
                                 const_cast<OgrFeature &>(feat));
        std::string content;
        double bb[4] = {0, 0, 0, 0}, zr[2] = {0, 0}, mrec[2] = {0, 0};
        bool haveGeom = !ctx.geomless && feat.hasGeom &&
                        !geomStructEmpty(feat.geom);
        OgrGeometry linGeom;
        const OgrGeometry *wgeom = &feat.geom;
        if (haveGeom && feat.geom.type >= 8 && feat.geom.type <= 12)
        {
            if (!ctx.curveWarned && !quiet)
            {
                ctx.curveWarned = true;
                std::string lname = base;
                size_t sl = lname.find_last_of('/');
                if (sl != std::string::npos)
                    lname = lname.substr(sl + 1);
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Attempt to write curve geometries to layer " +
                                lname +
                                " that does not support them. They will "
                                "be linearized");
            }
            linGeom = feat.geom;
            shpLinearizeCurve(linGeom);
            wgeom = &linGeom;
        }
        if (haveGeom)
        {
            if (!ctx.typeResolved)
            {
                // an Unknown layer decl defers to the first
                // geometry-bearing feature, whose OWN dims pick the
                // shape type and measured-ness (decl Z/M flags ignored)
                int t = shpTypeForGeom(wgeom->type, wgeom->hasZ,
                                       wgeom->hasM);
                if (t >= 0)
                {
                    ctx.shpType = t;
                    ctx.typeResolved = true;
                    ctx.hasM = wgeom->hasM;
                }
            }
            // layers of unknown type behave as arc until resolved
            int want = ctx.typeResolved ? ctx.shpType : 3;
            bool acceptable =
                want == 31 ? wgeom->type == 3 || wgeom->type == 6 ||
                                 wgeom->type >= 15
                           : geomAcceptable(*wgeom, want % 10);
            if (!acceptable)
            {
                std::string gname = kOgrGeomUpper[wgeom->type];
                std::string msg;
                switch (want == 31 ? 0 : want % 10)
                {
                    case 1:
                        msg = "Attempt to write non-point (" + gname +
                              ") geometry to point shapefile.";
                        break;
                    case 8:
                        msg = "Attempt to write non-multipoint (" + gname +
                              ") geometry to multipoint shapefile.";
                        break;
                    case 3:
                        msg = "Attempt to write non-linestring (" + gname +
                              ") geometry to ARC type shapefile.";
                        break;
                    case 5:
                        msg = "Attempt to write non-polygon (" + gname +
                              ") geometry to POLYGON type shapefile.";
                        break;
                }
                if (!msg.empty())
                    cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
                if (skipErrors)
                    continue;
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("Unable to write feature %lld from "
                                      "layer %s.",
                                      feat.fid, lyr.name.c_str()));
                shpFinalize(base, lyr, ctx);
                if (qixOut && lyr.geomType != 101 && !ctx.geomless)
                    qixOut->filesWritten = true;
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to write layer '" + lyr.name +
                                "'. Use --skip-errors to ignore errors and "
                                "continue writing.");
                layerFailed = true;
                return false;
            }
            if (!ctx.typeResolved)
            {
                ctx.shpType = wgeom->hasZ   ? 13
                              : wgeom->hasM ? 23
                                            : 3;
                ctx.typeResolved = true;
                ctx.hasM = wgeom->hasM;
            }
            content = shpRecordContent(*wgeom, ctx.shpType, bb, zr,
                                       ctx.hasM, mrec);
        }
        else
            put32(content, 0);  // null shape

        unsigned offWords = (unsigned)((100 + ctx.recs.size()) / 2);
        put32be(ctx.shxRecs, offWords);
        put32be(ctx.shxRecs, (unsigned)(content.size() / 2));
        put32be(ctx.recs, (unsigned)(ctx.nRecords + 1));
        put32be(ctx.recs, (unsigned)(content.size() / 2));
        ctx.recs += content;

        // shplib initializes file bounds at the first record: from its
        // first vertex when it has any, otherwise to zeros
        if (!ctx.boundsInit)
        {
            ctx.boundsInit = true;
            if (haveGeom)
            {
                memcpy(ctx.bbox, bb, sizeof bb);
                ctx.zmin = zr[0];
                ctx.zmax = zr[1];
                ctx.mmin = mrec[0];
                ctx.mmax = mrec[1];
            }
        }
        else if (haveGeom)
        {
            ctx.bbox[0] = std::min(ctx.bbox[0], bb[0]);
            ctx.bbox[1] = std::min(ctx.bbox[1], bb[1]);
            ctx.bbox[2] = std::max(ctx.bbox[2], bb[2]);
            ctx.bbox[3] = std::max(ctx.bbox[3], bb[3]);
            ctx.zmin = std::min(ctx.zmin, zr[0]);
            ctx.zmax = std::max(ctx.zmax, zr[1]);
            ctx.mmin = std::min(ctx.mmin, mrec[0]);
            ctx.mmax = std::max(ctx.mmax, mrec[1]);
        }
        if (qixOut)
        {
            QixShape sh;
            sh.id = ctx.nRecords;
            if (haveGeom)
                memcpy(sh.env, bb, sizeof sh.env);
            else
                memset(sh.env, 0, sizeof sh.env);
            qixOut->shapes.push_back(sh);
            ++qixOut->total;
            memcpy(qixOut->fileBox, ctx.bbox, sizeof qixOut->fileBox);
        }

        if (ctx.nRecords == 0 && ctx.fields.empty())
        {
            ShpFieldOut fid;
            fid.name = "FID";
            fid.type = 'N';
            fid.width = 11;
            fid.srcIndex = -2;
            ctx.fields.push_back(fid);
        }
        std::string rec = " ";
        for (const auto &fo : ctx.fields)
        {
            if (fo.srcIndex == -2)
            {
                rec += strPrintf("%11d", ctx.nRecords);
                continue;
            }
            OgrFieldValue empty;
            OgrFieldDefn defaultDefn;
            bool haveSrc = fo.srcIndex >= 0 &&
                           (size_t)fo.srcIndex < feat.values.size();
            const OgrFieldValue &fv =
                haveSrc ? feat.values[fo.srcIndex] : empty;
            const OgrFieldDefn &defn = fo.srcIndex >= 0
                                           ? lyr.fields[fo.srcIndex]
                                           : defaultDefn;
            rec += dbfCell(fo, fv, ctx, defn, feat.fid);
        }
        ctx.dbfRecs += rec;
        ctx.nRecords++;

        ps.update((double)(i + 1) / (double)total);
    }
    shpFinalize(base, lyr, ctx);
                if (qixOut && lyr.geomType != 101 && !ctx.geomless)
                    qixOut->filesWritten = true;
    return true;
}

bool fileExistsCv(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

void warnUnsupportedCo(const std::string &driver,
                       const std::vector<std::string> &co)
{
    for (const auto &kv : co)
        cplErrorStr(CE_Warning, CPLE_NotSupported,
                    "driver " + driver +
                        " does not support creation option " +
                        kv.substr(0, kv.find('=')));
}

bool lcoSupported(const std::string &driver, const std::string &key)
{
    static const char *geojsonLCO[] = {
        "WRITE_BBOX",     "COORDINATE_PRECISION",
        "SIGNIFICANT_FIGURES", "NATIVE_DATA",
        "NATIVE_MEDIA_TYPE",   "RFC7946",
        "WRAPDATELINE",   "WRITE_NAME",
        "DESCRIPTION",    "ID_FIELD",
        "ID_TYPE",        "ID_GENERATE",
        "WRITE_NON_FINITE_VALUES",
        "AUTODETECT_JSON_STRINGS", "FOREIGN_MEMBERS_FEATURE",
        "FOREIGN_MEMBERS_COLLECTION"};
    static const char *shapefileLCO[] = {
        "SHPT",          "2GB_LIMIT",  "ENCODING",
        "RESIZE",        "SPATIAL_INDEX", "DBF_DATE_LAST_UPDATE",
        "AUTO_REPACK",   "DBF_EOF_CHAR"};
    static const char *memLCO[] = {"FID", "ADVERTIZE_UTF8"};
    static const char *geojsonSeqLCO[] = {
        "RS",       "COORDINATE_PRECISION", "SIGNIFICANT_FIGURES",
        "ID_FIELD", "ID_TYPE",              "WRITE_BBOX"};
    const char *const *list = nullptr;
    size_t n = 0;
    if (driver == "GeoJSON")
    {
        list = geojsonLCO;
        n = sizeof(geojsonLCO) / sizeof(*geojsonLCO);
    }
    else if (driver == "GeoJSONSeq")
    {
        list = geojsonSeqLCO;
        n = sizeof(geojsonSeqLCO) / sizeof(*geojsonSeqLCO);
    }
    else if (driver == "ESRI Shapefile")
    {
        list = shapefileLCO;
        n = sizeof(shapefileLCO) / sizeof(*shapefileLCO);
    }
    else if (driver == "MEM")
    {
        list = memLCO;
        n = sizeof(memLCO) / sizeof(*memLCO);
    }
    for (size_t i = 0; i < n; i++)
        if (strEqualNoCase(key, list[i]))
            return true;
    return false;
}

struct LcoTypeInfo
{
    const char *name;
    const char *type;
    const char *values;  // '|'-separated for string-select
};

const LcoTypeInfo *lcoTypeInfo(const std::string &driver,
                               const std::string &key)
{
    static const LcoTypeInfo shp[] = {
        {"SHPT", "string-select",
         "POINT|ARC|POLYGON|MULTIPOINT|POINTZ|ARCZ|POLYGONZ|MULTIPOINTZ|"
         "POINTM|ARCM|POLYGONM|MULTIPOINTM|POINTZM|ARCZM|POLYGONZM|"
         "MULTIPOINTZM|MULTIPATCH|NONE|NULL"},
        {"2GB_LIMIT", "boolean", ""},
        {"RESIZE", "boolean", ""},
        {"SPATIAL_INDEX", "boolean", ""},
        {"AUTO_REPACK", "boolean", ""},
        {"DBF_EOF_CHAR", "boolean", ""},
    };
    static const LcoTypeInfo gj[] = {
        {"WRITE_BBOX", "boolean", ""},
        {"COORDINATE_PRECISION", "int", ""},
        {"SIGNIFICANT_FIGURES", "int", ""},
        {"RFC7946", "boolean", ""},
        {"WRAPDATELINE", "boolean", ""},
        {"WRITE_NAME", "boolean", ""},
        {"ID_TYPE", "string-select", "AUTO|String|Integer"},
        {"ID_GENERATE", "boolean", ""},
        {"WRITE_NON_FINITE_VALUES", "boolean", ""},
        {"AUTODETECT_JSON_STRINGS", "boolean", ""},
    };
    static const LcoTypeInfo gjs[] = {
        {"RS", "boolean", ""},
        {"COORDINATE_PRECISION", "int", ""},
        {"SIGNIFICANT_FIGURES", "int", ""},
        {"ID_TYPE", "string-select", "AUTO|String|Integer"},
        {"WRITE_BBOX", "boolean", ""},
    };
    const LcoTypeInfo *list = nullptr;
    size_t n = 0;
    if (driver == "ESRI Shapefile")
    {
        list = shp;
        n = sizeof(shp) / sizeof(*shp);
    }
    else if (driver == "GeoJSON")
    {
        list = gj;
        n = sizeof(gj) / sizeof(*gj);
    }
    else if (driver == "GeoJSONSeq")
    {
        list = gjs;
        n = sizeof(gjs) / sizeof(*gjs);
    }
    for (size_t i = 0; i < n; ++i)
        if (strEqualNoCase(key, list[i].name))
            return &list[i];
    return nullptr;
}

bool lcoValueOk(const LcoTypeInfo &ti, const std::string &val)
{
    std::string t = ti.type;
    if (t == "boolean")
    {
        static const char *ok[] = {"ON",   "OFF",  "YES",
                                   "NO",   "TRUE", "FALSE"};
        for (const char *o : ok)
            if (strEqualNoCase(val, o))
                return true;
        return false;
    }
    if (t == "int")
    {
        size_t i = 0;
        while (i < val.size() && val[i] == ' ')
            ++i;
        if (i < val.size() && (val[i] == '+' || val[i] == '-'))
            ++i;
        while (i < val.size() && isdigit((unsigned char)val[i]))
            ++i;
        while (i < val.size() && val[i] == ' ')
            ++i;
        return i == val.size();
    }
    if (t == "string-select")
    {
        std::string list = ti.values;
        size_t pos = 0;
        while (pos <= list.size())
        {
            size_t bar = list.find('|', pos);
            std::string item = list.substr(
                pos, bar == std::string::npos ? std::string::npos
                                              : bar - pos);
            if (strEqualNoCase(val, item))
                return true;
            if (bar == std::string::npos)
                break;
            pos = bar + 1;
        }
        return false;
    }
    return true;
}

void warnUnsupportedLco(const std::string &driver, const std::string &dsName,
                        const std::vector<std::string> &lco)
{
    for (const auto &kv : lco)
    {
        size_t eq = kv.find('=');
        std::string key = kv.substr(0, eq);
        if (!lcoSupported(driver, key))
        {
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "dataset " + dsName +
                            " does not support layer creation option " +
                            key);
            continue;
        }
        if (eq == std::string::npos)
            continue;
        std::string val = kv.substr(eq + 1);
        const LcoTypeInfo *ti = lcoTypeInfo(driver, key);
        if (ti && !lcoValueOk(*ti, val))
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "'" + val + "' is an unexpected value for " + key +
                            " layer creation option of type " + ti->type +
                            ".");
    }
}

void warnLcoIgnoredOnAppend(const std::vector<std::string> &lco)
{
    if (!lco.empty())
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "Layer creation options ignored since an existing "
                    "layer is\n         being appended to.");
}

// emitted after the failure's errors; stdout data writes keep the
// stream clean of progress. Middle writes run with success-progress
// suppressed yet still fake-complete the shared line on failure.
void pipelineFailProgress(bool quiet, const std::string &output)
{
    if (output == "/vsistdout/" && !g_dashStdout && !g_pipelineBarStdout)
        return;
    if (g_pipelineMode && (!quiet || g_pipelineFailProgressForce))
    {
        TermProgress tp;
        tp.update(1.0);
    }
}

// non-terminal pipeline writes pass the still-open written dataset to the
// next step; per driver, only parts of it are readable again
void captureWrittenGeoJson(const OgrLayer &lyr, const std::string &name,
                           const std::string &output, bool seq)
{
    if (!g_convertCaptureWritten)
        return;
    auto w = std::make_unique<OgrDataset>();
    w->path = output;
    w->driverShort = seq ? "GeoJSONSeq" : "GeoJSON";
    w->driverLong = seq ? "GeoJSON Sequence" : "GeoJSON";
    w->capturedStream = !seq;
    OgrLayer nl;
    nl.name = name;
    if (seq)
    {
        nl.geomType = 0;
        nl.hasSrs = lyr.hasSrs;
        if (lyr.hasSrs)
            nl.srs = lyr.srs.clone();
        nl.features = lyr.features;
    }
    else
    {
        nl.geomType = lyr.geomType;
        nl.geomHasZ = lyr.geomHasZ;
        nl.hasSrs = false;
    }
    nl.fields = lyr.fields;
    double env[4] = {0, 0, 0, 0};
    bool anyPt = false;
    for (const auto &feat : lyr.features)
        if (feat.hasGeom)
            envAccumGeom(feat.geom, env, anyPt);
    nl.hasExtent = anyPt;
    if (anyPt)
        for (int k = 0; k < 4; ++k)
            nl.extent[k] = env[k];
    w->layers.push_back(std::move(nl));
    g_convertWrittenDs = std::move(w);
}

int vectorConvertHandler(const CmdSpec &, ParseResult &r)
{
    g_convertLayerWriteFailed = false;
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool fAppend = r.flag("append");
    bool fUpdate = r.flag("update");
    bool fOverLayer = r.flag("overwrite-layer");
    bool fUpsert = r.flag("upsert");
    bool overwrite = r.flag("overwrite") || fOverLayer || fAppend ||
                     fUpdate || fUpsert;
    std::string format = r.str("output-format");
    std::vector<std::string> layerFilter = r.list("input-layer");
    std::string outLayerName = r.str("output-layer");

    std::string driver;
    {
        std::string ferr = vectorOutputDriverResolve(format, driver);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(true, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }

    std::unique_ptr<OgrDataset> preTarget;
    // declared before the close guards below so they can inspect it
    // safely when the scope unwinds
    std::unique_ptr<OgrDataset> target;

    // the source opens first: open failures and the output-exists
    // refusal are both validation errors, reported together before a
    // single usage block
    std::string err;
    bool openFailed = false;
    auto ds = g_convertSourceOverride
                  ? std::move(g_convertSourceOverride)
                  : openVectorDataset(input, err, r.list("input-format"),
                                      r.list("open-option"));
    if (!ds)
    {
        bool concatCaller = convertMsgPrefix() == "concat";
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported file "
                            "format.");
        if (concatCaller)
            return 1;
        openFailed = true;
    }

    // debug close traces: the source dataset closes before the output
    // on success; failed translations close the output first (the
    // sections fire the dst guard early on their error paths)
    struct DbgGuard
    {
        std::function<void()> fn;
        ~DbgGuard()
        {
            if (fn)
                fn();
        }
    };
    DbgGuard dstGuard;
    DbgGuard srcGuard;
    srcGuard.fn = [&ds] {
        if (ds)
            vectorDebugClose(*ds);
    };
    auto dstCloseFirst = [&dstGuard] {
        if (dstGuard.fn)
        {
            dstGuard.fn();
            dstGuard.fn = nullptr;
        }
    };

    bool gdalgOutput = driver == "GDALG" ||
                       strEndsWith(strToLower(output), ".gdalg.json");
    bool existsFailed = false;
    bool needExisting = fUpdate || fUpsert || fOverLayer;
    if (needExisting)
    {
        // update-family opens the output during validation; its errors
        // group with the source-open failure before a single usage block
        if (!fileExistsCv(output))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(output));
            existsFailed = true;
        }
        else
        {
            std::string terr;
            target = openVectorDataset(output, terr, {}, {}, false);
            if (!target)
            {
                if (outputExistsKind(output) == "Directory")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                output + ": Is a directory");
                else if (terr != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + output +
                                    "' not recognized as being in a "
                                    "supported file format.");
                existsFailed = true;
            }
            else
            {
                ogrFlushPendingDebug(*target);
                dstGuard.fn = [&target] {
                    if (target)
                        vectorDebugClose(*target);
                };
            }
        }
    }
    else if (driver != "MEM" && driver != "stream" && !gdalgOutput)
    {
        if (fileExistsCv(output) && !overwrite)
        {
            bool isDs = false;
            {
                std::string e2;
                cplPushQuietHandler();
                auto d2 = openVectorDataset(output, e2, {});
                cplPopHandler();
                isDs = d2 != nullptr;
            }
            if (!isDs)
                isDs = datasetIdentify(output, {"raster"});
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " +
                            (isDs ? "Dataset" : "File") + " '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--overwrite-layer/--append/"
                            "--update option.");
            existsFailed = true;
        }
    }

    // --overwrite deletes the destination fileset before anything else
    // (even when the source open failed)
    bool pureOverwrite = r.flag("overwrite") && !fUpdate && !fUpsert &&
                         !fOverLayer && !fAppend;
    if (pureOverwrite && driver != "MEM" && driver != "stream" &&
        fileExistsCv(output))
        overwriteDeleteFileset(output);

    if (openFailed || existsFailed)
    {
        handlerPrintUsage();
        return 1;
    }

    // conversion warnings only surface when the writer pulls features:
    // a streamed output nobody consumes and an unresolvable driver both
    // stay silent
    g_convertWritePulls = 1;
    if (driver == "stream")
        g_convertWritePulls = 0;
    else if (driver.empty() && !gdalgOutput)
    {
        std::string wpExt;
        size_t wpDot = output.find_last_of('.');
        size_t wpSlash = output.find_last_of('/');
        if (wpDot != std::string::npos &&
            (wpSlash == std::string::npos || wpDot > wpSlash))
            wpExt = strToLower(output.substr(wpDot + 1));
        bool wpKnown = wpExt == "json" || wpExt == "geojson" ||
                       wpExt == "geojsonl" || wpExt == "geojsons" ||
                       wpExt == "shp" || wpExt == "dbf";
        if (!wpKnown && !target &&
            !(fAppend && fileExistsCv(output)))
            g_convertWritePulls = 0;
    }

    if (g_convertDatasetPreCheck)
    {
        auto pc = std::move(g_convertDatasetPreCheck);
        g_convertDatasetPreCheck = nullptr;
        if (ds && pc(*ds))
        {
            handlerPrintUsage();
            return 1;
        }
    }

    bool usingLineDue = false;
    if (driver.empty() && strEndsWith(strToLower(output), ".gdalg.json"))
    {
        // GDALG is not eligible for non-terminal pipeline writes
        if (g_convertCaptureWritten)
        {
            g_pipelineCommitted = true;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot guess driver for " + output);
            return 1;
        }
        driver = "GDALG";
    }
    if (g_convertCaptureWritten && driver == "GDALG")
    {
        g_pipelineCommitted = true;
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Unable to find driver `GDALG'.");
        return 1;
    }

    if (driver == "GDALG")
    {
        if (g_pipelineHasMidWrite)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "pipeline: Step write is not natively streaming "
                        "compatible, and may cause significant processing "
                        "time at opening");
        JVal j;
        j.type = JVal::OBJECT;
        auto addStr = [&](const char *k, const std::string &v) {
            JVal s2;
            s2.type = JVal::STRING;
            s2.s = v;
            j.obj.emplace_back(k, std::move(s2));
        };
        addStr("type", "gdal_streamed_alg");
        addStr("command_line",
               !g_pipelineGdalgCli.empty()
                   ? g_pipelineGdalgCli
                   : vvGdalgHead(r, input, true, true) +
                         " --output-format stream --output "
                         "streamed_dataset");
        addStr("gdal_version", "3130000");
        std::string content = jsoncSerialize(j, true);
        if (fileExistsCv(output) && !overwrite)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": Dataset '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--overwrite-layer/--append/"
                            "--update option.");
            handlerPrintUsage();
            return 1;
        }
        writeStringToFile(output, content);
        return 0;
    }

    // resolve source layers; name lookup materializes them
    std::vector<const OgrLayer *> layers;
    auto resolveLayers = [&]() -> bool {
        layers.clear();
        if (!layerFilter.empty())
        {
            ogrFlushPendingDebug(*ds);
            for (const auto &want : layerFilter)
            {
                const OgrLayer *found = nullptr;
                for (const auto &l : ds->layers)
                    if (l.name == want)
                        found = &l;
                if (!found)
                    for (const auto &l : ds->layers)
                        if (strEqualNoCase(l.name, want))
                        {
                            found = &l;
                            break;
                        }
                if (!found)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "read: Cannot find source layer '" +
                                    want + "'");
                    return false;
                }
                layers.push_back(found);
            }
        }
        else
            for (const auto &l : ds->layers)
                layers.push_back(&l);
        return true;
    };
    // a transform chain sees the read selection already materialized
    bool hadLayerFilter = !layerFilter.empty();
    if (g_convertDatasetMutate && !layerFilter.empty())
    {
        ogrFlushPendingDebug(*ds);
        if (vectorReadSelectLayers(*ds, layerFilter))
            return 1;
        layerFilter.clear();
    }
    if (!resolveLayers())
        return 1;

    if (g_convertDatasetMutate)
    {
        g_convertMutateBarOk =
            !(output == "/vsistdout/" && !g_dashStdout &&
              !g_pipelineBarStdout) &&
            g_pipelineMode && (!quiet || g_pipelineFailProgressForce);
        int mrc = g_convertDatasetMutate(*ds);
        g_convertMutateBarOk = false;
        if (mrc)
        {
            if (g_pipelineMutateSilentFail)
                g_pipelineMutateSilentFail = false;
            else
                pipelineFailProgress(quiet, output);
            return mrc;
        }
        // the transform may have replaced the layer set
        if (!resolveLayers())
            return 1;
    }

    // write-step driver resolution runs only after the read/transform
    // steps: their errors preempt "Cannot guess driver"
    if (driver.empty())
    {
        std::string ext;
        size_t dot = output.find_last_of('.');
        if (dot != std::string::npos)
            ext = strToLower(output.substr(dot + 1));
        if ((ext == "json" || ext == "geojson") && !gdalSkipHas("GeoJSON"))
            driver = "GeoJSON";
        else if ((ext == "geojsonl" || ext == "geojsons") &&
                 !gdalSkipHas("GeoJSONSeq"))
            driver = "GeoJSONSeq";
        else if ((ext == "shp" || ext == "dbf") &&
                 !gdalSkipHas("ESRI Shapefile"))
            driver = "ESRI Shapefile";
        if (!driver.empty())
            usingLineDue = true;
        else
        {
            // update-family: the existing output decides the driver
            if (target)
                driver = target->driverShort;
            else if (fAppend && fileExistsCv(output))
            {
                std::string terr;
                cplPushQuietHandler(false);
                preTarget = openVectorDataset(output, terr, {});
                cplPopHandler();
                if (preTarget)
                    driver = preTarget->driverShort;
                else if (fAppend)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                outputExistsKind(output) + " '" + output +
                                    "' already exists. Specify the "
                                    "--overwrite option to overwrite it.");
                    return 1;
                }
            }
            if (driver.empty())
            {
                g_pipelineCommitted = true;
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Cannot guess driver for " + output);
                pipelineFailProgress(quiet, output);
                return 1;
            }
        }
    }

    // "Using" announces only genuine creation, never target adoption
    bool updateMode = fUpdate || fUpsert || fOverLayer ||
                      (fAppend && fileExistsCv(output));
    if (usingLineDue && !updateMode)
        cplDebug("GDAL", "Using " + driver + " driver");
    ogrFlushPendingDebug(*ds);

    // same-dataset guards (string comparison, as in ogr2ogr)
    if (!input.empty() && input == output && driver != "MEM" &&
        driver != "stream")
    {
        if (fAppend || fUpsert || fOverLayer)
        {
            bool nlnOk = !outLayerName.empty();
            if (nlnOk)
                for (const auto &l : ds->layers)
                    if (l.name == outLayerName)
                        nlnOk = false;
            if (!nlnOk)
            {
                g_pipelineCommitted = true;
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "--output-layer name must be specified "
                            "combined with a single source layer name and "
                            "it must be different from an existing layer.");
                pipelineFailProgress(quiet, output);
                return 1;
            }
        }
        else if (!fUpdate)
        {
            g_pipelineCommitted = true;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Source and destination datasets must be different "
                        "in non-update mode.");
            pipelineFailProgress(quiet, output);
            return 1;
        }
    }

    if (g_convertLayerGate)
        for (const OgrLayer *l : layers)
            if (int grc = g_convertLayerGate(*l))
            {
                pipelineFailProgress(quiet, output);
                return grc;
            }

    // replay reader diagnostics tied to feature iteration; streamed
    // (seq) events interleave with the writer's progress instead of
    // dumping upfront: each event fires just before its feature is
    // written
    auto cursors = std::make_shared<
        std::map<const OgrLayer *, std::pair<long long, size_t>>>();
    g_seqFeatureTick = [cursors](const OgrLayer &lyr) {
        auto &st = (*cursors)[&lyr];
        long long k = st.first++;
        while (st.second < lyr.seqEvents.size() &&
               lyr.seqEvents[st.second].featsBefore <= k)
        {
            const auto &e = lyr.seqEvents[st.second++];
            if (diagOnceGate(e.once))
                cplErrorStr((CPLErrClass)e.sev, CPLE_AppDefined, e.msg);
        }
    };
    auto flushSeq = [cursors, layers]() {
        for (const OgrLayer *l : layers)
        {
            auto &st = (*cursors)[l];
            while (st.second < l->seqEvents.size())
            {
                const auto &e = l->seqEvents[st.second++];
                if (diagOnceGate(e.once))
                    cplErrorStr((CPLErrClass)e.sev, CPLE_AppDefined,
                                e.msg);
            }
        }
    };
    for (const OgrLayer *l : layers)
    {
        for (const auto &e : l->matEvents)
            cplErrorStr((CPLErrClass)e.sev, CPLE_AppDefined, e.msg);
        for (const auto &d : l->pendingDiags)
            if (diagOnceGate(d))
                cplErrorStr((CPLErrClass)d.sev, CPLE_AppDefined, d.msg);
    }

    // a GeoJSONSeq source whose trailing text fails to parse ends the
    // copy iteration in error after the last feature
    auto seqTrailingFail = [&]() -> bool {
        flushSeq();
        if (r.flag("skip-errors"))
            return false;
        for (const OgrLayer *l : layers)
        {
            if (!l->seqRescan)
                continue;
            for (const auto &e : l->seqEvents)
                if (!e.isDiag &&
                    e.featsBefore >= (long long)l->features.size())
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Failed to write layer '" + l->name +
                                    "'. Use --skip-errors to ignore "
                                    "errors and continue writing.");
                    g_convertLayerWriteFailed = true;
                    return true;
                }
        }
        return false;
    };

    if (driver == "MEM" || driver == "stream")
    {
        // a MEM target name colliding with an existing file refuses like
        // a real dataset would
        if (driver == "MEM" && !g_convertCaptureWritten &&
            !r.flag("overwrite") && !r.flag("append") &&
            !r.flag("update") && !r.flag("overwrite-layer") &&
            !r.flag("upsert") && fileExistsCv(output))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Dataset '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            g_convertLayerWriteFailed = true;
            return 1;
        }
        // these writers reported streamed events upfront historically
        flushSeq();
        g_pipelineCommitted = true;
        if (driver == "MEM")
        {
            warnUnsupportedCo("MEM", r.list("creation-option"));
            if (!g_convertCaptureWritten)
            {
                cplDebug("GDAL",
                         "GDALDriver::Create(MEM," + output +
                             ",0,0,0,Unknown," +
                             (r.list("creation-option").empty()
                                  ? "(nil)"
                                  : cplDebugPtr()) +
                             ")");
                dstGuard.fn = [output] {
                    cplDebug("GDAL", "GDALClose(" + output +
                                         ", this=" + cplDebugPtr() + ")");
                };
            }
            warnUnsupportedLco("MEM", output,
                               r.list("layer-creation-option"));
            // MEM CreateField is case-insensitive about duplicates,
            // like the DBF writer
            for (const OgrLayer *l : layers)
            {
                OgrLayer renamed;
                if (renameDupFields(*l, renamed, true))
                    const_cast<OgrLayer *>(l)->fields =
                        std::move(renamed.fields);
            }
        }
        // stream output never iterates the layers, so clip's pull
        // errors stay unfired there; MEM materializes and fails
        if (g_convertClipPending.active && driver == "MEM")
            for (const OgrLayer *l : layers)
                if (const ConvertClipPending::L *cp =
                        g_convertClipPending.find(l->name))
                {
                    convertClipEmitLayerErrors(*cp);
                    if (cp->fail && !r.flag("skip-errors"))
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "Failed to write layer '" + l->name +
                                        "'. Use --skip-errors to ignore "
                                        "errors and continue writing.");
                        g_convertLayerWriteFailed = true;
                        pipelineFailProgress(quiet, output);
                        return 1;
                    }
                }
        TermProgress tp;
        // MEM materializes features (the hook's errors interleave with
        // ticks); stream never iterates
        if (driver == "MEM" && g_convertFeatureHook)
        {
            size_t total = 0;
            for (const OgrLayer *l : layers)
                total += l->features.size();
            size_t done = 0;
            for (const OgrLayer *l : layers)
                for (auto &f : const_cast<OgrLayer *>(l)->features)
                {
                    seqTick(*l);
                    g_convertFeatureHook(*l, f);
                    ++done;
                    if (!quiet)
                        tp.update((double)done / (double)total);
                }
        }
        if (!quiet)
            tp.update(1.0);
        if (driver == "MEM" && !g_convertCaptureWritten)
            for (const OgrLayer *l : layers)
            {
                cplDebug("GDALVectorTranslate",
                         strPrintf("%lld features written in layer '%s'",
                                   (long long)l->features.size(),
                                   l->name.c_str()));
                const_cast<OgrLayer *>(l)->debugFeaturesRead =
                    (long long)l->features.size();
            }
        if (driver == "MEM" && g_convertCaptureWritten)
        {
            // hand the materialized dataset itself to the next step
            std::vector<OgrLayer> kept;
            for (const OgrLayer *l : layers)
                kept.push_back(std::move(*const_cast<OgrLayer *>(l)));
            ds->layers = std::move(kept);
            ds->path = output;
            ds->driverShort = "MEM";
            ds->driverLong = "Memory";
            ds->files.clear();
            ds->debugAnnounced = false;
            ds->debugPtr.clear();
            for (auto &nl : ds->layers)
            {
                double env[4] = {0, 0, 0, 0};
                bool anyPt = false;
                for (const auto &feat : nl.features)
                    if (feat.hasGeom)
                        envAccumGeom(feat.geom, env, anyPt);
                nl.hasExtent = anyPt;
                if (anyPt)
                    for (int k = 0; k < 4; ++k)
                        nl.extent[k] = env[k];
            }
            g_convertWrittenDs = std::move(ds);
        }
        else if (driver == "stream" && g_convertCaptureWritten)
            g_convertWrittenDs = std::move(ds);
        return 0;
    }

    g_pipelineCommitted = true;

    // append targets an existing output dataset (update-family targets
    // were already opened during validation)
    bool outputExists = fileExistsCv(output);
    if (!target && fAppend && outputExists)
    {
        std::string terr;
        if (preTarget)
            target = std::move(preTarget);
        else
        {
            cplPushQuietHandler(false);
            target = openVectorDataset(output, terr, {});
            cplPopHandler();
        }
        if (!target)
        {
            // append falls back to creation, which then refuses to
            // clobber the existing unrecognized output
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        outputExistsKind(output) + " '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
        // the preTarget probe ran under a quiet handler; adopting it as
        // the real output announces the open now
        ogrDebugAnnounceOpen(*target);
        ogrFlushPendingDebug(*target);
        dstGuard.fn = [&target] {
            if (target)
                vectorDebugClose(*target);
        };
    }

    if (driver == "GeoJSON")
    {
        // zero layers (combine dropping attribute-only layers) trips the
        // same refusal when creating; appends to an existing target pass
        if (layers.size() > 1 || (layers.empty() && !target))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GeoJSON driver does not support multiple layers.");
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        std::string coPtr =
            r.list("creation-option").empty() ? "(nil)" : cplDebugPtr();
        if (!target)
        {
            warnUnsupportedCo("GeoJSON", r.list("creation-option"));
            cplDebug("GDAL", "GDALDriver::Create(GeoJSON," + output +
                                 ",0,0,0,Unknown," + coPtr + ")");
        }
        const bool tarOut = strncmp(output.c_str(), "/vsitar/", 8) == 0;
        const bool zipRefuse =
            strncmp(output.c_str(), "/vsizip/", 8) == 0 &&
            vsiZipWriteProbe(output) != 1;
        if (tarOut || zipRefuse)
        {
            if (tarOut)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Only read-only mode is supported for /vsitar");
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to create GeoJSON datasource: " + output +
                            ": ");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GeoJSON driver failed to create " + output);
            return 1;
        }
        struct stat st;
        std::string dir = output;
        size_t slash = dir.find_last_of('/');
        dir = slash == std::string::npos ? "." : dir.substr(0, slash);
        if (!vsiIsVirtual(output) && stat(dir.c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to create GeoJSON datasource: " + output +
                            ": " + output + ": No such file or directory");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GeoJSON driver failed to create " + output);
            return 1;
        }
        if (!target)
        {
            dstGuard.fn = [output] {
                cplDebug("GDAL", "GDALClose(" + output +
                                     ", this=" + cplDebugPtr() + ")");
            };
            warnUnsupportedLco("GeoJSON", output,
                               r.list("layer-creation-option"));
        }
        if (layers.empty())
            return 0;
        const OgrLayer &lyr = *layers[0];
        std::string name = outLayerName.empty() ? lyr.name : outLayerName;
        const OgrLayer *appendFrom = nullptr;
        if (target)
        {
            const OgrLayer *tgtLyr =
                target->layers.empty() ? nullptr : &target->layers[0];
            bool sameName = tgtLyr && tgtLyr->name == name;
            if (fAppend || fUpsert)
            {
                if (!sameName)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Layer '" + name +
                                    "' does not already exist in the "
                                    "output dataset, and cannot be created "
                                    "by the output driver.");
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Failed to write layer '" + lyr.name +
                                    "'. Use --skip-errors to ignore errors "
                                    "and continue writing.");
                    g_convertLayerWriteFailed = true;
                    dstCloseFirst();
                    pipelineFailProgress(quiet, output);
                    return 1;
                }
                appendFrom = tgtLyr;
                warnLcoIgnoredOnAppend(r.list("layer-creation-option"));
            }
            else if (fOverLayer && sameName)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "DeleteLayer() not supported by this dataset.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "DeleteLayer() failed when overwrite "
                            "requested.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to write layer '" + lyr.name +
                                "'. Use --skip-errors to ignore errors "
                                "and continue writing.");
                g_convertLayerWriteFailed = true;
                dstCloseFirst();
                pipelineFailProgress(quiet, output);
                return 1;
            }
            else if (fUpdate && sameName)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Layer " + name +
                                " already exists, and --append not "
                                "specified. Consider using --append, or "
                                "--overwrite-layer.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to write layer '" + lyr.name +
                                "'. Use --skip-errors to ignore errors "
                                "and continue writing.");
                g_convertLayerWriteFailed = true;
                dstCloseFirst();
                pipelineFailProgress(quiet, output);
                return 1;
            }
            else if (!sameName)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Layer '" + name +
                                "' does not already exist in the output "
                                "dataset, and cannot be created by the "
                                "output driver.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to write layer '" + lyr.name +
                                "'. Use --skip-errors to ignore errors "
                                "and continue writing.");
                g_convertLayerWriteFailed = true;
                dstCloseFirst();
                pipelineFailProgress(quiet, output);
                return 1;
            }
        }
        GeoJsonOpts gjOpts;
        if (!appendFrom)
            for (const auto &kv : r.list("layer-creation-option"))
            {
                size_t eq = kv.find('=');
                std::string key = kv.substr(0, eq);
                std::string val =
                    eq == std::string::npos ? "" : kv.substr(eq + 1);
                bool truthy = !(strEqualNoCase(val, "NO") ||
                                strEqualNoCase(val, "FALSE") ||
                                strEqualNoCase(val, "OFF") || val == "0");
                if (strEqualNoCase(key, "WRITE_BBOX"))
                    gjOpts.bbox = truthy;
                else if (strEqualNoCase(key, "RFC7946"))
                    gjOpts.rfc = truthy;
                else if (strEqualNoCase(key, "COORDINATE_PRECISION"))
                {
                    gjOpts.prec = atoi(val.c_str());
                    gjOpts.precSet = true;
                }
                else if (strEqualNoCase(key, "WRITE_NAME"))
                    gjOpts.writeName = truthy;
            }
        // a default OGRGeoJSON source layer name never gets written,
        // even over an explicit WRITE_NAME=YES
        if (outLayerName.empty() && strEqualNoCase(lyr.name, "OGRGeoJSON"))
            gjOpts.writeName = false;
        if (gjOpts.rfc && !gjOpts.precSet)
            gjOpts.prec = 7;
        gjOpts.skipErrors = r.flag("skip-errors");
        // GeoJSON cannot create Binary fields: the translate layer-setup
        // warns once per such field, silenced by --quiet and kept off
        // the stdout stream
        if (!quiet && output != "/vsistdout/")
            for (const auto &bf : lyr.fields)
                if (bf.type == OFTBinary)
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "The output driver does not natively "
                                "support Binary type for field " +
                                    bf.name +
                                    ". Misconversion can happen. "
                                    "-mapFieldType can be used to "
                                    "control field type conversion.");
        // a translate-failing streamed source (make-point): with a
        // count pass (progress) the setup-failure errors re-fire and
        // the write pass aborts before its bar; a single-pass write
        // materializes the features translated before the failure and
        // ends the layer in error, both silenced by --skip-errors
        bool tfWriteFail = false;
        OgrLayer tfSubst;
        const OgrLayer *tfLayer = &lyr;
        if (g_convertTranslateFail.active &&
            g_convertTranslateFail.layer == lyr.name)
        {
            bool countPass =
                !quiet && !(output == "/vsistdout/" && !g_dashStdout &&
                            !g_pipelineBarStdout);
            if (countPass)
            {
                for (const auto &e : g_convertTranslateFail.passErrors)
                    cplErrorStr(CE_Failure, CPLE_AppDefined, e);
                tfWriteFail = !g_convertTranslateFail.passErrors.empty();
            }
            else
            {
                tfSubst.name = lyr.name;
                tfSubst.metadata = lyr.metadata;
                tfSubst.fields = lyr.fields;
                tfSubst.geomType = lyr.geomType;
                tfSubst.geomHasZ = lyr.geomHasZ;
                tfSubst.geomHasM = lyr.geomHasM;
                tfSubst.hasGeomField = lyr.hasGeomField;
                tfSubst.geomColumnName = lyr.geomColumnName;
                tfSubst.fidColumn = lyr.fidColumn;
                tfSubst.hasSrs = lyr.hasSrs;
                if (lyr.hasSrs)
                    tfSubst.srs = lyr.srs.clone();
                tfSubst.emitNullFields = lyr.emitNullFields;
                tfSubst.features = g_convertTranslateFail.quietKept;
                tfLayer = &tfSubst;
                tfWriteFail = true;
            }
            g_convertTranslateFail.active = false;
            if (gjOpts.skipErrors)
                tfWriteFail = false;
        }
        bool cpFail = false;
        if (const ConvertClipPending::L *cp =
                g_convertClipPending.find(lyr.name))
        {
            convertClipEmitLayerErrors(*cp);
            cpFail = cp->fail && !gjOpts.skipErrors;
        }
        TermProgress tp;
        ProgressSpan ps;
        if (!quiet &&
            !(output == "/vsistdout/" && !g_dashStdout &&
              !g_pipelineBarStdout) &&
            !tfWriteFail && !cpFail)
            ps.tp = &tp;
        if (g_pipelineWriteBarAtEnd)
            gjOpts.barAtEnd = true;
        // a zero-feature append leaves the target file byte-untouched
        bool zeroAppend = appendFrom && tfLayer->features.empty();
        if (!zeroAppend &&
            !writeGeoJson(*tfLayer, name, output, quiet, ps, appendFrom,
                          gjOpts))
        {
            // a pipeline run completes its progress bar even on failure
            if (g_pipelineMode && ps.tp)
                tp.update(1.0);
            dstCloseFirst();
            return 1;
        }
        if (ps.tp && tp.lastTick < 40)
            tp.update(1.0);
        if (seqTrailingFail())
        {
            dstCloseFirst();
            return 1;
        }
        if (tfWriteFail || cpFail)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + lyr.name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            g_convertLayerWriteFailed = true;
            dstCloseFirst();
            if (cpFail)
                pipelineFailProgress(quiet, output);
            return 1;
        }
        cplDebug("GDALVectorTranslate",
                 strPrintf("%lld features written in layer '%s'",
                           (long long)lyr.features.size(), name.c_str()));
        const_cast<OgrLayer &>(lyr).debugFeaturesRead =
            (long long)lyr.features.size();
        captureWrittenGeoJson(lyr, name, output, false);
        return 0;
    }

    if (driver == "GeoJSONSeq")
    {
        if (layers.size() > 1)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GeoJSONSeq driver does not support multiple "
                        "layers.");
            dstCloseFirst();
            return 1;
        }
        if (!target)
            cplDebug("GDAL",
                     "GDALDriver::Create(GeoJSONSeq," + output +
                         ",0,0,0,Unknown," +
                         (r.list("creation-option").empty()
                              ? "(nil)"
                              : cplDebugPtr()) +
                         ")");
        struct stat st;
        std::string dir = output;
        size_t slash = dir.find_last_of('/');
        dir = slash == std::string::npos ? "." : dir.substr(0, slash);
        if (!vsiIsVirtual(output) && stat(dir.c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to create " + output + ": " + output +
                            ": No such file or directory");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GeoJSONSeq driver failed to create " + output);
            return 1;
        }
        if (!target)
            dstGuard.fn = [output] {
                cplDebug("GDAL", "GDALClose(" + output +
                                     ", this=" + cplDebugPtr() + ")");
            };
        if (layers.empty())
            return 0;
        const OgrLayer &lyr = *layers[0];
        std::string name = outLayerName.empty() ? lyr.name : outLayerName;
        GeoJsonSeqOpts so;
        so.skipErrors = r.flag("skip-errors");
        {
            size_t d2 = output.find_last_of('.');
            size_t s2 = output.find_last_of('/');
            if (d2 != std::string::npos &&
                (s2 == std::string::npos || d2 > s2))
                so.rs = strEqualNoCase(output.substr(d2 + 1), "geojsons");
        }
        bool appendData = false;
        bool applyLco = true;
        if (target)
        {
            const OgrLayer *tgtLyr =
                target->layers.empty() ? nullptr : &target->layers[0];
            bool sameName = tgtLyr && tgtLyr->name == name;
            if (fUpsert)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "GeoJSONSeq driver does not support upsert");
                dstCloseFirst();
                pipelineFailProgress(quiet, output);
                return 1;
            }
            if (sameName && fAppend)
            {
                // appending to the existing layer keeps its unlimited
                // coordinate precision
                applyLco = false;
                so.prec = -1;
                so.rawAppend = true;
                warnLcoIgnoredOnAppend(r.list("layer-creation-option"));
            }
            else if (sameName && fUpdate)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Layer " + name +
                                " already exists, and --append not "
                                "specified. Consider using --append, or "
                                "--overwrite-layer.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to write layer '" + lyr.name +
                                "'. Use --skip-errors to ignore errors "
                                "and continue writing.");
                g_convertLayerWriteFailed = true;
                dstCloseFirst();
                pipelineFailProgress(quiet, output);
                return 1;
            }
            else if (sameName && fOverLayer)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "DeleteLayer() not supported by this dataset.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "DeleteLayer() failed when overwrite "
                            "requested.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to write layer '" + lyr.name +
                                "'. Use --skip-errors to ignore errors "
                                "and continue writing.");
                g_convertLayerWriteFailed = true;
                dstCloseFirst();
                pipelineFailProgress(quiet, output);
                return 1;
            }
            appendData = true;
        }
        if (applyLco)
        {
            warnUnsupportedLco("GeoJSONSeq", output,
                               r.list("layer-creation-option"));
            for (const auto &kv : r.list("layer-creation-option"))
            {
                size_t eq = kv.find('=');
                std::string key = kv.substr(0, eq);
                std::string val =
                    eq == std::string::npos ? "" : kv.substr(eq + 1);
                bool truthy = !(strEqualNoCase(val, "NO") ||
                                strEqualNoCase(val, "FALSE") ||
                                strEqualNoCase(val, "OFF") || val == "0");
                if (strEqualNoCase(key, "RS"))
                    so.rs = truthy;
                else if (strEqualNoCase(key, "WRITE_BBOX"))
                    so.bbox = truthy;
                else if (strEqualNoCase(key, "COORDINATE_PRECISION"))
                    so.prec = atoi(val.c_str());
                else if (strEqualNoCase(key, "ID_FIELD"))
                    so.idField = val;
                else if (strEqualNoCase(key, "ID_TYPE"))
                    so.idType = val;
            }
        }
        if (!quiet && output != "/vsistdout/")
            for (const auto &bf : lyr.fields)
                if (bf.type == OFTBinary)
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "The output driver does not natively "
                                "support Binary type for field " +
                                    bf.name +
                                    ". Misconversion can happen. "
                                    "-mapFieldType can be used to "
                                    "control field type conversion.");
        bool cpFail = false;
        if (const ConvertClipPending::L *cp =
                g_convertClipPending.find(lyr.name))
        {
            convertClipEmitLayerErrors(*cp);
            cpFail = cp->fail && !r.flag("skip-errors");
        }
        TermProgress tp;
        ProgressSpan ps;
        if (!quiet && !(output == "/vsistdout/" && !g_dashStdout &&
                        !g_pipelineBarStdout) &&
            !cpFail)
            ps.tp = &tp;
        if (g_pipelineWriteBarAtEnd)
            so.barAtEnd = true;
        if (!writeGeoJsonSeq(lyr, output, ps, appendData, so, quiet))
        {
            dstCloseFirst();
            return 1;
        }
        if (ps.tp && tp.lastTick < 40)
            tp.update(1.0);
        if (seqTrailingFail())
        {
            dstCloseFirst();
            return 1;
        }
        if (cpFail)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + lyr.name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            g_convertLayerWriteFailed = true;
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        cplDebug("GDALVectorTranslate",
                 strPrintf("%lld features written in layer '%s'",
                           (long long)lyr.features.size(), name.c_str()));
        const_cast<OgrLayer &>(lyr).debugFeaturesRead =
            (long long)lyr.features.size();
        captureWrittenGeoJson(lyr, name, output, true);
        return 0;
    }

    // ESRI Shapefile
    std::string outExt;
    {
        size_t d2 = output.find_last_of('.');
        size_t s2 = output.find_last_of('/');
        if (d2 != std::string::npos &&
            (s2 == std::string::npos || d2 > s2))
            outExt = strToLower(output.substr(d2 + 1));
    }
    bool singleFile = layers.size() == 1 &&
                      (outExt == "shp" || outExt == "dbf");

    ShpAppendData ad;
    const ShpAppendData *ap = nullptr;
    bool newLayerFileset = false;
    if (target && singleFile)
    {
        std::string tname = output.substr(
            output.find_last_of('/') == std::string::npos
                ? 0
                : output.find_last_of('/') + 1);
        tname = tname.substr(0, tname.find_last_of('.'));
        if (fUpsert)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "ESRI Shapefile driver does not support upsert");
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        else if (!outLayerName.empty() && outLayerName != tname)
        {
            // update-family with a fresh layer name creates a new fileset
            // in the dataset directory
            size_t sl = output.find_last_of('/');
            std::string dir =
                sl == std::string::npos ? "" : output.substr(0, sl + 1);
            output = dir + outLayerName + ".shp";
            newLayerFileset = true;
        }
        else if (fUpdate)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Layer " + tname +
                            " already exists, and --append not specified. "
                            "Consider using --append, or "
                            "--overwrite-layer.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + layers[0]->name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        else if (fAppend)
        {
            std::string abase = output.substr(0, output.find_last_of('.'));
            if (loadShpAppendData(abase, *layers[0], ad))
                ap = &ad;
            warnLcoIgnoredOnAppend(r.list("layer-creation-option"));
        }
        // --overwrite-layer recreates the layer: identical to a plain
        // overwrite for single-file shapefile datasets
    }

    if (!target)
    {
        warnUnsupportedCo("ESRI Shapefile", r.list("creation-option"));
        cplDebug("GDAL", "GDALDriver::Create(ESRI Shapefile," + output +
                             ",0,0,0,Unknown," +
                             (r.list("creation-option").empty()
                                  ? "(nil)"
                                  : cplDebugPtr()) +
                             ")");
    }

    if (!singleFile)
    {
        if (mkdir(output.c_str(), 0755) != 0 && errno != EEXIST)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to create directory " + output +
                            "\nfor shapefile datastore.");
            return 1;
        }
    }
    if (!target)
        dstGuard.fn = [output] {
            cplDebug("GDAL", "GDALClose(" + output +
                                 ", this=" + cplDebugPtr() + ")");
        };

    if (target && !singleFile && fUpsert)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "ESRI Shapefile driver does not support upsert");
        dstCloseFirst();
        pipelineFailProgress(quiet, output);
        return 1;
    }

    if (!target || fOverLayer || newLayerFileset)
        warnUnsupportedLco("ESRI Shapefile", output,
                           r.list("layer-creation-option"));

    size_t nFeat = 0;
    for (const OgrLayer *l : layers)
        nFeat += l->features.size();
    // a pending clip failure ends the run before its bar ever starts
    bool cpAnySuppress =
        g_convertClipPending.anyFail() && !r.flag("skip-errors");
    TermProgress tp;
    double acc = 0;
    for (const OgrLayer *l : layers)
    {
        bool layerFailed = false;
        const ConvertClipPending::L *cpThis =
            g_convertClipPending.find(l->name);
        std::string outName =
            !outLayerName.empty() && layers.size() == 1 ? outLayerName
                                                        : l->name;
        std::string outPath =
            singleFile ? output : output + "/" + outName + ".shp";
        ProgressSpan ps;
        if (!quiet && !cpAnySuppress)
        {
            ps.tp = &tp;
            ps.base = nFeat ? acc / (double)nFeat : 0.0;
            ps.span = nFeat ? (double)l->features.size() / (double)nFeat
                            : 1.0;
        }
        acc += (double)l->features.size();
        const ShpAppendData *apUse = ap;
        ShpAppendData adLocal;
        const OgrLayer *tgtLayer = nullptr;
        if (target && !singleFile)
            for (const auto &tl : target->layers)
                if (tl.name == outName)
                    tgtLayer = &tl;
        if (tgtLayer && fAppend)
        {
            if (loadShpAppendData(output + "/" + outName, *l, adLocal))
                apUse = &adLocal;
            warnLcoIgnoredOnAppend(r.list("layer-creation-option"));
        }
        else if (tgtLayer && fUpdate)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Layer " + outName +
                            " already exists, and --append not specified. "
                            "Consider using --append, or "
                            "--overwrite-layer.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + l->name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        bool wantQix = false;
        int shptType = -1;
        bool shptM = false, shptNone = false, shptBad = false;
        std::string shptVal;
        std::string encRaw;
        bool haveEnc = false;
        if (!apUse)
            for (const auto &kv : r.list("layer-creation-option"))
            {
                size_t eq = kv.find('=');
                std::string key = kv.substr(0, eq);
                std::string val =
                    eq == std::string::npos ? "" : kv.substr(eq + 1);
                if (strEqualNoCase(key, "ENCODING"))
                {
                    encRaw = val;
                    haveEnc = true;
                }
                else if (strEqualNoCase(key, "SPATIAL_INDEX"))
                    wantQix = !(strEqualNoCase(val, "NO") ||
                                strEqualNoCase(val, "FALSE") ||
                                strEqualNoCase(val, "OFF") || val == "0");
                else if (strEqualNoCase(key, "SHPT"))
                {
                    static const struct
                    {
                        const char *name;
                        int type;
                        bool m;
                    } kShpt[] = {
                        {"POINT", 1, false},         {"ARC", 3, false},
                        {"POLYGON", 5, false},       {"MULTIPOINT", 8, false},
                        {"POINTZ", 11, false},       {"ARCZ", 13, false},
                        {"POLYGONZ", 15, false},     {"MULTIPOINTZ", 18, false},
                        {"POINTM", 21, true},        {"ARCM", 23, true},
                        {"POLYGONM", 25, true},      {"MULTIPOINTM", 28, true},
                        {"POINTZM", 11, true},       {"ARCZM", 13, true},
                        {"POLYGONZM", 15, true},     {"MULTIPOINTZM", 18, true},
                        {"MULTIPATCH", 31, false},
                    };
                    shptType = -1;
                    shptM = shptNone = shptBad = false;
                    shptVal = val;
                    if (strEqualNoCase(val, "NONE") ||
                        strEqualNoCase(val, "NULL"))
                        shptNone = true;
                    else
                    {
                        bool found = false;
                        for (const auto &e : kShpt)
                            if (strEqualNoCase(val, e.name))
                            {
                                shptType = e.type;
                                shptM = e.m;
                                found = true;
                                break;
                            }
                        if (!found)
                            shptBad = true;
                    }
                }
            }
        if (shptBad)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "Unknown SHPT value of `" + shptVal +
                            "' passed to Shapefile layercreation.  "
                            "Creation aborted.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + l->name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        std::string shpEnc;
        if (configIsSet("SHAPE_ENCODING"))
            shpEnc = configGet("SHAPE_ENCODING");
        else if (haveEnc)
        {
            if (encRaw.size() >= 5 &&
                strEqualNoCase(encRaw.substr(0, 5), "LDID/"))
                shpEnc = recodeFromLdid(atoi(encRaw.c_str() + 5));
            else
                shpEnc = recodeFromCpg(encRaw);
        }
        else
            shpEnc = "ISO-8859-1";
        if (!apUse)
        {
            // a freshly created layer logs its codepage the same way an
            // opened one does
            cplDebug("Shape", "DBF Codepage = " +
                                  (haveEnc ? encRaw
                                           : std::string("LDID/87")) +
                                  " for " + outPath);
            if (!shpEnc.empty())
                cplDebug("Shape",
                         "Treating as encoding '" + shpEnc + "'.");
        }
        if (cpThis)
            convertClipEmitLayerErrors(*cpThis);
        QixData qd;
        bool wrote =
            writeShapefile(*l, outPath, quiet, ps, layerFailed, apUse,
                           hadLayerFilter, wantQix ? &qd : nullptr,
                           shptType, shptM, shptNone, shpEnc,
                           haveEnc ? &encRaw : nullptr,
                           r.flag("skip-errors"));
        if (wantQix && qd.filesWritten)
            writeQix(qd,
                     outPath.substr(0, outPath.find_last_of('.')) +
                         ".qix");
        if (!wrote)
        {
            g_convertLayerWriteFailed = true;
            // the pipeline write step completes its progress line even
            // when a layer aborts mid-write
            if (g_pipelineMode && !quiet)
                tp.update(1.0);
            dstCloseFirst();
            return 1;
        }
        if (cpThis && cpThis->fail && !r.flag("skip-errors"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to write layer '" + l->name +
                            "'. Use --skip-errors to ignore errors and "
                            "continue writing.");
            g_convertLayerWriteFailed = true;
            dstCloseFirst();
            pipelineFailProgress(quiet, output);
            return 1;
        }
        std::string dbgName = outName;
        if (singleFile)
        {
            // the fileset names the layer regardless of --output-layer
            size_t sl = output.find_last_of('/');
            dbgName = sl == std::string::npos ? output
                                              : output.substr(sl + 1);
            size_t dd = dbgName.find_last_of('.');
            if (dd != std::string::npos)
                dbgName = dbgName.substr(0, dd);
        }
        cplDebug("GDALVectorTranslate",
                 strPrintf("%lld features written in layer '%s'",
                           (long long)l->features.size(),
                           dbgName.c_str()));
    }
    if (!quiet && tp.lastTick < 40)
        tp.update(1.0);
    if (seqTrailingFail())
    {
        dstCloseFirst();
        return 1;
    }
    for (const OgrLayer *l : layers)
        const_cast<OgrLayer *>(l)->debugFeaturesRead =
            (long long)l->features.size();
    if (g_convertCaptureWritten)
    {
        // the still-open shapefile hands over its layers, but the
        // DBF_DATE_LAST_UPDATE metadata only materializes on reopen
        std::string werr;
        cplPushQuietHandler();
        auto w = openVectorDataset(output, werr, {});
        cplPopHandler();
        if (w)
        {
            for (auto &nl : w->layers)
                for (size_t k = 0; k < nl.metadata.size();)
                {
                    if (nl.metadata[k].first == "DBF_DATE_LAST_UPDATE")
                        nl.metadata.erase(nl.metadata.begin() + k);
                    else
                        ++k;
                }
            // the layer object keeps the writer's SRS, not the .prj
            // reimport
            if (w->layers.size() == layers.size())
                for (size_t k = 0; k < w->layers.size(); ++k)
                {
                    w->layers[k].hasSrs = layers[k]->hasSrs;
                    w->layers[k].srs = layers[k]->hasSrs
                                           ? layers[k]->srs.clone()
                                           : Srs();
                    // an all-attribute-less layer only grows its
                    // synthetic FID column on reopen
                    if (layers[k]->fields.empty() &&
                        w->layers[k].fields.size() == 1 &&
                        w->layers[k].fields[0].name == "FID")
                    {
                        w->layers[k].fields.clear();
                        for (auto &f : w->layers[k].features)
                            f.values.clear();
                    }
                }
            g_convertWrittenDs = std::move(w);
        }
    }
    return 0;
}

int vectorConvertPreValidator(const CmdSpec &, ParseResult &r)
{
    std::string format = r.str("output-format");
    if (!format.empty())
    {
        if (strEqualNoCase(format, "Memory"))
            memoryDriverDeprecationWarnOnce();
        bool ok = strEqualNoCase(format, "GeoJSON") ||
                  strEqualNoCase(format, "GeoJSONSeq") ||
                  strEqualNoCase(format, "ESRI Shapefile") ||
                  strEqualNoCase(format, "MEM") ||
                  strEqualNoCase(format, "Memory") ||
                  strEqualNoCase(format, "GDALG") ||
                  strEqualNoCase(format, "stream");
        if (!ok)
        {
            if (strEqualNoCase(format, "GTiff") ||
                strEqualNoCase(format, "COG") ||
                strEqualNoCase(format, "VRT"))
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            convertMsgPrefix() +
                                ": Invalid value for argument "
                                "'output-format'. Driver '" +
                                format +
                                "' does not expose the required "
                                "'DCAP_VECTOR' capability.");
            else if (strEqualNoCase(format, "ESRIJSON") ||
                     strEqualNoCase(format, "TopoJSON"))
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            convertMsgPrefix() +
                                ": Invalid value for argument "
                                "'output-format'. Driver '" +
                                format + "' does not have write support.");
            else
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            convertMsgPrefix() +
                                ": Invalid value for argument "
                                "'output-format'. Driver '" +
                                format + "' does not exist.");
            handlerPrintUsage();
            return 1;
        }
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(true, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

// ---- vector create ----

int createFieldTypeFromName(const std::string &t, bool &ok)
{
    struct TN
    {
        const char *n;
        int t;
    };
    static const TN tab[] = {
        {"Integer", OFTInteger},
        {"IntegerList", OFTIntegerList},
        {"Real", OFTReal},
        {"RealList", OFTRealList},
        {"String", OFTString},
        {"StringList", OFTStringList},
        {"Binary", OFTBinary},
        {"Date", OFTDate},
        {"Time", OFTTime},
        {"DateTime", OFTDateTime},
        {"Integer64", OFTInteger64},
        {"Integer64List", OFTInteger64List}};
    for (const TN &e : tab)
        if (strEqualNoCase(t, e.n))
        {
            ok = true;
            return e.t;
        }
    ok = false;
    return OFTString;
}

int createFieldSubTypeFromName(const std::string &t)
{
    struct TN
    {
        const char *n;
        int t;
    };
    static const TN tab[] = {{"Boolean", OFSTBoolean},
                             {"Int16", OFSTInt16},
                             {"Float32", OFSTFloat32},
                             {"JSON", OFSTJSON},
                             {"UUID", OFSTUUID}};
    for (const TN &e : tab)
        if (strEqualNoCase(t, e.n))
            return e.t;
    return OFSTNone;
}

bool createParseFieldDef(const std::string &def, OgrFieldDefn &f,
                         std::string &err)
{
    static const char *kFmtErr =
        "Invalid field definition format. Expected "
        "<NAME>:<TYPE>[(<WIDTH>[,<PRECISION>])]";
    size_t c = def.find(':');
    if (c == std::string::npos || c == 0)
    {
        err = kFmtErr;
        return false;
    }
    f.name = def.substr(0, c);
    std::string t = def.substr(c + 1);
    if (t.empty())
    {
        err = kFmtErr;
        return false;
    }
    int width = 0, precision = 0;
    size_t par = t.find('(');
    if (par != std::string::npos)
    {
        if (t.back() != ')' || par + 2 > t.size())
        {
            err = kFmtErr;
            return false;
        }
        std::string wp = t.substr(par + 1, t.size() - par - 2);
        t = t.substr(0, par);
        std::string w = wp, p;
        size_t comma = wp.find(',');
        if (comma != std::string::npos)
        {
            w = wp.substr(0, comma);
            p = wp.substr(comma + 1);
        }
        auto digits = [](const std::string &s) {
            if (s.empty())
                return false;
            for (char ch : s)
                if (!isdigit((unsigned char)ch))
                    return false;
            return true;
        };
        if (!digits(w) || (comma != std::string::npos && !digits(p)))
        {
            err = kFmtErr;
            return false;
        }
        width = atoi(w.c_str());
        if (comma != std::string::npos)
            precision = atoi(p.c_str());
    }
    bool tok = false;
    f.type = createFieldTypeFromName(t, tok);
    if (!tok)
    {
        err = "Unsupported field type: " + t;
        return false;
    }
    f.width = width;
    f.precision = precision;
    return true;
}

int createTzAggrFromString(const std::string &tz)
{
    if (tz == "UTC")
        return 100;
    if (tz == "mixed timezones")
        return -2;
    if (tz.size() == 6 && (tz[0] == '+' || tz[0] == '-') && tz[3] == ':')
    {
        int h = atoi(tz.substr(1, 2).c_str());
        int m = atoi(tz.substr(4, 2).c_str());
        int quarters = (h * 60 + m) / 15;
        return tz[0] == '-' ? 100 - quarters : 100 + quarters;
    }
    return -1;
}

// OGR_SCHEMA ingestion; the returned message (empty when valid) feeds
// "create: Cannot parse OGR_SCHEMA: <msg>."; the pre-error lines the
// reference's schema loader prints are emitted here
std::string createIngestSchema(const std::string &content,
                               std::vector<OgrLayer> &defs)
{
    bool ok = false;
    JVal root = JVal::parse(content, &ok);
    if (!ok)
    {
        std::string desc;
        size_t off = 0;
        if (jsoncTokenerError(content, desc, off))
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JSON parsing error: %s (at offset %zu)",
                                  desc.c_str(), off));
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "SCHEMA info is invalid JSON");
        return "SCHEMA info is invalid JSON";
    }
    const JVal *lyrs = root.get("layers");
    if (!lyrs || lyrs->type != JVal::ARRAY)
        return "";
    for (const JVal &L : lyrs->arr)
    {
        std::string lname = L.getString("name");
        bool full = strEqualNoCase(L.getString("schemaType"), "Full");
        if (lname.empty())
        {
            std::string m = "Layer " + lname + " has no valid overrides";
            cplErrorStr(CE_Failure, CPLE_AppDefined, m);
            return m;
        }
        OgrLayer lyr;
        lyr.name = lname;
        lyr.geomType = 101;
        lyr.hasGeomField = false;
        const JVal *fl = L.get("fields");
        if (fl && fl->type == JVal::ARRAY)
            for (const JVal &F : fl->arr)
            {
                std::string fname = F.getString("name");
                if (fname.empty())
                {
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "Field name is missing");
                    return "Field name is missing";
                }
                bool hasOverride = false;
                if (F.type == JVal::OBJECT)
                    for (const auto &kv : F.obj)
                        if (kv.first != "name")
                            hasOverride = true;
                if (!full && !hasOverride)
                {
                    std::string m = "Field " + fname +
                                    " has no valid overrides and "
                                    "schemaType is not \"Full\"";
                    cplErrorStr(CE_Failure, CPLE_AppDefined, m);
                    return m;
                }
                OgrFieldDefn f;
                f.name = fname;
                f.type = OFTString;
                if (F.get("type"))
                {
                    std::string tn = F.getString("type");
                    bool tok = false;
                    f.type = createFieldTypeFromName(tn, tok);
                    if (!tok)
                    {
                        std::string m = "Unsupported field type: " + tn +
                                        " for field " + fname;
                        cplErrorStr(CE_Failure, CPLE_AppDefined, m);
                        return m;
                    }
                }
                if (F.get("subType"))
                    f.subType =
                        createFieldSubTypeFromName(F.getString("subType"));
                f.width = (int)F.getInt("width", 0);
                f.precision = (int)F.getInt("precision", 0);
                f.nullable = F.getBool("nullable", true);
                f.unique = F.getBool("uniqueConstraint", false);
                f.altName = F.getString("alias");
                if (F.get("timezone"))
                    f.tzAggr =
                        createTzAggrFromString(F.getString("timezone"));
                lyr.fields.push_back(std::move(f));
            }
        if (!full && lyr.fields.empty())
        {
            std::string m = "Layer " + lname + " has no valid overrides";
            cplErrorStr(CE_Failure, CPLE_AppDefined, m);
            return m;
        }
        const JVal *gf = L.get("geometryFields");
        if (gf && gf->type == JVal::ARRAY && !gf->arr.empty())
        {
            const JVal &G = gf->arr[0];
            lyr.hasGeomField = true;
            lyr.geomColumnName = G.getString("name");
            int gt;
            bool z, m2;
            if (ogrGeomTypeFromWktName(G.getString("type"), gt, z, m2))
            {
                lyr.geomType = gt;
                lyr.geomHasZ = z;
                lyr.geomHasM = m2;
            }
            else
                lyr.geomType = 0;
            const JVal *cs = G.get("coordinateSystem");
            std::string src;
            if (cs && cs->type == JVal::STRING)
                src = cs->s;
            else if (cs && cs->type == JVal::OBJECT)
            {
                src = cs->getString("authid");
                if (src.empty())
                    src = cs->getString("wkt");
            }
            if (!src.empty())
            {
                bool sok = false;
                Srs srs = Srs::fromCliInput(src, sok, true);
                if (sok)
                {
                    lyr.srs = std::move(srs);
                    lyr.hasSrs = true;
                }
            }
        }
        // OGR_SCHEMA keeps its field map ordered by name
        std::sort(lyr.fields.begin(), lyr.fields.end(),
                  [](const OgrFieldDefn &a, const OgrFieldDefn &b) {
                      return a.name < b.name;
                  });
        lyr.origName = lyr.name;
        defs.push_back(std::move(lyr));
    }
    return "";
}

std::string createBasenameNoExt(const std::string &out)
{
    size_t sl = out.find_last_of('/');
    std::string b = sl == std::string::npos ? out : out.substr(sl + 1);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

int vectorCreateCore(VectorCreateRun &p)
{
    bool barDue = p.stepMode && p.terminalStep && !p.quiet;
    auto endBar = [&]() {
        if (barDue)
        {
            TermProgress tp;
            tp.update(1.0);
        }
    };
    auto fail = [&](int rc) {
        endBar();
        return rc;
    };

    std::string driver;
    {
        std::string ferr = vectorOutputDriverResolve(p.format, driver);
        if (!ferr.empty())
        {
            // the leaf validates --of with usage; only steps land here
            cplErrorStr(CE_Failure, CPLE_AppDefined, "create: " + ferr);
            return fail(1);
        }
    }
    bool gdalgExt = strEndsWith(strToLower(p.output), ".gdalg.json");
    // a leaf GDALG output serializes the echo before any schema work
    if (!p.stepMode &&
        (driver == "GDALG" || (driver.empty() && gdalgExt)))
    {
        JVal j;
        j.type = JVal::OBJECT;
        auto addStr = [&](const char *k, const std::string &v) {
            JVal s2;
            s2.type = JVal::STRING;
            s2.s = v;
            j.obj.emplace_back(k, std::move(s2));
        };
        addStr("type", "gdal_streamed_alg");
        addStr("command_line", p.gdalgCli);
        addStr("gdal_version", "3130000");
        writeStringToFile(p.output, jsoncSerialize(j, true));
        return 0;
    }

    // layer definitions
    std::vector<OgrLayer> defs;
    bool fromSchema = false;
    if (p.likeDs)
    {
        p.schemaContent = vectorExportSchemaRender(*p.likeDs);
        p.haveSchemaContent = true;
    }
    if (p.haveSchemaContent || p.schemaSet)
    {
        fromSchema = true;
        std::string content;
        if (p.haveSchemaContent)
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "Cannot open file '" + p.schemaContent + "'");
            content = p.schemaContent;
        }
        else if (fileExistsCv(p.schemaSpec) &&
                 readFileToString(p.schemaSpec, content))
        {
        }
        else
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "Cannot open file '" + p.schemaSpec + "'");
            content = p.schemaSpec;
        }
        std::string msg = createIngestSchema(content, defs);
        if (!msg.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot parse OGR_SCHEMA: " + msg + ".");
            return fail(1);
        }
        if (p.outputLayerSet)
            for (OgrLayer &l : defs)
                l.name = p.outputLayer;
    }
    else
    {
        OgrLayer lyr;
        lyr.name = p.outputLayerSet ? p.outputLayer
                                    : createBasenameNoExt(p.output);
        lyr.geomType = 101;
        lyr.hasGeomField = false;
        if (p.geomTypeSet)
        {
            int gt;
            bool z, m;
            if (ogrGeomTypeFromWktName(p.geomTypeName, gt, z, m))
            {
                lyr.geomType = gt;
                lyr.geomHasZ = z;
                lyr.geomHasM = m;
                lyr.hasGeomField = true;
                lyr.geomColumnName =
                    p.geomFieldSet ? p.geomFieldName : "geom";
            }
        }
        // the SRS rides the geometry field: without --geometry-type no
        // geometry field is created and --crs leaves no trace
        if (p.crsSet && lyr.hasGeomField)
        {
            bool sok = false;
            Srs srs = Srs::fromCliInput(p.crsInput, sok, true);
            if (sok)
            {
                lyr.srs = std::move(srs);
                lyr.hasSrs = true;
            }
        }
        for (const auto &def : p.fieldDefs)
        {
            OgrFieldDefn f;
            std::string err;
            if (createParseFieldDef(def, f, err))
                lyr.fields.push_back(std::move(f));
        }
        if (p.fidSet)
            lyr.fidColumn = p.fid;
        defs.push_back(std::move(lyr));
    }
    if (defs.empty())
    {
        // a schema without layers falls back to the default layer
        OgrLayer lyr;
        lyr.name = p.outputLayerSet ? p.outputLayer
                                    : createBasenameNoExt(p.output);
        lyr.geomType = 101;
        lyr.hasGeomField = false;
        defs.push_back(std::move(lyr));
    }

    // driver guess after ingestion (matches the reference's error order)
    if (driver.empty())
    {
        if (gdalgExt)
        {
            // steps only: the leaf handled GDALG above
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot guess driver for " + p.output);
            return fail(1);
        }
        std::string ext;
        size_t dot = p.output.find_last_of('.');
        size_t slash = p.output.find_last_of('/');
        if (dot != std::string::npos &&
            (slash == std::string::npos || dot > slash))
            ext = strToLower(p.output.substr(dot + 1));
        if ((ext == "json" || ext == "geojson") && !gdalSkipHas("GeoJSON"))
            driver = "GeoJSON";
        else if ((ext == "geojsonl" || ext == "geojsons") &&
                 !gdalSkipHas("GeoJSONSeq"))
            driver = "GeoJSONSeq";
        else if ((ext == "shp" || ext == "dbf") &&
                 !gdalSkipHas("ESRI Shapefile"))
            driver = "ESRI Shapefile";
        else if (p.target)
            driver = p.target->driverShort;
        if (driver.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot guess driver for " + p.output);
            return fail(1);
        }
    }
    if (driver == "stream")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "create: Cannot find driver stream.");
        return fail(1);
    }
    if (driver == "GDALG")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "create: Cannot find driver GDALG.");
        return fail(1);
    }

    bool updateMode = (p.update || p.overwriteLayer) && !p.overwrite;
    if (updateMode && !p.target && driver != "MEM")
    {
        // step-mode target open (the leaf opened it during validation)
        std::string err;
        p.target = openVectorDataset(p.output, err, {});
        if (!p.target)
        {
            if (err == "missing")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            datasetMissingMessage(p.output));
            else if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + p.output +
                                "' not recognized as being in a "
                                "supported file format.");
            return fail(1);
        }
    }

    auto applySelection = [&]() -> bool {
        if (p.layerSel.empty() || !fromSchema)
            return true;
        std::vector<OgrLayer> out;
        for (const auto &want : p.layerSel)
        {
            const OgrLayer *hit = nullptr;
            for (const auto &l : defs)
                if (l.name == want)
                {
                    hit = &l;
                    break;
                }
            if (!hit)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: The specified input layer name '" +
                                want +
                                "' doesn't exist in the provided "
                                "template or schema.");
                return false;
            }
            out.push_back(*hit);
        }
        defs = std::move(out);
        return true;
    };

    auto buildHandover = [&]() {
        auto ds = std::make_unique<OgrDataset>();
        ds->path = p.output;
        ds->driverShort = driver;
        ds->driverLong = driver;
        for (const auto &l : defs)
        {
            OgrLayer c = l;
            if (driver == "GeoJSON" || driver == "GeoJSONSeq")
            {
                c.hasSrs = false;
                c.srs = Srs();
                c.geomColumnName = "";
            }
            else if (driver == "ESRI Shapefile")
                c.geomColumnName = "";
            ds->layers.push_back(std::move(c));
        }
        p.handover = std::move(ds);
    };

    if (driver == "MEM")
    {
        if (!applySelection())
            return fail(1);
        buildHandover();
        endBar();
        return 0;
    }

    auto multiRefused = [&]() -> bool {
        if (defs.size() <= 1)
            return false;
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "create: The output format " + driver +
                        " doesn't support multiple layers.");
        return true;
    };

    if (driver == "GeoJSON" || driver == "GeoJSONSeq")
    {
        bool seq = driver == "GeoJSONSeq";
        if (p.target && updateMode)
        {
            if (!applySelection())
                return fail(1);
            if (multiRefused())
                return fail(1);
            const std::string &want = defs[0].name;
            // the inner layer-creation attempt names the layer being
            // built (schema layers keep their pre-rename name, with a
            // period); the outer create wrapper repeats it without one
            // for schema/template definitions
            std::string inner = fromSchema && !defs[0].origName.empty()
                                    ? defs[0].origName
                                    : want;
            std::string outer = "create: Cannot create layer '" + inner +
                                "'" + (fromSchema ? "" : ".");
            const OgrLayer *ex = nullptr;
            for (const auto &l : p.target->layers)
                if (l.name == want)
                {
                    ex = &l;
                    break;
                }
            if (!ex)
                for (const auto &l : p.target->layers)
                    if (strEqualNoCase(l.name, want))
                    {
                        ex = &l;
                        break;
                    }
            if (p.overwriteLayer)
            {
                if (!ex)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "create: Cannot find layer '" + want +
                                    "'.");
                    cplErrorStr(CE_Failure, CPLE_AppDefined, outer);
                    return fail(1);
                }
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "DeleteLayer() not supported by this "
                            "dataset.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: Cannot delete layer '" + want +
                                "'.");
                cplErrorStr(CE_Failure, CPLE_AppDefined, outer);
                return fail(1);
            }
            if (ex)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: Layer '" + want +
                                "' already exists. Specify the "
                                "--overwrite-layer option to overwrite "
                                "it.");
                cplErrorStr(CE_Failure, CPLE_AppDefined, outer);
                return fail(1);
            }
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "GeoJSON driver doesn't support creating a layer "
                        "on a read-only datasource");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot create layer '" + inner + "'.");
            cplErrorStr(CE_Failure, CPLE_AppDefined, outer);
            return fail(1);
        }

        bool vout = p.output == "/vsistdout/";
        if (!seq && !vout)
        {
            struct stat st;
            std::string dir = p.output;
            size_t slash = dir.find_last_of('/');
            dir = slash == std::string::npos ? "." : dir.substr(0, slash);
            if (!vsiIsVirtual(p.output) && stat(dir.c_str(), &st) != 0)
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "Failed to create GeoJSON datasource: " +
                                p.output + ": " + p.output +
                                ": No such file or directory");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "GeoJSON driver failed to create " + p.output);
                return fail(1);
            }
        }
        if (!vout)
            writeStringToFile(p.output, "");
        if (!applySelection())
            return fail(1);
        if (multiRefused())
            return fail(1);
        OgrLayer &lyr = defs[0];
        if (seq)
        {
            // the seq driver exposes no creation-option list, so bogus
            // --co values pass silently; lco names are still validated
            warnUnsupportedLco("GeoJSONSeq", p.output, p.lco);
            if (!lyr.hasSrs)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "No SRS set on layer. Assuming it is long/lat "
                            "on WGS84 ellipsoid");
            GeoJsonSeqOpts so;
            ProgressSpan ps;
            writeGeoJsonSeq(lyr, p.output, ps, false, so, true);
        }
        else
        {
            warnUnsupportedCo("GeoJSON", p.co);
            warnUnsupportedLco("GeoJSON", p.output, p.lco);
            GeoJsonOpts gj;
            gj.writeName = !lyr.name.empty();
            for (const auto &kv : p.lco)
            {
                size_t eq = kv.find('=');
                std::string key = kv.substr(0, eq);
                std::string val =
                    eq == std::string::npos ? "" : kv.substr(eq + 1);
                bool truthy = !(strEqualNoCase(val, "NO") ||
                                strEqualNoCase(val, "FALSE") ||
                                strEqualNoCase(val, "OFF") || val == "0");
                if (strEqualNoCase(key, "WRITE_BBOX"))
                    gj.bbox = truthy;
                else if (strEqualNoCase(key, "RFC7946"))
                    gj.rfc = truthy;
                else if (strEqualNoCase(key, "COORDINATE_PRECISION"))
                {
                    gj.prec = atoi(val.c_str());
                    gj.precSet = true;
                }
                else if (strEqualNoCase(key, "WRITE_NAME") && !truthy)
                    gj.writeName = false;
            }
            if (gj.rfc && !gj.precSet)
                gj.prec = 7;
            if (gj.rfc && !lyr.hasSrs)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "No SRS set on layer. Assuming it is long/lat "
                            "on WGS84 ellipsoid");
            ProgressSpan ps;
            writeGeoJson(lyr, lyr.name, p.output, true, ps, nullptr, gj);
        }
        buildHandover();
        endBar();
        return 0;
    }

    if (driver == "ESRI Shapefile")
    {
        std::string outExt;
        {
            size_t d2 = p.output.find_last_of('.');
            size_t s2 = p.output.find_last_of('/');
            if (d2 != std::string::npos &&
                (s2 == std::string::npos || d2 > s2))
                outExt = strToLower(p.output.substr(d2 + 1));
        }
        bool singleFile = outExt == "shp" || outExt == "dbf";
        if (p.target && updateMode)
        {
            if (!applySelection())
                return fail(1);
            if (multiRefused())
                return fail(1);
            const std::string &want = defs[0].name;
            const OgrLayer *ex = nullptr;
            for (const auto &l : p.target->layers)
                if (l.name == want)
                {
                    ex = &l;
                    break;
                }
            if (!ex)
                for (const auto &l : p.target->layers)
                    if (strEqualNoCase(l.name, want))
                    {
                        ex = &l;
                        break;
                    }
            if (p.overwriteLayer)
            {
                if (!ex)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "create: Cannot find layer '" + want +
                                    "'.");
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "create: Cannot create layer '" + want +
                                    "'.");
                    return fail(1);
                }
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            ".shp file is unreadable, or corrupt.");
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "Failed to open file " + p.output + "/" +
                                ex->name +
                                ".shp.It may be corrupt or read-only "
                                "file accessed in update mode.");
                for (const char *sfx :
                     {".shp", ".shx", ".dbf", ".prj", ".cpg", ".qix"})
                    ::remove((p.output + "/" + ex->name + sfx).c_str());
            }
            else if (ex)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: Layer '" + want +
                                "' already exists. Specify the "
                                "--overwrite-layer option to overwrite "
                                "it.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: Cannot create layer '" + want +
                                "'.");
                return fail(1);
            }
            bool lf = false;
            ProgressSpan ps;
            g_shpCreateErrMode = fromSchema ? 2 : 1;
            writeShapefile(defs[0], p.output + "/" + defs[0].name + ".shp",
                           true, ps, lf);
            g_shpCreateErrMode = 0;
            buildHandover();
            endBar();
            return 0;
        }
        if (!singleFile)
        {
            if (mkdir(p.output.c_str(), 0755) != 0 && errno != EEXIST)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Failed to create directory " + p.output +
                                "\nfor shapefile datastore.");
                return fail(1);
            }
        }
        if (!applySelection())
            return fail(1);
        if (multiRefused())
            return fail(1);
        warnUnsupportedCo("ESRI Shapefile", p.co);
        warnUnsupportedLco("ESRI Shapefile", p.output, p.lco);
        OgrLayer &lyr = defs[0];
        std::string outPath =
            singleFile ? p.output : p.output + "/" + lyr.name + ".shp";
        bool lf = false;
        ProgressSpan ps;
        g_shpCreateErrMode = fromSchema ? 2 : 1;
        writeShapefile(lyr, outPath, true, ps, lf);
        g_shpCreateErrMode = 0;
        if (lf)
            return fail(1);
        buildHandover();
        endBar();
        return 0;
    }

    cplErrorStr(CE_Failure, CPLE_AppDefined,
                "create: output driver '" + driver +
                    "' is not supported by this build");
    return fail(1);
}

std::string createGdalgCli(ParseResult &r)
{
    std::string cli = handlerInvokedCli();
    for (const auto &v : r.list("input-format"))
        cli += " --input-format " + vvGq(v);
    for (const auto &v : r.list("open-option"))
        cli += " --open-option " + vvGq(v);
    if (r.get("input"))
        cli += " --input " + vvGq(r.str("input"));
    if (!r.list("input-layer").empty())
        cli += " --input-layer " + vvGq(vvJoinComma(r.list("input-layer")));
    for (const auto &v : r.list("output-open-option"))
        cli += " --output-open-option " + vvGq(v);
    for (const auto &v : r.list("creation-option"))
        cli += " --creation-option " + vvGq(v);
    for (const auto &v : r.list("layer-creation-option"))
        cli += " --layer-creation-option " + vvGq(v);
    if (r.flag("overwrite-layer"))
        cli += " --overwrite-layer";
    if (!r.str("output-layer").empty())
        cli += " --output-layer " + vvGq(r.str("output-layer"));
    if (r.get("geometry-type"))
        cli += " --geometry-type " + vvGq(r.str("geometry-type"));
    if (r.get("geometry-field"))
        cli += " --geometry-field " + vvGq(r.str("geometry-field"));
    if (r.get("crs"))
        cli += " --crs " + vvGq(r.str("crs"));
    if (r.get("fid"))
        cli += " --fid " + vvGq(r.str("fid"));
    if (r.get("schema"))
        cli += " --schema " + vvGq(r.str("schema"));
    for (const auto &v : r.list("field"))
        cli += " --field " + vvGq(v);
    cli += " --output-format stream --output streamed_dataset";
    return cli;
}

int vectorCreateHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::string output = r.str("output");
    std::string driver;
    if (vvResolveVerbFormats(cmd, r, driver))
        return 1;

    bool overwrite = r.flag("overwrite");
    bool update = r.flag("update");
    bool ovwl = r.flag("overwrite-layer");
    bool updateMode = (update || ovwl) && !overwrite;

    bool openFailed = false, existsFailed = false;
    std::unique_ptr<OgrDataset> likeDs;
    std::string like = r.str("input");
    if (!like.empty() && vvOpenInputDsNoUsage(cmd, r, like, likeDs))
        openFailed = true;
    if (output == "/vsistdout/")
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Read or update mode not supported on /vsistdout");

    std::unique_ptr<OgrDataset> target;
    bool memOut = strEqualNoCase(driver, "MEM");
    if (updateMode && !memOut && output != "/vsistdout/")
    {
        std::string err;
        target = openVectorDataset(output, err, {});
        if (!target)
        {
            if (err == "missing")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            datasetMissingMessage(output));
            else if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + output +
                                "' not recognized as being in a "
                                "supported file format.");
            openFailed = true;
        }
    }
    else if (!overwrite && !updateMode && !memOut &&
             fileExistsCv(output))
    {
        // the reference probes with an update-mode open: read-only
        // sources (GDALG streams among them) stay plain "File"
        bool isDs = false;
        if (!strEndsWith(strToLower(output), ".gdalg.json"))
        {
            std::string e2;
            cplPushQuietHandler();
            auto d2 = openVectorDataset(output, e2, {});
            cplPopHandler();
            isDs = d2 != nullptr;
            if (!isDs)
                isDs = datasetIdentify(output, {"raster"});
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    std::string("create: ") + (isDs ? "Dataset" : "File") +
                        " '" + output +
                        "' already exists. You may specify the "
                        "--overwrite/--overwrite-layer/--update option.");
        existsFailed = true;
    }
    if (overwrite && !memOut && fileExistsCv(output))
        overwriteDeleteFileset(output);
    if (openFailed)
    {
        handlerPrintUsage();
        return 1;
    }

    // like/schema/field mutual exclusion, then the create-specific
    // combined refusal; an exists failure accumulates with these
    // reports ahead of the single usage block
    bool mutexFailed = false;
    {
        // spec order: every set member reports the first-set one
        std::vector<std::string> group;
        for (const char *n : {"input", "schema", "field"})
            if (r.get(n))
                group.push_back(n);
        for (size_t i = 1; i < group.size(); ++i)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Argument '" + group[i] +
                            "' is mutually exclusive with '" + group[0] +
                            "'.");
            mutexFailed = true;
        }
        bool tmplGiven = r.get("input") || r.get("schema");
        bool extras = r.get("geometry-field") || r.get("geometry-type") ||
                      r.get("field") || r.get("crs") || r.get("fid");
        if (tmplGiven && extras)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: When --schema or --like is specified, "
                        "--geometry-field, --geometry-type, --field, "
                        "--crs and --fid options must not be specified.");
            mutexFailed = true;
        }
    }
    if (mutexFailed || existsFailed)
    {
        handlerPrintUsage();
        return 1;
    }

    VectorCreateRun p;
    p.output = output;
    p.format = r.str("output-format");
    p.overwrite = overwrite;
    p.update = update;
    p.overwriteLayer = ovwl;
    p.schemaSet = r.get("schema") != nullptr;
    p.schemaSpec = r.str("schema");
    p.layerSel = r.list("input-layer");
    p.outputLayerSet = r.get("output-layer") != nullptr;
    p.outputLayer = r.str("output-layer");
    p.geomTypeSet = r.get("geometry-type") != nullptr;
    p.geomTypeName = r.str("geometry-type");
    p.geomFieldSet = r.get("geometry-field") != nullptr;
    p.geomFieldName = r.str("geometry-field");
    p.crsSet = r.get("crs") != nullptr;
    p.crsInput = r.str("crs");
    p.fidSet = r.get("fid") != nullptr;
    p.fid = r.str("fid");
    p.fieldDefs = r.list("field");
    p.co = r.list("creation-option");
    p.lco = r.list("layer-creation-option");
    p.gdalgCli = createGdalgCli(r);
    p.likeDs = std::move(likeDs);
    p.target = std::move(target);
    return vectorCreateCore(p);
}

}  // namespace

std::string ogrJsonCoord(double v)
{
    return fmtCoord(v);
}

int shpTypeForGeomProbe(int geomType, bool hasZ, bool hasM)
{
    return shpTypeForGeom(geomType, hasZ, hasM);
}

size_t gjFeatureLineSize(const OgrLayer &lyr, const OgrFeature &f)
{
    cplPushQuietHandler();
    std::string tmp;
    geoJsonFeatureLine(tmp, lyr, f);
    cplPopHandler();
    return tmp.size();
}

std::string shpGeomMismatchError(int shpType, const OgrGeometry &g)
{
    if (geomStructEmpty(g))
        return "";
    int want = shpType < 0 ? 3 : shpType;
    bool acceptable = want == 31
                          ? g.type == 3 || g.type == 6
                          : geomAcceptable(g, want % 10);
    if (acceptable)
        return "";
    std::string gname = kOgrGeomUpper[g.type];
    switch (want == 31 ? 0 : want % 10)
    {
        case 1:
            return "Attempt to write non-point (" + gname +
                   ") geometry to point shapefile.";
        case 8:
            return "Attempt to write non-multipoint (" + gname +
                   ") geometry to multipoint shapefile.";
        case 3:
            return "Attempt to write non-linestring (" + gname +
                   ") geometry to ARC type shapefile.";
        case 5:
            return "Attempt to write non-polygon (" + gname +
                   ") geometry to POLYGON type shapefile.";
    }
    return "";
}

int vectorCreateCoreRun(VectorCreateRun &p)
{
    return vectorCreateCore(p);
}

std::string vectorCreateFieldDefError(const std::string &def)
{
    OgrFieldDefn f;
    std::string err;
    if (createParseFieldDef(def, f, err))
        return "";
    return err;
}

void registerVectorConvertHandler()
{
    registerHandler("vector_convert", vectorConvertHandler);
    registerPreValidator("vector_convert", vectorConvertPreValidator);
}

void registerVectorCreateHandler()
{
    registerHandler("vector_create", vectorCreateHandler);
    registerArgValueCheck(
        "vector_create",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName == "geometry-type")
            {
                int gt;
                bool z, m;
                if (!ogrGeomTypeFromWktName(value, gt, z, m))
                    return "Invalid geometry type '" + value + "'";
            }
            else if (argName == "crs")
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return "Invalid value for 'crs' argument";
            }
            else if (argName == "field")
                return vectorCreateFieldDefError(value);
            return "";
        });
}

namespace
{

void gjNativeFeatureLine(std::string &out, const OgrLayer &lyr,
                         const OgrFeature &feat)
{
    const JVal &nat = *feat.gjNative;
    // a source-level "id" member owns the id: the synthesized "id"
    // field stays out of the rebuilt properties
    bool natId = nat.type == JVal::OBJECT && nat.get("id");
    bool first = true;
    auto sep = [&] {
        if (!first)
            out += ',';
        first = false;
    };
    auto propsOut = [&] {
        out += "\"properties\":{";
        bool pf = true;
        for (size_t k = 0; k < lyr.fields.size(); ++k)
        {
            if (k >= feat.values.size() || !feat.values[k].set)
                continue;
            if (natId && lyr.fields[k].name == "id")
                continue;
            if (gjNonFiniteReal(lyr.fields[k], feat.values[k].v))
            {
                gjWarnNonFiniteOnce();
                continue;
            }
            if (!pf)
                out += ',';
            pf = false;
            out += '"' + jsonEscape(lyr.fields[k].name) + "\":";
            out += propValueJson(lyr.fields[k], feat.values[k].v);
        }
        out += '}';
    };
    auto geomOut = [&] {
        out += "\"geometry\":";
        int gfail = feat.hasGeom ? geomJsonExportFail(feat.geom) : 0;
        if (gfail == 2)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Infinite or NaN coordinate encountered");
        if (feat.hasGeom && !gfail)
            geomJson(out, feat.geom);
        else if (feat.hasGeom && feat.geom.type == 7)
            out += "{\"type\":\"GeometryCollection\",\"geometries\":null}";
        else
            out += "null";
    };
    out += '{';
    bool sawProps = false, sawGeom = false;
    if (nat.type == JVal::OBJECT)
        for (const auto &kv : nat.obj)
        {
            if (kv.first == "type")
            {
                sep();
                out += "\"type\":\"Feature\"";
            }
            else if (kv.first == "properties")
            {
                sep();
                propsOut();
                sawProps = true;
            }
            else if (kv.first == "geometry")
            {
                sep();
                geomOut();
                sawGeom = true;
            }
            else
            {
                sep();
                out += '"' + jsonEscape(kv.first) + "\":";
                serializeCompact(out, kv.second);
            }
        }
    if (!sawProps)
    {
        sep();
        propsOut();
    }
    if (!sawGeom)
    {
        sep();
        geomOut();
    }
    out += '}';
}

}  // namespace

std::string geoJsonUpdateRewriteDoc(const OgrLayer &lyr,
                                    const JVal *root)
{
    g_gjPrec = -1;
    std::vector<const OgrFeature *> order;
    order.reserve(lyr.features.size());
    for (const auto &f : lyr.features)
        order.push_back(&f);
    std::stable_sort(order.begin(), order.end(),
                     [](const OgrFeature *a, const OgrFeature *b)
                     { return a->fid < b->fid; });

    bool rootUsable = root && root->type == JVal::OBJECT &&
                      root->getString("type") == "FeatureCollection";

    std::string out = "{\n\"type\": \"FeatureCollection\",\n";
    auto nameLine = [&] {
        std::string esc = jsonEscape(lyr.name);
        std::string slashed;
        for (char ch : esc)
        {
            if (ch == '/')
                slashed += "\\/";
            else
                slashed += ch;
        }
        return "\"name\": \"" + slashed + "\",\n";
    };
    size_t bboxAt = std::string::npos;
    // the writer's own name/crs/bbox re-emit after the foreign native
    // members; only a native name equal to the layer name (i.e. no
    // rename) keeps its original position
    const JVal *natName = rootUsable ? root->get("name") : nullptr;
    bool nameInPlace = natName && natName->type == JVal::STRING &&
                       natName->s == lyr.name;
    if (!rootUsable)
        out += nameLine();
    if (rootUsable)
    {
        for (const auto &kv : root->obj)
        {
            if (kv.first == "type" || kv.first == "features" ||
                kv.first == "crs" || kv.first == "bbox")
                continue;
            if (kv.first == "name")
            {
                if (nameInPlace)
                    out += nameLine();
            }
            else
                out += '"' + jsonEscape(kv.first) + "\": " +
                       ogrJsonSpacedSerialize(kv.second) + ",\n";
        }
        if (!nameInPlace)
            out += nameLine();
        if (root->get("crs"))
            out += crsJsonLine(lyr);
        if (root->get("bbox"))
            bboxAt = out.size();
    }
    else
        out += crsJsonLine(lyr);

    GjBboxInfo lb;
    if (bboxAt != std::string::npos)
        for (const OgrFeature *f : order)
        {
            if (!f->hasGeom)
                continue;
            GjBboxInfo fb = gjGeomBbox(f->geom, false);
            if (!fb.any)
                continue;
            if (!lb.any)
                lb = fb;
            else
            {
                lb.minx = std::min(lb.minx, fb.minx);
                lb.maxx = std::max(lb.maxx, fb.maxx);
                lb.miny = std::min(lb.miny, fb.miny);
                lb.maxy = std::max(lb.maxy, fb.maxy);
                lb.minz = std::min(lb.minz, fb.minz);
                lb.maxz = std::max(lb.maxz, fb.maxz);
                lb.hasZ = lb.hasZ || fb.hasZ;
            }
        }

    out += "\"features\": [\n";
    bool any = false;
    for (const OgrFeature *f : order)
    {
        if (any)
            out += ",\n";
        any = true;
        if (f->gjNative)
            gjNativeFeatureLine(out, lyr, *f);
        else
            geoJsonFeatureLine(out, lyr, *f);
    }
    out += "\n]\n}\n";

    if (bboxAt != std::string::npos)
    {
        std::string line;
        if (lb.any)
        {
            double vals[6] = {lb.minx, lb.miny, lb.minz,
                              lb.maxx, lb.maxy, lb.maxz};
            line = "\"bbox\": [ ";
            int idx = 0;
            for (int k = 0; k < 6; ++k)
            {
                if (!lb.hasZ && (k == 2 || k == 5))
                    continue;
                if (idx++)
                    line += ", ";
                line += strPrintf("%.15g", vals[k]);
            }
            line += " ],";
        }
        while (line.size() < 131)
            line += ' ';
        out.insert(bboxAt, line + "\n");
    }
    g_gjPrec = -1;
    return out;
}

bool geoJsonUpdateRewrite(const OgrLayer &lyr, const JVal *root,
                          const std::string &path)
{
    return writeStringToFile(path, geoJsonUpdateRewriteDoc(lyr, root));
}
