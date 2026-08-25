#include "engine.h"
#include "cpl.h"
#include "dataset.h"
#include "ogr.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver);
bool rpfLcoSupported(const std::string &driver, const std::string &key);
bool rpfFileExists(const std::string &path);
std::string rpfGuessDriver(const std::string &output);

namespace
{

void asfForceStrip(ParseResult &r, const std::string &name,
                   const std::vector<std::string> &values)
{
    auto it = r.byName.find(name);
    if (it == r.byName.end())
        return;
    it->second.values = values;
    it->second.set = !values.empty();
}

// mirrors the convert-output validation: exists refusal, update-family
// probing, then the band range check against the opened input
bool asFeaturesPostValidator(const CmdSpec &cmd, ParseResult &r,
                             bool inputOpenFailed)
{
    bool bad = false;
    std::string output = r.str("output");
    std::string of = r.str("output-format");
    bool fOw = r.flag("overwrite");
    bool fApp = r.flag("append");
    bool fUpd = r.flag("update");
    bool fOwl = r.flag("overwrite-layer");
    bool fam = fApp || fUpd || fOwl;
    bool skipOut = strEqualNoCase(of, "MEM") ||
                   strEqualNoCase(of, "Memory") ||
                   strEqualNoCase(of, "stream");
    if (!skipOut && !output.empty())
    {
        bool exists = rpfFileExists(output);
        if (exists && !fOw && !fam)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + outputExistsKind(output) + " '" +
                            output +
                            "' already exists. You may specify the "
                            "--overwrite/--overwrite-layer/--append/"
                            "--update option.");
            bad = true;
        }
        else if ((fUpd || fOwl) && !fApp)
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
    if (!inputOpenFailed)
    {
        std::vector<std::string> bands = r.list("band");
        std::vector<std::string> inputs = r.list("input");
        if (!bands.empty() && !inputs.empty())
        {
            std::string derr;
            cplPushQuietHandler();
            auto ds = openRaster(inputs[0], derr);
            cplPopHandler();
            if (ds)
                for (const auto &bs : bands)
                    if (atoi(bs.c_str()) > (int)ds->bands.size())
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            strPrintf("%s: Value of 'band' should be "
                                      "greater or equal than 1 and less "
                                      "or equal than %d.",
                                      cmd.name.c_str(),
                                      (int)ds->bands.size()));
                        bad = true;
                        break;
                    }
        }
    }
    return bad;
}

int rasterAsFeaturesHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    std::string layerName = r.str("output-layer");
    if (layerName.empty())
        layerName = "pixels";
    std::string geomType = r.str("geometry-type");
    if (geomType.empty())
        geomType = "none";
    bool wantPoint = strEqualNoCase(geomType, "point");
    bool wantPoly = strEqualNoCase(geomType, "polygon");
    bool includeXY = r.flag("include-xy");
    bool includeRC = r.flag("include-row-col");
    bool skipNodata = r.flag("skip-nodata");

    std::string driver;
    vectorOutputDriverResolve(r.str("output-format"), driver);
    if (driver == "GDALG")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Unable to find driver `GDALG'.");
        return 1;
    }
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
    int w = ds->width, h = ds->height;
    int nb = (int)ds->bands.size();

    double gt[6] = {0, 1, 0, 0, 0, 1};
    if (ds->hasGT)
        memcpy(gt, ds->gt, sizeof(gt));

    OgrLayer lyr;
    lyr.name = layerName;
    if (wantPoint)
        lyr.geomType = 1;
    else if (wantPoly)
        lyr.geomType = 3;
    else
    {
        lyr.geomType = 101;
        lyr.hasGeomField = false;
    }
    // the SRS is consumed (and its lazy decode diagnostics replayed)
    // only when the features carry geometry
    if ((wantPoint || wantPoly) && ds->hasSrs)
    {
        lyr.hasSrs = true;
        lyr.srs = ds->srs;
        ds->replaySrsDecodeWarnings();
    }
    if (includeXY)
    {
        OgrFieldDefn fd;
        fd.name = "CENTER_X";
        fd.type = OFTReal;
        lyr.fields.push_back(fd);
        fd.name = "CENTER_Y";
        lyr.fields.push_back(fd);
    }
    if (includeRC)
    {
        OgrFieldDefn fd;
        fd.name = "ROW";
        fd.type = OFTInteger;
        lyr.fields.push_back(fd);
        fd.name = "COL";
        lyr.fields.push_back(fd);
    }
    for (int b = 1; b <= nb; b++)
    {
        OgrFieldDefn fd;
        fd.name = strPrintf("BAND_%d", b);
        fd.type = OFTReal;
        lyr.fields.push_back(fd);
    }

    bool fApp = r.flag("append");
    bool fUpd = r.flag("update");
    bool fOwl = r.flag("overwrite-layer");
    // the sequence writer re-creates its layer even on the update-family
    // paths, so its assumed-CRS note fires regardless of those flags
    bool famExisting = (fApp || fUpd || fOwl) && rpfFileExists(output) &&
                       driver != "GeoJSONSeq";

    // GeoJSONSeq (and RFC7946 GeoJSON) layer creation announces the
    // assumed CRS when the layer carries none; CreateLayer's co/lco
    // validation warnings precede that note, so they are pre-run here
    // and stripped from the delegation
    if (!lyr.hasSrs && !famExisting)
    {
        bool warnSrs = driver == "GeoJSONSeq";
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
                    warnSrs = true;
            }
        if (warnSrs)
        {
            if (driver == "GeoJSON")
            {
                for (const auto &kv : r.list("creation-option"))
                    cplErrorStr(CE_Warning, CPLE_NotSupported,
                                "driver GeoJSON does not support creation "
                                "option " +
                                    kv.substr(0, kv.find('=')));
                asfForceStrip(r, "creation-option", {});
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
            asfForceStrip(r, "layer-creation-option", keep);
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "No SRS set on layer. Assuming it is long/lat on "
                        "WGS84 ellipsoid");
        }
    }

    // per-band sample decode into the double the Real field carries
    std::vector<std::vector<uint8_t>> raw((size_t)nb);
    std::vector<DType> types((size_t)nb);
    for (int b = 0; b < nb; b++)
    {
        if (!ds->readBandRaw(b + 1, raw[b]))
            return 1;
        types[b] = ds->bands[b].type;
    }
    auto sample = [&](int b, size_t idx) -> double
    {
        const uint8_t *p =
            &raw[b][idx * dtypeSizeBytes(types[b])];
        switch (types[b])
        {
            case DType::Byte:
                return *p;
            case DType::Int8:
                return *(const int8_t *)p;
            case DType::UInt16:
                return *(const uint16_t *)p;
            case DType::Int16:
            case DType::CInt16:
                return *(const int16_t *)p;
            case DType::UInt32:
                return *(const uint32_t *)p;
            case DType::Int32:
            case DType::CInt32:
                return *(const int32_t *)p;
            case DType::Int64:
                return (double)*(const int64_t *)p;
            case DType::UInt64:
                return (double)*(const uint64_t *)p;
            case DType::Float16:
                return tailHalfToFloat(*(const uint16_t *)p);
            case DType::Float32:
            case DType::CFloat32:
                return *(const float *)p;
            case DType::Float64:
            case DType::CFloat64:
                return *(const double *)p;
            default:
                return 0.0;
        }
    };

    const Band &b1 = ds->bands[0];
    bool ndSkip = skipNodata && b1.hasNodata;
    double nd = b1.nodata;

    long long fid = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            size_t idx = (size_t)y * w + x;
            if (ndSkip)
            {
                double v1 = sample(0, idx);
                if (v1 == nd || (std::isnan(nd) && std::isnan(v1)))
                    continue;
            }
            OgrFeature f;
            f.fid = fid++;
            double cx = gt[0] + (x + 0.5) * gt[1] + (y + 0.5) * gt[2];
            double cy = gt[3] + (x + 0.5) * gt[4] + (y + 0.5) * gt[5];
            if (includeXY)
            {
                OgrFieldValue v;
                v.set = true;
                v.v.type = JVal::DOUBLE;
                v.v.d = cx;
                f.values.push_back(v);
                v.v.d = cy;
                f.values.push_back(v);
            }
            if (includeRC)
            {
                OgrFieldValue v;
                v.set = true;
                v.v.type = JVal::INT;
                v.v.i = y;
                f.values.push_back(v);
                v.v.i = x;
                f.values.push_back(v);
            }
            for (int b = 0; b < nb; b++)
            {
                // non-finite Reals are the writers' concern: the GeoJSON
                // family skips them with its once-only warning, the DBF
                // writer formats them into the cell
                OgrFieldValue v;
                v.set = true;
                v.v.type = JVal::DOUBLE;
                v.v.d = sample(b, idx);
                f.values.push_back(v);
            }
            if (wantPoint)
            {
                f.hasGeom = true;
                f.geom.type = 1;
                f.geom.coords = {cx, cy, 0.0};
            }
            else if (wantPoly)
            {
                f.hasGeom = true;
                f.geom.type = 3;
                OgrGeometry ring;
                ring.type = 2;
                const int cxs[5] = {0, 0, 1, 1, 0};
                const int cys[5] = {0, 1, 1, 0, 0};
                for (int k = 0; k < 5; k++)
                {
                    double px = x + cxs[k], py = y + cys[k];
                    ring.coords.push_back(gt[0] + px * gt[1] +
                                          py * gt[2]);
                    ring.coords.push_back(gt[3] + px * gt[4] +
                                          py * gt[5]);
                    ring.coords.push_back(0.0);
                }
                f.geom.parts.push_back(std::move(ring));
            }
            lyr.features.push_back(std::move(f));
        }
    ds.reset();

    auto ods = std::make_unique<OgrDataset>();
    ods->path = input;
    ods->driverShort = "MEM";
    ods->driverLong = "In Memory raster, vector and multidimensional "
                      "raster";
    ods->layers.push_back(std::move(lyr));

    return vvDelegateVerb(r, "as-features", std::move(ods), "", driver,
                          true, nullptr);
}

}  // namespace

void registerRasterAsFeaturesHandler()
{
    registerHandler("raster_as-features", rasterAsFeaturesHandler);
    registerArgValueCheck(
        "raster_as-features",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "output-format")
            {
                std::string drv;
                return vectorOutputDriverResolve(value, drv);
            }
            return "";
        });
    registerPostValidator("raster_as-features", asFeaturesPostValidator);
}
