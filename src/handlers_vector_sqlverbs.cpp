#include "cpl.h"
#include "engine.h"
#include "ogr.h"
#include "ogrsql.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// vector filter / select / sql: OGRSQL-engine transforms applied at the
// convert handler's dataset-mutate point so validation ordering (output
// exists, -l lookup) matches the reference's step pipeline.

namespace
{

std::string joinCommaSv(const std::vector<std::string> &v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i)
            s += ",";
        s += v[i];
    }
    return s;
}

// GDALG command-line serialization wraps values holding spaces, commas,
// quotes or backslashes; quotes and backslashes get backslash-escaped
std::string gqSv(const std::string &v)
{
    if (v.find_first_of(" ,\"\\") == std::string::npos)
        return v;
    std::string out = "\"";
    for (char ch : v)
    {
        if (ch == '"')
            out += "\\\"";
        else if (ch == '\\')
            out += "\\\\";
        else
            out += ch;
    }
    out += '"';
    return out;
}

std::string fmtRealSv(const std::string &raw)
{
    double d = strtod(raw.c_str(), nullptr);
    return strPrintf("%.17g", d);
}

void initResultSv(const CmdSpec &spec, ParseResult &r)
{
    for (const auto &a : spec.args)
        r.byName[a.name].spec = &a;
}

void setArgSv(const CmdSpec &spec, ParseResult &r,
              const std::string &longName,
              const std::vector<std::string> &vals)
{
    const ArgSpec *a = spec.findLong(longName);
    if (!a)
        return;
    ArgValue &v = r.byName[a->name];
    v.spec = a;
    v.set = true;
    v.values = vals;
    r.order.push_back(a->name);
}

// the shared head of the stream-echo command line; the verb-specific
// tail follows the argument declaration order observed on the reference
std::string gdalgHead(ParseResult &r, const std::string &input,
                      bool hasInputLayer, bool commonOutputLayer)
{
    std::string cli = handlerInvokedCli();
    for (const auto &v : r.list("input-format"))
        cli += " --input-format " + gqSv(v);
    for (const auto &v : r.list("open-option"))
        cli += " --open-option " + gqSv(v);
    cli += " --input " + gqSv(input);
    if (hasInputLayer && !r.list("input-layer").empty())
        cli += " --input-layer " + gqSv(joinCommaSv(r.list("input-layer")));
    // stdout serialization runs under forced quiet, and the echo says so
    if (r.flag("quiet") || r.str("output") == "/vsistdout/")
        cli += " --quiet";
    for (const auto &v : r.list("output-open-option"))
        cli += " --output-open-option " + gqSv(v);
    for (const auto &v : r.list("creation-option"))
        cli += " --creation-option " + gqSv(v);
    for (const auto &v : r.list("layer-creation-option"))
        cli += " --layer-creation-option " + gqSv(v);
    if (commonOutputLayer && !r.str("output-layer").empty())
        cli += " --output-layer " + gqSv(r.str("output-layer"));
    if (r.flag("skip-errors"))
        cli += " --skip-errors";
    return cli;
}

// common front matter: output driver + input format validation; nonzero
// rc means the error (with usage) was reported
int resolveVerbFormats(const CmdSpec &cmd, ParseResult &r,
                       std::string &driver)
{
    std::string format = r.str("output-format");
    {
        std::string ferr = vectorOutputDriverResolve(format, driver);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + ferr);
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

int openInputDsNoUsage(ParseResult &r, const std::string &input,
                       std::unique_ptr<OgrDataset> &ds)
{
    std::string err;
    ds = openVectorDataset(input, err, r.list("input-format"),
                           r.list("open-option"));
    if (!ds)
    {
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        return 1;
    }
    return 0;
}

int openInputDs(const CmdSpec &, ParseResult &r, const std::string &input,
                std::unique_ptr<OgrDataset> &ds)
{
    if (openInputDsNoUsage(r, input, ds))
    {
        handlerPrintUsage();
        return 1;
    }
    return 0;
}

int delegateVerb(ParseResult &r, const std::string &verb,
                 std::unique_ptr<OgrDataset> ds,
                 const std::string &gdalgCli, const std::string &driver,
                 bool forwardOutputLayer,
                 std::function<int(OgrDataset &)> mutate)
{
    if (g_pipelineTransCapture)
    {
        if (mutate && ds && mutate(*ds))
            return 1;
        g_pipelineTransCaptured = std::move(ds);
        return 0;
    }
    const Spec &spec = Spec::instance();
    const CmdSpec *cs = spec.findById("vector_convert");
    Handler h = findHandler("vector_convert");
    if (!cs || !h)
        return 1;
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    ParseResult cr;
    initResultSv(*cs, cr);
    setArgSv(*cs, cr, "input", {input});
    setArgSv(*cs, cr, "output", {r.str("output")});
    if (!r.str("output-format").empty())
        setArgSv(*cs, cr, "of", {r.str("output-format")});
    if (!r.list("input-format").empty())
        setArgSv(*cs, cr, "if", r.list("input-format"));
    if (!r.list("open-option").empty())
        setArgSv(*cs, cr, "oo", r.list("open-option"));
    // rename-layer's -l names the rename target, not a read filter
    if (verb != "rename-layer" && !r.list("input-layer").empty())
        setArgSv(*cs, cr, "layer", r.list("input-layer"));
    if (!r.list("creation-option").empty())
        setArgSv(*cs, cr, "co", r.list("creation-option"));
    if (!r.list("layer-creation-option").empty())
        setArgSv(*cs, cr, "lco", r.list("layer-creation-option"));
    if (forwardOutputLayer && !r.str("output-layer").empty())
        setArgSv(*cs, cr, "output-layer", {r.str("output-layer")});
    for (const char *fl : {"overwrite", "append", "update",
                           "overwrite-layer", "upsert", "skip-errors"})
        if (r.flag(fl))
            setArgSv(*cs, cr, fl, {"true"});
    if (r.flag("quiet"))
        setArgSv(*cs, cr, "quiet", {"true"});

    g_pipelineGdalgCli = gdalgCli;
    g_pipelineStepPrefix = verb;
    // these verbs hide the feature count from the writer: the whole
    // bar renders after the last feature, past any write-time warnings
    if (verb == "filter" || verb == "explode-collections" ||
        verb == "edit" || verb == "rename-layer" || verb == "clip" ||
        verb == "combine")
        g_pipelineWriteBarAtEnd = true;
    int rc;
    g_convertSourceOverride = std::move(ds);
    if (driver != "GDALG" && mutate)
    {
        if (g_convertDatasetMutate)
        {
            // a pipeline transition step chains its post-transforms
            // after the verb's own mutation
            auto post = std::move(g_convertDatasetMutate);
            auto first = std::move(mutate);
            g_convertDatasetMutate = [first, post](OgrDataset &d) -> int {
                int frc = first(d);
                if (frc)
                    return frc;
                return post(d);
            };
        }
        else
            g_convertDatasetMutate = std::move(mutate);
    }
    rc = h(*cs, cr);
    g_convertSourceOverride.reset();
    g_convertDatasetMutate = nullptr;
    g_convertTranslateFail = ConvertTranslateFail();
    g_convertClipPending = ConvertClipPending();
    g_pipelineWriteBarAtEnd = false;
    g_pipelineStepPrefix.clear();
    g_pipelineGdalgCli.clear();
    return rc;
}

bool layerSelectedSv(const std::vector<std::string> &sel,
                     const std::string &active, const OgrLayer &l)
{
    if (!active.empty() && l.name != active)
        return false;
    if (sel.empty())
        return true;
    for (const auto &n : sel)
        if (n == l.name)
            return true;
    return false;
}

// feature envelope over the stride-3 coord tuples; false when the
// geometry carries no vertex at all
bool geomEnvelope(const OgrGeometry &g, double env[4], bool &any)
{
    for (size_t i = 0; i + 2 < g.coords.size(); i += 3)
    {
        double x = g.coords[i], y = g.coords[i + 1];
        if (std::isnan(x) || std::isnan(y))
            continue;
        if (!any)
        {
            env[0] = env[2] = x;
            env[1] = env[3] = y;
            any = true;
        }
        else
        {
            if (x < env[0])
                env[0] = x;
            if (y < env[1])
                env[1] = y;
            if (x > env[2])
                env[2] = x;
            if (y > env[3])
                env[3] = y;
        }
    }
    for (const auto &p : g.parts)
        geomEnvelope(p, env, any);
    return any;
}

void recomputeExtentSv(OgrLayer &lyr)
{
    bool has = false;
    double e[4] = {0, 0, 0, 0};
    for (const OgrFeature &f : lyr.features)
    {
        if (!f.hasGeom)
            continue;
        geomEnvelope(f.geom, e, has);
    }
    lyr.hasExtent = has;
    for (int i = 0; i < 4; ++i)
        lyr.extent[i] = e[i];
}

}  // namespace

void vectorLayerRecomputeExtent(OgrLayer &lyr)
{
    recomputeExtentSv(lyr);
}

std::string vvJoinComma(const std::vector<std::string> &v)
{
    return joinCommaSv(v);
}

std::string vvGq(const std::string &v)
{
    return gqSv(v);
}

std::string vvFmtReal(const std::string &raw)
{
    return fmtRealSv(raw);
}

std::string vvGdalgHead(ParseResult &r, const std::string &input,
                        bool hasInputLayer, bool commonOutputLayer)
{
    return gdalgHead(r, input, hasInputLayer, commonOutputLayer);
}

int vvResolveVerbFormats(const CmdSpec &cmd, ParseResult &r,
                         std::string &driver)
{
    return resolveVerbFormats(cmd, r, driver);
}

int vvOpenInputDs(const CmdSpec &cmd, ParseResult &r,
                  const std::string &input,
                  std::unique_ptr<OgrDataset> &ds)
{
    return openInputDs(cmd, r, input, ds);
}

int vvOpenInputDsNoUsage(const CmdSpec &, ParseResult &r,
                         const std::string &input,
                         std::unique_ptr<OgrDataset> &ds)
{
    return openInputDsNoUsage(r, input, ds);
}

int vvDelegateVerb(ParseResult &r, const std::string &verb,
                   std::unique_ptr<OgrDataset> ds,
                   const std::string &gdalgCli, const std::string &driver,
                   bool forwardOutputLayer,
                   std::function<int(OgrDataset &)> mutate)
{
    return delegateVerb(r, verb, std::move(ds), gdalgCli, driver,
                        forwardOutputLayer, std::move(mutate));
}

bool vvLayerSelected(const std::vector<std::string> &sel,
                     const std::string &active, const OgrLayer &l)
{
    return layerSelectedSv(sel, active, l);
}

// shared transform bodies: the standalone verbs run them through the
// convert dataset-mutate hook, pipeline steps chain them in sequence

int vectorFilterApplyStep(OgrDataset &d, const std::string &where,
                          const std::string &activeLayer,
                          const std::vector<std::string> &layerSel,
                          bool hasBbox, const double bbox[4])
{
    for (OgrLayer &l : d.layers)
    {
        if (!layerSelectedSv(layerSel, activeLayer, l))
            continue;
        if (!where.empty() && !ogrApplyAttributeFilter(l, where, false))
            return 1;
        // a layer without a geometry field passes the spatial filter
        // untouched (no geometry to test)
        if (hasBbox && l.hasGeomField)
        {
            std::vector<OgrFeature> kept;
            for (OgrFeature &f : l.features)
            {
                if (!f.hasGeom)
                    continue;
                double env[4];
                bool any = false;
                if (!geomEnvelope(f.geom, env, any))
                    continue;
                if (env[0] <= bbox[2] && env[2] >= bbox[0] &&
                    env[1] <= bbox[3] && env[3] >= bbox[1])
                    kept.push_back(std::move(f));
            }
            l.features = std::move(kept);
            recomputeExtentSv(l);
        }
    }
    return 0;
}

int vectorSelectApplyStep(OgrDataset &d,
                          const std::vector<std::string> &fields,
                          bool exclude, bool ignoreMissing,
                          const std::string &activeLayer,
                          const std::vector<std::string> &layerSel)
{
    for (OgrLayer &l : d.layers)
    {
        if (!layerSelectedSv(layerSel, activeLayer, l))
            continue;
        std::vector<bool> keep(l.fields.size(), exclude);
        for (const auto &tok : fields)
        {
            if (tok.empty())
                continue;
            int idx = -1;
            for (size_t i = 0; i < l.fields.size(); ++i)
                if (strEqualNoCase(l.fields[i].name, tok))
                {
                    idx = (int)i;
                    break;
                }
            if (idx < 0)
            {
                if (exclude)
                    continue;
                if (ignoreMissing)
                {
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "Field '" + tok +
                                    "' does not exist in layer '" +
                                    l.name + "'. It will be ignored");
                    continue;
                }
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Field '" + tok +
                                "' does not exist in layer '" + l.name +
                                "'. You may specify "
                                "--ignore-missing-fields to skip it");
                return 1;
            }
            keep[idx] = !exclude;
        }
        std::vector<OgrFieldDefn> nf;
        for (size_t i = 0; i < l.fields.size(); ++i)
            if (keep[i])
                nf.push_back(l.fields[i]);
        for (OgrFeature &f : l.features)
        {
            std::vector<OgrFieldValue> nv;
            for (size_t i = 0; i < keep.size(); ++i)
                if (keep[i] && i < f.values.size())
                    nv.push_back(std::move(f.values[i]));
            nv.resize(nf.size());
            f.values = std::move(nv);
        }
        l.fields = std::move(nf);
        if (!exclude)
        {
            // the geometry field is kept only when listed; its name is
            // empty for our drivers, so listing can never match
            l.geomType = 101;
            l.geomHasZ = l.geomHasM = false;
            l.hasGeomField = false;
            l.hasSrs = false;
            l.hasExtent = false;
            for (OgrFeature &f : l.features)
            {
                f.hasGeom = false;
                f.geom = OgrGeometry();
            }
        }
    }
    return 0;
}

int vectorSqlApplyStep(OgrDataset &d,
                       const std::vector<std::string> &stmts,
                       const std::vector<std::string> &outNames,
                       const std::string &dialect, const std::string &verb)
{
    bool sqlite = strEqualNoCase(dialect, "SQLITE");
    bool oddDialect = !dialect.empty() && !sqlite &&
                      !strEqualNoCase(dialect, "OGRSQL");
    if (!outNames.empty() && outNames.size() != stmts.size())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    verb + ": There should be as many layer names in "
                           "--output-layer as in --statement");
        return 1;
    }
    if (sqlite)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "The SQLite driver needs to be compiled to support "
                    "the SQLite SQL dialect");
        return 1;
    }
    if (oddDialect)
        cplErrorStr(CE_Warning, CPLE_NotSupported,
                    "Dialect '" + dialect +
                        "' is unsupported. Only supported dialects are "
                        "'OGRSQL'. Defaulting to OGRSQL");
    std::vector<OgrLayer> results;
    for (size_t i = 0; i < stmts.size(); ++i)
    {
        auto res = ogrExecuteSql(d, stmts[i]);
        if (!res)
            return 1;
        std::string nm = !outNames.empty() ? outNames[i] : res->name;
        if (outNames.empty())
        {
            bool dup = false;
            for (const auto &rl : results)
                if (rl.name == nm)
                    dup = true;
            if (dup)
                nm += strPrintf("%d", (int)i + 1);
        }
        res->name = nm;
        results.push_back(std::move(*res));
    }
    d.layers = std::move(results);
    return 0;
}

namespace
{

// format capability, bbox ordering and fields uniqueness are
// argument-level checks: they fire during parse validation in argument
// declaration order, before the input-exists probe
int verbFormatArgCheck(const std::string &verb, const std::string &argName,
                       ParseResult &r)
{
    if (argName == "output-format")
    {
        std::string driver;
        std::string ferr =
            vectorOutputDriverResolve(r.str("output-format"), driver);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, verb + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    else if (argName == "input-format")
    {
        for (const auto &d : r.list("input-format"))
        {
            std::string ferr = inputFormatCapError(true, d);
            if (!ferr.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            verb + ": " + ferr);
                handlerPrintUsage();
                return 1;
            }
        }
    }
    return 0;
}

int vectorFilterArgCheck(const std::string &argName, ParseResult &r)
{
    if (int rc = verbFormatArgCheck("filter", argName, r))
        return rc;
    if (argName == "bbox")
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

int vectorSelectArgCheck(const std::string &argName, ParseResult &r)
{
    if (int rc = verbFormatArgCheck("select", argName, r))
        return rc;
    if (argName == "fields")
    {
        const std::vector<std::string> &fields = r.list("fields");
        for (size_t i = 0; i < fields.size(); ++i)
            for (size_t j = i + 1; j < fields.size(); ++j)
                if (fields[i] == fields[j])
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "'fields' must be a list of unique "
                                "values.");
                    handlerPrintUsage();
                    return 1;
                }
    }
    return 0;
}

// ------------------------------------------------------------- filter

int vectorFilterHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");

    std::string driver;
    if (int rc = resolveVerbFormats(cmd, r, driver))
        return rc;

    const ArgValue *bb = r.get("bbox");
    bool hasBbox = bb && bb->set && bb->values.size() == 4;
    double bx[4] = {0, 0, 0, 0};
    if (hasBbox)
        for (int i = 0; i < 4; ++i)
            bx[i] = strtod(bb->values[i].c_str(), nullptr);

    if (r.str("output-format").empty() &&
        strEndsWith(strToLower(output), ".gdalg.json"))
        driver = "GDALG";

    std::unique_ptr<OgrDataset> ds;
    if (int rc = openInputDs(cmd, r, input, ds))
        return rc;

    std::string cli = gdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + gqSv(r.str("active-layer"));
    if (hasBbox)
    {
        std::string v;
        for (size_t i = 0; i < 4; ++i)
            v += (i ? "," : "") + fmtRealSv(bb->values[i]);
        cli += " --bbox " + v;
    }
    if (!r.str("where").empty())
        cli += " --where " + gqSv(r.str("where"));
    if (r.flag("update-extent"))
        cli += " --update-extent";
    cli += " --output-format stream --output streamed_dataset";

    std::string where = r.str("where");
    std::string active = r.str("active-layer");
    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [where, active, sel, hasBbox, bx](OgrDataset &d) -> int {
        double bb[4] = {bx[0], bx[1], bx[2], bx[3]};
        return vectorFilterApplyStep(d, where, active, sel, hasBbox, bb);
    };
    return delegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                        mutate);
}

// ------------------------------------------------------------- select

int vectorSelectHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");

    std::string driver;
    if (int rc = resolveVerbFormats(cmd, r, driver))
        return rc;

    std::vector<std::string> fields = r.list("fields");
    if (r.str("output-format").empty() &&
        strEndsWith(strToLower(output), ".gdalg.json"))
        driver = "GDALG";

    std::unique_ptr<OgrDataset> ds;
    if (int rc = openInputDs(cmd, r, input, ds))
        return rc;

    std::string cli = gdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + gqSv(r.str("active-layer"));
    cli += " --fields " + gqSv(joinCommaSv(fields));
    if (r.flag("exclude"))
        cli += " --exclude";
    if (r.flag("ignore-missing-fields"))
        cli += " --ignore-missing-fields";
    cli += " --output-format stream --output streamed_dataset";

    bool exclude = r.flag("exclude");
    bool ignoreMissing = r.flag("ignore-missing-fields");
    std::string active = r.str("active-layer");
    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [fields, exclude, ignoreMissing, active,
                   sel](OgrDataset &d) -> int {
        return vectorSelectApplyStep(d, fields, exclude, ignoreMissing,
                                     active, sel);
    };
    return delegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                        mutate);
}

// ---------------------------------------------------------------- sql

int vectorSqlHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    const ArgValue *ov = r.get("output");
    std::string output = ov && ov->set && !ov->values.empty()
                             ? ov->values[0]
                             : std::string();

    std::string driver;
    if (int rc = resolveVerbFormats(cmd, r, driver))
        return rc;
    if (r.str("output-format").empty() &&
        strEndsWith(strToLower(output), ".gdalg.json"))
        driver = "GDALG";

    std::unique_ptr<OgrDataset> ds;
    if (int rc = openInputDs(cmd, r, input, ds))
        return rc;

    std::vector<std::string> stmts = r.list("sql");
    std::string dialect = r.str("dialect");
    bool sqlite = strEqualNoCase(dialect, "SQLITE");
    bool oddDialect = !dialect.empty() && !sqlite &&
                      !strEqualNoCase(dialect, "OGRSQL");
    bool quiet = r.flag("quiet");
    bool update = r.flag("update");

    if (output.empty())
    {
        // no output: statements run for their side effects only
        auto execFailed = [&](const std::string &stmt) {
            std::string msg = cmd.name +
                              ": Execution of the SQL statement '" + stmt +
                              "' failed.";
            if (!update)
                msg += ".\nPerhaps you need to specify the 'update' "
                       "argument?";
            cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        };
        if (sqlite)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "The SQLite driver needs to be compiled to "
                        "support the SQLite SQL dialect");
            execFailed(stmts.empty() ? "" : stmts[0]);
            return 1;
        }
        if (oddDialect)
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "Dialect '" + dialect +
                            "' is unsupported. Only supported dialects "
                            "are 'OGRSQL'. Defaulting to OGRSQL");
        for (const auto &stmt : stmts)
        {
            auto res = ogrExecuteSql(*ds, stmt);
            if (!res)
            {
                execFailed(stmt);
                return 1;
            }
            if (!quiet)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            cmd.name +
                                ": Execution of the SQL statement '" +
                                stmt +
                                "' returned a result set. It will be "
                                "ignored. You may silence this warning "
                                "with the 'quiet' argument.");
        }
        return 0;
    }

    std::string cli = gdalgHead(r, input, false, false);
    for (const auto &stmt : stmts)
        cli += " --sql " + gqSv(stmt);
    if (!r.list("output-layer").empty())
        cli += " --output-layer " +
               gqSv(joinCommaSv(r.list("output-layer")));
    if (!dialect.empty())
        cli += " --dialect " + gqSv(dialect);
    cli += " --output-format stream --output streamed_dataset";

    std::vector<std::string> names;
    for (const auto &v : r.list("output-layer"))
        for (const auto &part : strSplit(v, ','))
            names.push_back(part);

    std::string verb = cmd.name;
    auto mutate = [stmts, names, dialect, verb](OgrDataset &d) -> int {
        return vectorSqlApplyStep(d, stmts, names, dialect, verb);
    };
    return delegateVerb(r, cmd.name, std::move(ds), cli, driver, false,
                        mutate);
}

int vectorSetFieldTypeArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName != "field-type" && argName != "src-field-type")
        return verbFormatArgCheck("set-field-type", argName, r);
    static const char *names[] = {
        "Integer",     "Integer64",     "Real",    "String",
        "Date",        "DateTime",      "Time",    "Binary",
        "IntegerList", "Integer64List", "RealList", "StringList",
        "Boolean",     "Int16",         "Float32", "JSON",
        "UUID"};
    const ArgValue *v = r.get(argName.c_str());
    if (!v || v->values.empty())
        return 0;
    const std::string &val = v->values[0];
    for (const char *n : names)
        if (strToLower(n) == strToLower(val))
            return 0;
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                strPrintf("set-field-type: Invalid value for argument "
                          "'%s': '%s'",
                          argName.c_str(), val.c_str()));
    handlerPrintUsage();
    return 1;
}

}  // namespace

int vvVerbFormatArgCheck(const std::string &verb,
                         const std::string &argName, ParseResult &r)
{
    return verbFormatArgCheck(verb, argName, r);
}

void registerVectorSqlVerbHandlers()
{
    registerHandler("vector_filter", vectorFilterHandler);
    registerArgCheck("vector_filter", vectorFilterArgCheck);
    registerArgCheck("vector_set-field-type", vectorSetFieldTypeArgCheck);
    registerHandler("vector_select", vectorSelectHandler);
    registerArgCheck("vector_select", vectorSelectArgCheck);
    registerHandler("vector_sql", vectorSqlHandler);
    registerArgCheck("vector_sql",
                     [](const std::string &a, ParseResult &r)
                     { return verbFormatArgCheck("sql", a, r); });
    registerArgCheck("vector_reproject",
                     [](const std::string &a, ParseResult &r)
                     { return verbFormatArgCheck("reproject", a, r); });
}
