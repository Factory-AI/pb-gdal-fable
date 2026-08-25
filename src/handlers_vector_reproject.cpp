#include "cpl.h"
#include "engine.h"
#include "ogr.h"
#include "proj_min.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "vsi.h"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{

// the reference surfaces PROJ's own per-point transform errors through
// its error handler with a "PROJ: " prefix (level 1 = PJ_LOG_ERROR)
void projErrLogger(void *, int level, const char *msg)
{
    if (level == 1 && msg)
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    std::string("PROJ: ") + msg);
}

PJ *makeCrsOp(PJ *srcPj, PJ *dstPj)
{
    PJ *raw = proj_create_crs_to_crs_from_pj(projCtx(), srcPj, dstPj,
                                             nullptr, nullptr);
    if (!raw)
        return nullptr;
    PJ *norm = proj_normalize_for_visualization(projCtx(), raw);
    if (norm)
    {
        proj_destroy(raw);
        return norm;
    }
    return raw;
}

struct OpState
{
    PJ *op = nullptr;
    long long fails = 0;

    OpState() = default;
    OpState(const OpState &) = delete;
    OpState &operator=(const OpState &) = delete;
    ~OpState()
    {
        if (op)
            proj_destroy(op);
    }
};

struct ReprojState
{
    std::vector<VectorReprojectStep> steps;
    bool partialOk = false;
    std::map<const OgrLayer *, std::vector<std::shared_ptr<OpState>>>
        layerOps;
};

bool partialReprojectionEnabled()
{
    std::string v = configGet("OGR_ENABLE_PARTIAL_REPROJECTION", "NO");
    return !(strEqualNoCase(v, "NO") || strEqualNoCase(v, "FALSE") ||
             strEqualNoCase(v, "OFF") || v == "0");
}

bool transformVertex(OpState &st, double *xyz)
{
    proj_errno_reset(st.op);
    PJ_COORD c = proj_coord(xyz[0], xyz[1], xyz[2], HUGE_VAL);
    PJ_COORD o = proj_trans(st.op, PJ_FWD, c);
    int err = proj_errno(st.op);
    if (err != 0 || !std::isfinite(o.v[0]) || !std::isfinite(o.v[1]))
    {
        // the transform object's own error fires once, on its 20th
        // cumulative failure, after PROJ's log line for that point
        if (++st.fails == 20)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Reprojection failed, err = %d, further "
                                  "errors will be suppressed on the "
                                  "transform object.",
                                  err));
        return false;
    }
    xyz[0] = o.v[0];
    xyz[1] = o.v[1];
    xyz[2] = o.v[2];
    return true;
}

bool transformSequence(std::vector<double> &coords, std::vector<double> &m,
                       OpState &st, bool partialOk)
{
    size_t n = coords.size() / 3;
    if (!n)
        return true;
    std::vector<char> ok(n, 1);
    size_t nOk = 0;
    for (size_t i = 0; i < n; ++i)
    {
        ok[i] = transformVertex(st, &coords[i * 3]) ? 1 : 0;
        if (ok[i])
            ++nOk;
    }
    if (nOk == n)
        return true;
    if (!partialOk)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Full reprojection failed, but partial is possible if "
                    "you define OGR_ENABLE_PARTIAL_REPROJECTION "
                    "configuration option to TRUE");
        return false;
    }
    size_t w = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (!ok[i])
            continue;
        if (w != i)
        {
            coords[w * 3] = coords[i * 3];
            coords[w * 3 + 1] = coords[i * 3 + 1];
            coords[w * 3 + 2] = coords[i * 3 + 2];
            if (i < m.size())
                m[w] = m[i];
        }
        ++w;
    }
    coords.resize(w * 3);
    if (!m.empty() && m.size() > w)
        m.resize(w);
    return true;
}

bool transformNode(OgrGeometry &g, OpState &st, bool partialOk)
{
    switch (g.type)
    {
        case 1:
            if (g.empty || g.coords.size() < 3)
                return true;
            return transformVertex(st, &g.coords[0]);
        case 3:
            for (auto &ring : g.parts)
                if (!transformSequence(ring.coords, ring.m, st, partialOk))
                    return false;
            return true;
        case 4:
        case 5:
        case 6:
        case 7:
            for (auto &p : g.parts)
                if (!transformNode(p, st, partialOk))
                    return false;
            return true;
        default:
            for (auto &p : g.parts)
                if (!transformNode(p, st, partialOk))
                    return false;
            return transformSequence(g.coords, g.m, st, partialOk);
    }
}

int gateLayer(ReprojState &st, const OgrLayer &l)
{
    std::vector<std::shared_ptr<OpState>> chain;
    // the op only borrows src/dst during creation, but the pointers must
    // stay alive until then
    std::vector<std::unique_ptr<Srs>> owned;
    PJ *cur = l.hasSrs ? l.srs.pj() : nullptr;
    std::string finalCrs;
    for (const auto &def : st.steps)
    {
        if (!def.activeLayer.empty() && def.activeLayer != l.name)
            continue;
        PJ *src = cur;
        if (!def.srcCrs.empty())
        {
            bool ok = false;
            auto s = std::make_unique<Srs>(
                Srs::fromCliInput(def.srcCrs, ok));
            if (!ok)
                return 1;
            src = s->pj();
            owned.push_back(std::move(s));
        }
        if (!src)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "reproject: Layer '" + l.name +
                            "' has no spatial reference system");
            return 1;
        }
        bool ok = false;
        auto d = std::make_unique<Srs>(Srs::fromCliInput(def.dstCrs, ok));
        if (!ok)
            return 1;
        auto op = std::make_shared<OpState>();
        op->op = makeCrsOp(src, d->pj());
        if (!op->op)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "reproject: Cannot reproject layer '" + l.name +
                            "'");
            return 1;
        }
        chain.push_back(std::move(op));
        cur = d->pj();
        finalCrs = def.dstCrs;
        owned.push_back(std::move(d));
    }
    if (chain.empty())
        return 0;
    st.layerOps[&l] = std::move(chain);
    OgrLayer &ml = const_cast<OgrLayer &>(l);
    bool ok = false;
    ml.srs = Srs::fromCliInput(finalCrs, ok);
    ml.hasSrs = ok;
    return 0;
}

void hookFeature(ReprojState &st, const OgrLayer &l, OgrFeature &f)
{
    auto it = st.layerOps.find(&l);
    if (it == st.layerOps.end() || !f.hasGeom)
        return;
    proj_log_func(projCtx(), nullptr, projErrLogger);
    proj_log_level(projCtx(), 1);
    bool good = true;
    for (auto &op : it->second)
        if (!transformNode(f.geom, *op, st.partialOk))
        {
            good = false;
            break;
        }
    proj_log_level(projCtx(), 0);
    if (!good)
    {
        f.hasGeom = false;
        f.geom = OgrGeometry();
    }
}

std::string joinCommaRp(const std::vector<std::string> &v)
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

// GDALG command-line serialization wraps values holding spaces or quotes
std::string gq(const std::string &v)
{
    if (v.find(' ') == std::string::npos &&
        v.find('"') == std::string::npos)
        return v;
    std::string out = "\"";
    for (char ch : v)
    {
        if (ch == '"')
            out += "\\\"";
        else
            out += ch;
    }
    out += '"';
    return out;
}

void initResultRp(const CmdSpec &spec, ParseResult &r)
{
    for (const auto &a : spec.args)
        r.byName[a.name].spec = &a;
}

void setArgRp(const CmdSpec &spec, ParseResult &r,
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

std::string leafGdalgCli(ParseResult &r, const std::string &input)
{
    std::string cli = handlerInvokedCli();
    for (const auto &v : r.list("input-format"))
        cli += " --input-format " + gq(v);
    for (const auto &v : r.list("open-option"))
        cli += " --open-option " + gq(v);
    cli += " --input " + gq(input);
    if (!r.list("input-layer").empty())
        cli += " --input-layer " + gq(joinCommaRp(r.list("input-layer")));
    // stdout serialization runs under forced quiet, and the echo says so
    if (r.flag("quiet") || r.str("output") == "/vsistdout/")
        cli += " --quiet";
    for (const auto &v : r.list("output-open-option"))
        cli += " --output-open-option " + gq(v);
    for (const auto &v : r.list("creation-option"))
        cli += " --creation-option " + gq(v);
    for (const auto &v : r.list("layer-creation-option"))
        cli += " --layer-creation-option " + gq(v);
    if (!r.str("output-layer").empty())
        cli += " --output-layer " + gq(r.str("output-layer"));
    if (r.flag("skip-errors"))
        cli += " --skip-errors";
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + gq(r.str("active-layer"));
    if (!r.str("src-crs").empty())
        cli += " --src-crs " + gq(r.str("src-crs"));
    cli += " --dst-crs " + gq(r.str("dst-crs"));
    cli += " --output-format stream --output streamed_dataset";
    return cli;
}

int vectorReprojectHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    std::string format = r.str("output-format");
    bool quiet = r.flag("quiet");

    std::string driver;
    {
        std::string ferr = vectorOutputDriverResolve(format, driver);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "reproject: " + ferr);
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
                        "reproject: " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    if (format.empty() && strEndsWith(strToLower(output), ".gdalg.json"))
        driver = "GDALG";

    std::string err;
    auto ds = openVectorDataset(input, err, r.list("input-format"),
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
        handlerPrintUsage();
        return 1;
    }

    const Spec &spec = Spec::instance();
    const CmdSpec *cs = spec.findById("vector_convert");
    Handler h = findHandler("vector_convert");
    if (!cs || !h)
        return 1;
    ParseResult cr;
    initResultRp(*cs, cr);
    setArgRp(*cs, cr, "input", {input});
    setArgRp(*cs, cr, "output", {output});
    if (!format.empty())
        setArgRp(*cs, cr, "of", {format});
    if (!r.list("input-format").empty())
        setArgRp(*cs, cr, "if", r.list("input-format"));
    if (!r.list("open-option").empty())
        setArgRp(*cs, cr, "oo", r.list("open-option"));
    if (!r.list("input-layer").empty())
        setArgRp(*cs, cr, "layer", r.list("input-layer"));
    if (!r.list("creation-option").empty())
        setArgRp(*cs, cr, "co", r.list("creation-option"));
    if (!r.list("layer-creation-option").empty())
        setArgRp(*cs, cr, "lco", r.list("layer-creation-option"));
    if (!r.str("output-layer").empty())
        setArgRp(*cs, cr, "output-layer", {r.str("output-layer")});
    for (const char *fl : {"overwrite", "append", "update",
                           "overwrite-layer", "upsert", "skip-errors"})
        if (r.flag(fl))
            setArgRp(*cs, cr, fl, {"true"});
    if (quiet)
        setArgRp(*cs, cr, "quiet", {"true"});

    g_pipelineGdalgCli = leafGdalgCli(r, input);
    g_pipelineStepPrefix = "reproject";
    int rc;
    g_convertSourceOverride = std::move(ds);
    if (driver == "GDALG")
    {
        rc = h(*cs, cr);
        g_convertSourceOverride.reset();
    }
    else
    {
        vectorReprojectInstall({{r.str("src-crs"), r.str("dst-crs"),
                                 r.str("active-layer")}});
        rc = h(*cs, cr);
        g_convertSourceOverride.reset();
        vectorReprojectUninstall();
    }
    g_pipelineStepPrefix.clear();
    g_pipelineGdalgCli.clear();
    return rc;
}

}  // namespace

void vectorReprojectInstall(const std::vector<VectorReprojectStep> &steps)
{
    auto st = std::make_shared<ReprojState>();
    st->steps = steps;
    st->partialOk = partialReprojectionEnabled();
    g_convertLayerGate = [st](const OgrLayer &l) {
        return gateLayer(*st, l);
    };
    g_convertFeatureHook = [st](const OgrLayer &l, OgrFeature &f) {
        hookFeature(*st, l, f);
    };
}

void vectorReprojectUninstall()
{
    g_convertLayerGate = nullptr;
    g_convertFeatureHook = nullptr;
}

int vectorReprojectApply(OgrDataset &ds,
                         const std::vector<VectorReprojectStep> &steps,
                         const VectorReprojectInfoNeeds &needs)
{
    ReprojState st;
    st.steps = steps;
    st.partialOk = partialReprojectionEnabled();
    for (auto &l : ds.layers)
        if (int rc = gateLayer(st, l))
            return rc;
    auto selected = [&](const OgrLayer &l) {
        if (needs.layerFilter.empty())
            return true;
        for (const auto &n : needs.layerFilter)
            if (n == l.name)
                return true;
        return false;
    };
    proj_log_func(projCtx(), nullptr, projErrLogger);
    proj_log_level(projCtx(), 1);
    for (auto &l : ds.layers)
    {
        auto it = st.layerOps.find(&l);
        if (it == st.layerOps.end() || !selected(l))
            continue;
        if (needs.extent && l.hasExtent)
        {
            // OGRWarpedLayer envelope semantics: a 21x21 sample grid per
            // hop, any failed sample drops the extent entirely
            double env[4] = {l.extent[0], l.extent[1], l.extent[2],
                             l.extent[3]};
            for (auto &op : it->second)
            {
                const int kStep = 20;
                double out[4] = {0, 0, 0, 0};
                bool any = false, allOk = true;
                double dx = (env[2] - env[0]) / kStep;
                double dy = (env[3] - env[1]) / kStep;
                for (int j = 0; j <= kStep; ++j)
                    for (int i = 0; i <= kStep; ++i)
                    {
                        double xyz[3] = {env[0] + i * dx,
                                         env[1] + j * dy, 0};
                        if (!transformVertex(*op, xyz))
                        {
                            allOk = false;
                            continue;
                        }
                        if (!any)
                        {
                            out[0] = out[2] = xyz[0];
                            out[1] = out[3] = xyz[1];
                            any = true;
                        }
                        else
                        {
                            if (xyz[0] < out[0])
                                out[0] = xyz[0];
                            if (xyz[1] < out[1])
                                out[1] = xyz[1];
                            if (xyz[0] > out[2])
                                out[2] = xyz[0];
                            if (xyz[1] > out[3])
                                out[3] = xyz[1];
                        }
                    }
                if (!allOk)
                {
                    l.hasExtent = false;
                    break;
                }
                for (int k = 0; k < 4; ++k)
                    env[k] = out[k];
            }
            if (l.hasExtent)
                for (int k = 0; k < 4; ++k)
                    l.extent[k] = env[k];
        }
        if (needs.features)
        {
            long long pulled = 0;
            for (auto &f : l.features)
            {
                if (needs.limit >= 0 && pulled >= needs.limit)
                    break;
                ++pulled;
                if (!f.hasGeom)
                    continue;
                bool good = true;
                for (auto &op : it->second)
                    if (!transformNode(f.geom, *op, st.partialOk))
                    {
                        good = false;
                        break;
                    }
                if (!good)
                {
                    f.hasGeom = false;
                    f.geom = OgrGeometry();
                }
            }
        }
        else if (needs.hasFid)
        {
            // direct-FID pull: only the matched feature is transformed
            for (auto &f : l.features)
            {
                if (f.fid != needs.fid)
                    continue;
                if (f.hasGeom)
                {
                    bool good = true;
                    for (auto &op : it->second)
                        if (!transformNode(f.geom, *op, st.partialOk))
                        {
                            good = false;
                            break;
                        }
                    if (!good)
                    {
                        f.hasGeom = false;
                        f.geom = OgrGeometry();
                    }
                }
                break;
            }
        }
    }
    proj_log_level(projCtx(), 0);
    return 0;
}

std::string vectorReprojectStepEcho(const PipeStepArgs &args)
{
    auto get1 = [&](const char *k) -> std::string {
        auto it = args.find(k);
        return it == args.end() || it->second.empty() ? ""
                                                      : it->second[0];
    };
    std::string s = " ! reproject";
    std::string v;
    if (!(v = get1("active-layer")).empty())
        s += " --active-layer " + gq(v);
    if (!(v = get1("src-crs")).empty())
        s += " --src-crs " + gq(v);
    if (!(v = get1("dst-crs")).empty())
        s += " --dst-crs " + gq(v);
    return s;
}

void *vectorCrsOpCreate(const Srs &src, const Srs &dst)
{
    PJ *op = makeCrsOp(src.pj(), dst.pj());
    if (!op)
        return nullptr;
    auto *st = new OpState();
    st->op = op;
    return st;
}

bool vectorCrsOpApply(void *op, OgrGeometry &g)
{
    auto *st = static_cast<OpState *>(op);
    proj_log_func(projCtx(), nullptr, projErrLogger);
    proj_log_level(projCtx(), 1);
    bool ok = transformNode(g, *st, partialReprojectionEnabled());
    proj_log_level(projCtx(), 0);
    return ok;
}

void vectorCrsOpFree(void *op)
{
    delete static_cast<OpState *>(op);
}

void registerVectorReprojectHandler()
{
    registerHandler("vector_reproject", vectorReprojectHandler);
    registerArgValueCheck(
        "vector_reproject",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName != "src-crs" && argName != "dst-crs")
                return "";
            bool ok = false;
            Srs::fromCliInput(value, ok, true);
            if (!ok)
                return "Invalid value for '" + argName + "' argument";
            return "";
        });
}
