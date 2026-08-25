// color-map / nodata-to-alpha / rgb-to-palette: color-oriented verbs
// producing an in-memory result written through the convert path
#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "progress.h"
#include "spec.h"
#include "util.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>

namespace
{

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

std::string fmt18c(double d)
{
    if (std::isnan(d))
        return "nan";
    return strPrintf("%.18g", d);
}

std::string xmlEscC(const std::string &s)
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

std::string gdalgQuoteC(const std::string &tok)
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

std::string dirNameC(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

std::string baseNameC(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string relToOutputC(const std::string &input, const std::string &output,
                         int &relative)
{
    std::string outDir = dirNameC(output);
    if (outDir.empty())
    {
        relative = input.find('/') == std::string::npos ? 1 : 0;
        if (input.compare(0, 2, "./") == 0)
        {
            relative = 1;
            return input.substr(2);
        }
        return input;
    }
    std::string pfx = outDir + "/";
    if (input.compare(0, pfx.size(), pfx) == 0)
    {
        relative = 1;
        return input.substr(pfx.size());
    }
    relative = 0;
    return input;
}

bool parseFullDoubleC(const std::string &tok, double &out)
{
    if (tok.empty())
        return false;
    char *end = nullptr;
    out = strtod(tok.c_str(), &end);
    return end && *end == '\0';
}

std::string realEchoC(const std::string &tok)
{
    double v = 0;
    if (!parseFullDoubleC(tok, v))
        return tok;
    return strPrintf("%.17g", v);
}

// VRT LUT/GeoTransform-adjacent real serialization: %g when it
// round-trips, otherwise the full %.17g expansion
std::string fmtLutReal(double v)
{
    std::string s = strPrintf("%g", v);
    if (strtod(s.c_str(), nullptr) == v)
        return s;
    return strPrintf("%.17g", v);
}

void emitVrtHeaderC(std::string &x, const RasterDatasetBase &ds,
                    const std::string &pad = "")
{
    x += pad +
         strPrintf("<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\">\n",
                   ds.width, ds.height);
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
        x += pad + "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             xmlEscC(wkt) + "</SRS>\n";
    }
    if (ds.hasGT)
    {
        x += pad + "  <GeoTransform>";
        for (int i = 0; i < 6; i++)
        {
            if (i)
                x += ",";
            x += strPrintf("%24.16e", ds.gt[i]);
        }
        x += "</GeoTransform>\n";
    }
}

// ------------------------------------------------------------------
// materialized result dataset
// ------------------------------------------------------------------

class ClrDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    std::vector<std::vector<double>> data;
    bool evaluated = false;
    bool barSuppressed = false;
    std::function<std::string(const std::string &, const std::string &)>
        vrtCustom;

    ClrDataset(std::unique_ptr<RasterDatasetBase> s, bool copyMeta)
        : src(std::move(s))
    {
        path = src->path;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = src->width;
        height = src->height;
        hasGT = src->hasGT;
        memcpy(gt, src->gt, sizeof gt);
        srs = src->srs.clone();
        hasSrs = src->hasSrs;
        srsSynthetic = src->srsSynthetic;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();
        if (copyMeta)
        {
            metadata = src->metadata;
            domainOrder = src->domainOrder;
            sortedDomains = src->sortedDomains;
            xmlDomains = src->xmlDomains;
            files = src->files;
            pamPath = src->pamPath;
            pamExists = src->pamExists;
            pamSrsRaw = src->pamSrsRaw;
            pamSrsMapping = src->pamSrsMapping;
            pamGtRaw = src->pamGtRaw;
            pamMdi = src->pamMdi;
            pamXmlDomains = src->pamXmlDomains;
            pamBands = src->pamBands;
        }
        pamSuppressItems = true;
    }

    bool suppressWriteBar() const override { return barSuppressed; }

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
        const std::vector<double> &vals = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        size_t sz = (size_t)dtypeSizeBytes(t);
        out.assign(vals.size() * sz, 0);
        for (size_t i = 0; i < vals.size(); ++i)
            rasterEncodeReal(t, out.data() + i * sz,
                             rasterFinishReal(vals[i], t), 0);
        return true;
    }

    std::string customVrtXml(const std::string &input,
                             const std::string &output) override
    {
        if (vrtCustom)
            return vrtCustom(input, output);
        return std::string();
    }
};

// ------------------------------------------------------------------
// shared handler plumbing
// ------------------------------------------------------------------

struct ClrCommon
{
    std::string input, output, drv;
    bool quiet = false, overwrite = false, append = false;
    std::unique_ptr<RasterDatasetBase> src;
};

int clrBegin(ParseResult &r, ClrCommon &c)
{
    c.input = r.str("input");
    c.output = r.str("output");
    std::string of = r.str("output-format");
    rasterOutFormatIssue(of, c.drv);
    c.quiet = r.flag("quiet");
    c.overwrite = r.flag("overwrite");
    c.append = r.flag("append");
    std::string err;
    c.src = openRaster(c.input, err);
    if (!c.src)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + c.input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    return -1;
}

bool clrEvalTarget(const std::string &drv, const std::string &output)
{
    return drv != "GDALG" &&
           !(drv.empty() && strEndsWith(strToLower(output), ".gdalg.json"));
}

// the resolved output form as the write path will see it, for the
// per-form soft/hard error split of the algorithm-stage failures
std::string clrResolveForm(const std::string &drv,
                           const std::string &output)
{
    if (!drv.empty())
        return drv;
    std::string base = baseNameC(output);
    std::string lbase = strToLower(base);
    if (lbase.size() > 11 &&
        lbase.compare(lbase.size() - 11, 11, ".gdalg.json") == 0)
        return "GDALG";
    size_t dot = base.find_last_of('.');
    std::string ext =
        dot == std::string::npos ? "" : strToLower(base.substr(dot + 1));
    if (ext == "vrt")
        return "VRT";
    return "GTiff";
}

int clrPreValidator(const CmdSpec &cmd, ParseResult &r)
{
    if (r.get("band") && atoi(r.str("band").c_str()) < 1)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Value of 'band' should greater or equal to 1.");
        handlerPrintUsage();
        return 1;
    }
    std::string format = r.str("output-format");
    std::string drv;
    std::string issue = rasterOutFormatIssue(format, drv);
    if (!issue.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, cmd.name + ": " + issue);
        handlerPrintUsage();
        return 1;
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(false, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

std::unique_ptr<RasterDatasetBase> clrQuietOpen(const std::string &path)
{
    cplPushQuietHandler();
    std::string err;
    auto ds = openRaster(path, err);
    cplPopHandler();
    return ds;
}

bool clrPostBandCheck(const std::string &verb, ParseResult &r)
{
    const ArgValue *v = r.get("band");
    if (!v)
        return false;
    auto ds = clrQuietOpen(r.str("input"));
    if (!ds)
        return false;
    int b = atoi(v->str().c_str());
    if (b < 1 || b > (int)ds->bands.size())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Value of 'band' should be greater or "
                              "equal than 1 and less or equal than %d.",
                              verb.c_str(), (int)ds->bands.size()));
        return true;
    }
    return false;
}

bool clrPostValidator(const CmdSpec &cmd, ParseResult &r, bool)
{
    return clrPostBandCheck(cmd.name, r);
}

// partial-bar crash choreography: the reference dies mid-algorithm
// with the median-cut quarter of the bar already drawn
[[noreturn]] void crashNow(bool bar, double frac, int sig)
{
    if (bar)
    {
        TermProgress tp;
        tp.update(frac);
    }
    fflush(stdout);
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(1);
}

// ------------------------------------------------------------------
// text color map parsing
// ------------------------------------------------------------------

struct CMapEntry
{
    double value = 0;
    bool pct = false;
    double c[4] = {0, 0, 0, 255};
};

struct CMap
{
    std::vector<CMapEntry> entries;
    bool hasNv = false;
    CMapEntry nv;
    bool anyPct = false;
    std::string err;
};

struct NamedColor
{
    const char *name;
    int r, g, b;
};

const NamedColor kNamedColors[] = {
    {"white", 255, 255, 255}, {"black", 0, 0, 0},
    {"red", 255, 0, 0},       {"green", 0, 255, 0},
    {"blue", 0, 0, 255},      {"yellow", 255, 255, 0},
    {"magenta", 255, 0, 255}, {"cyan", 0, 255, 255},
    {"aqua", 0, 191, 191},    {"grey", 191, 191, 191},
    {"gray", 191, 191, 191},  {"orange", 255, 127, 0},
    {"brown", 191, 127, 63},  {"purple", 127, 0, 255},
    {"violet", 127, 0, 255},  {"indigo", 0, 127, 255},
};

bool lookupNamedColor(const std::string &tok, CMapEntry &e)
{
    for (const NamedColor &nc : kNamedColors)
        if (strEqualNoCase(tok, nc.name))
        {
            e.c[0] = nc.r;
            e.c[1] = nc.g;
            e.c[2] = nc.b;
            e.c[3] = 255;
            return true;
        }
    return false;
}

std::vector<std::string> cmapTokens(const std::string &line)
{
    std::vector<std::string> toks;
    std::string cur;
    for (char ch : line)
    {
        if (ch == ' ' || ch == '\t' || ch == ',' || ch == ':' ||
            ch == '\r')
        {
            if (!cur.empty())
            {
                toks.push_back(cur);
                cur.clear();
            }
        }
        else
            cur += ch;
    }
    if (!cur.empty())
        toks.push_back(cur);
    return toks;
}

CMap loadColorMap(const std::string &path)
{
    CMap m;
    std::string text;
    if (!readFileToString(path, text))
    {
        m.err = "Cannot find " + path;
        return m;
    }
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string line = nl == std::string::npos
                               ? text.substr(pos)
                               : text.substr(pos, nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        std::vector<std::string> toks = cmapTokens(line);
        if (toks.empty() || toks[0][0] == '#')
            continue;
        if (toks.size() < 2)
            continue;
        CMapEntry e;
        bool isNv = strEqualNoCase(toks[0], "nv");
        if (!isNv)
        {
            std::string vt = toks[0];
            if (!vt.empty() && vt.back() == '%')
            {
                e.pct = true;
                vt.pop_back();
            }
            e.value = strtod(vt.c_str(), nullptr);
        }
        if (toks.size() == 2)
        {
            if (!lookupNamedColor(toks[1], e))
            {
                m.err = "Unknown color : " + toks[1];
                m.entries.clear();
                m.hasNv = false;
                return m;
            }
        }
        else if (toks.size() >= 4)
        {
            for (int i = 0; i < 3; i++)
                e.c[i] = atoi(toks[(size_t)i + 1].c_str());
            e.c[3] = toks.size() >= 5 ? atoi(toks[4].c_str()) : 255;
        }
        else
        {
            if (!lookupNamedColor(toks[1], e))
            {
                m.err = "Unknown color : " + toks[1];
                m.entries.clear();
                m.hasNv = false;
                return m;
            }
        }
        if (isNv)
        {
            m.hasNv = true;
            m.nv = e;
        }
        else
        {
            if (e.pct)
                m.anyPct = true;
            m.entries.push_back(e);
        }
    }
    if (m.entries.empty())
        m.err = "No color association found in " + path;
    return m;
}

// GetStatistics(exact, force) semantics: cached exact PAM statistics
// are reused, otherwise a full compute lands in the input's PAM
bool cmapSrcMinMax(RasterDatasetBase &src, Band &b, double &mn, double &mx)
{
    const std::string *m0 = b.getMd("", "STATISTICS_MINIMUM");
    const std::string *m1 = b.getMd("", "STATISTICS_MAXIMUM");
    const std::string *m2 = b.getMd("", "STATISTICS_MEAN");
    const std::string *m3 = b.getMd("", "STATISTICS_STDDEV");
    const std::string *ap = b.getMd("", "STATISTICS_APPROXIMATE");
    if (m0 && m1 && m2 && m3 && !ap)
    {
        mn = strtod(m0->c_str(), nullptr);
        mx = strtod(m1->c_str(), nullptr);
        return true;
    }
    StatsResult sr = vrtAwareForcedStats(src, b, false);
    if (src.pamDirty)
        writePam(src);
    mn = sr.mn;
    mx = sr.mx;
    return sr.ok;
}

// ------------------------------------------------------------------
// breakpoint model: both the materialized pixels and the VRT <LUT>
// come from the same piecewise-linear breakpoint list
// ------------------------------------------------------------------

struct Bp
{
    double v;
    double c[4];
    int rank;  // nv entries sort before same-valued map entries
};

void pushBp(std::vector<Bp> &out, double v, const double c[4], int rank)
{
    Bp b;
    b.v = v;
    memcpy(b.c, c, sizeof b.c);
    b.rank = rank;
    out.push_back(b);
}

// entries must be value-sorted; mode: 0 interpolate, 1 exact, 2 nearest
std::vector<Bp> buildBreakpoints(const std::vector<CMapEntry> &ent,
                                 int mode, bool hasNv, const double nvc[4],
                                 double nd)
{
    const double miss[4] = {0, 0, 0, 0};
    std::vector<Bp> aug;
    if (mode == 1)
    {
        for (const CMapEntry &e : ent)
        {
            pushBp(aug, std::nextafter(e.value, -HUGE_VAL), miss, 1);
            pushBp(aug, e.value, e.c, 1);
            pushBp(aug, std::nextafter(e.value, HUGE_VAL), miss, 1);
        }
        if (hasNv)
        {
            pushBp(aug, std::nextafter(nd, -HUGE_VAL), miss, 1);
            pushBp(aug, nd, nvc, 0);
            pushBp(aug, std::nextafter(nd, HUGE_VAL), miss, 1);
        }
        std::stable_sort(aug.begin(), aug.end(),
                         [](const Bp &a, const Bp &b) {
                             if (a.v != b.v)
                                 return a.v < b.v;
                             return a.rank < b.rank;
                         });
        return aug;
    }
    for (const CMapEntry &e : ent)
        pushBp(aug, e.value, e.c, 1);
    if (hasNv)
    {
        const CMapEntry *lower = nullptr, *upper = nullptr;
        for (const CMapEntry &e : ent)
        {
            if (e.value < nd && (!lower || e.value > lower->value))
                lower = &e;
            if (e.value > nd && (!upper || e.value < upper->value))
                upper = &e;
        }
        if (lower)
            pushBp(aug, std::nextafter(nd, -HUGE_VAL), lower->c, 1);
        pushBp(aug, nd, nvc, 0);
        if (upper)
            pushBp(aug, std::nextafter(nd, HUGE_VAL), upper->c, 1);
    }
    std::stable_sort(aug.begin(), aug.end(),
                     [](const Bp &a, const Bp &b) {
                         if (a.v != b.v)
                             return a.v < b.v;
                         return a.rank < b.rank;
                     });
    if (mode == 0 || aug.empty())
        return aug;
    // nearest: a step list switching color at each midpoint; degenerate
    // midpoints (landing on the left value) contribute one breakpoint
    std::vector<Bp> out;
    out.push_back(aug[0]);
    for (size_t i = 1; i < aug.size(); ++i)
    {
        const Bp &a = aug[i - 1];
        const Bp &b = aug[i];
        double m = (a.v + b.v) / 2.0;
        double lo = std::nextafter(m, -HUGE_VAL);
        if (lo <= a.v)
            pushBp(out, m > a.v ? m : b.v, b.c, 1);
        else
        {
            pushBp(out, lo, a.c, 1);
            pushBp(out, m, b.c, 1);
        }
    }
    return out;
}

void evalBreakpoints(const std::vector<Bp> &bps, double v, double out[4])
{
    if (bps.empty())
    {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    size_t idx = bps.size();
    for (size_t i = 0; i < bps.size(); ++i)
        if (bps[i].v >= v)
        {
            idx = i;
            break;
        }
    if (idx == bps.size())
    {
        memcpy(out, bps.back().c, 4 * sizeof(double));
        return;
    }
    if (bps[idx].v == v || idx == 0)
    {
        memcpy(out, bps[idx].c, 4 * sizeof(double));
        return;
    }
    const Bp &a = bps[idx - 1];
    const Bp &b = bps[idx];
    double t = (v - a.v) / (b.v - a.v);
    for (int i = 0; i < 4; i++)
        out[i] = a.c[i] + t * (b.c[i] - a.c[i]);
}

std::string bandBlockAttrsC(int w, int h, int bx, int by)
{
    std::string a;
    if (bx != std::min(w, 128))
        a += strPrintf(" blockXSize=\"%d\"", bx);
    if (by != std::min(h, 128))
        a += strPrintf(" blockYSize=\"%d\"", by);
    return a;
}

double byteRound(double v)
{
    double r = std::floor(v + 0.5);
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    return r;
}

// ------------------------------------------------------------------
// color-map
// ------------------------------------------------------------------

int rasterColorMapHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("color-map");
    ClrCommon c;
    int rc = clrBegin(r, c);
    if (rc >= 0)
        return rc;

    int band = r.get("band") ? atoi(r.str("band").c_str()) : 1;
    bool haveMap = r.get("color-map") != nullptr;
    std::string mapFile = haveMap ? r.str("color-map") : std::string();
    std::string sel = r.str("color-selection", "interpolate");
    int mode = strEqualNoCase(sel, "exact")     ? 1
               : strEqualNoCase(sel, "nearest") ? 2
                                                : 0;
    bool addAlpha = r.flag("add-alpha");
    int nOut = addAlpha ? 4 : 3;

    auto ds = std::make_unique<ClrDataset>(std::move(c.src), !haveMap);
    ClrDataset *cd = ds.get();
    if (!haveMap)
        cd->setMd("IMAGE_STRUCTURE", "INTERLEAVE", "BAND");
    static const char *kRgbaNames[4] = {"Red", "Green", "Blue", "Alpha"};
    int srcBlockX = std::min(cd->width, 128);
    int srcBlockY = std::min(cd->height, 128);
    if (band >= 1 && band <= (int)cd->src->bands.size())
    {
        srcBlockX = cd->src->bands[(size_t)band - 1].blockX;
        srcBlockY = cd->src->bands[(size_t)band - 1].blockY;
    }
    for (int i = 0; i < nOut; i++)
    {
        Band b;
        b.index = i + 1;
        b.type = DType::Byte;
        b.colorInterp = kRgbaNames[i];
        b.blockX = srcBlockX;
        b.blockY = srcBlockY;
        cd->bands.push_back(std::move(b));
    }
    cd->data.resize((size_t)nOut);

    std::string extra;
    if (r.get("band"))
        extra += strPrintf(" --band %d", band);
    if (haveMap)
        extra += " --color-map " + gdalgQuoteC(mapFile);
    if (addAlpha)
        extra += " --add-alpha";
    if (r.get("color-selection"))
        extra += " --color-selection " + sel;

    std::string form = clrResolveForm(c.drv, c.output);
    std::string verb = cmd.name;
    cd->demWriteDefaultGt = haveMap && !cd->src->hasGT;
    if (form == "VRT")
        cd->barSuppressed = true;

    auto mat = [cd, band, haveMap, mapFile, mode, addAlpha, nOut, form,
                verb](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        RasterDatasetBase &src = *cd->src;
        Band &srcBand = src.bands[(size_t)band - 1];
        size_t n = (size_t)cd->width * cd->height;
        auto zeroFill = [&]()
        {
            for (int i = 0; i < nOut; i++)
                cd->data[(size_t)i].assign(n, 0.0);
            cd->evaluated = true;
        };

        if (!haveMap)
        {
            if (srcBand.colorTable.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            verb + ": Input dataset has no color table "
                                   "and 'color-map' option was not "
                                   "specified.");
                return 1;
            }
            // color-table expansion: a straight per-pixel table lookup
            const std::vector<ColorEntry> &ct = srcBand.colorTable;
            std::vector<double> vals;
            if (!src.readBand(band, vals))
                return 1;
            for (int i = 0; i < nOut; i++)
                cd->data[(size_t)i].assign(n, 0.0);
            for (size_t px = 0; px < n; ++px)
            {
                long long idx = (long long)vals[px];
                if (idx < 0 || idx >= (long long)ct.size())
                    continue;
                const ColorEntry &e = ct[(size_t)idx];
                cd->data[0][px] = e.c1;
                cd->data[1][px] = e.c2;
                cd->data[2][px] = e.c3;
                if (nOut == 4)
                    cd->data[3][px] = e.c4;
            }
            cd->evaluated = true;
            cd->vrtCustom = [cd, band, nOut](const std::string &input,
                                             const std::string &output)
            {
                RasterDatasetBase &s = *cd->src;
                const Band &sb = s.bands[(size_t)band - 1];
                int relative = 0;
                std::string rel = relToOutputC(input, output, relative);
                std::string x;
                emitVrtHeaderC(x, *cd);
                {
                    auto it = cd->metadata.find("");
                    if (it != cd->metadata.end())
                    {
                        bool any = false;
                        for (const auto &kv : it->second)
                            if (kv.first != "INTERLEAVE")
                                any = true;
                        if (any)
                        {
                            x += "  <Metadata>\n";
                            for (const auto &kv : it->second)
                                x += "    <MDI key=\"" +
                                     xmlEscC(kv.first) + "\">" +
                                     xmlEscC(kv.second) + "</MDI>\n";
                            x += "  </Metadata>\n";
                        }
                    }
                    x += "  <Metadata domain=\"IMAGE_STRUCTURE\">\n";
                    x += "    <MDI key=\"INTERLEAVE\">BAND</MDI>\n";
                    x += "  </Metadata>\n";
                }
                static const char *names[4] = {"Red", "Green", "Blue",
                                               "Alpha"};
                std::string battrs = bandBlockAttrsC(
                    cd->width, cd->height, sb.blockX, sb.blockY);
                for (int i = 0; i < nOut; i++)
                {
                    x += strPrintf("  <VRTRasterBand dataType=\"Byte\" "
                                   "band=\"%d\"%s>\n",
                                   i + 1, battrs.c_str());
                    x += strPrintf("    <ColorInterp>%s</ColorInterp>\n",
                                   names[i]);
                    x += "    <ComplexSource>\n";
                    x += strPrintf("      <SourceFilename "
                                   "relativeToVRT=\"%d\">%s"
                                   "</SourceFilename>\n",
                                   relative, xmlEscC(rel).c_str());
                    x += strPrintf("      <SourceBand>%d</SourceBand>\n",
                                   band);
                    x += strPrintf(
                        "      <SourceProperties RasterXSize=\"%d\" "
                        "RasterYSize=\"%d\" DataType=\"%s\" "
                        "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                        s.width, s.height, dtypeName(sb.type), sb.blockX,
                        sb.blockY);
                    x += strPrintf("      <SrcRect xOff=\"0\" yOff=\"0\" "
                                   "xSize=\"%d\" ySize=\"%d\" />\n",
                                   s.width, s.height);
                    x += strPrintf("      <DstRect xOff=\"0\" yOff=\"0\" "
                                   "xSize=\"%d\" ySize=\"%d\" />\n",
                                   s.width, s.height);
                    x += strPrintf("      <ColorTableComponent>%d"
                                   "</ColorTableComponent>\n",
                                   i + 1);
                    x += "    </ComplexSource>\n";
                    x += "  </VRTRasterBand>\n";
                }
                x += "</VRTDataset>\n";
                return x;
            };
            return 0;
        }

        CMap m = loadColorMap(mapFile);
        if (!m.err.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, m.err);
            if (form == "VRT" || form == "stream")
                return 1;
            cd->barSuppressed = true;
            zeroFill();
            cd->vrtCustom = nullptr;
            return 0;
        }
        if (m.hasNv && !srcBand.hasNodata)
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Input dataset has no nodata value. Ignoring "
                        "'nv' entry in color palette");
            m.hasNv = false;
        }
        if (m.anyPct)
        {
            double mn = 0, mx = 0;
            cmapSrcMinMax(src, srcBand, mn, mx);
            for (CMapEntry &e : m.entries)
                if (e.pct)
                {
                    e.value = mn + e.value / 100.0 * (mx - mn);
                    e.pct = false;
                }
        }
        std::stable_sort(m.entries.begin(), m.entries.end(),
                         [](const CMapEntry &a, const CMapEntry &b) {
                             return a.value < b.value;
                         });
        std::vector<Bp> bps =
            buildBreakpoints(m.entries, mode, m.hasNv, m.nv.c,
                             m.hasNv ? srcBand.nodata : 0.0);

        std::vector<double> vals;
        if (!src.readBand(band, vals))
            return 1;
        for (int i = 0; i < nOut; i++)
            cd->data[(size_t)i].resize(n);
        double col[4];
        for (size_t px = 0; px < n; ++px)
        {
            evalBreakpoints(bps, vals[px], col);
            for (int i = 0; i < nOut; i++)
                cd->data[(size_t)i][px] = byteRound(col[i]);
        }
        cd->evaluated = true;

        cd->vrtCustom = [cd, band, nOut, bps](const std::string &input,
                                              const std::string &output)
        {
            RasterDatasetBase &s = *cd->src;
            const Band &sb = s.bands[(size_t)band - 1];
            int relative = 0;
            std::string rel = relToOutputC(input, output, relative);
            std::string x;
            emitVrtHeaderC(x, *cd);
            static const char *names[4] = {"Red", "Green", "Blue",
                                           "Alpha"};
            std::string battrs = bandBlockAttrsC(cd->width, cd->height,
                                                 sb.blockX, sb.blockY);
            for (int i = 0; i < nOut; i++)
            {
                x += strPrintf("  <VRTRasterBand dataType=\"Byte\" "
                               "band=\"%d\"%s>\n",
                               i + 1, battrs.c_str());
                x += strPrintf("    <ColorInterp>%s</ColorInterp>\n",
                               names[i]);
                x += "    <ComplexSource>\n";
                x += strPrintf("      <SourceFilename "
                               "relativeToVRT=\"%d\">%s"
                               "</SourceFilename>\n",
                               relative, xmlEscC(rel).c_str());
                x += strPrintf("      <SourceBand>%d</SourceBand>\n",
                               band);
                x += strPrintf(
                    "      <SourceProperties RasterXSize=\"%d\" "
                    "RasterYSize=\"%d\" DataType=\"%s\" "
                    "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                    s.width, s.height, dtypeName(sb.type), sb.blockX,
                    sb.blockY);
                x += strPrintf("      <SrcRect xOff=\"0\" yOff=\"0\" "
                               "xSize=\"%d\" ySize=\"%d\" />\n",
                               s.width, s.height);
                x += strPrintf("      <DstRect xOff=\"0\" yOff=\"0\" "
                               "xSize=\"%d\" ySize=\"%d\" />\n",
                               s.width, s.height);
                std::string lut;
                for (const Bp &bp : bps)
                {
                    if (!lut.empty())
                        lut += ",";
                    lut += fmtLutReal(bp.v) + ":" +
                           fmtLutReal(bp.c[(size_t)i]);
                }
                x += "      <LUT>" + lut + "</LUT>\n";
                x += "    </ComplexSource>\n";
                x += "  </VRTRasterBand>\n";
            }
            x += "</VRTDataset>\n";
            return x;
        };
        return 0;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (clrEvalTarget(c.drv, c.output))
        matF = mat;
    return rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                    c.overwrite, c.append, c.drv, extra,
                                    matF, nullptr);
}

// ------------------------------------------------------------------
// nodata-to-alpha
// ------------------------------------------------------------------

bool ndRepresentable(double v, DType t)
{
    double f = rasterFinishReal(v, t);
    if (std::isnan(v) && std::isnan(f))
        return true;
    return f == v;
}

int rasterNodataToAlphaHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("nodata-to-alpha");
    ClrCommon c;
    int rc = clrBegin(r, c);
    if (rc >= 0)
        return rc;

    std::vector<std::string> ndToks = r.list("nodata");
    std::vector<double> ndVals;
    for (const std::string &t : ndToks)
        ndVals.push_back(strtod(t.c_str(), nullptr));

    std::string extra;
    if (!ndToks.empty())
    {
        extra += " --nodata ";
        for (size_t i = 0; i < ndToks.size(); ++i)
        {
            if (i)
                extra += ",";
            extra += gdalgQuoteC(realEchoC(ndToks[i]));
        }
    }

    int nBands = (int)c.src->bands.size();
    std::string verb = cmd.name;
    std::string form = clrResolveForm(c.drv, c.output);

    // pure passthrough (no trigger at all) is byte-identical to a plain
    // convert of the input
    if (ndVals.empty() && !(nBands > 0 && c.src->bands[0].hasNodata))
    {
        std::unique_ptr<RasterDatasetBase> base = std::move(c.src);
        std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
        if (clrEvalTarget(c.drv, c.output))
            matF = [](std::unique_ptr<RasterDatasetBase> &) { return 0; };
        return rasterConvertWriteOutput(base, r, c.input, c.output,
                                        c.quiet, c.overwrite, c.append,
                                        c.drv, extra, matF, nullptr);
    }

    bool countBad =
        !ndVals.empty() && (int)ndVals.size() != 1 &&
        (int)ndVals.size() != nBands;
    // a single override goes through per-band SetNoDataValue, which is
    // where representability is checked; the tuple path only records a
    // NODATA_VALUES string and never warns
    bool tuplePath = !countBad && ndVals.size() > 1;
    bool singlePath = !countBad && ndVals.size() == 1;
    std::vector<char> rep((size_t)nBands, 1);
    int unrepCount = 0;
    if (singlePath)
        for (int i = 0; i < nBands; ++i)
            if (!ndRepresentable(ndVals[0], c.src->bands[(size_t)i].type))
            {
                rep[(size_t)i] = 0;
                ++unrepCount;
            }
    bool alphaPath =
        !countBad && (ndVals.empty() || tuplePath ||
                      (singlePath && unrepCount < nBands));

    auto ds = std::make_unique<ClrDataset>(std::move(c.src), true);
    ClrDataset *cd = ds.get();
    for (int i = 0; i < nBands; ++i)
    {
        Band b = cd->src->bands[(size_t)i];
        b.index = i + 1;
        b.hasNodata = false;
        b.nodataIsI64 = b.nodataIsU64 = false;
        cd->bands.push_back(std::move(b));
    }
    if (alphaPath)
    {
        Band a;
        a.index = nBands + 1;
        a.type = DType::Byte;
        a.colorInterp = "Alpha";
        a.blockX = nBands > 0 ? cd->src->bands[0].blockX
                              : std::min(cd->width, 128);
        a.blockY = nBands > 0 ? cd->src->bands[0].blockY
                              : std::min(cd->height, 128);
        cd->bands.push_back(std::move(a));
    }
    cd->data.resize(cd->bands.size());

    bool vrtCrash = alphaPath && singlePath && form == "VRT";
    if (vrtCrash)
        cd->barSuppressed = true;

    auto mat = [cd, ndVals, nBands, verb, countBad, singlePath, rep,
                alphaPath](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        RasterDatasetBase &src = *cd->src;
        if (countBad)
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        strPrintf("%s: There should be %d nodata values "
                                  "given the input dataset has %d bands",
                                  verb.c_str(), nBands, nBands));
            return 1;
        }
        if (singlePath)
            for (int i = 0; i < nBands; ++i)
                if (!rep[(size_t)i])
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "Nodata value was not set to output "
                                "band, as it cannot be represented on "
                                "its data type.");
        size_t n = (size_t)cd->width * cd->height;
        std::vector<std::vector<double>> sv((size_t)nBands);
        for (int i = 0; i < nBands; ++i)
            if (!src.readBand(i + 1, sv[(size_t)i]))
                return 1;
        for (int i = 0; i < nBands; ++i)
            cd->data[(size_t)i] = sv[(size_t)i];
        if (!alphaPath)
        {
            cd->evaluated = true;
            return 0;
        }
        std::vector<double> &alpha = cd->data[(size_t)nBands];
        alpha.assign(n, 255.0);
        auto matches = [](double v, double nd)
        { return v == nd || (std::isnan(v) && std::isnan(nd)); };
        if (ndVals.empty())
        {
            double nd = src.bands[0].nodata;
            for (size_t px = 0; px < n; ++px)
                if (matches(sv[0][px], nd))
                    alpha[px] = 0.0;
        }
        else if (ndVals.size() == 1)
        {
            if (rep[0])
                for (size_t px = 0; px < n; ++px)
                    if (matches(sv[0][px], ndVals[0]))
                        alpha[px] = 0.0;
        }
        else
        {
            for (size_t px = 0; px < n; ++px)
            {
                bool all = true;
                for (int i = 0; i < nBands; ++i)
                    if (!matches(sv[(size_t)i][px], ndVals[(size_t)i]))
                        all = false;
                if (all)
                    alpha[px] = 0.0;
            }
        }
        cd->evaluated = true;
        return 0;
    };

    cd->vrtCustom = [cd, nBands, ndVals, alphaPath, singlePath, tuplePath,
                     rep](const std::string &input,
                          const std::string &output)
    {
        RasterDatasetBase &s = *cd->src;
        int relative = 0;
        std::string rel = relToOutputC(input, output, relative);
        bool nested = alphaPath && !ndVals.empty();
        std::string x;
        emitVrtHeaderC(x, *cd);
        auto emitMdEcho = [&](const std::string &pad, bool nodataValues)
        {
            std::map<std::string, MetaDomain> md = cd->metadata;
            std::vector<std::string> order = cd->domainOrder;
            if (nodataValues)
            {
                std::string joined;
                for (size_t i = 0; i < ndVals.size(); ++i)
                {
                    if (i)
                        joined += " ";
                    joined += strPrintf("%.17g", ndVals[i]);
                }
                md[""].push_back({"NODATA_VALUES", joined});
                bool haveDef = false;
                for (const auto &d : order)
                    if (d.empty())
                        haveDef = true;
                if (!haveDef)
                    order.push_back("");
            }
            emitVrtMetadataEcho(x, md, order, pad + "  ");
        };
        emitMdEcho("", false);
        auto emitSrcProps = [&](const std::string &pad, const Band &sb,
                                DType t, bool props = true)
        {
            if (props)
                x += pad +
                     strPrintf("<SourceProperties RasterXSize=\"%d\" "
                               "RasterYSize=\"%d\" DataType=\"%s\" "
                               "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                               s.width, s.height, dtypeName(t), sb.blockX,
                               sb.blockY);
            x += pad + strPrintf("<SrcRect xOff=\"0\" yOff=\"0\" "
                                 "xSize=\"%d\" ySize=\"%d\" />\n",
                                 s.width, s.height);
            x += pad + strPrintf("<DstRect xOff=\"0\" yOff=\"0\" "
                                 "xSize=\"%d\" ySize=\"%d\" />\n",
                                 s.width, s.height);
        };
        auto emitNested = [&]()
        {
            emitVrtHeaderC(x, *cd, "      ");
            emitMdEcho("      ", tuplePath);
            for (int j = 0; j < nBands; ++j)
            {
                const Band &nb = s.bands[(size_t)j];
                x += strPrintf(
                    "        <VRTRasterBand dataType=\"%s\" "
                    "band=\"%d\"%s>\n",
                    dtypeName(nb.type), j + 1,
                    bandBlockAttrsC(cd->width, cd->height, nb.blockX,
                                    nb.blockY)
                        .c_str());
                auto nbit = nb.metadata.find("");
                if (nbit != nb.metadata.end() && !nbit->second.empty())
                {
                    x += "          <Metadata>\n";
                    for (const auto &kv : nbit->second)
                        x += "            <MDI key=\"" +
                             xmlEscC(kv.first) + "\">" +
                             xmlEscC(kv.second) + "</MDI>\n";
                    x += "          </Metadata>\n";
                }
                if (singlePath && rep[(size_t)j])
                    x += "          <NoDataValue>" + fmt18c(ndVals[0]) +
                         "</NoDataValue>\n";
                else if (tuplePath && nb.hasNodata)
                    x += "          <NoDataValue>" + fmt18c(nb.nodata) +
                         "</NoDataValue>\n";
                if (nb.colorInterp != "Undefined")
                    x += "          <ColorInterp>" + nb.colorInterp +
                         "</ColorInterp>\n";
                x += "          <SimpleSource>\n";
                x += strPrintf("            <SourceFilename "
                               "relativeToVRT=\"%d\">%s"
                               "</SourceFilename>\n",
                               relative, xmlEscC(rel).c_str());
                x += strPrintf("            <SourceBand>%d"
                               "</SourceBand>\n",
                               j + 1);
                emitSrcProps("            ", nb, nb.type);
                x += "          </SimpleSource>\n";
                x += "        </VRTRasterBand>\n";
            }
            x += "      </VRTDataset>\n";
        };
        for (int i = 0; i < nBands; ++i)
        {
            const Band &sb = s.bands[(size_t)i];
            x += strPrintf(
                "  <VRTRasterBand dataType=\"%s\" band=\"%d\"%s>\n",
                dtypeName(sb.type), i + 1,
                bandBlockAttrsC(cd->width, cd->height, sb.blockX,
                                sb.blockY)
                    .c_str());
            const MetaDomain *bmd = nullptr;
            auto bit = sb.metadata.find("");
            if (bit != sb.metadata.end() && !bit->second.empty())
                bmd = &bit->second;
            if (bmd)
            {
                x += "    <Metadata>\n";
                for (const auto &kv : *bmd)
                    x += "      <MDI key=\"" + xmlEscC(kv.first) + "\">" +
                         xmlEscC(kv.second) + "</MDI>\n";
                x += "    </Metadata>\n";
            }
            if (!alphaPath && sb.hasNodata)
                x += "    <NoDataValue>" + fmt18c(sb.nodata) +
                     "</NoDataValue>\n";
            if (sb.colorInterp != "Undefined")
                x += "    <ColorInterp>" + sb.colorInterp +
                     "</ColorInterp>\n";
            x += "    <SimpleSource>\n";
            if (nested)
            {
                // the reference inlines the nodata-overridden source as
                // a full nested VRT dataset copy per band
                emitNested();
            }
            else
            {
                x += strPrintf("      <SourceFilename "
                               "relativeToVRT=\"%d\">%s"
                               "</SourceFilename>\n",
                               relative, xmlEscC(rel).c_str());
            }
            x += strPrintf("      <SourceBand>%d</SourceBand>\n", i + 1);
            // the unrepresentable-override passthrough serializes with
            // only the lone opened band-1 source carrying properties
            emitSrcProps("      ", sb, sb.type,
                         !(singlePath && !alphaPath) || i == 0);
            x += "    </SimpleSource>\n";
            x += "  </VRTRasterBand>\n";
        }
        if (alphaPath)
        {
            const Band &sb1 = s.bands[0];
            x += strPrintf(
                "  <VRTRasterBand dataType=\"Byte\" band=\"%d\"%s>\n",
                nBands + 1,
                bandBlockAttrsC(cd->width, cd->height, sb1.blockX,
                                sb1.blockY)
                    .c_str());
            x += "    <ColorInterp>Alpha</ColorInterp>\n";
            x += "    <SimpleSource>\n";
            if (nested && tuplePath)
                emitNested();
            else if (nested)
                x += "      <SourceFilename relativeToVRT=\"0\">"
                     "</SourceFilename>\n";
            else
                x += strPrintf("      <SourceFilename "
                               "relativeToVRT=\"%d\">%s"
                               "</SourceFilename>\n",
                               relative, xmlEscC(rel).c_str());
            x += "      <SourceBand>mask,1</SourceBand>\n";
            emitSrcProps("      ", sb1, DType::Byte);
            x += "    </SimpleSource>\n";
            x += "  </VRTRasterBand>\n";
        }
        x += "</VRTDataset>\n";
        return x;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (clrEvalTarget(c.drv, c.output))
        matF = mat;
    rc = rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                  c.overwrite, c.append, c.drv, extra,
                                  matF, nullptr);
    if (rc == 0 && vrtCrash)
    {
        // the reference reopens the just-written VRT, chokes on the
        // empty mask SourceFilename, and dies
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "Missing <SourceFilename> or <VRTDataset> element in "
                    "<SimpleSource>.");
        crashNow(false, 0.0, SIGSEGV);
    }
    return rc;
}

// ------------------------------------------------------------------
// rgb-to-palette
// ------------------------------------------------------------------

// x86 shift semantics: the reference computes 8-bit reductions with a
// raw (8 - nBits) shift whose count wraps mod 32
inline uint32_t shrx(uint32_t v, int sh)
{
    return v >> ((unsigned)sh & 31u);
}

inline int shlx(int v, int sh)
{
    return (int)((uint32_t)v << ((unsigned)sh & 31u));
}

struct McBox
{
    int rmin, rmax, gmin, gmax, bmin, bmax;
    long long total;
};

void mcShrink(McBox &box, const std::map<uint32_t, uint32_t> &hist)
{
    int rmin = 1 << 30, rmax = -1, gmin = 1 << 30, gmax = -1,
        bmin = 1 << 30, bmax = -1;
    long long total = 0;
    for (const auto &kv : hist)
    {
        int r = (int)(kv.first >> 16) & 0xff;
        int g = (int)(kv.first >> 8) & 0xff;
        int b = (int)kv.first & 0xff;
        if (r < box.rmin || r > box.rmax || g < box.gmin ||
            g > box.gmax || b < box.bmin || b > box.bmax)
            continue;
        total += kv.second;
        rmin = std::min(rmin, r);
        rmax = std::max(rmax, r);
        gmin = std::min(gmin, g);
        gmax = std::max(gmax, g);
        bmin = std::min(bmin, b);
        bmax = std::max(bmax, b);
    }
    box.total = total;
    if (total > 0)
    {
        box.rmin = rmin;
        box.rmax = rmax;
        box.gmin = gmin;
        box.gmax = gmax;
        box.bmin = bmin;
        box.bmax = bmax;
    }
}

// tiffmedian-style split: the left half becomes a fresh box at the list
// head, the split box keeps the right half
std::vector<ColorEntry> medianCut(const std::map<uint32_t, uint32_t> &hist,
                                  int colorCount, int nBits)
{
    std::vector<McBox> boxes;
    McBox first;
    first.rmin = first.gmin = first.bmin = 0;
    first.rmax = first.gmax = first.bmax = (1 << (nBits & 31)) - 1;
    mcShrink(first, hist);
    boxes.push_back(first);
    while ((int)boxes.size() < colorCount)
    {
        int bestIdx = -1;
        long long bestTotal = -1;
        for (size_t i = 0; i < boxes.size(); ++i)
        {
            const McBox &b = boxes[i];
            bool splittable = b.rmax > b.rmin || b.gmax > b.gmin ||
                              b.bmax > b.bmin;
            if (splittable && b.total > bestTotal)
            {
                bestTotal = b.total;
                bestIdx = (int)i;
            }
        }
        if (bestIdx < 0)
            break;
        McBox box = boxes[(size_t)bestIdx];
        int ir = box.rmax - box.rmin;
        int ig = box.gmax - box.gmin;
        int ib = box.bmax - box.bmin;
        int axis = (ir >= ig && ir >= ib) ? 0 : (ig >= ib ? 1 : 2);
        int lo = axis == 0 ? box.rmin : axis == 1 ? box.gmin : box.bmin;
        int hi = axis == 0 ? box.rmax : axis == 1 ? box.gmax : box.bmax;
        std::vector<long long> marg((size_t)(hi - lo + 1), 0);
        for (const auto &kv : hist)
        {
            int r = (int)(kv.first >> 16) & 0xff;
            int g = (int)(kv.first >> 8) & 0xff;
            int b = (int)kv.first & 0xff;
            if (r < box.rmin || r > box.rmax || g < box.gmin ||
                g > box.gmax || b < box.bmin || b > box.bmax)
                continue;
            int cell = axis == 0 ? r : axis == 1 ? g : b;
            marg[(size_t)(cell - lo)] += kv.second;
        }
        long long half = box.total / 2;
        long long cum = 0;
        int i = lo;
        for (; i <= hi; ++i)
        {
            cum += marg[(size_t)(i - lo)];
            if (cum >= half)
                break;
        }
        if (i == lo)
            i = lo + 1;
        if (i > hi)
            i = hi;
        McBox left = box, right = box;
        if (axis == 0)
        {
            left.rmax = i - 1;
            right.rmin = i;
        }
        else if (axis == 1)
        {
            left.gmax = i - 1;
            right.gmin = i;
        }
        else
        {
            left.bmax = i - 1;
            right.bmin = i;
        }
        mcShrink(left, hist);
        mcShrink(right, hist);
        boxes[(size_t)bestIdx] = right;
        boxes.insert(boxes.begin(), left);
    }
    std::vector<ColorEntry> pal;
    int sh = 8 - nBits;
    for (const McBox &b : boxes)
    {
        ColorEntry e;
        e.c1 = shlx(b.rmin + b.rmax, sh) / 2;
        e.c2 = shlx(b.gmin + b.gmax, sh) / 2;
        e.c3 = shlx(b.bmin + b.bmax, sh) / 2;
        e.c4 = 255;
        pal.push_back(e);
    }
    return pal;
}

int rasterRgbToPaletteHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("rgb-to-palette");
    ClrCommon c;
    int rc = clrBegin(r, c);
    if (rc >= 0)
        return rc;

    int colorCount =
        r.get("color-count") ? atoi(r.str("color-count").c_str()) : 256;
    bool noDither = r.flag("no-dither");
    int nBits = r.get("bit-depth") ? atoi(r.str("bit-depth").c_str()) : 5;
    bool haveMap = r.get("color-map") != nullptr;
    std::string mapFile = haveMap ? r.str("color-map") : std::string();
    bool haveDstNd = r.get("dst-nodata") != nullptr;
    int dstNd = haveDstNd ? atoi(r.str("dst-nodata").c_str()) : 0;

    std::string extra;
    if (r.get("color-count"))
        extra += strPrintf(" --color-count %d", colorCount);
    if (haveMap)
        extra += " --color-map " + gdalgQuoteC(mapFile);
    if (haveDstNd)
        extra += strPrintf(" --dst-nodata %d", dstNd);
    if (noDither)
        extra += " --no-dither";
    if (r.get("bit-depth"))
        extra += strPrintf(" --bit-depth %d", nBits);

    auto ds = std::make_unique<ClrDataset>(std::move(c.src), true);
    ClrDataset *cd = ds.get();
    cd->setMd("IMAGE_STRUCTURE", "INTERLEAVE", "BAND");
    {
        Band b;
        b.index = 1;
        b.type = DType::Byte;
        b.colorInterp = "Palette";
        b.hasNodata = haveDstNd;
        b.nodata = dstNd;
        b.blockX = std::min(cd->width, 128);
        b.blockY = std::min(cd->height, 128);
        cd->bands.push_back(std::move(b));
    }
    cd->data.resize(1);

    std::string verb = cmd.name;
    bool quiet = c.quiet;

    auto mat = [cd, colorCount, noDither, nBits, haveMap, mapFile,
                haveDstNd, dstNd, verb,
                quiet](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        RasterDatasetBase &src = *cd->src;
        int nBands = (int)src.bands.size();
        if (nBands < 3)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        verb + ": Input dataset must have at least 3 "
                               "bands");
            return 1;
        }
        for (int i = 0; i < nBands; ++i)
            if (src.bands[(size_t)i].type != DType::Byte)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            verb +
                                ": Non-byte band found and not supported");
                return 1;
            }
        bool alpha4 = nBands >= 4 &&
                      src.bands[3].colorInterp == "Alpha";
        if (nBands > 3 && !alpha4)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        verb + ": Only R,G,B bands of input dataset will "
                               "be taken into account");
        if (nBands == 3)
        {
            const std::string &i0 = src.bands[0].colorInterp;
            const std::string &i1 = src.bands[1].colorInterp;
            const std::string &i2 = src.bands[2].colorInterp;
            bool exact = i0 == "Red" && i1 == "Green" && i2 == "Blue";
            auto isRgb = [](const std::string &s)
            { return s == "Red" || s == "Green" || s == "Blue"; };
            if (!exact && (isRgb(i0) || isRgb(i1) || isRgb(i2)))
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    verb + ": Assuming first band is red, second green "
                           "and third blue, despite at least one band "
                           "with one of those color interpretation "
                           "found");
        }

        size_t n = (size_t)cd->width * cd->height;
        std::vector<double> rv, gv, bv;
        if (!src.readBand(1, rv) || !src.readBand(2, gv) ||
            !src.readBand(3, bv))
            return 1;

        std::vector<ColorEntry> pal;
        if (haveMap)
        {
            CMap m = loadColorMap(mapFile);
            if (!m.err.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined, m.err);
                return 1;
            }
            if (m.hasNv)
            {
                if (!src.bands[0].hasNodata)
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "Input dataset has no nodata value. "
                                "Ignoring 'nv' entry in color palette");
                else
                {
                    m.nv.value = src.bands[0].nodata;
                    m.nv.pct = false;
                    m.entries.push_back(m.nv);
                }
                m.hasNv = false;
            }
            if (m.anyPct)
            {
                double mn = 0, mx = 0;
                cmapSrcMinMax(src, src.bands[0], mn, mx);
                for (CMapEntry &e : m.entries)
                    if (e.pct)
                    {
                        e.value = mn + e.value / 100.0 * (mx - mn);
                        e.pct = false;
                    }
            }
            long long maxIdx = -1;
            for (const CMapEntry &e : m.entries)
            {
                double v = e.value;
                if (v != std::floor(v) || v < 0 || v > 65535)
                {
                    cplErrorStr(
                        CE_Failure, CPLE_NotSupported,
                        strPrintf("Unsupported value '%f' for color "
                                  "entry. Only integer value in [0, "
                                  "65535] range supported",
                                  v));
                    return 1;
                }
                maxIdx = std::max(maxIdx, (long long)v);
            }
            pal.assign((size_t)maxIdx + 1, ColorEntry{0, 0, 0, 255});
            for (const CMapEntry &e : m.entries)
            {
                ColorEntry ce;
                ce.c1 = (int)e.c[0];
                ce.c2 = (int)e.c[1];
                ce.c3 = (int)e.c[2];
                ce.c4 = (int)e.c[3];
                pal[(size_t)(long long)e.value] = ce;
            }
            if (haveDstNd)
            {
                std::vector<ColorEntry> shifted;
                size_t newSize =
                    std::max(pal.size() + 1, (size_t)dstNd + 1);
                shifted.assign(newSize, ColorEntry{0, 0, 0, 255});
                for (size_t i = 0; i < pal.size(); ++i)
                {
                    size_t di = i >= (size_t)dstNd ? i + 1 : i;
                    if (di < shifted.size())
                        shifted[di] = pal[i];
                }
                shifted[(size_t)dstNd] = ColorEntry{0, 0, 0, 0};
                pal.swap(shifted);
            }
            if (pal.size() > 256)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "GDALDitherRGB2PCT(): Color table cannot "
                            "have more than 256 entries.");
                return 1;
            }
            int nCLevels = 1 << ((unsigned)nBits & 31u);
            if (nCLevels <= 1)
                crashNow(false, 0.0, SIGFPE);
        }
        else
        {
            if (alpha4)
                crashNow(!quiet, 0.25, SIGSEGV);
            if (haveDstNd)
                crashNow(!quiet, 0.25, SIGSEGV);
            std::map<uint32_t, uint32_t> hist;
            std::vector<uint32_t> firstSeen;
            bool exceeded = false;
            int sh = 8 - nBits;
            for (size_t px = 0; px < n; ++px)
            {
                uint32_t rc8 = shrx((uint32_t)(int)rv[px], sh) & 0xff;
                uint32_t gc8 = shrx((uint32_t)(int)gv[px], sh) & 0xff;
                uint32_t bc8 = shrx((uint32_t)(int)bv[px], sh) & 0xff;
                uint32_t key = (rc8 << 16) | (gc8 << 8) | bc8;
                if (hist[key]++ == 0 && nBits == 8 && !exceeded)
                {
                    if (firstSeen.size() < (size_t)colorCount)
                        firstSeen.push_back(key);
                    else
                        exceeded = true;
                }
            }
            if (nBits == 8 && !exceeded)
            {
                for (uint32_t key : firstSeen)
                    pal.push_back(ColorEntry{(int)(key >> 16) & 0xff,
                                             (int)(key >> 8) & 0xff,
                                             (int)key & 0xff, 255});
            }
            else
                pal = medianCut(hist, colorCount, nBits);
            int nCLevels = 1 << ((unsigned)nBits & 31u);
            if (nCLevels <= 1)
                crashNow(!quiet, 0.25, SIGFPE);
        }

        // nearest palette entry per reduced cell (memoized); ties keep
        // the lowest palette index
        int nCLevels = 1 << ((unsigned)nBits & 31u);
        int sh = 8 - nBits;
        std::map<uint32_t, int> nearMemo;
        auto nearestFor = [&](uint32_t rc8, uint32_t gc8,
                              uint32_t bc8) -> int
        {
            uint32_t key = (rc8 << 16) | (gc8 << 8) | bc8;
            auto it = nearMemo.find(key);
            if (it != nearMemo.end())
                return it->second;
            int rr = (int)((rc8 * 255) / (uint32_t)(nCLevels - 1));
            int rg = (int)((gc8 * 255) / (uint32_t)(nCLevels - 1));
            int rb = (int)((bc8 * 255) / (uint32_t)(nCLevels - 1));
            int best = 0;
            int bestD = -1;
            for (size_t i = 0; i < pal.size(); ++i)
            {
                int d = std::abs(rr - pal[i].c1) +
                        std::abs(rg - pal[i].c2) +
                        std::abs(rb - pal[i].c3);
                if (bestD < 0 || d < bestD)
                {
                    bestD = d;
                    best = (int)i;
                }
            }
            nearMemo[key] = best;
            return best;
        };

        bool srcNd = haveDstNd && src.bands[0].hasNodata &&
                     src.bands[1].hasNodata && src.bands[2].hasNodata;
        double nd0 = srcNd ? src.bands[0].nodata : 0;
        double nd1 = srcNd ? src.bands[1].nodata : 0;
        double nd2 = srcNd ? src.bands[2].nodata : 0;

        std::vector<double> &out = cd->data[0];
        out.assign(n, 0.0);
        int W = cd->width, H = cd->height;
        if (noDither)
        {
            for (size_t px = 0; px < n; ++px)
            {
                if (srcNd && rv[px] == nd0 && gv[px] == nd1 &&
                    bv[px] == nd2)
                {
                    out[px] = dstNd;
                    continue;
                }
                uint32_t rc8 = shrx((uint32_t)(int)rv[px], sh) & 0xff;
                uint32_t gc8 = shrx((uint32_t)(int)gv[px], sh) & 0xff;
                uint32_t bc8 = shrx((uint32_t)(int)bv[px], sh) & 0xff;
                out[px] = nearestFor(rc8, gc8, bc8);
            }
        }
        else
        {
            // sixths-based error diffusion: nSixth=e/6 (trunc), 2/6
            // right, 1/6 below-left, e-5*nSixth below, 1/6 below-right
            std::vector<int> curErr((size_t)W * 3, 0);
            std::vector<int> nextErr((size_t)W * 3, 0);
            auto clamp255 = [](int v)
            { return v < 0 ? 0 : v > 255 ? 255 : v; };
            for (int y = 0; y < H; ++y)
            {
                std::fill(nextErr.begin(), nextErr.end(), 0);
                for (int x = 0; x < W; ++x)
                {
                    size_t px = (size_t)y * W + x;
                    if (srcNd && rv[px] == nd0 && gv[px] == nd1 &&
                        bv[px] == nd2)
                    {
                        out[px] = dstNd;
                        continue;
                    }
                    int adj[3];
                    adj[0] = clamp255((int)rv[px] +
                                      curErr[(size_t)x * 3 + 0]);
                    adj[1] = clamp255((int)gv[px] +
                                      curErr[(size_t)x * 3 + 1]);
                    adj[2] = clamp255((int)bv[px] +
                                      curErr[(size_t)x * 3 + 2]);
                    uint32_t rc8 = shrx((uint32_t)adj[0], sh) & 0xff;
                    uint32_t gc8 = shrx((uint32_t)adj[1], sh) & 0xff;
                    uint32_t bc8 = shrx((uint32_t)adj[2], sh) & 0xff;
                    int idx = nearestFor(rc8, gc8, bc8);
                    out[px] = idx;
                    int err[3] = {adj[0] - pal[(size_t)idx].c1,
                                  adj[1] - pal[(size_t)idx].c2,
                                  adj[2] - pal[(size_t)idx].c3};
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        int e = err[ch];
                        int s = e / 6;
                        if (x + 1 < W)
                            curErr[(size_t)(x + 1) * 3 + ch] += 2 * s;
                        if (x > 0)
                            nextErr[(size_t)(x - 1) * 3 + ch] += s;
                        nextErr[(size_t)x * 3 + ch] += e - 5 * s;
                        if (x + 1 < W)
                            nextErr[(size_t)(x + 1) * 3 + ch] += s;
                    }
                }
                curErr.swap(nextErr);
            }
        }
        cd->bands[0].colorTable = pal;
        cd->evaluated = true;
        return 0;
    };

    cd->vrtCustom = [cd](const std::string &, const std::string &)
    {
        std::string x;
        emitVrtHeaderC(x, *cd);
        const MetaDomain *def = nullptr;
        auto it = cd->metadata.find("");
        if (it != cd->metadata.end() && !it->second.empty())
            def = &it->second;
        if (def)
        {
            x += "  <Metadata>\n";
            for (const auto &kv : *def)
                x += "    <MDI key=\"" + xmlEscC(kv.first) + "\">" +
                     xmlEscC(kv.second) + "</MDI>\n";
            x += "  </Metadata>\n";
        }
        x += "  <Metadata domain=\"IMAGE_STRUCTURE\">\n";
        x += "    <MDI key=\"INTERLEAVE\">BAND</MDI>\n";
        x += "  </Metadata>\n";
        const Band &b = cd->bands[0];
        std::string battrs;
        if (cd->width > 128)
            battrs += strPrintf(" blockXSize=\"%d\"", cd->width);
        if (std::min(cd->height, 128) != 1)
            battrs += " blockYSize=\"1\"";
        x += strPrintf("  <VRTRasterBand dataType=\"Byte\" band=\"1\"%s>"
                       "\n",
                       battrs.c_str());
        if (b.hasNodata)
            x += "    <NoDataValue>" + fmt18c(b.nodata) +
                 "</NoDataValue>\n";
        x += "    <ColorInterp>Palette</ColorInterp>\n";
        x += "    <ColorTable>\n";
        for (const ColorEntry &e : b.colorTable)
            x += strPrintf("      <Entry c1=\"%d\" c2=\"%d\" c3=\"%d\" "
                           "c4=\"%d\" />\n",
                           e.c1, e.c2, e.c3, e.c4);
        x += "    </ColorTable>\n";
        x += "    <SimpleSource>\n";
        x += "      <SourceFilename relativeToVRT=\"0\">"
             "</SourceFilename>\n";
        x += "      <SourceBand>1</SourceBand>\n";
        x += strPrintf("      <SourceProperties RasterXSize=\"%d\" "
                       "RasterYSize=\"%d\" DataType=\"Byte\" "
                       "BlockXSize=\"%d\" BlockYSize=\"1\" />\n",
                       cd->width, cd->height, cd->width);
        x += strPrintf("      <SrcRect xOff=\"0\" yOff=\"0\" "
                       "xSize=\"%d\" ySize=\"%d\" />\n",
                       cd->width, cd->height);
        x += strPrintf("      <DstRect xOff=\"0\" yOff=\"0\" "
                       "xSize=\"%d\" ySize=\"%d\" />\n",
                       cd->width, cd->height);
        x += "    </SimpleSource>\n";
        x += "  </VRTRasterBand>\n";
        x += "</VRTDataset>\n";
        return x;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (clrEvalTarget(c.drv, c.output))
        matF = mat;
    return rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                    c.overwrite, c.append, c.drv, extra,
                                    matF, nullptr);
}

}  // namespace

void registerRasterColorHandlers()
{
    registerHandler("raster_color-map", rasterColorMapHandler);
    registerPreValidator("raster_color-map", clrPreValidator);
    registerPostValidator("raster_color-map", clrPostValidator);
    registerHandler("raster_nodata-to-alpha", rasterNodataToAlphaHandler);
    registerPreValidator("raster_nodata-to-alpha", clrPreValidator);
    registerHandler("raster_rgb-to-palette", rasterRgbToPaletteHandler);
    registerPreValidator("raster_rgb-to-palette", clrPreValidator);
}
