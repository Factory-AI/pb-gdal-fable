// proximity / sieve / fill-nodata / neighbors: pixel-algorithm verbs
// producing a fresh in-memory result written through the convert path
#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "progress.h"
#include "spec.h"
#include "util.h"
#include "vsi.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

std::string fmt18(double d)
{
    if (std::isnan(d))
        return "nan";
    return strPrintf("%.18g", d);
}

std::string xmlEscT(const std::string &s)
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

std::string gdalgQuote(const std::string &tok)
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

std::string dirNameLocal(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

std::string relToOutput(const std::string &input, const std::string &output,
                        int &relative)
{
    std::string outDir = dirNameLocal(output);
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

bool parseFullDouble(const std::string &tok, double &out)
{
    if (tok.empty())
        return false;
    char *end = nullptr;
    out = strtod(tok.c_str(), &end);
    return end && *end == '\0';
}

// argument echo for GDALG command lines: %.17g matches the reference's
// round-trip real serialization ("4.0" -> 4, 1e10 stays expanded)
std::string realEcho(const std::string &tok)
{
    double v = 0;
    if (!parseFullDouble(tok, v))
        return tok;
    return strPrintf("%.17g", v);
}

void emitVrtHeaderPx(std::string &x, const RasterDatasetBase &ds)
{
    x += strPrintf("<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\">\n",
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
        x += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             xmlEscT(wkt) + "</SRS>\n";
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
}

// ------------------------------------------------------------------
// materialized result dataset (fresh MEM-style dataset carrying the
// computed pixels; per-verb flags choose the propagated pieces)
// ------------------------------------------------------------------

class PxDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    std::vector<std::vector<double>> data;
    bool evaluated = false;
    // VRT serialization: the materialized form carries an empty
    // SourceFilename (in-memory source) with MEM 1-row blocks
    bool vrtMaterialized = true;
    std::function<std::string(const std::string &, const std::string &)>
        vrtCustom;

    PxDataset(std::unique_ptr<RasterDatasetBase> s, bool copyMeta)
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
        std::string x;
        emitVrtHeaderPx(x, *this);
        const MetaDomain *def = nullptr;
        auto it = metadata.find("");
        if (it != metadata.end() && !it->second.empty())
            def = &it->second;
        if (def)
        {
            x += "  <Metadata>\n";
            for (const auto &kv : *def)
                x += "    <MDI key=\"" + xmlEscT(kv.first) + "\">" +
                     xmlEscT(kv.second) + "</MDI>\n";
            x += "  </Metadata>\n";
        }
        x += "  <Metadata domain=\"IMAGE_STRUCTURE\">\n";
        x += "    <MDI key=\"INTERLEAVE\">BAND</MDI>\n";
        x += "  </Metadata>\n";
        for (size_t bi = 0; bi < bands.size(); ++bi)
        {
            const Band &b = bands[bi];
            std::string battrs;
            if (width > 128)
                battrs += strPrintf(" blockXSize=\"%d\"", width);
            if (std::min(height, 128) != 1)
                battrs += " blockYSize=\"1\"";
            x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\"%s>\n",
                           dtypeName(b.type), (int)bi + 1, battrs.c_str());
            if (b.hasNodata)
                x += "    <NoDataValue>" + fmt18(b.nodata) +
                     "</NoDataValue>\n";
            if (b.colorInterp != "Undefined")
                x += "    <ColorInterp>" + b.colorInterp +
                     "</ColorInterp>\n";
            x += "    <SimpleSource>\n";
            x += "      <SourceFilename relativeToVRT=\"0\">"
                 "</SourceFilename>\n";
            x += strPrintf("      <SourceBand>%d</SourceBand>\n",
                           (int)bi + 1);
            x += strPrintf("      <SourceProperties RasterXSize=\"%d\" "
                           "RasterYSize=\"%d\" DataType=\"%s\" "
                           "BlockXSize=\"%d\" BlockYSize=\"1\" />\n",
                           width, height, dtypeName(b.type), width);
            x += strPrintf("      <SrcRect xOff=\"0\" yOff=\"0\" "
                           "xSize=\"%d\" ySize=\"%d\" />\n",
                           width, height);
            x += strPrintf("      <DstRect xOff=\"0\" yOff=\"0\" "
                           "xSize=\"%d\" ySize=\"%d\" />\n",
                           width, height);
            x += "    </SimpleSource>\n";
            x += "  </VRTRasterBand>\n";
        }
        x += "</VRTDataset>\n";
        return x;
    }
};

// ------------------------------------------------------------------
// shared handler plumbing
// ------------------------------------------------------------------

struct PxCommon
{
    std::string input, output, drv;
    bool quiet = false, overwrite = false, append = false;
    std::unique_ptr<RasterDatasetBase> src;
};

// returns <0 to continue, otherwise the exit code
int pxBegin(ParseResult &r, PxCommon &c)
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

// every output form except the pure GDALG serialization materializes
// the result (stream/MEM/VRT all run the algorithm)
bool pxEvalTarget(const std::string &drv, const std::string &output)
{
    return drv != "GDALG" &&
           !(drv.empty() && strEndsWith(strToLower(output), ".gdalg.json"));
}

// stream output surfaces the algorithm's own progress; file outputs
// compress it into the first half of the bar (write is the second half)
double pxProgressScale(const std::string &drv)
{
    return drv == "stream" ? 1.0 : 0.5;
}

int pxBandArg(ParseResult &r, int def = 1)
{
    const ArgValue *v = r.get("band");
    return v ? atoi(v->str().c_str()) : def;
}

// generic <verb>: band range message used by the validation stack
void pxBandRangeError(const std::string &verb, int nBands)
{
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                strPrintf("%s: Value of 'band' should be greater or equal "
                          "than 1 and less or equal than %d.",
                          verb.c_str(), nBands));
}

// quiet open used by validation-stage checks
std::unique_ptr<RasterDatasetBase> pxQuietOpen(const std::string &path)
{
    cplPushQuietHandler();
    std::string err;
    auto ds = openRaster(path, err);
    cplPopHandler();
    return ds;
}

// band-range check shared by the post-validators; true = failed; runs
// even after an output-exists refusal, but silently skips when the
// input itself does not open
bool pxPostBandCheck(const std::string &verb, ParseResult &r)
{
    const ArgValue *v = r.get("band");
    if (!v)
        return false;
    auto ds = pxQuietOpen(r.str("input"));
    if (!ds)
        return false;
    int b = atoi(v->str().c_str());
    if (b < 1 || b > (int)ds->bands.size())
    {
        pxBandRangeError(verb, (int)ds->bands.size());
        return true;
    }
    return false;
}

// mask dataset open check (missing / unrecognized), stacked before the
// band check like the reference's dataset-argument opening
bool pxPostMaskCheck(ParseResult &r)
{
    const ArgValue *m = r.get("mask");
    if (!m)
        return false;
    std::string path = m->str();
    if (!vsiExists(path))
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    path + ": No such file or directory");
        return true;
    }
    auto ds = pxQuietOpen(path);
    if (!ds)
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "`" + path +
                        "' not recognized as being in a supported file "
                        "format.");
        return true;
    }
    return false;
}

// pre-validation shared by the pixel verbs: -b 0 refusal then format
// checks (mirrors the scale/calc ordering)
int pxPreValidator(const CmdSpec &cmd, ParseResult &r)
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

std::string pxCanonOt(const CmdSpec &cmd, const std::string &val)
{
    for (const auto &a : cmd.args)
        if (a.name == "output-data-type")
            for (const auto &c : a.choices)
                if (strEqualNoCase(c, val))
                    return c;
    if (strEqualNoCase(val, "byte"))
        return "Byte";
    return val;
}

DType pxOtType(const std::string &canon)
{
    if (canon == "UInt8" || canon == "Byte")
        return DType::Byte;
    return dtypeFromName(canon);
}

// int32 conversion used by the scan buffers (GDALCopyWords semantics)
int32_t pxToInt32(double v)
{
    return (int32_t)(long long)rasterFinishReal(v, DType::Int32);
}

// ------------------------------------------------------------------
// proximity
// ------------------------------------------------------------------

// exact squared euclidean distance transform (per-row nearest target,
// then a per-column lower-envelope pass); integer distances kept exact
// in doubles
void pxDistanceTransform(const std::vector<uint8_t> &target, int W, int H,
                         std::vector<double> &d2)
{
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> rowd(W * (size_t)H, INF);
    for (int y = 0; y < H; ++y)
    {
        double *row = rowd.data() + (size_t)y * W;
        const uint8_t *trow = target.data() + (size_t)y * W;
        double last = INF;
        for (int x = 0; x < W; ++x)
        {
            if (trow[x])
                last = x;
            if (last != INF)
                row[x] = (x - last) * (x - last);
        }
        last = INF;
        for (int x = W - 1; x >= 0; --x)
        {
            if (trow[x])
                last = x;
            if (last != INF)
            {
                double d = (last - x) * (last - x);
                if (d < row[x])
                    row[x] = d;
            }
        }
    }
    d2.assign((size_t)W * H, INF);
    std::vector<int> v(H);
    std::vector<double> z(H + 1);
    std::vector<double> f(H);
    for (int x = 0; x < W; ++x)
    {
        int m = 0;
        for (int y = 0; y < H; ++y)
        {
            f[y] = rowd[(size_t)y * W + x];
            if (f[y] != INF)
                v[m++] = y;
        }
        if (m == 0)
            continue;
        // lower envelope of parabolas y' -> (y-y')^2 + f[y']
        std::vector<int> hull;
        std::vector<double> zb;
        hull.push_back(v[0]);
        zb.push_back(-INF);
        zb.push_back(INF);
        auto sect = [&](int p, int q)
        {
            return ((f[q] + (double)q * q) - (f[p] + (double)p * p)) /
                   (2.0 * (q - p));
        };
        for (int i = 1; i < m; ++i)
        {
            int q = v[i];
            double s = sect(hull.back(), q);
            while (s <= zb[zb.size() - 2])
            {
                hull.pop_back();
                zb.pop_back();
                s = sect(hull.back(), q);
            }
            zb.back() = s;
            hull.push_back(q);
            zb.push_back(INF);
        }
        size_t k = 0;
        for (int y = 0; y < H; ++y)
        {
            while (zb[k + 1] < y)
                ++k;
            int p = hull[k];
            d2[(size_t)y * W + x] = (double)(y - p) * (y - p) + f[p];
        }
    }
}

int rasterProximityHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("proximity");
    PxCommon c;
    int rc = pxBegin(r, c);
    if (rc >= 0)
        return rc;

    std::string otTyped = r.str("output-data-type");
    bool otSet = r.get("output-data-type") != nullptr;
    std::string otCanon = otSet ? pxCanonOt(cmd, otTyped) : "Float32";
    DType ot = pxOtType(otCanon);
    int band = pxBandArg(r);
    std::vector<std::string> targetToks = r.list("target-values");
    bool geo = strEqualNoCase(r.str("distance-units", "pixel"), "geo");
    double maxDist = atof(r.str("max-distance", "0").c_str());
    bool fixedGiven = r.get("fixed-value") != nullptr;
    double fixedVal = fixedGiven ? atof(r.str("fixed-value").c_str()) : 0;
    bool ndGiven = r.get("nodata") != nullptr;
    double ndVal = ndGiven ? atof(r.str("nodata").c_str()) : 65535.0;

    auto ds = std::make_unique<PxDataset>(std::move(c.src), false);
    PxDataset *pd = ds.get();
    {
        Band b;
        b.index = 1;
        b.type = ot;
        b.hasNodata = ndGiven;
        b.nodata = ndVal;
        b.blockX = std::min(pd->width, 128);
        b.blockY = std::min(pd->height, 128);
        pd->bands.push_back(std::move(b));
        pd->data.resize(1);
    }

    std::string extra;
    if (otSet)
        extra += " --output-data-type " + otCanon;
    if (r.get("band"))
        extra += strPrintf(" --band %d", band);
    if (!targetToks.empty())
    {
        extra += " --target-values ";
        for (size_t i = 0; i < targetToks.size(); ++i)
        {
            if (i)
                extra += ",";
            extra += gdalgQuote(realEcho(targetToks[i]));
        }
    }
    if (r.get("distance-units"))
        extra += " --distance-units " + r.str("distance-units");
    if (r.get("max-distance"))
        extra += " --max-distance " + realEcho(r.str("max-distance"));
    if (fixedGiven)
        extra += " --fixed-value " + realEcho(r.str("fixed-value"));
    if (ndGiven)
        extra += " --nodata " + realEcho(r.str("nodata"));

    auto mat = [pd, band, targetToks, geo, maxDist, fixedGiven, fixedVal,
                ndGiven, ndVal](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        RasterDatasetBase &src = *pd->src;
        int W = pd->width, H = pd->height;
        std::vector<double> vals;
        if (!src.readBand(band, vals))
            return 1;
        std::vector<int32_t> targets;
        for (const std::string &t : targetToks)
        {
            long v = strtol(realEcho(t).c_str(), nullptr, 10);
            targets.push_back((int32_t)v);
        }
        std::vector<uint8_t> isTarget((size_t)W * H, 0);
        std::vector<int32_t> scan(vals.size());
        for (size_t i = 0; i < vals.size(); ++i)
        {
            int32_t v = pxToInt32(vals[i]);
            scan[i] = v;
            bool t;
            if (targets.empty())
                t = v != 0;
            else
                t = std::find(targets.begin(), targets.end(), v) !=
                    targets.end();
            isTarget[i] = t ? 1 : 0;
        }
        // USE_INPUT_NODATA: non-target pixels whose int32 scan value
        // equals the truncating cast of the source band nodata get the
        // output nodata fill (never a distance, never the fixed value)
        bool srcNdMask = false;
        int32_t srcNdInt = 0;
        if (band >= 1 && band <= (int)src.bands.size() &&
            src.bands[band - 1].hasNodata)
        {
            double nd = src.bands[band - 1].nodata;
            srcNdMask = true;
            if (std::isnan(nd) || nd >= 2147483648.0 ||
                nd <= -2147483649.0)
                srcNdInt = INT32_MIN;
            else
                srcNdInt = (int32_t)nd;
        }
        double distMult = 1.0;
        if (geo && pd->hasGT)
        {
            distMult = std::fabs(pd->gt[1]);
            if (std::fabs(pd->gt[1]) != std::fabs(pd->gt[5]))
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Pixels not square, distances will be "
                            "inaccurate.");
        }
        double maxDistPx =
            maxDist != 0.0 ? maxDist / distMult : (double)(W + H);
        std::vector<double> d2;
        pxDistanceTransform(isTarget, W, H, d2);
        std::vector<double> &out = pd->data[0];
        out.resize((size_t)W * H);
        float ndF = (float)ndVal;
        (void)ndGiven;
        for (size_t i = 0; i < out.size(); ++i)
        {
            float v;
            if (isTarget[i])
                v = 0.0f;
            else if (srcNdMask && scan[i] == srcNdInt)
                v = ndF;
            else if (d2[i] <= maxDistPx * maxDistPx)
            {
                if (fixedGiven)
                    v = (float)fixedVal;
                else
                    v = (float)((float)std::sqrt(d2[i]) * distMult);
            }
            else
                v = ndF;
            out[i] = v;
        }
        pd->evaluated = true;
        return 0;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    // GDALG-only outputs never evaluate (a null callback also keeps the
    // deferred open warnings unreplayed, as the reference's lazy SRS
    // decode does)
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (pxEvalTarget(c.drv, c.output))
        matF = mat;
    return rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                    c.overwrite, c.append, c.drv, extra,
                                    matF, nullptr);
}

// ------------------------------------------------------------------
// sieve
// ------------------------------------------------------------------

struct MaskReadFail
{
    bool failed = false;
    int failRow = 0;
    std::string msg1, msg2;
};

// reads the mask's first band as a validity grid; a too-small mask
// reproduces the reference's RasterIO window error (fails at the first
// out-of-range row: row 0 for a too-narrow mask, row mask.height for a
// too-short one)
bool pxReadMask(RasterDatasetBase &mask, const std::string &maskPath,
                int W, int H, std::vector<uint8_t> &valid, MaskReadFail &mf)
{
    if (mask.width < W || mask.height < H)
    {
        mf.failed = true;
        mf.failRow = mask.width < W ? 0 : mask.height;
        mf.msg1 = strPrintf("%s, band 1: Access window out of range in "
                            "RasterIO().  Requested",
                            maskPath.c_str());
        mf.msg2 = strPrintf("(0,%d) of size %dx1 on raster of %dx%d.",
                            mf.failRow, W, mask.width, mask.height);
        return false;
    }
    std::vector<double> mv;
    if (!mask.readBand(1, mv))
        return false;
    valid.assign((size_t)W * H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            valid[(size_t)y * W + x] =
                mv[(size_t)y * mask.width + x] != 0 ? 1 : 0;
    return true;
}

// aborting mask failure: partial bar at the mask-read stage (which
// starts at algorithm progress 0.5 and spans segW), then the error
void pxMaskFailAbort(bool quiet, double scale, double segW, int H,
                     const MaskReadFail &mf)
{
    if (!quiet)
    {
        TermProgress tp;
        tp.update(scale * (0.5 + segW * (double)mf.failRow / H));
    }
    cplErrorStr(CE_Failure, CPLE_IllegalArg, mf.msg1 + "\n" + mf.msg2);
}

int rasterSieveHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    PrefixScope prefix("sieve");
    PxCommon c;
    int rc = pxBegin(r, c);
    if (rc >= 0)
        return rc;

    int band = pxBandArg(r);
    int thr = atoi(r.str("size-threshold", "2").c_str());
    bool conn8 = r.flag("connect-diagonal-pixels");
    std::string maskPath = r.str("mask");
    bool maskGiven = r.get("mask") != nullptr;

    auto ds = std::make_unique<PxDataset>(std::move(c.src), true);
    PxDataset *pd = ds.get();
    {
        Band b = pd->src->bands[(size_t)band - 1];
        b.index = 1;
        pd->bands.push_back(std::move(b));
        pd->data.resize(1);
    }

    std::string extra;
    if (maskGiven)
        extra += " --mask " + gdalgQuote(maskPath);
    if (r.get("band"))
        extra += strPrintf(" --band %d", band);
    if (r.get("size-threshold"))
        extra += strPrintf(" --size-threshold %d", thr);
    if (conn8)
        extra += " --connect-diagonal-pixels";

    bool quiet = c.quiet;
    double scale = pxProgressScale(c.drv);
    auto mat = [pd, band, thr, conn8, maskGiven, maskPath, quiet,
                scale](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        RasterDatasetBase &src = *pd->src;
        int W = pd->width, H = pd->height;
        std::vector<double> vals;
        if (!src.readBand(band, vals))
            return 1;
        std::vector<uint8_t> valid;
        if (maskGiven)
        {
            std::string err;
            auto mask = openRaster(maskPath, err);
            if (!mask)
                return 1;
            MaskReadFail mf;
            if (!pxReadMask(*mask, maskPath, W, H, valid, mf))
            {
                if (mf.failed && mf.failRow == 0)
                {
                    // too-narrow mask: error reported, the algorithm is
                    // skipped and the original values pass through
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                mf.msg1 + "\n" + mf.msg2);
                    pd->data[0] = vals;
                    pd->evaluated = true;
                    return 0;
                }
                if (mf.failed)
                {
                    pxMaskFailAbort(quiet, scale, 0.125, H, mf);
                    return 1;
                }
                return 1;
            }
        }
        size_t n = (size_t)W * H;
        std::vector<int32_t> g(n);
        for (size_t i = 0; i < n; ++i)
            g[i] = pxToInt32(vals[i]);
        auto isValid = [&](size_t i)
        { return valid.empty() || valid[i] != 0; };

        // connected-component labels (union-find over equal-valued
        // 4/8-neighbours)
        std::vector<int> lbl(n, -1);
        std::vector<int> parent;
        std::function<int(int)> find = [&](int a)
        {
            while (parent[a] != a)
            {
                parent[a] = parent[parent[a]];
                a = parent[a];
            }
            return a;
        };
        auto unite = [&](int a, int b)
        {
            a = find(a);
            b = find(b);
            if (a != b)
                parent[std::max(a, b)] = std::min(a, b);
        };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                size_t i = (size_t)y * W + x;
                if (!isValid(i))
                    continue;
                int cur = -1;
                auto adopt = [&](size_t j)
                {
                    if (!isValid(j) || g[j] != g[i])
                        return;
                    int lj = lbl[j];
                    if (cur == -1)
                        cur = lj;
                    else
                        unite(cur, lj);
                };
                if (x > 0)
                    adopt(i - 1);
                if (y > 0)
                {
                    if (conn8 && x > 0)
                        adopt(i - W - 1);
                    adopt(i - W);
                    if (conn8 && x + 1 < W)
                        adopt(i - W + 1);
                }
                if (cur == -1)
                {
                    cur = (int)parent.size();
                    parent.push_back(cur);
                }
                lbl[i] = cur;
            }
        int np = (int)parent.size();
        std::vector<int> sizes(np, 0);
        for (size_t i = 0; i < n; ++i)
            if (lbl[i] != -1)
            {
                lbl[i] = find(lbl[i]);
                sizes[lbl[i]]++;
            }
        // largest-neighbour selection: first neighbour in scan order
        // wins ties (a strictly greater size is needed to replace)
        std::vector<int> bigNb(np, -1);
        auto cmpNb = [&](int a, int b)
        {
            if (bigNb[a] == -1 || sizes[bigNb[a]] < sizes[b])
                bigNb[a] = b;
        };
        auto pair = [&](size_t i, size_t j)
        {
            if (!isValid(i) || !isValid(j))
                return;
            int a = lbl[i], b = lbl[j];
            if (a == b)
                return;
            cmpNb(a, b);
            cmpNb(b, a);
        };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                size_t i = (size_t)y * W + x;
                if (!isValid(i))
                    continue;
                if (x > 0)
                    pair(i, i - 1);
                if (y > 0)
                {
                    if (conn8 && x > 0)
                        pair(i, i - W - 1);
                    pair(i, i - W);
                    if (conn8 && x + 1 < W)
                        pair(i, i - W + 1);
                }
            }
        // chain-following merge: below-threshold links are followed to
        // the first big polygon; cycles leave the polygon untouched
        std::vector<int> mergeTo(np, -1);
        for (int p = 0; p < np; ++p)
        {
            if (sizes[p] >= thr)
                continue;
            int nb = bigNb[p];
            int sanity = np;
            while (nb != -1 && sizes[nb] < thr && sanity-- > 0)
                nb = bigNb[nb];
            if (nb != -1 && sizes[nb] >= thr && sanity > 0)
                mergeTo[p] = nb;
        }
        std::vector<int32_t> polyVal(np, 0);
        for (size_t i = 0; i < n; ++i)
            if (lbl[i] != -1)
                polyVal[lbl[i]] = g[i];
        std::vector<double> &out = pd->data[0];
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            int32_t v = g[i];
            if (lbl[i] != -1 && mergeTo[lbl[i]] != -1)
                v = polyVal[mergeTo[lbl[i]]];
            out[i] = (double)v;
        }
        pd->evaluated = true;
        return 0;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    // GDALG-only outputs never evaluate (a null callback also keeps the
    // deferred open warnings unreplayed, as the reference's lazy SRS
    // decode does)
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (pxEvalTarget(c.drv, c.output))
        matF = mat;
    return rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                    c.overwrite, c.append, c.drv, extra,
                                    matF, nullptr);
}

// ------------------------------------------------------------------
// fill-nodata
// ------------------------------------------------------------------

int rasterFillNodataHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    PrefixScope prefix("fill-nodata");
    PxCommon c;
    int rc = pxBegin(r, c);
    if (rc >= 0)
        return rc;

    int band = pxBandArg(r);
    int maxDist = atoi(r.str("max-distance", "100").c_str());
    int smoothIt = atoi(r.str("smoothing-iterations", "0").c_str());
    std::string maskPath = r.str("mask");
    bool maskGiven = r.get("mask") != nullptr;
    bool nearest = r.str("strategy", "invdist") == "nearest";

    auto ds = std::make_unique<PxDataset>(std::move(c.src), true);
    PxDataset *pd = ds.get();
    {
        Band b = pd->src->bands[(size_t)band - 1];
        b.index = 1;
        pd->bands.push_back(std::move(b));
        pd->data.resize(1);
    }

    std::string extra;
    if (r.get("band"))
        extra += strPrintf(" --band %d", band);
    if (r.get("max-distance"))
        extra += strPrintf(" --max-distance %d", maxDist);
    if (r.get("smoothing-iterations"))
        extra += strPrintf(" --smoothing-iterations %d", smoothIt);
    if (maskGiven)
        extra += " --mask " + gdalgQuote(maskPath);
    if (r.get("strategy"))
        extra += " --strategy " + r.str("strategy");

    bool quiet = c.quiet;
    double scale = pxProgressScale(c.drv);
    auto mat = [pd, band, maxDist, smoothIt, maskGiven, maskPath, nearest,
                quiet, scale](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        RasterDatasetBase &src = *pd->src;
        int W = pd->width, H = pd->height;
        std::vector<double> vals;
        if (!src.readBand(band, vals))
            return 1;
        size_t n = (size_t)W * H;
        const Band &sb = src.bands[(size_t)band - 1];
        std::vector<uint8_t> target(n, 0);
        bool anyTargeting = false;
        if (maskGiven)
        {
            std::string err;
            auto mask = openRaster(maskPath, err);
            if (!mask)
                return 1;
            MaskReadFail mf;
            std::vector<uint8_t> valid;
            if (!pxReadMask(*mask, maskPath, W, H, valid, mf))
            {
                if (mf.failed)
                    pxMaskFailAbort(quiet, scale, 0.25, H, mf);
                return 1;
            }
            for (size_t i = 0; i < n; ++i)
                target[i] = valid[i] ? 0 : 1;
            anyTargeting = true;
        }
        else if (sb.hasNodata)
        {
            for (size_t i = 0; i < n; ++i)
                target[i] = (std::isnan(sb.nodata)
                                 ? std::isnan(vals[i])
                                 : vals[i] == sb.nodata)
                                ? 1
                                : 0;
            anyTargeting = true;
        }
        std::vector<float> grid(n);
        for (size_t i = 0; i < n; ++i)
            grid[i] = (float)vals[i];
        if (anyTargeting && maxDist >= 0)
        {
            double maxD2 = (double)maxDist * maxDist;
            bool unlimited = maxDist == 0;
            int R = unlimited ? std::max(W, H) : maxDist;
            std::vector<float> filled = grid;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    size_t i = (size_t)y * W + x;
                    if (!target[i])
                        continue;
                    // four slots fed from per-column nearest top/bottom
                    // candidates: 0/1 = top/bottom over the current and
                    // left columns, 2/3 = top/bottom over the strictly
                    // right columns, scanned outward.  Replacement is
                    // d2 < fl(sqrt(prev))^2: the sqrt round-trip decides
                    // exact ties (e.g. sqrt(5)^2 > 5 lets a later tie
                    // steal the slot while sqrt(13)^2 < 13 keeps the
                    // first), reproducing the reference's tie choices
                    double bestD[4];
                    float bestV[4];
                    bool has[4] = {false, false, false, false};
                    for (int q = 0; q < 4; ++q)
                        bestD[q] = std::numeric_limits<double>::infinity();
                    auto addCand = [&](int slot, int dx, int dy, size_t j)
                    {
                        double d2 = (double)dx * dx + (double)dy * dy;
                        if (!unlimited && d2 > maxD2)
                            return;
                        if (d2 < bestD[slot] * bestD[slot])
                        {
                            bestD[slot] = std::sqrt(d2);
                            bestV[slot] = grid[j];
                            has[slot] = true;
                        }
                    };
                    auto scanColumn = [&](int cx, int topSlot, int botSlot)
                    {
                        if (cx < 0 || cx >= W)
                            return;
                        int dx = cx - x;
                        for (int dy = dx == 0 ? -1 : 0; dy >= -R; --dy)
                        {
                            int sy = y + dy;
                            if (sy < 0)
                                break;
                            size_t j = (size_t)sy * W + cx;
                            if (!target[j])
                            {
                                addCand(topSlot, dx, dy, j);
                                break;
                            }
                        }
                        for (int dy = 1; dy <= R; ++dy)
                        {
                            int sy = y + dy;
                            if (sy >= H)
                                break;
                            size_t j = (size_t)sy * W + cx;
                            if (!target[j])
                            {
                                addCand(botSlot, dx, dy, j);
                                break;
                            }
                        }
                    };
                    scanColumn(x, 0, 1);
                    for (int a = 1; a <= R; ++a)
                    {
                        if (has[0] && has[1] && has[2] && has[3])
                        {
                            double worst = 0;
                            for (int q = 0; q < 4; ++q)
                                worst = std::max(worst, bestD[q]);
                            if ((double)a > worst)
                                break;
                        }
                        scanColumn(x - a, 0, 1);
                        scanColumn(x + a, 2, 3);
                    }
                    if (!(has[0] || has[1] || has[2] || has[3]))
                        continue;
                    if (nearest)
                    {
                        int bq = -1;
                        float bd = 0;
                        for (int q = 0; q < 4; ++q)
                        {
                            if (!has[q])
                                continue;
                            float fd = (float)bestD[q];
                            if (bq == -1 || fd < bd)
                            {
                                bq = q;
                                bd = fd;
                            }
                        }
                        filled[i] = bestV[bq];
                    }
                    else
                    {
                        double vs = 0, ws = 0;
                        for (int q = 0; q < 4; ++q)
                        {
                            if (!has[q])
                                continue;
                            float fd = (float)bestD[q];
                            double w = 1.0 / fd;
                            ws += w;
                            vs += bestV[q] * w;
                        }
                        filled[i] = (float)(vs / ws);
                    }
                }
            grid = filled;
            // Jacobi 3x3 mean smoothing over the target pixels only
            for (int it = 0; it < smoothIt; ++it)
            {
                std::vector<float> next = grid;
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                    {
                        size_t i = (size_t)y * W + x;
                        if (!target[i])
                            continue;
                        // float32 row-major accumulation: the rounding
                        // of the running sum is observable in the output
                        float sum = 0;
                        int cnt = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                int sx = x + dx, sy = y + dy;
                                if (sx < 0 || sy < 0 || sx >= W ||
                                    sy >= H)
                                    continue;
                                sum += grid[(size_t)sy * W + sx];
                                ++cnt;
                            }
                        next[i] = sum / (float)cnt;
                    }
                grid = next;
            }
        }
        std::vector<double> &out = pd->data[0];
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
            out[i] = grid[i];
        pd->evaluated = true;
        return 0;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    // GDALG-only outputs never evaluate (a null callback also keeps the
    // deferred open warnings unreplayed, as the reference's lazy SRS
    // decode does)
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (pxEvalTarget(c.drv, c.output))
        matF = mat;
    return rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                    c.overwrite, c.append, c.drv, extra,
                                    matF, nullptr);
}

// ------------------------------------------------------------------
// neighbors
// ------------------------------------------------------------------

struct NbKernel
{
    std::string name;  // empty for inline matrices
    std::string raw;   // token as given (GDALG echo)
    int size = 3;
    std::vector<double> coefs;  // row-major
    bool sizeLocked = false;    // named kernels with fixed sizes
};

bool nbParseMatrix(const std::string &tok, std::vector<double> &coefs,
                   bool &nonNumeric)
{
    nonNumeric = false;
    std::string cur;
    std::vector<std::string> toks;
    for (char ch : tok)
    {
        if (ch == '[' || ch == ']' || ch == ',' || ch == ' ')
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
    for (const std::string &t : toks)
    {
        double v;
        if (!parseFullDouble(t, v))
        {
            nonNumeric = true;
            return false;
        }
        coefs.push_back(v);
    }
    size_t m = coefs.size();
    size_t rt = (size_t)std::lround(std::sqrt((double)m));
    return m > 0 && rt * rt == m && rt % 2 == 1;
}

const char *kNbKernelListMsg =
    "Valid values for 'kernel' argument are: 'edge1', 'edge2', 'equal', "
    "'gaussian', 'sharpen', 'u', 'unsharp-masking', 'v' or "
    "[[val00,val10,...,valN0],...,[val0N,val1N,...valNN]]";

bool nbNamedKernel(const std::string &name)
{
    static const char *kNames[] = {"edge1", "edge2",  "equal",
                                   "gaussian", "sharpen", "u",
                                   "unsharp-masking", "v"};
    for (const char *k : kNames)
        if (name == k)
            return true;
    return false;
}

// builds the coefficient grid for a named kernel at the given size;
// empty result = unsupported size (sizesMsg carries the allowed sizes)
std::vector<double> nbNamedCoefs(const std::string &name, int size,
                                 std::string &sizesMsg)
{
    auto fixed3 = [&](std::initializer_list<double> v) -> std::vector<double>
    {
        sizesMsg = "3";
        if (size != 3)
            return {};
        return std::vector<double>(v);
    };
    if (name == "edge1")
        return fixed3({0, -1, 0, -1, 4, -1, 0, -1, 0});
    if (name == "edge2")
        return fixed3({-1, -1, -1, -1, 8, -1, -1, -1, -1});
    if (name == "sharpen")
        return fixed3({0, -1, 0, -1, 5, -1, 0, -1, 0});
    if (name == "u")
        return fixed3({0, 0, 0, -0.5, 0, 0.5, 0, 0, 0});
    if (name == "v")
        return fixed3({0, -0.5, 0, 0, 0, 0, 0, 0.5, 0});
    if (name == "equal")
    {
        sizesMsg = "";
        return std::vector<double>((size_t)size * size, 1.0);
    }
    if (name == "gaussian")
    {
        sizesMsg = "3 or 5";
        if (size == 3)
        {
            std::vector<double> g = {1, 2, 1, 2, 4, 2, 1, 2, 1};
            for (double &v : g)
                v /= 16.0;
            return g;
        }
        if (size == 5)
        {
            double w[5] = {1, 4, 6, 4, 1};
            std::vector<double> g;
            for (int r0 = 0; r0 < 5; ++r0)
                for (int c0 = 0; c0 < 5; ++c0)
                    g.push_back(w[r0] * w[c0] / 256.0);
            return g;
        }
        return {};
    }
    if (name == "unsharp-masking")
    {
        sizesMsg = "5";
        if (size != 5)
            return {};
        double w[5] = {1, 4, 6, 4, 1};
        std::vector<double> g;
        for (int r0 = 0; r0 < 5; ++r0)
            for (int c0 = 0; c0 < 5; ++c0)
            {
                double gv = w[r0] * w[c0] / 256.0;
                g.push_back(r0 == 2 && c0 == 2 ? 2.0 - gv : -gv);
            }
        return g;
    }
    return {};
}

// structural kernel/method validation shared by the post-validator and
// the handler; failed entries print and accumulate
bool nbValidate(const CmdSpec &cmd, ParseResult &r,
                std::vector<NbKernel> *outKernels)
{
    (void)cmd;
    std::vector<std::string> kernToks = r.list("kernel");
    std::vector<std::string> methods = r.list("method");
    bool sizeGiven = r.get("size") != nullptr;
    int size = sizeGiven ? atoi(r.str("size").c_str()) : 0;
    if (!methods.empty() && methods.size() != 1 &&
        methods.size() != kernToks.size())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "neighbors: The number of values for the 'method' "
                    "argument should be one or exactly the number of "
                    "values of 'kernel'");
        return true;
    }
    for (const std::string &tok : kernToks)
    {
        NbKernel k;
        k.raw = tok;
        if (!tok.empty() && tok[0] == '[')
        {
            bool nn = false;
            nbParseMatrix(tok, k.coefs, nn);
            k.size = (int)std::lround(std::sqrt((double)k.coefs.size()));
            if (sizeGiven && size != k.size)
            {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("neighbors: Value of 'size' argument (%d) "
                              "inconsistent with the one deduced from "
                              "the kernel matrix (%d)",
                              size, k.size));
                return true;
            }
        }
        else
        {
            k.name = tok;
            k.size = sizeGiven ? size : 3;
            if (k.name == "unsharp-masking" && !sizeGiven)
                k.size = 5;
            std::string sizesMsg;
            k.coefs = nbNamedCoefs(k.name, k.size, sizesMsg);
            if (k.coefs.empty())
            {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("neighbors: Currently only size = %s is "
                              "supported for kernel '%s'",
                              sizesMsg.c_str(), k.name.c_str()));
                return true;
            }
        }
        if (outKernels)
            outKernels->push_back(std::move(k));
    }
    return false;
}

std::string nbKernelValueCheck(const std::string &argName,
                               const std::string &value)
{
    if (argName == "size")
    {
        long v = atol(value.c_str());
        if (v >= 3 && v % 2 == 0)
            return "\x05The value of 'size' must be an odd number.";
        return "";
    }
    if (argName == "nodata")
    {
        if (value == "none" || strEqualNoCase(value, "none"))
            return "";
        double v;
        if (parseFullDouble(value, v))
            return "";
        return "\x05Value of 'nodata' should be 'none', a numeric value, "
               "'nan', 'inf' or '-inf'";
    }
    if (argName != "kernel")
        return "";
    if (!value.empty() && value[0] == '[')
    {
        std::vector<double> coefs;
        bool nn = false;
        if (!nbParseMatrix(value, coefs, nn))
        {
            if (nn)
                return "\x05Non-numeric value found in the 'kernel' "
                       "argument";
            return "\x05The number of values in the 'kernel' argument "
                   "must be an odd square number.";
        }
        return "";
    }
    if (!nbNamedKernel(value))
        return std::string("\x05") + kNbKernelListMsg;
    return "";
}

int rasterNeighborsHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("neighbors");
    PxCommon c;
    int rc = pxBegin(r, c);
    if (rc >= 0)
        return rc;

    std::vector<NbKernel> kernels;
    if (nbValidate(cmd, r, &kernels))
    {
        handlerPrintUsage();
        return 1;
    }
    std::vector<std::string> methods = r.list("method");
    std::string otTyped = r.str("output-data-type");
    bool otSet = r.get("output-data-type") != nullptr;
    std::string otCanon = otSet ? pxCanonOt(cmd, otTyped) : "Float64";
    DType ot = pxOtType(otCanon);
    bool bandGiven = r.get("band") != nullptr;
    int band = pxBandArg(r);
    std::string ndTok = r.str("nodata");
    bool ndGiven = r.get("nodata") != nullptr;
    // only the exact lowercase spelling suppresses the metadata; other
    // casings pass the value check but fall through to CPLAtof (0)
    bool ndNone = ndGiven && ndTok == "none";
    double ndOverride = ndGiven && !ndNone ? strtod(ndTok.c_str(), nullptr)
                                           : 0.0;

    auto ds = std::make_unique<PxDataset>(std::move(c.src), false);
    PxDataset *pd = ds.get();
    std::vector<int> srcBands;
    if (bandGiven)
        srcBands.push_back(band);
    else
        for (size_t i = 0; i < pd->src->bands.size(); ++i)
            srcBands.push_back((int)i + 1);
    int nOut = (int)srcBands.size() * (int)kernels.size();
    for (int i = 0; i < nOut; ++i)
    {
        const Band &sb =
            pd->src->bands[(size_t)srcBands[(size_t)i /
                                            kernels.size()] - 1];
        Band b;
        b.index = i + 1;
        b.type = ot;
        b.colorInterp = i == 0 ? "Gray" : "Undefined";
        if (ndGiven)
        {
            b.hasNodata = !ndNone;
            b.nodata = ndOverride;
        }
        else
        {
            b.hasNodata = sb.hasNodata;
            b.nodata = sb.nodata;
        }
        b.blockX = std::min(pd->width, 128);
        b.blockY = std::min(pd->height, 128);
        pd->bands.push_back(std::move(b));
    }
    pd->data.resize((size_t)nOut);

    std::string extra;
    if (bandGiven)
        extra += strPrintf(" --band %d", band);
    if (!methods.empty())
    {
        extra += " --method ";
        for (size_t i = 0; i < methods.size(); ++i)
        {
            if (i)
                extra += ",";
            extra += methods[i];
        }
    }
    if (r.get("size"))
        extra += strPrintf(" --size %d", atoi(r.str("size").c_str()));
    for (const NbKernel &k : kernels)
        extra += " --kernel " + gdalgQuote(k.raw);
    if (otSet)
        extra += " --output-data-type " + otCanon;
    if (ndGiven)
        extra += " --nodata " + ndTok;

    auto methodOf = [&](size_t kIdx) -> std::string
    {
        if (methods.empty())
            return "mean";
        return methods.size() == 1 ? methods[0] : methods[kIdx];
    };

    bool methodsGiven = !methods.empty();
    auto mat = [pd, srcBands, kernels, methodOf, methodsGiven,
                &r](std::unique_ptr<RasterDatasetBase> &) -> int
    {
        (void)r;
        // an explicitly requested mean over an inline matrix whose
        // coefficients sum to zero is refused at run time (named
        // zero-sum kernels and the implicit default are allowed)
        for (size_t kIdx = 0; kIdx < kernels.size(); ++kIdx)
        {
            const NbKernel &k = kernels[kIdx];
            if (k.name.empty() && methodsGiven &&
                methodOf(kIdx) == "mean")
            {
                double s = 0;
                for (double cv : k.coefs)
                    s += cv;
                if (s == 0)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "neighbors: Specifying method = 'mean' "
                                "for a kernel whose sum of coefficients "
                                "is zero is not allowed. Use 'sum' "
                                "instead");
                    return 1;
                }
            }
        }
        RasterDatasetBase &src = *pd->src;
        int W = pd->width, H = pd->height;
        size_t n = (size_t)W * H;
        int ob = 0;
        for (size_t sbIdx = 0; sbIdx < srcBands.size(); ++sbIdx)
        {
            const Band &sb = src.bands[(size_t)srcBands[sbIdx] - 1];
            std::vector<double> vals;
            if (!src.readBand(srcBands[sbIdx], vals))
                return 1;
            std::vector<float> fv(n);
            std::vector<uint8_t> nd(n, 0);
            for (size_t i = 0; i < n; ++i)
            {
                fv[i] = (float)vals[i];
                if (sb.hasNodata &&
                    (std::isnan(sb.nodata) ? std::isnan(vals[i])
                                           : vals[i] == sb.nodata))
                    nd[i] = 1;
            }
            for (size_t kIdx = 0; kIdx < kernels.size(); ++kIdx, ++ob)
            {
                const NbKernel &k = kernels[kIdx];
                std::string method = methodOf(kIdx);
                int sz = k.size, half = sz / 2;
                std::vector<double> &out = pd->data[(size_t)ob];
                out.resize(n);
                std::vector<float> vw;
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                    {
                        size_t i = (size_t)y * W + x;
                        if (nd[i])
                        {
                            out[i] = sb.nodata;
                            continue;
                        }
                        vw.clear();
                        float sumP = 0.0f, sumK = 0.0f;
                        for (int r0 = 0; r0 < sz; ++r0)
                            for (int c0 = 0; c0 < sz; ++c0)
                            {
                                double kw = k.coefs[(size_t)r0 * sz + c0];
                                if (kw == 0)
                                    continue;
                                int sx = x + c0 - half;
                                int sy = y + r0 - half;
                                sx = std::min(std::max(sx, 0), W - 1);
                                sy = std::min(std::max(sy, 0), H - 1);
                                size_t j = (size_t)sy * W + sx;
                                if (nd[j])
                                    continue;
                                float p = (float)kw * fv[j];
                                sumP += p;
                                sumK += (float)kw;
                                vw.push_back(p);
                            }
                        float res = 0.0f;
                        if (method == "mean")
                            res = sumK != 0.0f ? sumP / sumK : sumP;
                        else if (method == "sum")
                            res = sumP;
                        else if (vw.empty())
                            res = 0.0f;
                        else if (method == "min")
                            res = *std::min_element(vw.begin(), vw.end());
                        else if (method == "max")
                            res = *std::max_element(vw.begin(), vw.end());
                        else if (method == "median")
                        {
                            std::vector<float> s = vw;
                            std::sort(s.begin(), s.end());
                            size_t m = s.size();
                            res = m % 2 ? s[m / 2]
                                        : (s[m / 2 - 1] + s[m / 2]) / 2.0f;
                        }
                        else if (method == "mode")
                        {
                            // winner = the value whose winning count is
                            // reached first in window scan order
                            std::map<float, int> tot;
                            for (float v : vw)
                                tot[v]++;
                            int wcnt = 0;
                            for (const auto &kv : tot)
                                wcnt = std::max(wcnt, kv.second);
                            std::map<float, int> run;
                            for (float v : vw)
                                if (++run[v] == wcnt)
                                {
                                    res = v;
                                    break;
                                }
                        }
                        else if (method == "stddev")
                        {
                            double m = 0;
                            for (float v : vw)
                                m += v;
                            m /= (double)vw.size();
                            double var = 0;
                            for (float v : vw)
                                var += ((double)v - m) * ((double)v - m);
                            var /= (double)vw.size();
                            res = (float)std::sqrt(var);
                        }
                        out[i] = res;
                    }
            }
        }
        pd->evaluated = true;
        return 0;
    };

    // semantic VRT serialization (KernelFilteredSource per band)
    std::string inputPath = c.input;
    pd->vrtCustom = [pd, srcBands, kernels, methodOf, inputPath](
                        const std::string &, const std::string &output)
    {
        std::string x;
        emitVrtHeaderPx(x, *pd);
        int ob = 0;
        for (size_t sbIdx = 0; sbIdx < srcBands.size(); ++sbIdx)
            for (size_t kIdx = 0; kIdx < kernels.size(); ++kIdx, ++ob)
            {
                const NbKernel &k = kernels[kIdx];
                const Band &b = pd->bands[(size_t)ob];
                const Band &sb =
                    pd->src->bands[(size_t)srcBands[sbIdx] - 1];
                std::string method = methodOf(kIdx);
                x += strPrintf("  <VRTRasterBand dataType=\"%s\" "
                               "band=\"%d\">\n",
                               dtypeName(b.type), ob + 1);
                if (b.hasNodata)
                    x += "    <NoDataValue>" + fmt18(b.nodata) +
                         "</NoDataValue>\n";
                int relative = 0;
                std::string rel = relToOutput(inputPath, output, relative);
                x += "    <KernelFilteredSource>\n";
                x += strPrintf("      <SourceFilename relativeToVRT="
                               "\"%d\">%s</SourceFilename>\n",
                               relative, xmlEscT(rel).c_str());
                x += strPrintf("      <SourceBand>%d</SourceBand>\n",
                               srcBands[sbIdx]);
                int bw = 0, bh = 0;
                pd->src->realBlockDims(bw, bh);
                x += strPrintf("      <SourceProperties RasterXSize="
                               "\"%d\" RasterYSize=\"%d\" DataType=\"%s\""
                               " BlockXSize=\"%d\" BlockYSize=\"%d\" "
                               "/>\n",
                               pd->src->width, pd->src->height,
                               dtypeName(sb.type), bw, bh);
                if (sb.hasNodata)
                    x += "      <NODATA>" + fmt18(sb.nodata) +
                         "</NODATA>\n";
                bool normalized = method != "sum";
                x += strPrintf("      <Kernel normalized=\"%d\">\n",
                               normalized ? 1 : 0);
                x += strPrintf("        <Size>%d</Size>\n", k.size);
                double sumK = 0;
                for (double kv : k.coefs)
                    sumK += kv;
                x += "        <Coefs>";
                for (size_t ci = 0; ci < k.coefs.size(); ++ci)
                {
                    double cv = k.coefs[ci];
                    if (method == "mean" && sumK != 0)
                        cv /= sumK;
                    if (ci)
                        x += " ";
                    x += strPrintf("%.8g", cv);
                }
                x += "</Coefs>\n";
                x += "      </Kernel>\n";
                if (method != "mean" && method != "sum")
                    x += "      <Function>" + method + "</Function>\n";
                x += "    </KernelFilteredSource>\n";
                x += "  </VRTRasterBand>\n";
            }
        x += "</VRTDataset>\n";
        return x;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    // GDALG-only outputs never evaluate (a null callback also keeps the
    // deferred open warnings unreplayed, as the reference's lazy SRS
    // decode does)
    std::function<int(std::unique_ptr<RasterDatasetBase> &)> matF;
    if (pxEvalTarget(c.drv, c.output))
        matF = mat;
    return rasterConvertWriteOutput(base, r, c.input, c.output, c.quiet,
                                    c.overwrite, c.append, c.drv, extra,
                                    matF, nullptr);
}

// ------------------------------------------------------------------
// validation-stage stacking (band range, mask opening, neighbors
// structural checks) after the output-exists processing
// ------------------------------------------------------------------

bool pxPostValidator(const CmdSpec &cmd, ParseResult &r, bool)
{
    bool failed = false;
    if (cmd.id == "raster_sieve" || cmd.id == "raster_fill-nodata")
        failed |= pxPostMaskCheck(r);
    failed |= pxPostBandCheck(cmd.name, r);
    if (cmd.id == "raster_neighbors")
        failed |= nbValidate(cmd, r, nullptr);
    return failed;
}

}  // namespace

void registerRasterPxHandlers()
{
    registerHandler("raster_proximity", rasterProximityHandler);
    registerPreValidator("raster_proximity", pxPreValidator);
    registerPostValidator("raster_proximity", pxPostValidator);
    registerHandler("raster_sieve", rasterSieveHandler);
    registerPreValidator("raster_sieve", pxPreValidator);
    registerPostValidator("raster_sieve", pxPostValidator);
    registerHandler("raster_fill-nodata", rasterFillNodataHandler);
    registerPreValidator("raster_fill-nodata", pxPreValidator);
    registerPostValidator("raster_fill-nodata", pxPostValidator);
    registerHandler("raster_neighbors", rasterNeighborsHandler);
    registerPreValidator("raster_neighbors", pxPreValidator);
    registerPostValidator("raster_neighbors", pxPostValidator);
    registerArgValueCheck("raster_neighbors", nbKernelValueCheck);
}
