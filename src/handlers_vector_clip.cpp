#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "ogr.h"
#include "ogrsql.h"
#include "proj_min.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "vectorverbs.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// vector clip: a GEOS-less build drops every feature of a clipped layer.
// Envelope-disjoint features drop silently at the spatial pre-filter;
// the rest reach the GEOS intersection and fail it, one ERROR 6 per
// feature per stream pull, ending the layer in "Failed to write layer".

int g_vectorClipEmitPulls = 0;

namespace
{

bool clipPolygonal(const OgrGeometry &g)
{
    if (g.type == 3 || g.type == 6)
        return true;
    if (g.type == 7)
        for (const OgrGeometry &p : g.parts)
            if (clipPolygonal(p))
                return true;
    return false;
}

void clipVertexWalk(const OgrGeometry &g,
                    const std::function<void(double, double)> &fn)
{
    for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
        fn(g.coords[i], g.coords[i + 1]);
    for (const OgrGeometry &p : g.parts)
        clipVertexWalk(p, fn);
}

// feature envelope with the spatial filter's NaN-skipping semantics
bool clipFeatureEnvelope(const OgrGeometry &g, double env[4])
{
    bool any = false;
    clipVertexWalk(g,
                   [&](double x, double y)
                   {
                       if (std::isnan(x) || std::isnan(y))
                           return;
                       if (!any || x < env[0])
                           env[0] = x;
                       if (!any || y < env[1])
                           env[1] = y;
                       if (!any || x > env[2])
                           env[2] = x;
                       if (!any || y > env[3])
                           env[3] = y;
                       any = true;
                   });
    return any;
}

// vertex-accurate envelope of the clip geometry in the layer's CRS (the
// reference transforms the geometry itself, then takes its envelope)
bool clipGeomEnvTransformed(const OgrGeometry &g, const Srs &src,
                            const Srs &dst, double env[4])
{
    PJ *raw = proj_create_crs_to_crs_from_pj(projCtx(), src.pj(), dst.pj(),
                                             nullptr, nullptr);
    if (!raw)
        return false;
    PJ *op = proj_normalize_for_visualization(projCtx(), raw);
    proj_destroy(raw);
    if (!op)
        return false;
    bool any = false;
    clipVertexWalk(g,
                   [&](double x, double y)
                   {
                       PJ_COORD c = proj_coord(x, y, 0, 0);
                       c = proj_trans(op, PJ_FWD, c);
                       double ox = c.xyzt.x, oy = c.xyzt.y;
                       if (!std::isfinite(ox) || !std::isfinite(oy))
                           return;
                       if (!any || ox < env[0])
                           env[0] = ox;
                       if (!any || oy < env[1])
                           env[1] = oy;
                       if (!any || ox > env[2])
                           env[2] = ox;
                       if (!any || oy > env[3])
                           env[3] = oy;
                       any = true;
                   });
    proj_destroy(op);
    return any;
}

// resolve --like bounds; 0 = bounds set (raster template), nonzero =
// reported failure. Vector templates never succeed in this build: the
// GEOS validity pre-check fails on the first polygonal geometry and
// anything else ends in "No clipping geometry found".
int clipLikeResolve(const VectorClipOpts &o, double env[4], bool &haveEnv,
                    Srs &clipSrs, bool &haveSrs)
{
    std::string err;
    cplPushQuietHandler();
    auto r = openRaster(o.like, err);
    cplPopHandler();
    if (r)
    {
        if (!r->hasGT)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Dataset '" + o.like +
                            "' has no geotransform matrix. Its bounds "
                            "cannot be established.");
            return 1;
        }
        double xs[4], ys[4];
        int k = 0;
        for (int j = 0; j <= 1; j++)
            for (int i = 0; i <= 1; i++)
            {
                double px = i ? r->width : 0, py = j ? r->height : 0;
                xs[k] = r->gt[0] + px * r->gt[1] + py * r->gt[2];
                ys[k] = r->gt[3] + px * r->gt[4] + py * r->gt[5];
                ++k;
            }
        env[0] = env[2] = xs[0];
        env[1] = env[3] = ys[0];
        for (int i = 1; i < 4; i++)
        {
            env[0] = std::min(env[0], xs[i]);
            env[2] = std::max(env[2], xs[i]);
            env[1] = std::min(env[1], ys[i]);
            env[3] = std::max(env[3], ys[i]);
        }
        haveEnv = true;
        if (r->hasSrs && r->srs.valid())
        {
            clipSrs = std::move(r->srs);
            haveSrs = true;
        }
        else
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "clip: Dataset '" + o.like +
                            "' has no CRS. Assuming its CRS is the same "
                            "as the input vector.");
        return 0;
    }
    std::string verr;
    cplPushQuietHandler();
    auto v = openVectorDataset(o.like, verr, {}, {}, false);
    cplPopHandler();
    if (!v)
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "`" + o.like +
                        "' not recognized as being in a supported file "
                        "format.");
        return 1;
    }
    OgrLayer sqlResult;
    OgrLayer *lyr = nullptr;
    if (!o.likeSql.empty())
    {
        auto res = ogrExecuteSql(*v, o.likeSql);
        if (!res)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Failed to identify source layer from "
                        "clipping dataset.");
            return 1;
        }
        sqlResult = std::move(*res);
        lyr = &sqlResult;
    }
    else if (!o.likeLayer.empty())
    {
        for (auto &l : v->layers)
            if (l.name == o.likeLayer)
            {
                lyr = &l;
                break;
            }
        if (!lyr)
            for (auto &l : v->layers)
                if (strEqualNoCase(l.name, o.likeLayer))
                {
                    lyr = &l;
                    break;
                }
        if (!lyr)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Failed to identify source layer from "
                        "clipping dataset.");
            return 1;
        }
    }
    else if (v->layers.size() == 1)
        lyr = &v->layers[0];
    else
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "clip: Only single layer dataset can be specified "
                    "with --like when neither --like-layer or --like-sql "
                    "have been specified");
        return 1;
    }
    // a failing attribute filter reports its cause and stays unset
    if (!o.likeWhere.empty())
        ogrApplyAttributeFilter(*lyr, o.likeWhere, false);
    bool warned = false;
    for (const OgrFeature &f : lyr->features)
    {
        if (!f.hasGeom)
            continue;
        if (clipPolygonal(f.geom))
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "GEOS support not enabled.");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("clip: Geometry of feature %lld of %s "
                                  "is invalid. You may be able to correct "
                                  "it with 'gdal vector geom make-valid'.",
                                  f.fid, o.like.c_str()));
            return 1;
        }
        if (!warned)
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Non-polygonal geometry encountered in clipping "
                        "dataset will be ignored. Further messages of "
                        "this type will be suppressed.");
            warned = true;
        }
    }
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                "clip: No clipping geometry found");
    return 1;
}

}  // namespace

int vectorClipApplyStep(OgrDataset &d, const VectorClipOpts &o,
                        const std::vector<std::string> &layerSel)
{
    double env[4] = {0, 0, 0, 0};
    bool haveEnv = false;
    Srs clipSrs;
    bool haveSrs = false;
    OgrGeometry clipGeom;
    bool haveGeom = false;

    if (!o.hasBbox && o.geometry.empty() && !o.hasLike)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "clip: --bbox, --geometry or --like must be "
                    "specified");
        return 1;
    }
    if (o.hasBbox)
    {
        for (int i = 0; i < 4; ++i)
            env[i] = o.bbox[i];
        haveEnv = true;
        if (!o.bboxCrs.empty())
        {
            bool ok = false;
            clipSrs = Srs::fromCliInput(o.bboxCrs, ok);
            haveSrs = ok;
        }
    }
    else if (!o.geometry.empty())
    {
        if (!clipGeometryParseText(o.geometry, clipGeom))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clip: Clipping geometry is neither a valid WKT "
                        "or GeoJSON geometry");
            return 1;
        }
        haveEnv = clipGeometryEnvelope(clipGeom, env[0], env[1], env[2],
                                       env[3]);
        haveGeom = haveEnv;
        if (!o.geometryCrs.empty())
        {
            bool ok = false;
            clipSrs = Srs::fromCliInput(o.geometryCrs, ok);
            haveSrs = ok;
        }
    }
    else if (clipLikeResolve(o, env, haveEnv, clipSrs, haveSrs))
        return 1;

    for (OgrLayer &l : d.layers)
    {
        if (!vvLayerSelected(layerSel, o.activeLayer, l))
            continue;
        if (!l.hasGeomField)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot set spatial filter: no geometry field "
                        "present in layer.");
            l.features.clear();
            vectorLayerRecomputeExtent(l);
            continue;
        }
        double lenv[4] = {env[0], env[1], env[2], env[3]};
        bool lHave = haveEnv;
        if (haveEnv && haveSrs && clipSrs.valid() && l.hasSrs &&
            l.srs.valid() &&
            clipSrs.wkt2SingleLine() != l.srs.wkt2SingleLine())
        {
            if (haveGeom)
            {
                double tenv[4] = {0, 0, 0, 0};
                if (clipGeomEnvTransformed(clipGeom, clipSrs, l.srs, tenv))
                    for (int i = 0; i < 4; ++i)
                        lenv[i] = tenv[i];
            }
            else
            {
                double x0 = lenv[0], y0 = lenv[1], x1 = lenv[2],
                       y1 = lenv[3];
                if (clipSrs.transformBoundsTo(l.srs, x0, y0, x1, y1))
                {
                    lenv[0] = x0;
                    lenv[1] = y0;
                    lenv[2] = x1;
                    lenv[3] = y1;
                }
            }
        }
        ConvertClipPending::L pend;
        pend.layer = l.name;
        for (const OgrFeature &f : l.features)
        {
            if (!f.hasGeom)
                continue;
            double fe[4] = {0, 0, 0, 0};
            if (!clipFeatureEnvelope(f.geom, fe))
                continue;
            if (!lHave)
                continue;
            if (fe[0] <= lenv[2] && fe[2] >= lenv[0] && fe[1] <= lenv[3] &&
                fe[3] >= lenv[1])
                pend.errors.push_back({6, "GEOS support not enabled."});
        }
        l.features.clear();
        vectorLayerRecomputeExtent(l);
        if (pend.errors.empty())
            continue;
        pend.fail = true;
        if (g_vectorClipEmitPulls > 0)
            for (int k = 0; k < g_vectorClipEmitPulls; ++k)
                convertClipEmitLayerErrors(pend);
        else if (g_vectorClipEmitPulls == 0)
        {
            g_convertClipPending.active = true;
            g_convertClipPending.layers.push_back(std::move(pend));
        }
    }
    return 0;
}

namespace
{

int vectorClipHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");

    std::string driver;
    if (int rc = vvResolveVerbFormats(cmd, r, driver))
        return rc;
    if (r.str("output-format").empty() &&
        strEndsWith(strToLower(output), ".gdalg.json"))
        driver = "GDALG";

    std::unique_ptr<OgrDataset> ds;
    if (int rc = vvOpenInputDs(cmd, r, input, ds))
        return rc;

    VectorClipOpts o;
    o.hasBbox = r.get("bbox") != nullptr;
    if (o.hasBbox)
    {
        std::vector<std::string> parts;
        for (const auto &raw : r.list("bbox"))
            for (const auto &p : strSplit(raw, ','))
                parts.push_back(p);
        for (size_t i = 0; i < 4 && i < parts.size(); ++i)
            o.bbox[i] = strtod(parts[i].c_str(), nullptr);
    }
    o.bboxCrs = r.str("bbox-crs");
    o.geometry = r.str("geometry");
    o.geometryCrs = r.str("geometry-crs");
    o.like = r.str("like");
    o.hasLike = !o.like.empty();
    o.likeSql = r.str("like-sql");
    o.likeLayer = r.str("like-layer");
    o.likeWhere = r.str("like-where");
    o.activeLayer = r.str("active-layer");

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!o.activeLayer.empty())
        cli += " --active-layer " + vvGq(o.activeLayer);
    if (o.hasBbox)
    {
        std::string joined;
        for (const auto &raw : r.list("bbox"))
            for (const auto &p : strSplit(raw, ','))
            {
                if (!joined.empty())
                    joined += ",";
                joined += vvFmtReal(p);
            }
        cli += " --bbox " + joined;
    }
    if (!o.bboxCrs.empty())
        cli += " --bbox-crs " + vvGq(o.bboxCrs);
    if (!o.geometry.empty())
        cli += " --geometry " + vvGq(o.geometry);
    if (!o.geometryCrs.empty())
        cli += " --geometry-crs " + vvGq(o.geometryCrs);
    if (o.hasLike)
        cli += " --like " + vvGq(o.like);
    if (!o.likeSql.empty())
        cli += " --like-sql " + vvGq(o.likeSql);
    if (!o.likeLayer.empty())
        cli += " --like-layer " + vvGq(o.likeLayer);
    if (!o.likeWhere.empty())
        cli += " --like-where " + vvGq(o.likeWhere);
    cli += " --output-format stream --output streamed_dataset";

    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [o, sel](OgrDataset &d) -> int
    { return vectorClipApplyStep(d, o, sel); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

}  // namespace

void registerVectorClipHandler()
{
    registerHandler("vector_clip", vectorClipHandler);
    registerArgCheck(
        "vector_clip",
        [](const std::string &a, ParseResult &r) -> int
        {
            if (a == "bbox")
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
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "Value of 'bbox' should be xmin,ymin,xmax,"
                            "ymax with xmin <= xmax and ymin <= ymax");
                        handlerPrintUsage();
                        return 1;
                    }
                }
            }
            return vvVerbFormatArgCheck("clip", a, r);
        });
    registerArgValueCheck(
        "vector_clip",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "bbox-crs" || argName == "geometry-crs")
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return "Invalid value for '" + argName + "' argument";
            }
            return "";
        });
}
