#include "engine.h"
#include "cpl.h"
#include "dataset.h"
#include "ogr.h"
#include "rasterpolyfoot.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/stat.h>

std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver);

// GeoJSON/GeoJSONSeq layer-creation-option name lists (the convert
// writer's tables are internal to its translation unit)
bool rpfLcoSupported(const std::string &driver, const std::string &key)
{
    static const char *geojsonLCO[] = {
        "WRITE_BBOX",          "COORDINATE_PRECISION",
        "SIGNIFICANT_FIGURES", "NATIVE_DATA",
        "NATIVE_MEDIA_TYPE",   "RFC7946",
        "WRAPDATELINE",        "WRITE_NAME",
        "DESCRIPTION",         "ID_FIELD",
        "ID_TYPE",             "ID_GENERATE",
        "WRITE_NON_FINITE_VALUES",
        "AUTODETECT_JSON_STRINGS",
        "FOREIGN_MEMBERS_FEATURE",
        "FOREIGN_MEMBERS_COLLECTION"};
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
    for (size_t i = 0; i < n; i++)
        if (strEqualNoCase(key, list[i]))
            return true;
    return false;
}

bool rpfFileExists(const std::string &path)
{
    if (vsiIsVirtual(path))
        return vsiExists(path);
    struct stat sb;
    return stat(path.c_str(), &sb) == 0;
}

std::vector<RpfRegionInfo> rpfRegionOrder(const std::vector<int> &canon,
                                          int w, int h)
{
    std::map<int, RpfRegionInfo> m;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            size_t idx = (size_t)y * w + x;
            int id = canon[idx];
            auto it = m.find(id);
            if (it == m.end())
                m[id] = RpfRegionInfo{id, y, idx};
            else
            {
                it->second.lastRow = y;
                it->second.lastIdx = idx;
            }
        }
    std::vector<RpfRegionInfo> out;
    out.reserve(m.size());
    for (auto &kv : m)
        out.push_back(kv.second);
    std::sort(out.begin(), out.end(),
              [](const RpfRegionInfo &a, const RpfRegionInfo &b)
              {
                  if (a.lastRow != b.lastRow)
                      return a.lastRow < b.lastRow;
                  return a.id < b.id;
              });
    return out;
}

namespace
{
struct RpfEdge
{
    int sx, sy, ex, ey;
    int dir;  // 0 down, 1 right, 2 up, 3 left
    bool used = false;
};
}  // namespace

std::vector<std::vector<std::pair<int, int>>> rpfTraceRegion(
    const std::vector<int> &canon, int w, int h, int region)
{
    std::vector<RpfEdge> edges;
    auto in = [&](int x, int y)
    {
        return x >= 0 && y >= 0 && x < w && y < h &&
               canon[(size_t)y * w + x] == region;
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            if (!in(x, y))
                continue;
            if (!in(x - 1, y))
                edges.push_back({x, y, x, y + 1, 0, false});
            if (!in(x, y + 1))
                edges.push_back({x, y + 1, x + 1, y + 1, 1, false});
            if (!in(x + 1, y))
                edges.push_back({x + 1, y + 1, x + 1, y, 2, false});
            if (!in(x, y - 1))
                edges.push_back({x + 1, y, x, y, 3, false});
        }
    std::map<long long, std::vector<int>> outAt;
    for (size_t i = 0; i < edges.size(); i++)
        outAt[(long long)edges[i].sy * (w + 2) + edges[i].sx].push_back(
            (int)i);
    std::vector<int> order(edges.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = (int)i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b)
              {
                  const RpfEdge &A = edges[a], &B = edges[b];
                  if (A.sy != B.sy)
                      return A.sy < B.sy;
                  if (A.sx != B.sx)
                      return A.sx < B.sx;
                  return A.dir < B.dir;
              });
    auto rightOf = [](int d)
    {
        switch (d)
        {
            case 0:
                return 3;
            case 3:
                return 2;
            case 2:
                return 1;
            default:
                return 0;
        }
    };
    std::vector<std::vector<std::pair<int, int>>> rings;
    for (int oi : order)
    {
        if (edges[oi].used)
            continue;
        std::vector<std::pair<int, int>> pts;
        int startX = edges[oi].sx, startY = edges[oi].sy;
        pts.push_back({startX, startY});
        int cur = oi;
        for (;;)
        {
            edges[cur].used = true;
            int cx = edges[cur].ex, cy = edges[cur].ey, d = edges[cur].dir;
            if (cx == startX && cy == startY)
            {
                pts.push_back({startX, startY});
                break;
            }
            auto it = outAt.find((long long)cy * (w + 2) + cx);
            int next = -1;
            if (it != outAt.end())
            {
                int rr = rightOf(d);
                int want[3] = {rr, d, rightOf(rightOf(rr))};
                for (int wi = 0; wi < 3 && next < 0; wi++)
                    for (int e : it->second)
                        if (!edges[e].used && edges[e].dir == want[wi])
                        {
                            next = e;
                            break;
                        }
            }
            if (next < 0)
                break;
            if (edges[next].dir != d)
                pts.push_back({cx, cy});
            cur = next;
        }
        rings.push_back(std::move(pts));
    }
    return rings;
}

std::string rpfGuessDriver(const std::string &output)
{
    std::string ext;
    size_t dot = output.find_last_of('.');
    size_t slash = output.find_last_of('/');
    if (dot != std::string::npos &&
        (slash == std::string::npos || dot > slash))
        ext = strToLower(output.substr(dot + 1));
    if (ext == "json" || ext == "geojson")
        return "GeoJSON";
    if (ext == "geojsonl" || ext == "geojsons")
        return "GeoJSONSeq";
    if (ext == "shp" || ext == "dbf")
        return "ESRI Shapefile";
    return "";
}

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

OgrGeometry ringsToPolygon(
    const std::vector<std::vector<std::pair<int, int>>> &rings,
    const double *gt)
{
    OgrGeometry poly;
    poly.type = 3;
    for (const auto &ring : rings)
    {
        OgrGeometry rg;
        rg.type = 2;
        for (const auto &pt : ring)
        {
            double cx = pt.first, cy = pt.second;
            rg.coords.push_back(gt[0] + cx * gt[1] + cy * gt[2]);
            rg.coords.push_back(gt[3] + cx * gt[4] + cy * gt[5]);
            rg.coords.push_back(0.0);
        }
        poly.parts.push_back(std::move(rg));
    }
    return poly;
}

// mirrors the engine's convert-output validation for this non-convert
// verb: exists refusal, update-family output probing
bool polygonizePostValidator(const CmdSpec &cmd, ParseResult &r,
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
        else if (fApp && !rpfFileExists(output))
        {
            // append creates a fresh dataset; nothing to validate
        }
    }
    if (!inputOpenFailed)
    {
        const ArgValue *bv = r.get("band");
        int band = (bv && bv->set) ? atoi(bv->str().c_str()) : 1;
        std::vector<std::string> inputs = r.list("input");
        if (band >= 1 && !inputs.empty())
        {
            std::string derr;
            cplPushQuietHandler();
            auto ds = openRaster(inputs[0], derr);
            cplPopHandler();
            if (ds && band > (int)ds->bands.size())
            {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Value of 'band' should be greater or "
                              "equal than 1 and less or equal than %d.",
                              cmd.name.c_str(), (int)ds->bands.size()));
                bad = true;
            }
        }
    }
    return bad;
}

int rasterPolygonizeHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    std::string layerName = r.str("output-layer");
    if (layerName.empty())
        layerName = "polygonize";
    std::string attrName = r.str("attribute-name");
    if (attrName.empty())
        attrName = "DN";
    bool conn8 = r.flag("connect-diagonal-pixels");

    std::string driver;
    vectorOutputDriverResolve(r.str("output-format"), driver);
    if (driver.empty())
    {
        if (!strEndsWith(strToLower(output), ".gdalg.json"))
            driver = rpfGuessDriver(output);
        if (driver.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "polygonize: Cannot guess driver for " + output);
            return 1;
        }
    }
    if (driver == "GDALG")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "polygonize: Cannot find driver GDALG");
        return 1;
    }
    if (driver == "stream")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "polygonize: Cannot find driver stream");
        return 1;
    }

    bool fApp = r.flag("append");
    bool fUpd = r.flag("update");
    bool fOwl = r.flag("overwrite-layer");
    bool appendToExisting = false;
    bool delegAppend = false;
    bool owlRewrite = false;
    bool gjRewrite = false;
    OgrLayer oldLayer;
    auto basenameStem = [](const std::string &p)
    {
        std::string base = p;
        size_t sl = base.find_last_of('/');
        if (sl != std::string::npos)
            base = base.substr(sl + 1);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos)
            base = base.substr(0, dot);
        return base;
    };
    // update-family choreography against the existing target: the
    // driver of the file on disk decides layer naming and abilities
    // (shapefile layers answer to the basename and can be replaced;
    // the GeoJSON family can neither delete nor create in place, except
    // that the sequence writer grows a new layer by appending)
    if ((fApp || fUpd || fOwl) && rpfFileExists(output))
    {
        std::string terr;
        auto tds = openVectorDataset(output, terr, {});
        if (tds)
        {
            std::string diskDriver = tds->driverShort;
            if (diskDriver == "ESRI Shapefile")
                layerName = basenameStem(output);
            bool found = false;
            const OgrLayer *fl = nullptr;
            for (const auto &l : tds->layers)
                if (l.name == layerName)
                {
                    found = true;
                    fl = &l;
                }
            if (fApp)
            {
                if (!found)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "polygonize: Cannot find layer '" +
                                    layerName + "'");
                    return 1;
                }
                bool haveField = false;
                for (const auto &fd2 : fl->fields)
                    if (fd2.name == attrName)
                        haveField = true;
                if (!haveField)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "polygonize: Cannot find field '" +
                                    attrName + "' in layer '" +
                                    layerName + "'");
                    return 1;
                }
                appendToExisting = true;
                if (diskDriver == "GeoJSON" &&
                    !rpfGjRootIsCollection(output))
                {
                    gjRewrite = true;
                    oldLayer = *fl;
                }
                else
                {
                    delegAppend = true;
                    driver = diskDriver;
                }
            }
            else if (fOwl)
            {
                if (!found)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "polygonize: Cannot find layer '" +
                                    layerName + "'");
                    return 1;
                }
                if (diskDriver == "ESRI Shapefile")
                {
                    owlRewrite = true;
                    driver = diskDriver;
                }
                else
                {
                    cplErrorStr(CE_Failure, CPLE_NotSupported,
                                "DeleteLayer() not supported by this "
                                "dataset.");
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "polygonize: Cannot delete layer '" +
                                    layerName + "'");
                    return 1;
                }
            }
            else if (fUpd)
            {
                if (found)
                {
                    cplErrorStr(
                        CE_Failure, CPLE_AppDefined,
                        "polygonize: Layer '" + layerName +
                            "' already exists. Specify the "
                            "--overwrite-layer option to overwrite it, or "
                            "--append to append to it.");
                    return 1;
                }
                if (diskDriver == "GeoJSONSeq")
                {
                    delegAppend = true;
                    driver = diskDriver;
                }
                else
                {
                    cplErrorStr(CE_Failure, CPLE_NotSupported,
                                "GeoJSON driver doesn't support creating "
                                "a layer on a read-only datasource");
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "polygonize: Cannot create layer '" +
                                    layerName + "'");
                    return 1;
                }
            }
        }
    }
    else if (fApp)
    {
        // append without an existing target: the empty output is
        // created and the layer lookup fails
        if (driver == "ESRI Shapefile")
            layerName = basenameStem(output);
        else if ((driver == "GeoJSON" || driver == "GeoJSONSeq") &&
                 !output.empty())
        {
            FILE *f = fopen(output.c_str(), "wb");
            if (f)
                fclose(f);
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "polygonize: Cannot find layer '" + layerName + "'");
        return 1;
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
    const ArgValue *bv = r.get("band");
    int band = (bv && bv->set) ? atoi(bv->str().c_str()) : 1;
    if (band < 1 || band > (int)ds->bands.size())
        return 1;
    const Band &b = ds->bands[band - 1];
    int w = ds->width, h = ds->height;

    std::vector<uint8_t> raw;
    if (!ds->readBandRaw(band, raw))
        return 1;
    size_t n = (size_t)w * h;
    int esz = dtypeSizeBytes(b.type);
    bool cplx = dtypeIsComplex(b.type);
    int step = cplx ? esz / 2 : esz;

    enum
    {
        PATH_INT,
        PATH_F32,
        PATH_F64
    } path;
    switch (b.type)
    {
        case DType::Float16:
        case DType::Float32:
        case DType::CFloat32:
            path = PATH_F32;
            break;
        case DType::Float64:
        case DType::CFloat64:
            path = PATH_F64;
            break;
        default:
            path = PATH_INT;
            break;
    }
    int fieldType = OFTInteger;
    if (b.type == DType::Int64 || b.type == DType::UInt64)
        fieldType = OFTInteger64;
    else if (path != PATH_INT)
        fieldType = OFTReal;

    std::vector<long long> bufI;
    std::vector<float> bufF;
    std::vector<double> bufD;
    if (path == PATH_INT)
    {
        bufI.resize(n);
        for (size_t i = 0; i < n; i++)
        {
            const uint8_t *p = &raw[i * esz];
            long long v = 0;
            switch (b.type)
            {
                case DType::Byte:
                    v = *p;
                    break;
                case DType::Int8:
                    v = *(const int8_t *)p;
                    break;
                case DType::UInt16:
                    v = *(const uint16_t *)p;
                    break;
                case DType::Int16:
                case DType::CInt16:
                    v = *(const int16_t *)p;
                    break;
                case DType::UInt32:
                    v = *(const uint32_t *)p;
                    break;
                case DType::Int32:
                case DType::CInt32:
                    v = *(const int32_t *)p;
                    break;
                case DType::Int64:
                    v = *(const int64_t *)p;
                    break;
                case DType::UInt64:
                {
                    // GDALCopyWords clamp into the Int64 work buffer
                    uint64_t u = *(const uint64_t *)p;
                    v = u > 0x7fffffffffffffffULL
                            ? (long long)0x7fffffffffffffffULL
                            : (long long)u;
                    break;
                }
                default:
                    break;
            }
            bufI[i] = v;
        }
    }
    else if (path == PATH_F32)
    {
        bufF.resize(n);
        for (size_t i = 0; i < n; i++)
        {
            const uint8_t *p = &raw[i * esz];
            bufF[i] = b.type == DType::Float16
                          ? tailHalfToFloat(*(const uint16_t *)p)
                          : *(const float *)p;
        }
    }
    else
    {
        bufD.resize(n);
        for (size_t i = 0; i < n; i++)
            bufD[i] = *(const double *)&raw[i * esz];
    }
    (void)step;

    if (b.hasNodata)
    {
        if (path == PATH_INT)
        {
            bool haveNd = true;
            long long nd = 0;
            if (b.nodataIsI64)
                nd = b.nodataI64;
            else if (b.nodataIsU64)
                nd = b.nodataU64 > 0x7fffffffffffffffULL
                         ? (long long)0x7fffffffffffffffULL
                         : (long long)b.nodataU64;
            else if (std::floor(b.nodata) == b.nodata &&
                     b.nodata >= -9.223372036854775808e18 &&
                     b.nodata < 9.223372036854775808e18)
                nd = (long long)b.nodata;
            else
                haveNd = false;
            if (haveNd)
                for (size_t i = 0; i < n; i++)
                    if (bufI[i] == nd)
                        bufI[i] = kRpfMarkerInt;
        }
        else if (path == PATH_F32)
        {
            float nd = (float)b.nodata;
            for (size_t i = 0; i < n; i++)
                if (bufF[i] == nd ||
                    (std::isnan(nd) && std::isnan(bufF[i])))
                    bufF[i] = (float)kRpfMarkerInt;
        }
        else
        {
            double nd = b.nodata;
            for (size_t i = 0; i < n; i++)
                if (bufD[i] == nd ||
                    (std::isnan(nd) && std::isnan(bufD[i])))
                    bufD[i] = (double)kRpfMarkerInt;
        }
    }

    std::vector<int> canon;
    if (path == PATH_INT)
        rpfLabel(bufI, w, h, conn8, RpfEqInt(), canon);
    else if (path == PATH_F32)
        rpfLabel(bufF, w, h, conn8, RpfEqFloat(), canon);
    else
        rpfLabel(bufD, w, h, conn8, RpfEqDouble(), canon);
    std::vector<RpfRegionInfo> regions = rpfRegionOrder(canon, w, h);

    OgrLayer lyr;
    lyr.name = layerName;
    lyr.geomType = 3;
    OgrFieldDefn fd;
    fd.name = attrName;
    fd.type = fieldType;
    lyr.fields.push_back(fd);
    if (ds->hasSrs)
    {
        lyr.hasSrs = true;
        lyr.srs = ds->srs;
    }

    // GeoJSONSeq (and RFC7946 GeoJSON) layer creation announces the
    // assumed CRS when the source carries none; the layer is created
    // before pixels are scanned, so this precedes emission warnings
    if (!lyr.hasSrs && !appendToExisting)
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
            // the reference's CreateLayer emits the assumed-CRS note
            // after the co/lco validation warnings: pre-run those here
            // and strip the already-warned entries from the delegation
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
    }

    bool nanWarned = false;
    for (const auto &reg : regions)
    {
        double dv;
        bool skip;
        if (path == PATH_INT)
        {
            skip = RpfEqInt()(bufI[reg.lastIdx], kRpfMarkerInt);
            dv = (double)bufI[reg.lastIdx];
        }
        else if (path == PATH_F32)
        {
            skip = RpfEqFloat()(bufF[reg.lastIdx], (float)kRpfMarkerInt);
            dv = (double)bufF[reg.lastIdx];
        }
        else
        {
            skip = RpfEqDouble()(bufD[reg.lastIdx], (double)kRpfMarkerInt);
            dv = bufD[reg.lastIdx];
        }
        if (skip)
            continue;
        OgrFeature f;
        f.hasGeom = true;
        f.geom = ringsToPolygon(rpfTraceRegion(canon, w, h, reg.id),
                                ds->gt);
        f.values.resize(1);
        if (fieldType == OFTReal)
        {
            if (std::isnan(dv) || std::isinf(dv))
            {
                if (!nanWarned)
                {
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "NaN of Infinity value found. Skipped. "
                                "Further messages of this type will be "
                                "suppressed.");
                    nanWarned = true;
                }
            }
            else
            {
                f.values[0].set = true;
                f.values[0].v.type = JVal::DOUBLE;
                f.values[0].v.d = dv;
            }
        }
        else if (fieldType == OFTInteger)
        {
            int out;
            bool lossy = false;
            if (dv < -2147483648.0)
            {
                out = -2147483647 - 1;
                lossy = true;
            }
            else if (dv > 2147483647.0)
            {
                out = 2147483647;
                lossy = true;
            }
            else
                out = (int)dv;
            if (lossy)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("Field %s.%s: Lossy conversion occurred "
                              "when trying to set 32 bit integer field "
                              "from real value %.17g.",
                              layerName.c_str(), attrName.c_str(), dv));
            f.values[0].set = true;
            f.values[0].v.type = JVal::INT;
            f.values[0].v.i = out;
        }
        else
        {
            long long out;
            if (!(dv >= -9223372036854775808.0 &&
                  dv < 9223372036854775808.0))
                out = (long long)0x8000000000000000ULL;
            else
                out = (long long)dv;
            if ((double)out != dv)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("Field %s.%s: Lossy conversion occurred "
                              "when trying to set 64 bit integer field "
                              "from real value %.17g.",
                              layerName.c_str(), attrName.c_str(), dv));
            f.values[0].set = true;
            f.values[0].v.type = JVal::INT;
            f.values[0].v.i = out;
        }
        lyr.features.push_back(std::move(f));
    }
    ds.reset();

    if (gjRewrite)
    {
        // single-feature sequence target opened through the GeoJSON
        // driver: the whole file is rewritten as a pretty collection
        // under the assumed CRS84 layer CRS, existing features first
        OgrLayer merged = oldLayer;
        merged.name = layerName;
        bool ok84 = false;
        Srs s84 = Srs::fromCliInput("EPSG:4326", ok84);
        if (ok84)
        {
            merged.hasSrs = true;
            merged.srs = s84;
        }
        int ai = -1;
        for (size_t i = 0; i < merged.fields.size(); i++)
            if (merged.fields[i].name == attrName)
                ai = (int)i;
        for (auto &nf : lyr.features)
        {
            OgrFeature mf;
            mf.hasGeom = nf.hasGeom;
            mf.geom = std::move(nf.geom);
            mf.values.resize(merged.fields.size());
            if (ai >= 0 && !nf.values.empty())
                mf.values[ai] = nf.values[0];
            merged.features.push_back(std::move(mf));
        }
        lyr = std::move(merged);
        driver = "GeoJSON";
    }

    auto ods = std::make_unique<OgrDataset>();
    ods->path = input;
    ods->driverShort = "MEM";
    ods->driverLong = "In Memory raster, vector and multidimensional "
                      "raster";
    ods->layers.push_back(std::move(lyr));

    forceArg(r, "output-format", driver);
    if (gjRewrite)
    {
        forceArg(r, "overwrite", "true");
        forceArg(r, "output-layer", layerName);
        clearArg(r, "append");
        clearArg(r, "update");
        clearArg(r, "overwrite-layer");
    }
    else if (delegAppend)
    {
        forceArg(r, "append", "true");
        clearArg(r, "update");
        clearArg(r, "overwrite-layer");
    }
    else if (owlRewrite)
    {
        forceArg(r, "overwrite", "true");
        clearArg(r, "append");
        clearArg(r, "update");
        clearArg(r, "overwrite-layer");
    }
    return vvDelegateVerb(r, "polygonize", std::move(ods), "", driver,
                          true, nullptr);
}

}  // namespace

void registerRasterPolygonizeHandler()
{
    registerHandler("raster_polygonize", rasterPolygonizeHandler);
    registerArgValueCheck(
        "raster_polygonize",
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
    registerPreValidator(
        "raster_polygonize",
        [](const CmdSpec &, ParseResult &r) -> int
        {
            const ArgValue *bv = r.get("band");
            if (bv && bv->set && atoi(bv->str().c_str()) < 1)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Value of 'band' should greater or equal "
                            "to 1.");
                handlerPrintUsage();
                return 1;
            }
            // --append re-opens the input while wiring the output: the
            // open failure is reported twice, the sourceless append then
            // fails its layer lookup (or dereferences the null source)
            // with no usage block
            if (r.flag("append"))
            {
                std::vector<std::string> inputs = r.list("input");
                std::string in = inputs.empty() ? "" : inputs[0];
                std::string openErr;
                if (!in.empty() && in.rfind("GTIFF_DIR:", 0) != 0)
                {
                    if (!rpfFileExists(in))
                        openErr = datasetMissingMessage(in);
                    else if (!datasetIdentify(in, {"raster"}))
                        openErr = "`" + in +
                                  "' not recognized as being in a "
                                  "supported file format.";
                }
                if (!openErr.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                    cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                    std::string output = r.str("output");
                    std::string driver;
                    vectorOutputDriverResolve(r.str("output-format"),
                                              driver);
                    if (driver.empty() &&
                        !strEndsWith(strToLower(output), ".gdalg.json"))
                        driver = rpfGuessDriver(output);
                    if (driver.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "polygonize: Cannot guess driver "
                                    "for " +
                                        output);
                        return 1;
                    }
                    std::string layerName = r.str("output-layer");
                    if (layerName.empty())
                        layerName = "polygonize";
                    if (driver == "ESRI Shapefile")
                    {
                        // shapefile layers answer to the basename
                        std::string base = output;
                        size_t sl = base.find_last_of('/');
                        if (sl != std::string::npos)
                            base = base.substr(sl + 1);
                        size_t dot = base.find_last_of('.');
                        if (dot != std::string::npos)
                            base = base.substr(0, dot);
                        layerName = base;
                    }
                    if (driver != "MEM" && !output.empty())
                    {
                        if (rpfFileExists(output))
                        {
                            std::string terr;
                            auto tds =
                                openVectorDataset(output, terr, {});
                            if (tds)
                                for (const auto &l : tds->layers)
                                    if (l.name == layerName)
                                    {
                                        fflush(nullptr);
                                        raise(SIGSEGV);
                                    }
                        }
                        else if (driver == "GeoJSON" ||
                                 driver == "GeoJSONSeq")
                        {
                            FILE *f = fopen(output.c_str(), "wb");
                            if (f)
                                fclose(f);
                        }
                    }
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "polygonize: Cannot find layer '" +
                                    layerName + "'");
                    return 1;
                }
            }
            return 0;
        });
    registerPostValidator("raster_polygonize", polygonizePostValidator);
}
