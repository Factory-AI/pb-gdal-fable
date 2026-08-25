#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "proj_min.h"
#include "spec.h"
#include "srs.h"
#include "util.h"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sys/stat.h>

#include "jsonwriter.h"

namespace
{

std::string piFmt17(double d)
{
    if (std::isnan(d))
        return "NaN";
    if (std::isinf(d))
        return d > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", d);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E'))
        strcat(buf, ".0");
    return buf;
}

JVal piDbl(double d)
{
    JVal v;
    v.type = JVal::DOUBLE;
    v.d = d;
    v.s = piFmt17(d);
    return v;
}

// column/line and pixel values render with a conditional precision
// trim over %.17g: the first 6+ run of '0'/'9' in the fraction is cut
// when it sits past the first fraction digit with at most 3 digits
// behind it, or starts the fraction and reaches the very end
// (exponent forms are never trimmed); input_coordinate and geometry
// keep raw %.17g
std::string piCleanFmt(double d)
{
    if (std::isnan(d))
        return "NaN";
    if (std::isinf(d))
        return d > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot != std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos)
    {
        size_t p = dot + 1;
        while (p < s.size())
        {
            char c = s[p];
            if (c != '0' && c != '9')
            {
                ++p;
                continue;
            }
            size_t q = p;
            while (q < s.size() && s[q] == c)
                ++q;
            size_t runLen = q - p;
            if (runLen < 6)
            {
                p = q;
                continue;
            }
            size_t idx = p - (dot + 1);
            size_t trail = s.size() - q;
            if ((idx == 0 && trail == 0) || (idx >= 1 && trail <= 3))
            {
                int sig = 0;
                bool leading = true;
                for (size_t k = 0; k < p; ++k)
                {
                    if (!isdigit((unsigned char)s[k]))
                        continue;
                    if (leading && s[k] == '0')
                        continue;
                    leading = false;
                    ++sig;
                }
                if (sig == 0)
                    sig = 1;
                snprintf(buf, sizeof(buf), "%.*g", sig, d);
                s = buf;
            }
            break;
        }
    }
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos)
        s += ".0";
    return s;
}

JVal piDblOgr(double d)
{
    JVal v;
    v.type = JVal::DOUBLE;
    v.d = d;
    v.s = piCleanFmt(d);
    return v;
}

JVal piInt(long long i)
{
    JVal v;
    v.type = JVal::INT;
    v.i = i;
    return v;
}

JVal piStr(const std::string &s)
{
    JVal v;
    v.type = JVal::STRING;
    v.s = s;
    return v;
}

bool piIntegralType(DType t)
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

std::string piCrsUrn(const Srs &srs)
{
    int code = srs.epsgCode();
    if (code == 4326)
        return "urn:ogc:def:crs:OGC:1.3:CRS84";
    if (code > 0)
        return strPrintf("urn:ogc:def:crs:EPSG::%d", code);
    std::string auth = srs.authName();
    std::string c = srs.code();
    if (!auth.empty() && !c.empty())
        return "urn:ogc:def:crs:" + auth + "::" + c;
    return "";
}

struct PiSampler
{
    RasterDatasetBase *ds = nullptr;
    int width = 0, height = 0;
    std::map<int, std::vector<double>> cache;

    const std::vector<double> *bandVals(int b)
    {
        auto it = cache.find(b);
        if (it != cache.end())
            return &it->second;
        std::vector<double> vals;
        if (!ds->readBand(b, vals))
            return nullptr;
        auto r = cache.emplace(b, std::move(vals));
        return &r.first->second;
    }

    double at(const std::vector<double> &v, int x, int y) const
    {
        x = x < 0 ? 0 : (x >= width ? width - 1 : x);
        y = y < 0 ? 0 : (y >= height ? height - 1 : y);
        return v[(size_t)y * width + x];
    }

    // kernels evaluated as distance functions K(t+1), K(t), K(t-1),
    // K(t-2); the FP shapes below reproduce the reference to the last
    // ulp at binary-exact fractions (inexact fractions can still differ
    // in the final digit)
    static double cubicK(double x)
    {
        double a = std::fabs(x);
        if (a <= 1)
            return (1.5 * a - 2.5) * a * a + 1;
        if (a <= 2)
            return ((-0.5 * a + 2.5) * a - 4) * a + 2;
        return 0.0;
    }

    static double bsplineK(double x)
    {
        double a = std::fabs(x);
        if (a >= 2)
            return 0.0;
        if (a <= 1)
            return (4.0 - 6.0 * a * a + 3.0 * a * a * a) *
                   0.16666666666666666667;
        double u = 2.0 - a;
        return u * u * u * 0.16666666666666666667;
    }

    static void kernelCubic(double t, double w[4])
    {
        w[0] = cubicK(t + 1);
        w[1] = cubicK(t);
        w[2] = cubicK(t - 1);
        w[3] = cubicK(t - 2);
    }

    static void kernelBSpline(double t, double w[4])
    {
        w[0] = bsplineK(t + 1);
        w[1] = bsplineK(t);
        w[2] = bsplineK(t - 1);
        w[3] = bsplineK(t - 2);
    }

    // GDALRasterInterpolateAtPoint semantics: valid domain
    // [0,w]x[0,h], pixel-center convention, edge-replicated windows.
    // nearest/bilinear invalidate the result when any touched sample is
    // nodata; cubic/cubicspline skip nodata samples and renormalize by
    // the remaining weight sum (NaN samples pass through untouched)
    bool interpolate(int band, double px, double py,
                     const std::string &alg, bool hasNd, double nd,
                     double &out)
    {
        if (std::isnan(px) || std::isnan(py))
            return false;
        if (px < 0 || px > width || py < 0 || py > height)
            return false;
        const std::vector<double> *vals = bandVals(band);
        if (!vals)
            return false;
        auto isNd = [&](double v)
        { return hasNd && (v == nd || (std::isnan(v) && std::isnan(nd))); };
        // rasters too small for the kernel window degrade the algorithm
        std::string alg2 = alg;
        if ((alg2 == "cubic" || alg2 == "cubicspline") &&
            (width < 4 || height < 4))
            alg2 = "bilinear";
        if (alg2 == "bilinear" && (width < 2 || height < 2))
            alg2 = "nearest";
        if (alg2.empty() || alg2 == "nearest")
        {
            int x = (int)std::floor(px);
            int y = (int)std::floor(py);
            double v = at(*vals, x, y);
            if (isNd(v))
                return false;
            out = v;
            return true;
        }
        double dx = px - 0.5, dy = py - 0.5;
        int x0 = (int)std::floor(dx), y0 = (int)std::floor(dy);
        double fx = dx - x0, fy = dy - y0;
        if (alg2 == "bilinear")
        {
            double v00 = at(*vals, x0, y0);
            double v10 = at(*vals, x0 + 1, y0);
            double v01 = at(*vals, x0, y0 + 1);
            double v11 = at(*vals, x0 + 1, y0 + 1);
            if (isNd(v00) || isNd(v10) || isNd(v01) || isNd(v11))
                return false;
            double r0 = v00 * (1 - fx) + v10 * fx;
            double r1 = v01 * (1 - fx) + v11 * fx;
            out = r0 * (1 - fy) + r1 * fy;
            return true;
        }
        double wx[4], wy[4];
        if (alg2 == "cubicspline")
        {
            kernelBSpline(fx, wx);
            kernelBSpline(fy, wy);
        }
        else
        {
            kernelCubic(fx, wx);
            kernelCubic(fy, wy);
        }
        double acc = 0, ws = 0;
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
            {
                double v = at(*vals, x0 - 1 + i, y0 - 1 + j);
                if (isNd(v))
                    continue;
                double w = wx[i] * wy[j];
                acc += w * v;
                ws += w;
            }
        if (ws == 0)
            return false;
        out = acc / ws;
        return true;
    }
};

struct PiPos
{
    double x = 0, y = 0;  // as given (position CRS / pixel space)
    // carried fields from the position dataset (parallel to piFieldDefs)
    std::vector<OgrFieldValue> fields;
    // extra stdin tokens past the pair, space-joined
    bool hasExtra = false;
    std::string extra;
};

int piError(const std::string &msg, int cls = CPLE_AppDefined)
{
    cplErrorStr(CE_Failure, cls, msg);
    handlerPrintUsage();
    return 1;
}

// the reference's std::string(nullptr) escape when serializing carried
// fields the doc renderer cannot represent (dates, unset lists/JSON)
int piUnexpected()
{
    fflush(stdout);
    fprintf(stderr, "Unexpected exception: basic_string::_M_construct "
                    "null not valid");
    return 255;
}

void piCompact(std::string &out, const JVal &v);

void piCompact(std::string &out, const JVal &v)
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
            out += v.s.empty() ? piFmt17(v.d) : v.s;
            break;
        case JVal::STRING:
            out += '"' + JsonStreamWriter::escape(v.s) + '"';
            break;
        case JVal::ARRAY:
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i)
            {
                if (i)
                    out += ',';
                piCompact(out, v.arr[i]);
            }
            out += ']';
            break;
        case JVal::OBJECT:
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i)
            {
                if (i)
                    out += ',';
                out += '"' + JsonStreamWriter::escape(v.obj[i].first) + "\":";
                piCompact(out, v.obj[i].second);
            }
            out += '}';
            break;
    }
}

int pixelInfoHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool fileMode = !output.empty();

    std::string fmt = r.str("output-format");
    if (!fmt.empty() && !strEqualNoCase(fmt, "GeoJSON"))
        return piError("pixel-info: Invalid value for argument "
                       "'output-format'. Driver '" +
                       fmt + "' does not exist.");

    std::vector<int> bandSel;
    for (const auto &b : r.list("band"))
        bandSel.push_back(atoi(b.c_str()));
    for (int b : bandSel)
        if (b < 1)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Value of 'band' should greater or equal to 1.");
            handlerPrintUsage();
            return 1;
        }

    std::vector<double> posVals;
    bool hasPos = r.get("position") != nullptr;
    for (const auto &raw : r.list("position"))
        for (const auto &p : strSplit(raw, ','))
            posVals.push_back(strtod(p.c_str(), nullptr));
    if (posVals.size() % 2)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "pixel-info: An even number of values must be "
                    "specified for 'position' argument");
        handlerPrintUsage();
        return 1;
    }

    OpenOptions oo;
    oo.allowedDrivers = r.list("input-format");
    for (const auto &kv : r.list("open-option"))
    {
        size_t eq = kv.find('=');
        oo.raw.emplace_back(eq == std::string::npos ? kv : kv.substr(0, eq),
                            eq == std::string::npos ? "" : kv.substr(eq + 1));
    }
    std::string err;
    auto ds = openRaster(input, err, oo);
    if (!ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported file "
                            "format.");
        handlerPrintUsage();
        return 1;
    }

    // validation errors accumulate (refusal, band range, overview) and
    // share a single usage block
    bool preFail = false;
    if (fileMode && !r.flag("overwrite"))
    {
        struct stat st;
        if (stat(output.c_str(), &st) == 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pixel-info: " + outputExistsKind(output) + " '" +
                            output +
                            "' already exists. You may specify the "
                            "--overwrite option.");
            preFail = true;
        }
    }

    int nBands = (int)ds->bands.size();
    for (int b : bandSel)
        if (b > nBands)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("pixel-info: Value of 'band' should "
                                  "be greater or equal than 1 and less "
                                  "or equal than %d.",
                                  nBands));
            preFail = true;
            break;
        }
    if (bandSel.empty())
        for (int b = 1; b <= nBands; ++b)
            bandSel.push_back(b);

    // an overview only redirects the sampling: the column/line domain,
    // geotransform, CRS and band metadata stay those of the base dataset
    std::unique_ptr<RasterDatasetBase> ovDs;
    double sampleScaleX = 1.0, sampleScaleY = 1.0;
    if (r.get("overview"))
    {
        if (ds->dispOverviews().empty())
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "pixel-info: Source dataset has no overviews. "
                        "Argument 'overview' must not be specified.");
            preFail = true;
        }
        else
        {
            long long lvl = atoll(r.str("overview").c_str());
            if (lvl >= (long long)ds->dispOverviews().size())
            {
                cplErrorStr(
                    CE_Failure, CPLE_IllegalArg,
                    strPrintf("pixel-info: Source dataset has only %d "
                              "overview levels. 'overview' value must be "
                              "strictly lower than this number.",
                              (int)ds->dispOverviews().size()));
                preFail = true;
            }
            else if (!preFail)
            {
                ovDs = ds->openOverviewEntry(
                    ds->dispOverviews()[(size_t)lvl]);
                if (!ovDs)
                    return piError("pixel-info: Cannot open overview "
                                   "level " +
                                   r.str("overview"));
                sampleScaleX = (double)ovDs->width / ds->width;
                sampleScaleY = (double)ovDs->height / ds->height;
            }
        }
    }
    if (preFail)
    {
        handlerPrintUsage();
        return 1;
    }

    ds->replayDeferred();

    std::string resampling = r.str("resampling");

    // position CRS resolution: "pixel" (default with -p) means raw
    // column/line; otherwise positions are georeferenced coordinates
    // transformed into the raster CRS then run through the inverse GT
    std::string posCrsDef = r.str("position-crs");
    bool pixelSpace = posCrsDef.empty() || posCrsDef == "pixel";
    Srs posSrs;
    bool posSrsSet = false;
    if (!pixelSpace)
    {
        bool ok = false;
        posSrs = Srs::fromCliInput(posCrsDef, ok, true);
        if (!ok)
            return piError("pixel-info: Invalid value for 'position-crs' "
                           "argument");
        posSrsSet = true;
    }

    std::vector<PiPos> positions;
    std::vector<OgrFieldDefn> carryFields;
    std::string posLayerName;
    Srs posLayerSrs;
    bool posLayerHasSrs = false;
    if (r.get("position-dataset"))
    {
        std::string pdPath = r.str("position-dataset");
        std::string verr;
        auto vds = openVectorDataset(pdPath, verr, {});
        if (!vds)
        {
            if (verr != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + pdPath +
                                "' not recognized as being in a supported "
                                "file format.");
            handlerPrintUsage();
            return 1;
        }
        OgrLayer *lyr = nullptr;
        std::string wantLayer = r.str("input-layer");
        if (!wantLayer.empty())
        {
            for (auto &l : vds->layers)
                if (l.name == wantLayer)
                    lyr = &l;
            if (!lyr)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "pixel-info: Cannot find layer '" + wantLayer +
                                "' in '" + pdPath + "'");
                return 1;
            }
        }
        else if (!vds->layers.empty())
            lyr = &vds->layers[0];
        if (!lyr)
            return piError("pixel-info: '" + pdPath + "' has no layer");
        posLayerName = lyr->name;
        posLayerHasSrs = lyr->hasSrs;
        if (posLayerHasSrs)
            posLayerSrs = lyr->srs;

        std::vector<std::string> want = r.list("include-field");
        bool all = want.empty();
        bool none = false;
        for (const auto &w : want)
        {
            if (w == "ALL")
                all = true;
            else if (w == "NONE")
                none = true;
        }
        std::vector<int> fieldIdx;
        if (all && !none)
        {
            for (size_t i = 0; i < lyr->fields.size(); ++i)
                fieldIdx.push_back((int)i);
        }
        else if (!none)
        {
            for (const auto &w : want)
            {
                int found = -1;
                for (size_t i = 0; i < lyr->fields.size(); ++i)
                    if (lyr->fields[i].name == w)
                        found = (int)i;
                if (found < 0)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Field '" + w + "' does not exist in "
                                                "layer '" +
                                    lyr->name + "'");
                    return 1;
                }
                fieldIdx.push_back(found);
            }
        }
        for (int fi : fieldIdx)
            carryFields.push_back(lyr->fields[fi]);
        for (const auto &f : lyr->features)
        {
            if (!f.hasGeom || f.geom.type != 1 || f.geom.coords.size() < 2)
                continue;
            PiPos p;
            p.x = f.geom.coords[0];
            p.y = f.geom.coords[1];
            for (int fi : fieldIdx)
                p.fields.push_back(fi < (int)f.values.size()
                                       ? f.values[fi]
                                       : OgrFieldValue());
            positions.push_back(std::move(p));
        }
        // positions from a vector dataset are georeferenced in the
        // layer CRS unless --position-crs overrides it
        if (!posSrsSet && posLayerHasSrs)
        {
            posSrs = posLayerSrs;
            posSrsSet = true;
        }
        pixelSpace = false;
    }
    else if (hasPos)
    {
        for (size_t i = 0; i + 1 < posVals.size() || i + 2 == posVals.size();
             i += 2)
        {
            if (i + 1 >= posVals.size())
                break;
            PiPos p;
            p.x = posVals[i];
            p.y = posVals[i + 1];
            positions.push_back(p);
        }
    }

    // output file resolution + pre-creation happens before stdin
    // positions are read (but after -p/--position-dataset collection)
    std::string lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    bool jsonFile = fileMode && strEndsWith(lower, ".json") &&
                    !strEndsWith(lower, ".geojson");
    bool isGj = fileMode && strEndsWith(lower, ".geojson");
    if (fileMode)
    {
        if (!jsonFile && !isGj)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pixel-info: Cannot guess driver for " + output);
            return 1;
        }
        FILE *pre = fopen(output.c_str(), "wb");
        if (!pre)
        {
            if (jsonFile)
                cplErrorStr(CE_Failure, CPLE_FileIO,
                            "pixel-info: Cannot create '" + output + "'");
            else
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "Failed to create GeoJSON datasource: " +
                                output + ": " + output + ": " +
                                strerror(errno));
            return 1;
        }
        fclose(pre);
    }

    if (!r.get("position-dataset") && !hasPos)
    {
        // with no positions on the command line the reference reads
        // column/line (or X/Y) pairs from stdin, one per line, split on
        // spaces; an empty line terminates, a short line aborts
        int lineNo = 0;
        std::string line;
        while (std::getline(std::cin, line))
        {
            ++lineNo;
            while (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                break;
            std::vector<std::string> toks;
            size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && line[i] == ' ')
                    ++i;
                size_t j = i;
                while (j < line.size() && line[j] != ' ')
                    ++j;
                if (j > i)
                    toks.push_back(line.substr(i, j - i));
                i = j;
            }
            if (toks.size() < 2)
            {
                fprintf(stderr, "Not enough values at line %d\n", lineNo);
                return 1;
            }
            PiPos p;
            p.x = strtod(toks[0].c_str(), nullptr);
            p.y = strtod(toks[1].c_str(), nullptr);
            for (size_t k = 2; k < toks.size(); ++k)
            {
                if (p.hasExtra)
                    p.extra += ' ';
                p.hasExtra = true;
                p.extra += toks[k];
            }
            positions.push_back(std::move(p));
        }
    }

    PJ *op = nullptr;
    if (!pixelSpace && posSrsSet && ds->hasSrs &&
        !posSrs.isEquivalentTo(ds->srs))
    {
        PJ *raw = proj_create_crs_to_crs_from_pj(projCtx(), posSrs.pj(),
                                                 ds->srs.pj(), nullptr,
                                                 nullptr);
        if (raw)
        {
            op = proj_normalize_for_visualization(projCtx(), raw);
            proj_destroy(raw);
        }
        if (!op)
            return piError("pixel-info: Cannot create transformation from "
                           "'" +
                           posCrsDef + "' to the raster CRS");
    }

    const double *gt = ds->gt;
    double det = gt[1] * gt[5] - gt[2] * gt[4];
    bool invertible = det != 0;
    double inv[6] = {0, 0, 0, 0, 0, 0};
    if (invertible)
    {
        inv[1] = gt[5] / det;
        inv[2] = -gt[2] / det;
        inv[4] = -gt[4] / det;
        inv[5] = gt[1] / det;
        inv[0] = -(gt[0] * inv[1] + gt[3] * inv[2]);
        inv[3] = -(gt[0] * inv[4] + gt[3] * inv[5]);
    }

    PiSampler sampler;
    RasterDatasetBase *sampleDs = ovDs ? ovDs.get() : ds.get();
    sampler.ds = sampleDs;
    sampler.width = sampleDs->width;
    sampler.height = sampleDs->height;

    bool promoteZ = r.flag("promote-pixel-value-to-z");

    struct PiOut
    {
        PiPos pos;
        double col = 0, line = 0;
        double geoX = 0, geoY = 0;
        struct BandVal
        {
            int band;
            bool valid = false;
            double raw = 0;
            double unscaled = 0;
        };
        std::vector<BandVal> bands;
    };
    std::vector<PiOut> outs;
    for (const auto &p : positions)
    {
        PiOut o;
        o.pos = p;
        if (pixelSpace)
        {
            o.col = p.x;
            o.line = p.y;
            o.geoX = gt[0] + o.col * gt[1] + o.line * gt[2];
            o.geoY = gt[3] + o.col * gt[4] + o.line * gt[5];
        }
        else
        {
            double gx = p.x, gy = p.y;
            if (op)
            {
                PJ_COORD c = proj_coord(gx, gy, 0, 0);
                c = proj_trans(op, PJ_FWD, c);
                gx = c.xyzt.x;
                gy = c.xyzt.y;
            }
            if (invertible)
            {
                o.col = inv[0] + gx * inv[1] + gy * inv[2];
                o.line = inv[3] + gx * inv[4] + gy * inv[5];
            }
            else
            {
                o.col = std::nan("");
                o.line = std::nan("");
            }
            // the geometry always round-trips through the geotransform
            o.geoX = gt[0] + o.col * gt[1] + o.line * gt[2];
            o.geoY = gt[3] + o.col * gt[4] + o.line * gt[5];
        }
        for (int b : bandSel)
        {
            PiOut::BandVal bv;
            bv.band = b;
            const Band &bd = ds->bands[(size_t)b - 1];
            double v = 0;
            if (sampler.interpolate(b, o.col * sampleScaleX,
                                    o.line * sampleScaleY, resampling,
                                    bd.hasNodata, bd.nodata, v))
            {
                bv.valid = true;
                bv.raw = v;
                bv.unscaled = v * bd.scale + bd.offset;
            }
            o.bands.push_back(bv);
        }
        outs.push_back(std::move(o));
    }
    if (op)
        proj_destroy(op);

    if (!fileMode || jsonFile)
    {
        JVal doc;
        doc.type = JVal::OBJECT;
        doc.obj.emplace_back("type", piStr("FeatureCollection"));
        if (ds->hasSrs)
        {
            std::string urn = piCrsUrn(ds->srs);
            if (!urn.empty())
            {
                JVal props;
                props.type = JVal::OBJECT;
                props.obj.emplace_back("name", piStr(urn));
                JVal crs;
                crs.type = JVal::OBJECT;
                crs.obj.emplace_back("type", piStr("name"));
                crs.obj.emplace_back("properties", props);
                doc.obj.emplace_back("crs", crs);
            }
        }
        JVal feats;
        feats.type = JVal::ARRAY;
        for (const auto &o : outs)
        {
            JVal props;
            props.type = JVal::OBJECT;
            JVal ic;
            ic.type = JVal::ARRAY;
            ic.arr.push_back(piDbl(o.pos.x));
            ic.arr.push_back(piDbl(o.pos.y));
            props.obj.emplace_back("input_coordinate", ic);
            if (o.pos.hasExtra)
                props.obj.emplace_back("extra_content", piStr(o.pos.extra));
            props.obj.emplace_back("column", piDblOgr(o.col));
            props.obj.emplace_back("line", piDblOgr(o.line));
            for (size_t fi = 0; fi < carryFields.size(); ++fi)
            {
                const OgrFieldDefn &fd = carryFields[fi];
                if (fd.type == OFTDate || fd.type == OFTTime ||
                    fd.type == OFTDateTime)
                    return piUnexpected();
                bool have = fi < o.pos.fields.size() &&
                            o.pos.fields[fi].set &&
                            o.pos.fields[fi].v.type != JVal::NUL;
                JVal v;
                if (have)
                    v = o.pos.fields[fi].v;
                else
                    switch (fd.type)
                    {
                        case OFTString:
                            if (fd.subType == OFSTJSON)
                                return piUnexpected();
                            v = piStr("");
                            break;
                        case OFTInteger:
                            if (fd.subType == OFSTBoolean)
                            {
                                v.type = JVal::BOOL;
                                v.b = false;
                            }
                            else
                                v = piInt(0);
                            break;
                        case OFTInteger64:
                            v = piInt(0);
                            break;
                        case OFTReal:
                            v = piDbl(0.0);
                            break;
                        default:
                            return piUnexpected();
                    }
                props.obj.emplace_back(fd.name, v);
            }
            JVal bands;
            bands.type = JVal::ARRAY;
            for (const auto &bv : o.bands)
            {
                JVal bj;
                bj.type = JVal::OBJECT;
                bj.obj.emplace_back("band_number", piInt(bv.band));
                if (bv.valid)
                {
                    const Band &bd = ds->bands[(size_t)bv.band - 1];
                    if (piIntegralType(bd.type))
                        bj.obj.emplace_back("raw_value",
                                            piInt((long long)bv.raw));
                    else
                        bj.obj.emplace_back("raw_value",
                                            piDblOgr(bv.raw));
                    bj.obj.emplace_back("unscaled_value",
                                        piDblOgr(bv.unscaled));
                }
                bands.arr.push_back(bj);
            }
            props.obj.emplace_back("bands", bands);
            JVal geom;
            if (ds->hasGT)
            {
                JVal coords;
                coords.type = JVal::ARRAY;
                coords.arr.push_back(piDbl(o.geoX));
                coords.arr.push_back(piDbl(o.geoY));
                if (promoteZ && o.bands.size() == 1 && o.bands[0].valid)
                    coords.arr.push_back(piDbl(o.bands[0].unscaled));
                geom.type = JVal::OBJECT;
                geom.obj.emplace_back("type", piStr("Point"));
                geom.obj.emplace_back("coordinates", coords);
            }
            JVal feat;
            feat.type = JVal::OBJECT;
            feat.obj.emplace_back("type", piStr("Feature"));
            feat.obj.emplace_back("properties", props);
            feat.obj.emplace_back("geometry", geom);
            feats.arr.push_back(feat);
        }
        doc.obj.emplace_back("features", feats);
        std::string text = jsoncSerialize(doc);
        if (jsonFile)
            return writeStringToFile(output, text) ? 0 : 1;
        fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }

    // file mode: flat band_N fields written through the GeoJSON driver
    std::string layerName;
    if (!posLayerName.empty())
        layerName = posLayerName;
    else
    {
        layerName = output;
        size_t slash = layerName.find_last_of('/');
        if (slash != std::string::npos)
            layerName = layerName.substr(slash + 1);
        size_t dot = layerName.find_last_of('.');
        if (dot != std::string::npos)
            layerName = layerName.substr(0, dot);
    }

    std::string out = "{\n\"type\": \"FeatureCollection\",\n\"name\": \"" +
                      JsonStreamWriter::escape(layerName) + "\",\n";
    const Srs *fileSrs = nullptr;
    if (posSrsSet)
        fileSrs = &posSrs;
    if (fileSrs)
    {
        std::string urn = piCrsUrn(*fileSrs);
        if (!urn.empty())
            out += "\"crs\": { \"type\": \"name\", \"properties\": { "
                   "\"name\": \"" +
                   urn + "\" } },\n";
    }
    out += "\"features\": [\n";
    for (size_t i = 0; i < outs.size(); ++i)
    {
        const PiOut &o = outs[i];
        out += "{\"type\":\"Feature\",\"properties\":{";
        bool first = true;
        auto addProp = [&](const std::string &k, const std::string &val)
        {
            if (!first)
                out += ",";
            first = false;
            out += "\"" + JsonStreamWriter::escape(k) + "\":" + val;
        };
        for (size_t fi = 0; fi < carryFields.size(); ++fi)
        {
            // absent-on-this-feature fields are omitted; explicit nulls
            // are kept
            if (fi >= o.pos.fields.size() || !o.pos.fields[fi].set)
                continue;
            std::string val;
            piCompact(val, o.pos.fields[fi].v);
            addProp(carryFields[fi].name, val);
        }
        if (o.pos.hasExtra)
            addProp("extra_content",
                    "\"" + JsonStreamWriter::escape(o.pos.extra) + "\"");
        addProp("column", piCleanFmt(o.col));
        addProp("line", piCleanFmt(o.line));
        for (const auto &bv : o.bands)
        {
            if (!bv.valid)
                continue;
            const Band &bd = ds->bands[(size_t)bv.band - 1];
            double raw = piIntegralType(bd.type) ? (double)(long long)bv.raw
                                                 : bv.raw;
            addProp(strPrintf("band_%d_raw_value", bv.band),
                    piCleanFmt(raw));
            addProp(strPrintf("band_%d_unscaled_value", bv.band),
                    piCleanFmt(bv.unscaled));
        }
        out += "},\"geometry\":{\"type\":\"Point\",\"coordinates\":[" +
               ogrJsonCoord(o.pos.x) + "," + ogrJsonCoord(o.pos.y) +
               "]}}";
        out += i + 1 < outs.size() ? ",\n" : "\n";
    }
    out += "]\n}\n";
    if (!writeStringToFile(output, out))
        return 1;
    return 0;
}

}  // namespace

void registerRasterPixelInfoHandlers()
{
    registerHandler("raster_pixel-info", pixelInfoHandler);
}
