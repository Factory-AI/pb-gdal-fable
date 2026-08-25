#include "cpl.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// vector verbs whose algorithm is unavailable in this trimmed build
// (no GEOS): they validate, can serialize to GDALG, then fail.

namespace
{

bool pathExistsVg(const std::string &p)
{
    struct stat sb;
    return stat(p.c_str(), &sb) == 0;
}

std::string fmtRealArg(const std::string &raw)
{
    double d = strtod(raw.c_str(), nullptr);
    return strPrintf("%.17g", d);
}

std::string geosGdalgCli(const CmdSpec &cmd, const ParseResult &r)
{
    std::string cl = handlerInvokedCli();
    for (const auto &a : cmd.args)
    {
        if (a.name == "output" || a.name == "output-format" ||
            a.name == "overwrite")
            continue;
        const ArgValue *v = r.get(a.name);
        if (!v)
            continue;
        if (a.isBool())
        {
            if (v->str() == "true")
                cl += " --" + a.name;
            continue;
        }
        std::string val;
        for (size_t i = 0; i < v->values.size(); ++i)
        {
            if (i)
                val += ",";
            val += (a.type == "real" || a.type == "real_list")
                       ? fmtRealArg(v->values[i])
                       : v->values[i];
        }
        if (val.empty() || val.find(' ') != std::string::npos)
            val = "\"" + val + "\"";
        cl += " --" + a.name + " " + val;
    }
    cl += " --output-format stream --output streamed_dataset";
    return cl;
}

int writeGeosGdalg(const CmdSpec &cmd, const ParseResult &r,
                   const std::string &output)
{
    std::string cl = geosGdalgCli(cmd, r);

    JVal j;
    j.type = JVal::OBJECT;
    auto addStr = [&](const char *k, const std::string &s)
    {
        JVal sv;
        sv.type = JVal::STRING;
        sv.s = s;
        j.obj.emplace_back(k, std::move(sv));
    };
    addStr("type", "gdal_streamed_alg");
    addStr("command_line", cl);
    addStr("gdal_version", "3130000");
    writeStringToFile(output, jsoncSerialize(j, true));
    return 0;
}

int geosStubHandler(const CmdSpec &cmd, ParseResult &r)
{
    bool isLa = cmd.id == "vector_layer-algebra";
    std::vector<const char *> inputArgs = {"input"};
    if (isLa)
        inputArgs.push_back("method");
    std::unique_ptr<OgrDataset> src;
    for (const char *an : inputArgs)
    {
        std::string val = r.str(an);
        std::string err;
        auto ds = openVectorDataset(val, err, r.list("input-format"),
                                    r.list("open-option"));
        if (!ds)
        {
            if (err == "missing")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            datasetMissingMessage(val));
            else if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + val +
                                "' not recognized as being in a supported "
                                "file format.");
            handlerPrintUsage();
            return 1;
        }
        if (!src)
            src = std::move(ds);
    }

    std::string output = r.str("output");
    std::string of = r.str("output-format");
    if (!isLa && r.flag("append") && pathExistsVg(output))
    {
        std::string terr;
        cplPushQuietHandler();
        auto tgt = openVectorDataset(output, terr, {});
        cplPopHandler();
        if (!tgt)
        {
            // append falls back to creation, which refuses to clobber
            // the existing unrecognized output
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        outputExistsKind(output) + " '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
    }
    bool isGdalg =
        strEqualNoCase(of, "GDALG") ||
        (of.empty() && strEndsWith(strToLower(output), ".gdalg.json"));
    if (isGdalg && !isLa)
        return writeGeosGdalg(cmd, r, output);

    if (!isLa)
    {
        for (const auto &lf : r.list("input-layer"))
        {
            bool found = false;
            for (const auto &l : src->layers)
                if (l.name == lf)
                    found = true;
            if (!found)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "read: Cannot find source layer '" + lf + "'");
                return 1;
            }
        }
    }

    const std::string &n = cmd.name;
    if (n == "check-coverage" || n == "simplify-coverage")
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    n + ": " + n +
                        " requires GDAL to be built against version 3.12 "
                        "or later of the GEOS library.");
    else if (n == "clean-coverage")
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    n + ": " + n +
                        " requires GDAL to be built against version 3.14 "
                        "or later of the GEOS library.");
    else if (n == "check-geometry")
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    n + ": " + n +
                        " requires GDAL to be built against the GEOS "
                        "library.");
    else
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    n + ": This algorithm is only supported for builds "
                        "against GEOS");
    return 1;
}

int geosStubPreValidator(const CmdSpec &cmd, ParseResult &r)
{
    std::string format = r.str("output-format");
    if (!format.empty())
    {
        if (strEqualNoCase(format, "Memory"))
            memoryDriverDeprecationWarnOnce();
        bool ok = strEqualNoCase(format, "GeoJSON") ||
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
                            cmd.name +
                                ": Invalid value for argument "
                                "'output-format'. Driver '" +
                                format +
                                "' does not expose the required "
                                "'DCAP_VECTOR' capability.");
            else
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            cmd.name +
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
                        cmd.name + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

// classic order-16 Hilbert index (matches the reference's visiting
// order probed on an 8x8 grid; see NOTES.md "vector sort")
unsigned long long hilbertD16(unsigned x, unsigned y)
{
    unsigned long long d = 0;
    for (unsigned s = 1u << 15; s > 0; s >>= 1)
    {
        unsigned rx = (x & s) ? 1u : 0u;
        unsigned ry = (y & s) ? 1u : 0u;
        d += (unsigned long long)s * s * ((3 * rx) ^ ry);
        if (!ry)
        {
            if (rx)
            {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            unsigned t = x;
            x = y;
            y = t;
        }
    }
    return d;
}

void hilbertSortLayer(OgrLayer &l)
{
    size_t n = l.features.size();
    std::vector<double> cx(n), cy(n);
    std::vector<bool> hasEnv(n, false);
    bool anyExt = false;
    double ext[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < n; ++i)
    {
        const OgrFeature &f = l.features[i];
        if (!f.hasGeom)
            continue;
        double xmin, ymin, xmax, ymax;
        if (!clipGeometryEnvelope(f.geom, xmin, ymin, xmax, ymax))
            continue;
        hasEnv[i] = true;
        cx[i] = (xmin + xmax) / 2;
        cy[i] = (ymin + ymax) / 2;
        if (!anyExt)
        {
            ext[0] = xmin;
            ext[1] = ymin;
            ext[2] = xmax;
            ext[3] = ymax;
            anyExt = true;
        }
        else
        {
            ext[0] = std::min(ext[0], xmin);
            ext[1] = std::min(ext[1], ymin);
            ext[2] = std::max(ext[2], xmax);
            ext[3] = std::max(ext[3], ymax);
        }
    }
    // cells span a 65534-wide grid, halves rounding up; keys keep full
    // 32-bit precision and ties fall to std::sort's introsort artifacts
    // exactly as in the reference (stable only below the 16-element
    // insertion-sort threshold)
    auto cell = [](double v, double mn, double mx) -> unsigned
    {
        double w = mx - mn;
        if (!(w > 0))
            return 0;
        return (unsigned)std::floor(65534.0 * (v - mn) / w + 0.5);
    };
    struct Ent
    {
        unsigned key;
        bool has;
        size_t idx;
    };
    std::vector<Ent> ents(n);
    for (size_t i = 0; i < n; ++i)
    {
        unsigned k = hasEnv[i]
                         ? (unsigned)hilbertD16(cell(cx[i], ext[0], ext[2]),
                                                cell(cy[i], ext[1], ext[3]))
                         : 0u;
        ents[i] = {k, (bool)hasEnv[i], i};
    }
    std::sort(ents.begin(), ents.end(),
              [](const Ent &a, const Ent &b)
              {
                  if (!a.has)
                      return false;
                  if (!b.has)
                      return true;
                  return a.key < b.key;
              });
    std::vector<OgrFeature> out;
    out.reserve(n);
    for (const Ent &e : ents)
        out.push_back(std::move(l.features[e.idx]));
    l.features = std::move(out);
}

int vectorSortHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::string input = r.str("input");
    std::string err;
    auto src = openVectorDataset(input, err, r.list("input-format"),
                                 r.list("open-option"));
    if (!src)
    {
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    std::string output = r.str("output");
    std::string of = r.str("output-format");
    bool isGdalg =
        strEqualNoCase(of, "GDALG") ||
        (of.empty() && strEndsWith(strToLower(output), ".gdalg.json"));
    if (isGdalg)
        return writeGeosGdalg(cmd, r, output);

    std::string driver;
    if (int rc = vvResolveVerbFormats(cmd, r, driver))
        return rc;

    std::string gf = r.str("geometry-field");
    bool strtree = strEqualNoCase(r.str("method"), "strtree");
    auto mutate = [gf, strtree](OgrDataset &d) -> int
    {
        for (const OgrLayer &l : d.layers)
            if (!gf.empty() && gf != l.geomColumnName)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "sort: Specified geometry field '" + gf +
                                "' does not exist in layer '" + l.name +
                                "'");
                return 1;
            }
        if (strtree)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "--method strtree requires a GDAL build against "
                        "the GEOS library.");
            return 1;
        }
        for (OgrLayer &l : d.layers)
            hilbertSortLayer(l);
        return 0;
    };
    return vvDelegateVerb(r, cmd.name, std::move(src),
                          geosGdalgCli(cmd, r), driver, true,
                          std::move(mutate));
}

}  // namespace

void registerVectorGeosHandlers()
{
    static const char *ids[] = {
        "vector_buffer",          "vector_simplify",
        "vector_convex-hull",     "vector_concave-hull",
        "vector_make-valid",      "vector_dissolve",
        "vector_check-coverage",  "vector_clean-coverage",
        "vector_simplify-coverage", "vector_check-geometry",
        "vector_layer-algebra"};
    for (const char *id : ids)
    {
        registerHandler(id, geosStubHandler);
        registerPreValidator(id, geosStubPreValidator);
    }
    registerHandler("vector_sort", vectorSortHandler);
    registerPreValidator("vector_sort", geosStubPreValidator);
}
