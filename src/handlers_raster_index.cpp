// gdal raster index: vector index of raster footprints written through
// the shared vector_convert delegate (gdaltindex semantics).
#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "ogr.h"
#include "progress.h"
#include "proj_min.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{

void setArg(const CmdSpec &spec, ParseResult &r, const std::string &longName,
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

void initResult(const CmdSpec &spec, ParseResult &r)
{
    for (const auto &a : spec.args)
        r.byName[a.name].spec = &a;
}

bool fileExistsRx(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool isDirRx(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string absPathRx(const std::string &p)
{
    if (!p.empty() && p[0] == '/')
        return p;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return p;
    return std::string(cwd) + "/" + p;
}

// case-insensitive fnmatch-style glob used by the filename filter
bool globMatchRx(const char *pat, const char *str)
{
    while (*pat)
    {
        if (*pat == '*')
        {
            ++pat;
            if (!*pat)
                return true;
            for (const char *s = str;; ++s)
            {
                if (globMatchRx(pat, s))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (!*str)
            return false;
        if (*pat != '?' && tolower((unsigned char)*pat) !=
                               tolower((unsigned char)*str))
            return false;
        ++pat;
        ++str;
    }
    return !*str;
}

std::string baseNameRx(const std::string &p)
{
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// CPLGetBasename: directory and last extension stripped
std::string layerNameOf(const std::string &p)
{
    std::string b = baseNameRx(p);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

struct CrsOpRx
{
    PJ *op = nullptr;
    ~CrsOpRx()
    {
        if (op)
            proj_destroy(op);
    }
    bool init(const Srs &src, const Srs &dst)
    {
        PJ *raw = proj_create_crs_to_crs_from_pj(projCtx(), src.pj(),
                                                 dst.pj(), nullptr, nullptr);
        if (!raw)
            return false;
        PJ *norm = proj_normalize_for_visualization(projCtx(), raw);
        if (norm)
        {
            proj_destroy(raw);
            op = norm;
        }
        else
            op = raw;
        return true;
    }
    void transform(double &x, double &y) const
    {
        if (!op)
            return;
        PJ_COORD c = proj_coord(x, y, 0, HUGE_VAL);
        PJ_COORD o = proj_trans(op, PJ_FWD, c);
        if (std::isfinite(o.v[0]) && std::isfinite(o.v[1]))
        {
            x = o.v[0];
            y = o.v[1];
        }
    }
};

std::string srsProjString(const Srs &s)
{
    const int kProj4 = 1;  // PJ_PROJ_STRING_TYPE
    const char *p = proj_as_proj_string(projCtx(), s.pj(), kProj4, nullptr);
    if (!p)
        return "";
    std::string v = p;
    size_t t = v.find(" +type=crs");
    if (t != std::string::npos)
        v.erase(t, strlen(" +type=crs"));
    return v;
}

// shapefile targets cap the field at 254 chars: explicit WKT warns and
// stays unset, auto falls back to the PROJ string silently
std::string srsFieldValueRx(const Srs &s, const std::string &format,
                            bool shpLimit, bool &tooLong)
{
    tooLong = false;
    if (strEqualNoCase(format, "EPSG"))
    {
        std::string c = s.code();
        return c.empty() ? "" : "EPSG:" + c;
    }
    if (strEqualNoCase(format, "PROJ"))
        return srsProjString(s);
    if (strEqualNoCase(format, "WKT"))
    {
        std::string w = s.wkt1GdalFull();
        if (shpLimit && w.size() > 254)
        {
            tooLong = true;
            return "";
        }
        return w;
    }
    // auto: authority id when catalogued, WKT otherwise
    std::string a = s.authName(), c = s.code();
    if (!a.empty() && !c.empty())
        return a + ":" + c;
    std::string w = s.wkt1GdalFull();
    if (shpLimit && w.size() > 254)
        return srsProjString(s);
    return w;
}

int runConvertDelegateRx(std::unique_ptr<OgrDataset> srcDs,
                         const std::string &inputPath,
                         const std::string &output,
                         const std::string &format,
                         const std::vector<std::string> &co, bool append,
                         bool update, const std::string &outputLayer)
{
    const Spec &spec = Spec::instance();
    const CmdSpec *cs = spec.findById("vector_convert");
    Handler h = findHandler("vector_convert");
    if (!cs || !h)
        return 1;
    ParseResult cr;
    initResult(*cs, cr);
    setArg(*cs, cr, "input", {inputPath});
    setArg(*cs, cr, "output", {output});
    if (!format.empty())
        setArg(*cs, cr, "of", {format});
    if (!co.empty())
        setArg(*cs, cr, "co", co);
    if (append)
        setArg(*cs, cr, "append", {"true"});
    if (update)
        setArg(*cs, cr, "update", {"true"});
    if (!outputLayer.empty())
        setArg(*cs, cr, "output-layer", {outputLayer});
    setArg(*cs, cr, "quiet", {"true"});
    g_convertSourceOverride = std::move(srcDs);
    g_pipelineStepPrefix = "index";
    int rc = h(*cs, cr);
    g_pipelineStepPrefix.clear();
    g_convertSourceOverride.reset();
    return rc;
}

int rasterIndexHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string output = r.str("output");
    std::string format = r.str("output-format");
    bool toStdout = output == "/vsistdout/";
    bool quiet = r.flag("quiet") || toStdout;
    bool profileParquet =
        strEqualNoCase(r.str("profile"), "STAC-GeoParquet");

    std::string driver;
    if (!format.empty())
    {
        // parse-time value checks already rejected bad formats
        vectorOutputDriverResolve(format, driver);
    }
    else
    {
        std::string low = strToLower(output);
        if (strEndsWith(low, ".json") || strEndsWith(low, ".geojson"))
            driver = "GeoJSON";
        else if (strEndsWith(low, ".geojsonl") ||
                 strEndsWith(low, ".geojsons"))
            driver = "GeoJSONSeq";
        else if (strEndsWith(low, ".shp") || strEndsWith(low, ".dbf"))
            driver = "ESRI Shapefile";
        else if (profileParquet)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "index: Cannot find driver Parquet");
            return 1;
        }
        else
        {
            // update-family runs adopt the driver of the existing output
            if ((r.flag("append") || r.flag("update") ||
                 r.flag("overwrite-layer")) &&
                fileExistsRx(output))
            {
                cplPushQuietHandler();
                std::string terr;
                auto tgt = openVectorDataset(output, terr, {});
                cplPopHandler();
                if (tgt)
                    driver = tgt->driverShort;
            }
            if (driver.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "index: Cannot guess driver for " + output);
                return 1;
            }
        }
    }
    bool isMem = driver == "MEM";
    std::string nln = r.str("output-layer");
    bool nlnSet = r.get("output-layer") != nullptr;
    if ((isMem || toStdout) && !nlnSet)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "index: Argument 'layer' must be specified");
        return 1;
    }

    bool fAppend = r.flag("append");
    bool fUpdate = r.flag("update");  // explicit only: implied stores false
    bool fOwLayer = r.flag("overwrite-layer");
    std::string locName = r.str("location-name");
    if (!r.get("location-name"))
        locName = "location";
    bool absPath = r.flag("absolute-path");
    bool recursive = r.flag("recursive");
    bool skipErrors = r.flag("skip-errors");
    std::vector<std::string> filters = r.list("filename-filter");
    bool minPxSet = r.get("min-pixel-size") != nullptr;
    bool maxPxSet = r.get("max-pixel-size") != nullptr;
    double minPx = minPxSet ? atof(r.str("min-pixel-size").c_str()) : 0;
    double maxPx = maxPxSet ? atof(r.str("max-pixel-size").c_str()) : 0;
    std::string crsField = r.str("source-crs-field-name");
    bool crsFieldSet = r.get("source-crs-field-name") != nullptr;
    std::string crsFormat = r.str("source-crs-format");
    if (crsFormat.empty())
        crsFormat = "auto";
    bool hasDst = r.get("dst-crs") != nullptr;
    Srs dstCrs;
    if (hasDst)
    {
        bool ok = false;
        dstCrs = Srs::fromCliInput(r.str("dst-crs"), ok);
        if (!ok)
            hasDst = false;
    }

    std::string precreatedFile, precreatedDir;
    auto precreate = [&]()
    {
        if (isMem || toStdout)
            return;
        if (driver == "GeoJSON")
        {
            if (!fileExistsRx(output))
            {
                writeStringToFile(output, "");
                precreatedFile = output;
            }
        }
        else if (driver == "ESRI Shapefile")
        {
            std::string low = strToLower(output);
            if (!strEndsWith(low, ".shp") && !strEndsWith(low, ".dbf") &&
                !isDirRx(output))
            {
                mkdir(output.c_str(), 0755);
                precreatedDir = output;
            }
        }
    };

    enum
    {
        M_CREATE,
        M_APPEND,
        M_UPDATE_CREATE
    } mode = M_CREATE;
    std::string targetLayerName;
    std::set<std::string> apExisting;
    bool prevRefValid = false;
    std::string prevRef;
    if (!isMem && !toStdout && fAppend)
    {
        bool exists = fileExistsRx(output);
        if (!exists)
        {
            // append on a missing output creates the empty dataset, then
            // fails resolving the layer in it
            precreate();
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "index: Cannot find layer '" +
                            (nlnSet ? nln : std::string()) + "'");
            return 1;
        }
        cplPushQuietHandler();
        std::string terr;
        auto tgt = openVectorDataset(output, terr, {});
        cplPopHandler();
        if (!tgt)
        {
            if (driver == "GeoJSON")
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "The GeoJSON driver does not overwrite "
                            "existing files.");
            else
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            output + " is not a directory.");
            return 1;
        }
        const OgrLayer *found = nullptr;
        std::string want = nlnSet ? nln
                                  : (tgt->layers.size() == 1
                                         ? tgt->layers[0].name
                                         : std::string());
        // single-file shapefile targets resolve to their sole layer
        // whatever the requested name
        if (tgt->driverShort == "ESRI Shapefile" && !isDirRx(output) &&
            !tgt->layers.empty())
            want = tgt->layers[0].name;
        for (const auto &l : tgt->layers)
            if (l.name == want)
            {
                found = &l;
                break;
            }
        if (!found)
            for (const auto &l : tgt->layers)
                if (strEqualNoCase(l.name, want))
                {
                    found = &l;
                    break;
                }
        if (!found)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "index: Cannot find layer '" + want + "'");
            return 1;
        }
        auto fieldIdx = [&](const std::string &nm) -> int
        {
            for (size_t i = 0; i < found->fields.size(); ++i)
                if (strEqualNoCase(found->fields[i].name, nm))
                    return (int)i;
            return -1;
        };
        int locIdx = fieldIdx(locName);
        if (locIdx < 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unable to find field `" + locName + "' in file `" +
                            output + "'.");
            return 1;
        }
        if (crsFieldSet && fieldIdx(crsField) < 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unable to find field `" + crsField +
                            "' in file `" + output + "'.");
            return 1;
        }
        mode = M_APPEND;
        targetLayerName = found->name;
        std::string firstLoc;
        for (const auto &f : found->features)
            if (locIdx < (int)f.values.size() && f.values[locIdx].set &&
                f.values[locIdx].v.type == JVal::STRING)
            {
                if (apExisting.empty())
                    firstLoc = f.values[locIdx].v.s;
                apExisting.insert(f.values[locIdx].v.s);
            }
        // the reference CRS comes from the first tile the existing index
        // references, not from the index layer itself
        if (!firstLoc.empty())
        {
            cplPushQuietHandler();
            std::string rerr;
            auto rds = openRaster(firstLoc, rerr);
            cplPopHandler();
            if (rds)
            {
                prevRefValid = true;
                prevRef = rds->hasSrs && rds->srs.valid()
                              ? rds->srs.wkt1GdalFull()
                              : std::string();
            }
        }
    }
    else if (!isMem && !toStdout && (fUpdate || fOwLayer))
    {
        cplPushQuietHandler();
        std::string terr;
        auto tgt = openVectorDataset(output, terr, {});
        cplPopHandler();
        if (!tgt)
            return 1;  // engine validation already reported this
        std::string want = nlnSet ? nln : layerNameOf(output);
        if (tgt->driverShort == "ESRI Shapefile" && !isDirRx(output) &&
            !tgt->layers.empty())
            want = tgt->layers[0].name;
        const OgrLayer *found = nullptr;
        for (const auto &l : tgt->layers)
            if (l.name == want)
            {
                found = &l;
                break;
            }
        if (!found)
            for (const auto &l : tgt->layers)
                if (strEqualNoCase(l.name, want))
                {
                    found = &l;
                    break;
                }
        if (found)
        {
            if (fOwLayer)
            {
                if (tgt->driverShort == "ESRI Shapefile" && isDirRx(output))
                {
                    for (const char *ext :
                         {".shp", ".shx", ".dbf", ".prj", ".cpg", ".qix"})
                        unlink((output + "/" + found->name + ext).c_str());
                    mode = M_UPDATE_CREATE;
                    targetLayerName = want;
                }
                else if (tgt->driverShort == "ESRI Shapefile")
                {
                    std::string base = output;
                    size_t dot = base.find_last_of('.');
                    size_t sl = base.find_last_of('/');
                    if (dot != std::string::npos &&
                        (sl == std::string::npos || dot > sl))
                        base.erase(dot);
                    for (const char *ext :
                         {".shp", ".shx", ".dbf", ".prj", ".cpg", ".qix"})
                        unlink((base + ext).c_str());
                    mode = M_CREATE;
                }
                else
                {
                    cplErrorStr(CE_Failure, CPLE_NotSupported,
                                "DeleteLayer() not supported by this "
                                "dataset.");
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "index: Cannot delete layer '" + want +
                                    "'");
                    return 1;
                }
            }
            else
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "index: Layer '" + want +
                                "' already exists. Specify the "
                                "--overwrite-layer option to overwrite it, "
                                "or --append to append to it.");
                return 1;
            }
        }
        else
        {
            if (tgt->driverShort == "GeoJSON")
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "GeoJSON driver doesn't support creating a "
                            "layer on a read-only datasource");
                return 1;
            }
            mode = M_UPDATE_CREATE;
            targetLayerName = want;
        }
    }
    else
    {
        // engine only clears identifiable datasets on --overwrite; the
        // oracle also replaces plain unrecognized files
        if (r.flag("overwrite") && !isMem && !toStdout &&
            fileExistsRx(output) && !isDirRx(output))
            unlink(output.c_str());
        precreate();
    }

    // ---- file list
    bool anyDirInput = false;
    std::vector<std::string> files;
    auto filterOk = [&](const std::string &base)
    {
        if (filters.empty())
            return true;
        for (const auto &pat : filters)
            if (globMatchRx(pat.c_str(), base.c_str()))
                return true;
        return false;
    };
    std::function<void(const std::string &)> expandDir =
        [&](const std::string &dirPath)
    {
        DIR *d = opendir(dirPath.c_str());
        if (!d)
            return;
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            std::string entry = dirPath;
            if (!entry.empty() && entry.back() != '/')
                entry += '/';
            entry += e->d_name;
            if (isDirRx(entry))
            {
                if (recursive)
                    expandDir(entry);
                continue;
            }
            if (filterOk(e->d_name))
                files.push_back(entry);
        }
        closedir(d);
    };
    for (const auto &in : inputs)
    {
        if (isDirRx(in))
        {
            anyDirInput = true;
            expandDir(in);
        }
        else
            files.push_back(in);
    }

    // ---- gather
    struct Gathered
    {
        double ring[10];
        std::string loc;
        bool crsPresent = false;
        std::string crsVal;
    };
    std::vector<Gathered> feats;
    TermProgress tp;
    bool bar = !quiet;
    long long total = (long long)files.size();
    long long written = 0;
    int rc = 0;
    bool aborted = false;

    // layer SRS: destination CRS, else the SRS of the first openable
    // source (even when that source ends up skipped); a failed first
    // open also means no layer is created, so aborts leave the
    // precreated empty file untouched
    bool layerHasSrs = false;
    bool layerReady = false;
    Srs layerSrs;
    if (hasDst)
    {
        layerHasSrs = true;
        layerReady = true;
        layerSrs = dstCrs;
    }
    else if (mode != M_APPEND && !files.empty())
    {
        cplPushQuietHandler();
        std::string oerr;
        auto pre = openRaster(files[0], oerr);
        cplPopHandler();
        if (pre)
        {
            layerReady = true;
            if (pre->hasSrs && pre->srs.valid())
            {
                layerHasSrs = true;
                layerSrs = pre->srs;
            }
        }
    }

    auto writeOut = [&]() -> int
    {
        if (isMem)
            return 0;
        auto uds = std::make_unique<OgrDataset>();
        uds->path = output;
        uds->driverShort = "MEM";
        OgrLayer ul;
        ul.name = mode == M_CREATE
                      ? (nlnSet ? nln : layerNameOf(output))
                      : targetLayerName;
        ul.geomType = 3;
        ul.hasGeomField = true;
        if (layerHasSrs)
        {
            ul.hasSrs = true;
            ul.srs = layerSrs;
        }
        OgrFieldDefn locFd;
        locFd.name = locName;
        locFd.type = OFTString;
        locFd.width = 254;
        ul.fields.push_back(locFd);
        if (crsFieldSet)
        {
            OgrFieldDefn cf;
            cf.name = crsField;
            cf.type = OFTString;
            cf.width = 254;
            ul.fields.push_back(cf);
        }
        for (size_t i = 0; i < feats.size(); ++i)
        {
            Gathered &g = feats[i];
            OgrFeature f;
            f.fid = (long long)i;
            f.hasGeom = true;
            f.geom.type = 3;
            OgrGeometry rg;
            rg.type = 0;
            for (int k = 0; k < 5; ++k)
            {
                rg.coords.push_back(g.ring[k * 2]);
                rg.coords.push_back(g.ring[k * 2 + 1]);
                rg.coords.push_back(0.0);
            }
            f.geom.parts.push_back(std::move(rg));
            OgrFieldValue lv;
            lv.set = true;
            lv.v.type = JVal::STRING;
            lv.v.s = g.loc;
            f.values.push_back(std::move(lv));
            if (crsFieldSet)
            {
                OgrFieldValue cv;
                if (g.crsPresent)
                {
                    cv.set = true;
                    cv.v.type = JVal::STRING;
                    cv.v.s = g.crsVal;
                }
                f.values.push_back(std::move(cv));
            }
            ul.features.push_back(std::move(f));
        }
        vectorLayerRecomputeExtent(ul);
        uds->layers.push_back(std::move(ul));
        if (mode == M_CREATE)
        {
            if (!precreatedFile.empty())
                unlink(precreatedFile.c_str());
            if (!precreatedDir.empty())
                rmdir(precreatedDir.c_str());
        }
        std::string inputPath = inputs.empty() ? output : inputs[0];
        return runConvertDelegateRx(
            std::move(uds), inputPath, output, driver,
            r.list("creation-option"), mode == M_APPEND,
            mode == M_UPDATE_CREATE,
            mode == M_CREATE ? "" : targetLayerName);
    };

    if (files.empty())
    {
        // hard failure before the layer is written: the precreated empty
        // output file stays as-is
        cplErrorStr(CE_Failure, CPLE_AppDefined, "Cannot find any tile");
        return 1;
    }

    for (const auto &f : files)
    {
        std::string loc = absPath ? absPathRx(f) : f;
        if (mode == M_APPEND && apExisting.count(loc))
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "File " + loc +
                            " is already in tileindex. Skipping it.");
            continue;
        }
        CPLErrClass sev = skipErrors ? CE_Warning : CE_Failure;
        std::unique_ptr<RasterDatasetBase> ds;
        std::string oerr;
        if (skipErrors)
            cplPushQuietHandler();
        struct stat st;
        bool missing = stat(f.c_str(), &st) != 0 && f.find(':') ==
                                                        std::string::npos;
        if (!missing)
            ds = openRaster(f, oerr);
        if (skipErrors)
            cplPopHandler();
        if (!ds)
        {
            if (missing || oerr == "missing")
                cplErrorStr(sev, CPLE_OpenFailed, datasetMissingMessage(f));
            else if (oerr != "reported" || skipErrors)
                cplErrorStr(sev, CPLE_OpenFailed,
                            "`" + f +
                                "' not recognized as being in a supported "
                                "file format.");
            cplErrorStr(sev, CPLE_AppDefined,
                        "Unable to open " + f +
                            (skipErrors ? ", skipping." : "."));
            if (skipErrors)
                continue;
            aborted = true;
            break;
        }
        if (!ds->hasGT)
        {
            cplErrorStr(sev, CPLE_AppDefined,
                        "It appears no georeferencing is available for\n`" +
                            f + "'" + (skipErrors ? ", skipping." : "."));
            if (skipErrors)
                continue;
            aborted = true;
            break;
        }
        const double *gt = ds->gt;
        double w = ds->width, h = ds->height;
        double xs[5] = {gt[0], gt[0] + w * gt[1], gt[0] + w * gt[1] + h * gt[2],
                        gt[0] + h * gt[2], gt[0]};
        double ys[5] = {gt[3], gt[3] + w * gt[4], gt[3] + w * gt[4] + h * gt[5],
                        gt[3] + h * gt[5], gt[3]};
        std::string wkt = ds->hasSrs && ds->srs.valid()
                              ? ds->srs.wkt1GdalFull()
                              : std::string();
        if (!hasDst)
        {
            if (!prevRefValid)
            {
                prevRefValid = true;
                prevRef = wkt;
            }
            else if (!prevRef.empty() &&
                     (wkt.size() != prevRef.size() ||
                      !strEqualNoCase(wkt, prevRef)))
            {
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    loc +
                        " is not using the same projection system as "
                        "other files in the tileindex.\nThis may cause "
                        "problems when using it in MapServer for "
                        "example.\nUse -t_srs option to set target "
                        "projection system. " +
                        (crsFieldSet ? "" : "Skipping this file."));
                if (!crsFieldSet)
                    continue;
            }
        }
        else if (ds->hasSrs && ds->srs.valid() &&
                 ds->srs.wkt1GdalFull() != dstCrs.wkt1GdalFull())
        {
            CrsOpRx op;
            if (op.init(ds->srs, dstCrs))
            {
                // GDALSuggestedWarpOutput semantics: bounds from a
                // densified pixel-grid boundary, square pixel size from
                // the (0,0)-(w,h) diagonal, extent snapped to rounded
                // pixel counts
                const int kSteps = 21;
                double mnx = 0, mxx = 0, mny = 0, mxy = 0;
                bool first = true;
                auto sample = [&](double px, double py)
                {
                    double sx = gt[0] + px * gt[1] + py * gt[2];
                    double sy = gt[3] + px * gt[4] + py * gt[5];
                    op.transform(sx, sy);
                    if (first)
                    {
                        mnx = mxx = sx;
                        mny = mxy = sy;
                        first = false;
                    }
                    else
                    {
                        mnx = std::min(mnx, sx);
                        mxx = std::max(mxx, sx);
                        mny = std::min(mny, sy);
                        mxy = std::max(mxy, sy);
                    }
                };
                for (int i = 0; i <= kSteps; ++i)
                {
                    double t = i == kSteps ? 1.0 : i / (double)kSteps;
                    sample(t * w, 0);
                    sample(t * w, h);
                    sample(0, t * h);
                    sample(w, t * h);
                }
                double ulx = gt[0], uly = gt[3];
                double lrx = gt[0] + w * gt[1] + h * gt[2];
                double lry = gt[3] + w * gt[4] + h * gt[5];
                op.transform(ulx, uly);
                op.transform(lrx, lry);
                double diag = sqrt((lrx - ulx) * (lrx - ulx) +
                                   (lry - uly) * (lry - uly));
                double pxSz = diag / sqrt(w * w + h * h);
                int nPix = (int)((mxx - mnx) / pxSz + 0.5);
                int nLin = (int)((mxy - mny) / pxSz + 0.5);
                xs[0] = mnx;
                ys[0] = mxy;
                xs[1] = mnx + nPix * pxSz;
                ys[1] = mxy;
                xs[2] = mnx + nPix * pxSz;
                ys[2] = mxy - nLin * pxSz;
                xs[3] = mnx;
                ys[3] = mxy - nLin * pxSz;
                xs[4] = xs[0];
                ys[4] = ys[0];
            }
        }
        if (minPxSet || maxPxSet)
        {
            double mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];
            for (int k = 1; k < 5; ++k)
            {
                mnx = std::min(mnx, xs[k]);
                mxx = std::max(mxx, xs[k]);
                mny = std::min(mny, ys[k]);
                mxy = std::max(mxy, ys[k]);
            }
            double px = sqrt(fabs((mxx - mnx) * (mxy - mny)) / (w * h));
            if (minPxSet && px < minPx)
            {
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("%s has %f as pixel size (< %f). "
                                      "Skipping",
                                      loc.c_str(), px, minPx));
                continue;
            }
            if (maxPxSet && px > maxPx)
            {
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("%s has %f as pixel size (> %f). "
                                      "Skipping",
                                      loc.c_str(), px, maxPx));
                continue;
            }
        }
        Gathered g;
        for (int k = 0; k < 5; ++k)
        {
            g.ring[k * 2] = xs[k];
            g.ring[k * 2 + 1] = ys[k];
        }
        g.loc = loc;
        if (crsFieldSet && ds->hasSrs && ds->srs.valid())
        {
            bool tooLong = false;
            std::string v =
                srsFieldValueRx(ds->srs, crsFormat,
                                driver == "ESRI Shapefile", tooLong);
            if (tooLong)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Cannot write WKT for file " + loc +
                                " as it is too long!");
            else if (!v.empty())
            {
                g.crsPresent = true;
                g.crsVal = v;
            }
        }
        feats.push_back(std::move(g));
        ++written;
        // directory expansion leaves the total open-ended: progress then
        // runs on a done/(done+1) scale instead of done/(files+1)
        if (bar)
            tp.update(anyDirInput
                          ? (double)written / (double)(written + 1)
                          : (double)written / (double)(total + 1));
    }

    if (aborted)
    {
        if (mode == M_APPEND ? !feats.empty() : layerReady)
            writeOut();
        return 1;
    }
    if (bar)
        tp.update(1.0);
    rc = writeOut();
    return rc;
}

}  // namespace

std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver);

void registerRasterIndexHandler()
{
    registerHandler("raster_index", rasterIndexHandler);
    registerArgValueCheck(
        "raster_index",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "output-format")
            {
                if (strEqualNoCase(value, "GDALG"))
                    return "\x07GDALG output is not supported.";
                std::string drv;
                std::string err = vectorOutputDriverResolve(value, drv);
                if (!err.empty())
                    return err;
                if (drv == "stream")
                    return "Invalid value for argument 'output-format'. "
                           "Driver 'stream' does not exist.";
            }
            if (argName == "dst-crs")
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return "Invalid value for 'dst-crs' argument";
            }
            return "";
        });
    registerPostValidator(
        "raster_index",
        [](const CmdSpec &, ParseResult &r, bool) -> bool
        {
            bool bad = false;
            std::string of = r.str("output-format");
            std::string out = r.str("output");
            if (of.empty() && strEndsWith(strToLower(out), ".gdalg.json"))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "index: GDALG output is not supported");
                bad = true;
            }
            if (strEqualNoCase(r.str("profile"), "STAC-GeoParquet"))
            {
                std::string drv;
                if (!of.empty())
                    vectorOutputDriverResolve(of, drv);
                else
                {
                    std::string low = strToLower(out);
                    if (strEndsWith(low, ".json") ||
                        strEndsWith(low, ".geojson"))
                        drv = "GeoJSON";
                    else if (strEndsWith(low, ".geojsonl") ||
                             strEndsWith(low, ".geojsons"))
                        drv = "GeoJSONSeq";
                    else if (strEndsWith(low, ".shp") ||
                             strEndsWith(low, ".dbf"))
                        drv = "ESRI Shapefile";
                }
                if (!drv.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_NotSupported,
                                "index: STAC-GeoParquet profile is only "
                                "compatible with Parquet output format");
                    bad = true;
                }
            }
            return bad;
        });
}
