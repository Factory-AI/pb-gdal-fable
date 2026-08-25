#include "engine.h"
#include "cpl.h"
#include "dataset.h"
#include "ogr.h"
#include "rasterpolyfoot.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver);
bool rpfFileExists(const std::string &path);
bool rpfLcoSupported(const std::string &driver, const std::string &key);

namespace
{

void forceArg(ParseResult &r, const std::string &name,
              const std::string &val)
{
    auto it = r.byName.find(name);
    if (it == r.byName.end())
        return;
    it->second.values.assign(1, val);
    it->second.set = true;
}

void clearArg(ParseResult &r, const std::string &name)
{
    auto it = r.byName.find(name);
    if (it == r.byName.end())
        return;
    it->second.values.clear();
    it->second.set = false;
}

double ringArea(const OgrGeometry &ring)
{
    double s = 0;
    size_t nPts = ring.coords.size() / 3;
    for (size_t i = 0; i + 1 < nPts; i++)
    {
        double x0 = ring.coords[i * 3], y0 = ring.coords[i * 3 + 1];
        double x1 = ring.coords[(i + 1) * 3],
               y1 = ring.coords[(i + 1) * 3 + 1];
        s += x0 * y1 - x1 * y0;
    }
    return std::fabs(s) / 2.0;
}

long long geomPointCount(const OgrGeometry &g)
{
    long long c = 0;
    if (g.type == 3)
    {
        for (const auto &ring : g.parts)
            if (ring.coords.size() >= 3)
                c += (long long)(ring.coords.size() / 3) - 1;
    }
    else
        for (const auto &p : g.parts)
            c += geomPointCount(p);
    return c;
}

bool footprintPostValidator(const CmdSpec &cmd, ParseResult &r,
                            bool inputOpenFailed)
{
    bool bad = false;
    std::string output = r.str("output");
    std::string of = r.str("output-format");
    bool fOw = r.flag("overwrite");
    bool fApp = r.flag("append");
    bool fUpd = r.flag("update");
    bool fam = fApp || fUpd;
    bool skipOut = strEqualNoCase(of, "MEM") ||
                   strEqualNoCase(of, "Memory") ||
                   strEqualNoCase(of, "stream") ||
                   strEqualNoCase(of, "GDALG");
    if (!skipOut && !output.empty())
    {
        bool exists = rpfFileExists(output);
        if (exists && !fOw && !fam)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + outputExistsKind(output) + " '" +
                            output +
                            "' already exists. You may specify the "
                            "--overwrite/--append/--update option.");
            bad = true;
        }
        else if (fUpd && !fApp)
        {
            if (!exists)
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            datasetMissingMessage(output));
                bad = true;
            }
            else if (outputExistsKind(output) == "Directory")
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            output + ": Is a directory");
                bad = true;
            }
            else
            {
                std::string oerr;
                cplPushQuietHandler();
                auto probe = openVectorDataset(output, oerr, {});
                cplPopHandler();
                if (!probe)
                {
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + output +
                                    "' not recognized as being in a "
                                    "supported file format.");
                    bad = true;
                }
            }
        }
    }
    // mutual exclusions surface between the output checks and the GDALG
    // refusal; the engine's later loop is silenced by unsetting the
    // reporting argument
    for (size_t ai = 0; ai < cmd.args.size(); ai++)
    {
        const ArgSpec &a = cmd.args[ai];
        ArgValue &av = r.byName[a.name];
        if (!av.set || a.mutex.empty())
            continue;
        for (size_t bi = 0; bi < ai; bi++)
        {
            const ArgSpec &b = cmd.args[bi];
            if (b.mutex == a.mutex && r.byName[b.name].set)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            cmd.name + ": Argument '" + a.name +
                                "' is mutually exclusive with '" +
                                b.name + "'.");
                av.set = false;
                bad = true;
                break;
            }
        }
    }
    if (of.empty() && strEndsWith(strToLower(output), ".gdalg.json"))
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "footprint: GDALG output is not supported");
        bad = true;
    }
    if (!inputOpenFailed)
    {
        std::vector<std::string> inputs = r.list("input");
        std::unique_ptr<RasterDatasetBase> ds;
        if (!inputs.empty())
        {
            std::string derr;
            cplPushQuietHandler();
            ds = openRaster(inputs[0], derr);
            cplPopHandler();
        }
        if (ds)
        {
            for (const auto &bs : r.list("band"))
            {
                int b = atoi(bs.c_str());
                if (b > (int)ds->bands.size())
                {
                    cplErrorStr(
                        CE_Failure, CPLE_AppDefined,
                        strPrintf("%s: Value of 'band' should be greater "
                                  "or equal than 1 and less or equal "
                                  "than %d.",
                                  cmd.name.c_str(),
                                  (int)ds->bands.size()));
                    bad = true;
                    break;
                }
            }
            const ArgValue *ov = r.get("overview");
            if (ov && ov->set)
            {
                int idx = atoi(ov->str().c_str());
                int n = (int)ds->dispOverviews().size();
                if (n == 0)
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                cmd.name +
                                    ": Source dataset has no overviews. "
                                    "Argument 'overview' should not be "
                                    "specified.");
                    bad = true;
                }
                else if (idx >= n)
                {
                    cplErrorStr(
                        CE_Failure, CPLE_IllegalArg,
                        strPrintf("%s: Source dataset has only %d "
                                  "overview levels. 'overview' value "
                                  "should be strictly lower than this "
                                  "number.",
                                  cmd.name.c_str(), n));
                    bad = true;
                }
            }
        }
    }
    return bad;
}

int rasterFootprintHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    std::string layerName = r.str("output-layer");
    if (layerName.empty())
        layerName = "footprint";

    std::string driver;
    vectorOutputDriverResolve(r.str("output-format"), driver);
    if (driver.empty())
    {
        if (!strEndsWith(strToLower(output), ".gdalg.json"))
            driver = rpfGuessDriver(output);
        if (driver.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot guess driver for " + output);
            return 1;
        }
    }

    bool fApp = r.flag("append") || r.flag("update");
    bool appendToExisting = false;
    bool seqAppend = false;
    bool seqRewrite = false;
    OgrLayer oldLayer;
    if (fApp && rpfFileExists(output))
    {
        const ArgValue *ofArg = r.get("output-format");
        if (ofArg && ofArg->set)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "hDstDS != NULL but options that imply creating "
                        "a new dataset have been set.");
            return 1;
        }
        std::string terr;
        auto tds = openVectorDataset(output, terr, {});
        if (tds)
        {
            const OgrLayer *fl = nullptr;
            for (const auto &l : tds->layers)
                if (l.name == layerName)
                    fl = &l;
            if (!fl)
            {
                // the reference crashes after the read-only CreateLayer
                // refusal (SIGSEGV parity)
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "GeoJSON driver doesn't support creating a "
                            "layer on a read-only datasource");
                fflush(nullptr);
                raise(SIGSEGV);
            }
            appendToExisting = true;
            if (tds->driverShort == "GeoJSONSeq")
            {
                // multi-feature sequence: the layer claims CRS84 and the
                // new features are appended as further lines
                seqAppend = true;
                driver = "GeoJSONSeq";
            }
            else if (tds->driverShort == "GeoJSON" &&
                     !rpfGjRootIsCollection(output))
            {
                // single-feature sequence opens through the GeoJSON
                // driver: the whole file is rewritten as a pretty
                // collection under the assumed CRS84 layer CRS
                seqAppend = true;
                seqRewrite = true;
                oldLayer = *fl;
            }
        }
    }

    std::string derr;
    auto ds = openRaster(input, derr);
    if (!ds)
    {
        if (derr == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(input));
        else if (derr != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        return 1;
    }
    if (g_pipelineTailMaterialize && g_pipelineTailMaterialize(ds))
        return 1;

    int W = ds->width, H = ds->height;
    double gt[6];
    memcpy(gt, ds->gt, sizeof(gt));
    bool hasGT = ds->hasGT;
    std::unique_ptr<RasterDatasetBase> ov;
    RasterDatasetBase *src = ds.get();
    const ArgValue *ovArg = r.get("overview");
    if (ovArg && ovArg->set)
    {
        int idx = atoi(ovArg->str().c_str());
        const auto &ovs = ds->dispOverviews();
        if (idx >= 0 && idx < (int)ovs.size())
        {
            ov = ds->openOverviewEntry(ovs[idx]);
            if (ov)
            {
                src = ov.get();
                gt[1] *= (double)W / src->width;
                gt[4] *= (double)W / src->width;
                gt[2] *= (double)H / src->height;
                gt[5] *= (double)H / src->height;
                W = src->width;
                H = src->height;
            }
        }
    }

    std::vector<int> bands;
    for (const auto &bs : r.list("band"))
        bands.push_back(atoi(bs.c_str()));
    if (bands.empty())
        for (size_t i = 0; i < ds->bands.size(); i++)
            bands.push_back((int)i + 1);
    std::vector<double> snd;
    for (const auto &vs : r.list("src-nodata"))
        snd.push_back(atof(vs.c_str()));

    std::string cs = r.str("coordinate-system");
    // without a geotransform an SRS still selects georeferenced mode,
    // mapping through the identity transform
    bool georef = cs.empty() ? (hasGT || ds->hasSrs)
                             : strEqualNoCase(cs, "georeferenced");
    if (!hasGT)
    {
        static const double ident[6] = {0, 1, 0, 0, 0, 1};
        memcpy(gt, ident, sizeof(gt));
    }

    bool hasDst = false;
    Srs dstSrs;
    std::string dstDef = r.str("dst-crs");
    if (!dstDef.empty())
    {
        bool ok = false;
        dstSrs = Srs::fromCliInput(dstDef, ok);
        hasDst = ok;
    }

    // output layer setup runs before the option validation: the layer
    // SRS is the dst CRS, else the source SRS in georeferenced mode
    bool lyrHasSrs = false;
    Srs lyrSrs;
    if (hasDst)
    {
        lyrHasSrs = true;
        lyrSrs = dstSrs;
    }
    else if (georef && ds->hasSrs)
    {
        lyrHasSrs = true;
        lyrSrs = ds->srs;
    }
    bool seqFresh = driver == "GeoJSONSeq" && !appendToExisting;
    bool warnNoSrs = false;
    if (!lyrHasSrs && !appendToExisting)
    {
        warnNoSrs = driver == "GeoJSONSeq";
        if (driver == "GeoJSON")
            for (const auto &kv : r.list("layer-creation-option"))
            {
                size_t eq = kv.find('=');
                std::string key = kv.substr(0, eq);
                std::string val =
                    eq == std::string::npos ? "" : kv.substr(eq + 1);
                bool truthy = !(strEqualNoCase(val, "NO") ||
                                strEqualNoCase(val, "FALSE") ||
                                strEqualNoCase(val, "OFF") || val == "0");
                if (strEqualNoCase(key, "RFC7946") && truthy)
                    warnNoSrs = true;
            }
    }
    if (warnNoSrs)
    {
        // CreateLayer's co/lco validation warnings precede the
        // assumed-CRS note; pre-run them and strip the delegated copies
        if (driver == "GeoJSON")
        {
            for (const auto &kv : r.list("creation-option"))
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "driver GeoJSON does not support creation "
                            "option " +
                                kv.substr(0, kv.find('=')));
            clearArg(r, "creation-option");
        }
        std::vector<std::string> keep;
        for (const auto &kv : r.list("layer-creation-option"))
        {
            std::string key = kv.substr(0, kv.find('='));
            if (rpfLcoSupported(driver, key))
                keep.push_back(kv);
            else
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "dataset " + output +
                                " does not support layer creation "
                                "option " +
                                key);
        }
        auto it = r.byName.find("layer-creation-option");
        if (it != r.byName.end())
        {
            it->second.values = keep;
            it->second.set = !keep.empty();
        }
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "No SRS set on layer. Assuming it is long/lat on "
                    "WGS84 ellipsoid");
    }
    // the RFC7946 sequence writer always claims a layer CRS: an
    // ungeoreferenced source cannot feed it, a georeferenced one gets
    // transformed here (and again by the writer when the layer keeps
    // the source SRS)
    bool preErr = false;
    bool seqTransform = false;
    if (seqFresh || seqAppend)
    {
        if (!ds->hasSrs)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Output layer has CRS, but input is not "
                        "georeferenced");
            if (seqAppend)
                return 1;
            preErr = true;
        }
        else if (!hasDst)
            seqTransform = true;
    }
    else if (hasDst && !ds->hasSrs)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Output layer has CRS, but input is not "
                    "georeferenced");
        preErr = true;
    }

    if (!preErr && !snd.empty() && snd.size() != 1 &&
        snd.size() != bands.size())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Number of values in -srcnodata should be 1 or the "
                    "number of bands");
        preErr = true;
    }

    if (!preErr && strEqualNoCase(cs, "georeferenced") && !hasGT)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Georeferenced coordinates requested, but input "
                    "dataset has no geotransform.");
        preErr = true;
    }

    bool split = r.flag("split-multipolygons");
    bool rcErr = false;
    std::vector<OgrGeometry> parts;
    if (!preErr)
    {
        bool inter = strEqualNoCase(r.str("combine-bands"),
                                    "intersection");
        size_t n = (size_t)W * H;
        std::vector<char> mask(n, inter ? 1 : 0);
        for (size_t bi = 0; bi < bands.size(); bi++)
        {
            const Band &b = ds->bands[bands[bi] - 1];
            std::vector<double> vals;
            if (!src->readBand(bands[bi], vals))
                return 1;
            bool hasNd;
            double nd = 0;
            if (!snd.empty())
            {
                hasNd = true;
                nd = snd.size() == 1 ? snd[0] : snd[bi];
            }
            else
            {
                hasNd = b.hasNodata;
                nd = b.nodataIsI64 ? (double)b.nodataI64
                     : b.nodataIsU64
                         ? (double)b.nodataU64
                         : b.nodata;
            }
            for (size_t i = 0; i < n; i++)
            {
                bool valid =
                    !hasNd || !(vals[i] == nd || (std::isnan(nd) &&
                                                  std::isnan(vals[i])));
                if (inter)
                    mask[i] = mask[i] && valid;
                else
                    mask[i] = mask[i] || valid;
            }
        }
        std::vector<long long> buf(n);
        for (size_t i = 0; i < n; i++)
            buf[i] = mask[i] ? 255 : kRpfMarkerInt;
        std::vector<int> canon;
        rpfLabel(buf, W, H, false, RpfEqInt(), canon);
        for (const auto &reg : rpfRegionOrder(canon, W, H))
        {
            if (buf[reg.lastIdx] == kRpfMarkerInt)
                continue;
            OgrGeometry poly;
            poly.type = 3;
            for (const auto &ring : rpfTraceRegion(canon, W, H, reg.id))
            {
                OgrGeometry rg;
                rg.type = 2;
                for (const auto &pt : ring)
                {
                    double cx = pt.first, cy = pt.second;
                    if (georef)
                    {
                        rg.coords.push_back(gt[0] + cx * gt[1] +
                                            cy * gt[2]);
                        rg.coords.push_back(gt[3] + cx * gt[4] +
                                            cy * gt[5]);
                    }
                    else
                    {
                        rg.coords.push_back(cx);
                        rg.coords.push_back(cy);
                    }
                    rg.coords.push_back(0.0);
                }
                poly.parts.push_back(std::move(rg));
            }
            parts.push_back(std::move(poly));
        }
        if (!parts.empty())
        {
            if (r.flag("convex-hull"))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "GEOS support not enabled.");
                parts.clear();
            }
            const ArgValue *dd = r.get("densify-distance");
            if (!parts.empty() && dd && dd->set)
            {
                double dist = atof(dd->str().c_str());
                for (auto &p : parts)
                    ogrSegmentize(p, dist);
            }
            const ArgValue *st = r.get("simplify-tolerance");
            if (!parts.empty() && st && st->set)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "GEOS support not enabled.");
                parts.clear();
                rcErr = true;
            }
            const ArgValue *mra = r.get("min-ring-area");
            if (!parts.empty() && mra && mra->set)
            {
                double minArea = atof(mra->str().c_str());
                std::vector<OgrGeometry> kept;
                for (auto &p : parts)
                {
                    if (p.parts.empty() ||
                        ringArea(p.parts[0]) < minArea)
                        continue;
                    OgrGeometry np;
                    np.type = 3;
                    np.parts.push_back(std::move(p.parts[0]));
                    for (size_t ri = 1; ri < p.parts.size(); ri++)
                        if (ringArea(p.parts[ri]) >= minArea)
                            np.parts.push_back(std::move(p.parts[ri]));
                    kept.push_back(std::move(np));
                }
                parts = std::move(kept);
            }
        }
    }

    // final geometries: one multipolygon, or one polygon per part
    std::vector<OgrGeometry> geoms;
    if (split)
        geoms = std::move(parts);
    else if (!parts.empty())
    {
        OgrGeometry mp;
        mp.type = 6;
        mp.parts = std::move(parts);
        geoms.push_back(std::move(mp));
    }

    long long maxPts = 100;
    std::string mps = r.str("max-points");
    if (strEqualNoCase(mps, "unlimited"))
        maxPts = -1;
    else if (!mps.empty())
        maxPts = atoll(mps.c_str());
    if (maxPts > 0)
    {
        std::vector<OgrGeometry> kept;
        for (auto &g : geoms)
        {
            if (geomPointCount(g) > maxPts)
            {
                // the reference's simplification loop needs GEOS: 20
                // attempts then the re-raised failure; the feature is
                // dropped
                for (int i = 0; i < 20; i++)
                    cplErrorStr(CE_Failure, CPLE_NotSupported,
                                "GEOS support not enabled.");
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "GEOS support not enabled.");
                rcErr = true;
                continue;
            }
            kept.push_back(std::move(g));
        }
        geoms = std::move(kept);
    }

    void *crsOp = nullptr;
    if (hasDst && ds->hasSrs && !preErr)
        crsOp = vectorCrsOpCreate(ds->srs, dstSrs);
    else if (seqTransform && !preErr)
    {
        bool ok = false;
        Srs s84 = Srs::fromCliInput("EPSG:4326", ok);
        if (ok)
            crsOp = vectorCrsOpCreate(ds->srs, s84);
    }
    if (crsOp)
    {
        for (auto &g : geoms)
            vectorCrsOpApply(crsOp, g);
        vectorCrsOpFree(crsOp);
    }

    OgrLayer lyr;
    lyr.name = layerName;
    lyr.geomType = split ? 3 : 6;
    if (lyrHasSrs)
    {
        lyr.hasSrs = true;
        lyr.srs = lyrSrs;
    }
    bool noLoc = r.flag("no-location-field");
    std::string locName = r.str("location-field");
    if (locName.empty())
        locName = "location";
    if (!noLoc)
    {
        OgrFieldDefn fd;
        fd.name = locName;
        fd.type = OFTString;
        lyr.fields.push_back(fd);
    }
    std::string locVal = input;
    if (r.flag("absolute-path") && !input.empty() && input[0] != '/')
    {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)))
            locVal = std::string(cwd) + "/" + input;
    }
    for (auto &g : geoms)
    {
        OgrFeature f;
        f.hasGeom = true;
        f.geom = std::move(g);
        if (!noLoc)
        {
            OgrFieldValue v;
            v.set = true;
            v.v.type = JVal::STRING;
            v.v.s = locVal;
            f.values.push_back(v);
        }
        lyr.features.push_back(std::move(f));
    }
    ds.reset();
    ov.reset();

    if (seqRewrite)
    {
        // the sequence file is rewritten as a standard GeoJSON
        // collection: existing features first, then the new ones mapped
        // into the existing schema, under the assumed CRS84 layer CRS
        OgrLayer merged = oldLayer;
        merged.name = layerName;
        bool ok84 = false;
        Srs s84 = Srs::fromCliInput("EPSG:4326", ok84);
        if (ok84)
        {
            merged.hasSrs = true;
            merged.srs = s84;
        }
        int locIdx = -1;
        if (!noLoc)
            for (size_t i = 0; i < merged.fields.size(); i++)
                if (merged.fields[i].name == locName)
                    locIdx = (int)i;
        for (auto &nf : lyr.features)
        {
            OgrFeature mf;
            mf.hasGeom = nf.hasGeom;
            mf.geom = std::move(nf.geom);
            mf.values.resize(merged.fields.size());
            if (locIdx >= 0 && !nf.values.empty())
                mf.values[locIdx] = nf.values[0];
            merged.features.push_back(std::move(mf));
        }
        auto sds = std::make_unique<OgrDataset>();
        sds->path = input;
        sds->driverShort = "MEM";
        sds->driverLong = "In Memory raster, vector and multidimensional "
                          "raster";
        sds->layers.push_back(std::move(merged));
        forceArg(r, "output-format", "GeoJSON");
        forceArg(r, "overwrite", "true");
        forceArg(r, "output-layer", layerName);
        clearArg(r, "append");
        clearArg(r, "update");
        int rc = vvDelegateVerb(r, "footprint", std::move(sds), "",
                                "GeoJSON", true, nullptr);
        if (rcErr)
            return 1;
        return rc;
    }

    auto ods = std::make_unique<OgrDataset>();
    ods->path = input;
    ods->driverShort = "MEM";
    ods->driverLong = "In Memory raster, vector and multidimensional "
                      "raster";
    ods->layers.push_back(std::move(lyr));

    forceArg(r, "output-format", driver);
    if (fApp)
    {
        forceArg(r, "append", "true");
        clearArg(r, "update");
    }
    if (preErr)
        forceArg(r, "quiet", "true");
    int rc = vvDelegateVerb(r, "footprint", std::move(ods), "", driver,
                            true, nullptr);
    if (preErr || rcErr)
        return 1;
    return rc;
}

}  // namespace

void registerRasterFootprintHandler()
{
    registerHandler("raster_footprint", rasterFootprintHandler);
    // the reference accepts a hidden --update flag (absent from help)
    {
        Spec &spec = Spec::instance();
        auto it = spec.cmds.find("raster_footprint");
        if (it != spec.cmds.end() && !it->second.findByName("update"))
        {
            ArgSpec u;
            u.name = "update";
            u.aliases = {"update"};
            u.type = "boolean";
            u.kind = "input_arguments";
            u.category = "Base";
            u.section = "Options";
            u.display = "--update";
            u.description = "Whether to open existing dataset in update "
                            "mode";
            u.hasDefault = true;
            u.defValue = "false";
            it->second.args.push_back(std::move(u));
        }
    }
    registerArgValueCheck(
        "raster_footprint",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "output-format")
            {
                std::string drv;
                std::string err = vectorOutputDriverResolve(value, drv);
                if (!err.empty())
                    return err;
                if (drv == "stream")
                    return "Invalid value for argument 'output-format'. "
                           "Driver 'stream' does not exist.";
                // --of GDALG is refused while the value parses, before
                // any dataset is opened
                if (strEqualNoCase(value, "GDALG"))
                    return "\x07GDALG output is not supported.";
                return "";
            }
            if (argName == "max-points")
            {
                if (strEqualNoCase(value, "unlimited"))
                    return "";
                char *end = nullptr;
                long long v = strtoll(value.c_str(), &end, 10);
                if (value.empty() || end == nullptr || *end != '\0' ||
                    v < 4)
                    return "\x05Value of 'max-points' should be a "
                           "positive integer greater or equal to 4, or "
                           "'unlimited'";
                return "";
            }
            return "";
        });
    registerPreValidator(
        "raster_footprint",
        [](const CmdSpec &, ParseResult &r) -> int
        {
            for (const auto &bs : r.list("band"))
                if (atoi(bs.c_str()) < 1)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Value of 'band' should greater or equal "
                                "to 1.");
                    handlerPrintUsage();
                    return 1;
                }
            // --append re-opens the single input while wiring the
            // output: the open failure is reported twice and the null
            // source is then surfaced, with no usage block
            if (r.flag("append"))
            {
                std::vector<std::string> inputs = r.list("input");
                if (inputs.size() == 1 && !inputs[0].empty() &&
                    inputs[0].rfind("GTIFF_DIR:", 0) != 0)
                {
                    const std::string &in = inputs[0];
                    std::string openErr;
                    if (!rpfFileExists(in))
                        openErr = datasetMissingMessage(in);
                    else if (!datasetIdentify(in, {"raster"}))
                        openErr = "`" + in +
                                  "' not recognized as being in a "
                                  "supported file format.";
                    if (!openErr.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                        cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "hSrcDataset== NULL");
                        return 1;
                    }
                }
            }
            return 0;
        });
    registerPostValidator("raster_footprint", footprintPostValidator);
}
