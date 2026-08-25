#include "cpl.h"
#include "engine.h"
#include "ogr.h"
#include "progress.h"
#include "proj_min.h"
#include "spec.h"
#include "util.h"
#include "vsi.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <dirent.h>
#include <map>
#include <set>
#include <memory>
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

bool fileExistsIx(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool isDirIx(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

long long fileSizeIx(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}

void rmTreeIx(const std::string &p)
{
    struct stat st;
    if (lstat(p.c_str(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
    {
        DIR *d = opendir(p.c_str());
        if (d)
        {
            struct dirent *e;
            while ((e = readdir(d)))
            {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                    continue;
                rmTreeIx(p + "/" + e->d_name);
            }
            closedir(d);
        }
        rmdir(p.c_str());
    }
    else
        unlink(p.c_str());
}

std::string absPathIx(const std::string &p)
{
    if (!p.empty() && p[0] == '/')
        return p;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return p;
    return std::string(cwd) + "/" + p;
}

// halving ladder ticked once per opened dataset, right after the open:
// 0.1 for the first, then steps of 0.1 halved every five datasets
double gatherLadder(int k)
{
    double v = 0.1;
    double step = 0.1;
    for (int j = 1; j <= k; ++j)
    {
        if (j % 5 == 0)
            step /= 2;
        v += step;
    }
    return v > 1.0 ? 1.0 : v;
}

bool globMatchIx(const char *pat, const char *str)
{
    while (*pat)
    {
        if (*pat == '*')
        {
            ++pat;
            if (!*pat)
                return true;
            for (const char *s = str; ; ++s)
            {
                if (globMatchIx(pat, s))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (!*str)
            return false;
        if (*pat != '?' && *pat != *str)
            return false;
        ++pat;
        ++str;
    }
    return !*str;
}

std::string baseNameIx(const std::string &p)
{
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

struct CrsOp
{
    PJ *op = nullptr;
    ~CrsOp()
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

int runConvertDelegate(std::unique_ptr<OgrDataset> srcDs,
                       const std::string &inputPath,
                       const std::string &output, const std::string &format,
                       const std::vector<std::string> &co,
                       const std::vector<std::string> &lco, bool append,
                       bool update, const std::string &outputLayer,
                       const char *prefix)
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
    if (!lco.empty())
        setArg(*cs, cr, "lco", lco);
    if (append)
        setArg(*cs, cr, "append", {"true"});
    if (update)
        setArg(*cs, cr, "update", {"true"});
    if (!outputLayer.empty())
        setArg(*cs, cr, "output-layer", {outputLayer});
    setArg(*cs, cr, "quiet", {"true"});
    g_convertSourceOverride = std::move(srcDs);
    g_pipelineStepPrefix = prefix;
    int rc = h(*cs, cr);
    g_pipelineStepPrefix.clear();
    g_convertSourceOverride.reset();
    return rc;
}

// ---------------------------------------------------------------- index

void geomExtentDeep(const OgrGeometry &g, double ext[4], bool &any)
{
    size_t n = g.coords.size() / 3;
    for (size_t i = 0; i < n; ++i)
    {
        double x = g.coords[i * 3], y = g.coords[i * 3 + 1];
        if (!any)
        {
            ext[0] = ext[2] = x;
            ext[1] = ext[3] = y;
            any = true;
        }
        else
        {
            ext[0] = std::min(ext[0], x);
            ext[2] = std::max(ext[2], x);
            ext[1] = std::min(ext[1], y);
            ext[3] = std::max(ext[3], y);
        }
    }
    for (const auto &p : g.parts)
        geomExtentDeep(p, ext, any);
}

bool layerExtentIx(const OgrLayer &lyr, double ext[4])
{
    bool any = false;
    for (const auto &f : lyr.features)
        if (f.hasGeom)
            geomExtentDeep(f.geom, ext, any);
    return any;
}

struct IndexGather
{
    bool haveRef = false;
    bool refHasSrs = false;
    Srs refSrs;
    std::vector<OgrFieldDefn> refFields;
    long long considered = 0;
    std::vector<OgrFeature> feats;
    std::vector<std::string> locs;
    std::vector<std::pair<bool, std::string>> crsVals;
};

std::string srsFieldValue(const Srs &s, const std::string &format)
{
    if (strEqualNoCase(format, "EPSG"))
    {
        std::string c = s.code();
        return c.empty() ? "" : "EPSG:" + c;
    }
    if (strEqualNoCase(format, "PROJ"))
    {
        const int kProj4 = 1;  // PJ_PROJ_STRING_TYPE
        const char *p =
            proj_as_proj_string(projCtx(), s.pj(), kProj4, nullptr);
        if (!p)
            return "";
        std::string v = p;
        size_t t = v.find(" +type=crs");
        if (t != std::string::npos)
            v.erase(t, strlen(" +type=crs"));
        return v;
    }
    if (strEqualNoCase(format, "WKT"))
        return s.wkt1GdalFull();
    // auto: authority id when catalogued, WKT otherwise
    std::string a = s.authName(), c = s.code();
    if (!a.empty() && !c.empty())
        return a + ":" + c;
    return s.wkt1GdalFull();
}

int vectorIndexHandler(const CmdSpec &, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string output = r.str("output");
    std::string format = r.str("output-format");
    bool toStdout = output == "/vsistdout/";
    bool quiet = r.flag("quiet") || toStdout;

    std::string driver;
    if (!format.empty())
    {
        if (strEqualNoCase(format, "stream"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "index: Invalid value for argument 'output-format'. "
                        "Driver 'stream' does not exist.");
            handlerPrintUsage();
            return 1;
        }
        std::string ferr = vectorOutputDriverResolve(format, driver);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "index: " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    else
    {
        std::string low = strToLower(output);
        if (strEndsWith(low, ".json") || strEndsWith(low, ".geojson"))
            driver = "GeoJSON";
        else if (strEndsWith(low, ".shp") || strEndsWith(low, ".dbf"))
            driver = "ESRI Shapefile";
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "index: Cannot guess driver for " + output);
            return 1;
        }
    }
    bool isMem = driver == "MEM";

    bool fOverwrite = r.flag("overwrite");
    bool fAppend = r.flag("append");
    bool fUpdate = r.flag("update");  // explicit only: implied stores false
    bool fOwLayer = r.flag("overwrite-layer");
    (void)fOverwrite;
    std::string nln = r.str("output-layer");
    bool nlnSet = r.get("output-layer") != nullptr;
    std::string locName = r.str("location-name");
    if (!r.get("location-name"))
        locName = "location";
    bool absPath = r.flag("absolute-path");
    bool dsOnly = r.flag("dataset-name-only");
    bool recursive = r.flag("recursive");
    bool acceptCrs = r.flag("accept-different-crs");
    bool acceptSchemas = r.flag("accept-different-schemas");
    std::vector<std::string> filters = r.list("filename-filter");
    std::vector<std::string> selNames = r.list("source-layer-name");
    std::vector<long long> selIdx;
    for (const auto &v : r.list("source-layer-index"))
        selIdx.push_back(strtoll(v.c_str(), nullptr, 10));
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
            if (!fileExistsIx(output))
            {
                writeStringToFile(output, "");
                precreatedFile = output;
            }
        }
        else if (driver == "ESRI Shapefile")
        {
            std::string low = strToLower(output);
            if (!strEndsWith(low, ".shp") && !strEndsWith(low, ".dbf") &&
                !isDirIx(output))
            {
                mkdir(output.c_str(), 0755);
                precreatedDir = output;
            }
        }
    };

    // target resolution runs before the gather: append/update layer
    // failures abort without a progress bar
    enum
    {
        M_CREATE,
        M_APPEND,
        M_UPDATE_CREATE
    } mode = M_CREATE;
    std::string targetLayerName;
    std::set<std::string> apExisting;
    bool apHaveRef = false, apRefHasSrs = false;
    Srs apRefSrs;
    std::vector<OgrFieldDefn> apRefFields;
    if (!isMem && !toStdout && fAppend)
    {
        bool exists = fileExistsIx(output);
        if (!exists && (fUpdate || fOwLayer))
        {
            // update-flavored append on a missing output creates the
            // empty dataset, then fails resolving the layer in it
            precreate();
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "index: Cannot find layer '" +
                            (nlnSet ? nln : std::string()) + "'");
            return 1;
        }
        if (exists)
        {
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
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + output +
                                    "' not recognized as being in a "
                                    "supported file format.");
                return 1;
            }
            const OgrLayer *found = nullptr;
            std::string want = nlnSet
                                   ? nln
                                   : (tgt->layers.size() == 1
                                          ? tgt->layers[0].name
                                          : std::string());
            // single-file shapefile targets resolve to their sole layer
            // whatever the requested name
            if (tgt->driverShort == "ESRI Shapefile" &&
                !isDirIx(output) && !tgt->layers.empty())
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
            auto hasField = [&](const std::string &nm)
            {
                for (const auto &f : found->fields)
                    if (strEqualNoCase(f.name, nm))
                        return true;
                return false;
            };
            if (!hasField(locName))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "index: Unable to find field '" + locName +
                                "' in output layer.");
                return 1;
            }
            if (crsFieldSet && !hasField(crsField))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "index: Unable to find field '" + crsField +
                                "' in output layer.");
                return 1;
            }
            mode = M_APPEND;
            targetLayerName = found->name;
            int locIdx = -1;
            for (size_t i = 0; i < found->fields.size(); ++i)
                if (strEqualNoCase(found->fields[i].name, locName))
                {
                    locIdx = (int)i;
                    break;
                }
            std::string firstLoc;
            if (locIdx >= 0)
                for (const auto &f : found->features)
                    if (locIdx < (int)f.values.size() &&
                        f.values[locIdx].set &&
                        f.values[locIdx].v.type == JVal::STRING)
                    {
                        if (apExisting.empty())
                            firstLoc = f.values[locIdx].v.s;
                        apExisting.insert(f.values[locIdx].v.s);
                    }
            // the reference schema/CRS come from the first tile the
            // existing index references, not from the index layer itself
            if (!firstLoc.empty())
            {
                const std::string &first = firstLoc;
                std::string tilePath = first;
                long long tileIdx = 0;
                size_t comma = first.rfind(',');
                if (comma != std::string::npos &&
                    comma + 1 < first.size() &&
                    first.find_first_not_of("0123456789", comma + 1) ==
                        std::string::npos)
                {
                    tilePath = first.substr(0, comma);
                    tileIdx = strtoll(first.c_str() + comma + 1, nullptr,
                                      10);
                }
                cplPushQuietHandler();
                std::string rerr;
                auto rds = openVectorDataset(tilePath, rerr, {});
                cplPopHandler();
                if (rds && tileIdx >= 0 &&
                    (size_t)tileIdx < rds->layers.size())
                {
                    const OgrLayer &rl = rds->layers[tileIdx];
                    apHaveRef = true;
                    apRefHasSrs = rl.hasSrs;
                    apRefSrs = rl.srs;
                    apRefFields = rl.fields;
                }
            }
        }
        else
        {
            mode = M_CREATE;
            precreate();
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
        std::string want = nlnSet ? nln : "tileindex";
        if (tgt->driverShort == "ESRI Shapefile" && !isDirIx(output) &&
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
                if (tgt->driverShort == "ESRI Shapefile" &&
                    isDirIx(output))
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
        precreate();
    }

    // ---- gather
    TermProgress tp;
    bool bar = !quiet;
    int tickN = 0;
    IndexGather g;
    if (apHaveRef)
    {
        g.haveRef = true;
        g.refHasSrs = apRefHasSrs;
        g.refSrs = apRefSrs;
        g.refFields = apRefFields;
    }

    auto selected = [&](const OgrLayer &l, size_t i)
    {
        if (selNames.empty() && selIdx.empty())
            return true;
        for (const auto &n : selNames)
            if (l.name == n)
                return true;
        for (long long ix : selIdx)
            if (ix == (long long)i)
                return true;
        return false;
    };

    auto processLayer = [&](const OgrLayer &lyr, size_t li,
                            const std::string &locPath)
    {
        ++g.considered;
        std::string loc = absPath ? absPathIx(locPath) : locPath;
        if (!dsOnly)
            loc += strPrintf(",%d", (int)li);
        if (mode == M_APPEND && apExisting.count(loc))
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "index: '" + loc +
                            "' already referenced in tile index");
            return;
        }
        if (!g.haveRef)
        {
            g.haveRef = true;
            g.refHasSrs = lyr.hasSrs;
            g.refSrs = lyr.srs;
            g.refFields = lyr.fields;
        }
        else
        {
            if (!hasDst)
            {
                bool same =
                    lyr.hasSrs == g.refHasSrs &&
                    (!lyr.hasSrs || lyr.srs.isEquivalentTo(g.refSrs));
                if (!same)
                {
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        "index: Warning: layer " + lyr.name + " of " +
                            locPath +
                            " is not using the same CRS as other files in "
                            "the tileindex. This may cause problems when "
                            "using it in MapServer for example" +
                            (acceptCrs ? "" : ". Skipping it"));
                    if (!acceptCrs)
                        return;
                }
            }
            if (!acceptSchemas)
            {
                if (lyr.fields.size() != g.refFields.size())
                {
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "index: Number of attributes of layer " +
                                    lyr.name + " of " + locPath +
                                    " does not match. Skipping it.");
                    return;
                }
                for (size_t i = 0; i < lyr.fields.size(); ++i)
                    if (lyr.fields[i].name != g.refFields[i].name ||
                        lyr.fields[i].type != g.refFields[i].type ||
                        lyr.fields[i].subType != g.refFields[i].subType ||
                        lyr.fields[i].width != g.refFields[i].width ||
                        lyr.fields[i].precision !=
                            g.refFields[i].precision)
                    {
                        cplErrorStr(CE_Warning, CPLE_AppDefined,
                                    "index: Schema of attributes of "
                                    "layer " +
                                        lyr.name + " of " + locPath +
                                        " does not match. Skipping it.");
                        return;
                    }
            }
        }
        double ext[4];
        if (!layerExtentIx(lyr, ext))
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "index: GetExtent() failed on layer " + lyr.name +
                            " of " + locPath + ", skipping.");
            return;
        }
        double ring[10] = {ext[0], ext[1], ext[0], ext[3], ext[2],
                           ext[3], ext[2], ext[1], ext[0], ext[1]};
        if (hasDst && lyr.hasSrs)
        {
            CrsOp op;
            if (op.init(lyr.srs, dstCrs))
                for (int i = 0; i < 5; ++i)
                    op.transform(ring[i * 2], ring[i * 2 + 1]);
        }
        OgrFeature f;
        f.fid = (long long)g.feats.size();
        f.hasGeom = true;
        f.geom.type = 3;
        OgrGeometry rg;
        rg.type = 0;
        for (int i = 0; i < 5; ++i)
        {
            rg.coords.push_back(ring[i * 2]);
            rg.coords.push_back(ring[i * 2 + 1]);
            rg.coords.push_back(0.0);
        }
        f.geom.parts.push_back(std::move(rg));
        g.locs.push_back(loc);
        bool crsPresent = false;
        std::string crsVal;
        if (crsFieldSet && (hasDst || lyr.hasSrs))
        {
            crsVal = srsFieldValue(hasDst ? dstCrs : lyr.srs, crsFormat);
            crsPresent = !crsVal.empty();
            if (crsPresent && driver == "ESRI Shapefile" &&
                strEqualNoCase(crsFormat, "WKT") && crsVal.size() > 80)
            {
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "index: Cannot write WKT for file " + locPath +
                                " as it is too long");
                crsPresent = false;
                crsVal.clear();
            }
        }
        g.crsVals.emplace_back(crsPresent, crsVal);
        g.feats.push_back(std::move(f));
    };

    auto processDataset = [&](const OgrDataset &ds,
                              const std::string &locPath)
    {
        if (bar)
            tp.update(gatherLadder(tickN++));
        for (size_t i = 0; i < ds.layers.size(); ++i)
            if (selected(ds.layers[i], i))
                processLayer(ds.layers[i], i, locPath);
    };

    std::function<void(const std::string &, bool)> expandDir =
        [&](const std::string &dirPath, bool topLevel)
    {
        (void)topLevel;
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
            if (isDirIx(entry))
            {
                if (recursive)
                    expandDir(entry, false);
                continue;
            }
            if (!filters.empty())
            {
                bool ok = false;
                std::string base = baseNameIx(entry);
                for (const auto &pat : filters)
                    if (globMatchIx(pat.c_str(), base.c_str()))
                        ok = true;
                if (!ok)
                    continue;
            }
            cplPushQuietHandler();
            std::string err;
            auto ds = openVectorDataset(entry, err, {});
            cplPopHandler();
            if (ds)
                processDataset(*ds, entry);
        }
        closedir(d);
    };

    for (const auto &in : inputs)
    {
        cplPushQuietHandler();
        std::string err;
        auto ds = openVectorDataset(in, err, {});
        cplPopHandler();
        if (ds)
            processDataset(*ds, in);
        else if (isDirIx(in))
            expandDir(in, true);
        else
        {
            if (g.considered == 0)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "index: No layer to index");
                return 1;
            }
        }
    }
    if (g.considered == 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "index: No layer to index");
        return 1;
    }
    if (bar)
        tp.update(1.0);

    // ---- write through the convert delegate
    auto uds = std::make_unique<OgrDataset>();
    uds->path = output;
    uds->driverShort = "MEM";
    OgrLayer ul;
    ul.name = mode == M_APPEND
                  ? targetLayerName
                  : (mode == M_UPDATE_CREATE
                         ? targetLayerName
                         : (nlnSet ? nln : std::string("tileindex")));
    ul.geomType = 3;
    ul.hasGeomField = true;
    if (hasDst)
    {
        ul.hasSrs = true;
        ul.srs = dstCrs;
    }
    else if (g.refHasSrs)
    {
        ul.hasSrs = true;
        ul.srs = g.refSrs;
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
        cf.width = 80;
        ul.fields.push_back(cf);
    }
    for (size_t i = 0; i < g.feats.size(); ++i)
    {
        OgrFeature &f = g.feats[i];
        OgrFieldValue lv;
        lv.set = true;
        lv.v.type = JVal::STRING;
        lv.v.s = g.locs[i];
        f.values.push_back(std::move(lv));
        if (crsFieldSet)
        {
            OgrFieldValue cv;
            if (g.crsVals[i].first)
            {
                cv.set = true;
                cv.v.type = JVal::STRING;
                cv.v.s = g.crsVals[i].second;
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
    return runConvertDelegate(
        std::move(uds), inputPath, output, driver, r.list("creation-option"),
        r.list("layer-creation-option"), mode == M_APPEND,
        mode == M_UPDATE_CREATE, mode == M_CREATE ? "" : targetLayerName,
        "index");
}

// ------------------------------------------------------------ partition

long long wkbSizeOf(const OgrGeometry &g, bool z)
{
    z = z || g.hasZ;
    long long coord = z ? 24 : 16;
    long long n = (long long)(g.coords.size() / 3);
    switch (g.type)
    {
        case 1:
            return 5 + coord;
        case 2:
            return 9 + n * coord;
        case 3:
        {
            long long b = 9;
            for (const auto &r : g.parts)
                b += 4 + (long long)(r.coords.size() / 3) *
                             ((z || r.hasZ) ? 24 : 16);
            return b;
        }
        default:
        {
            long long b = 9;
            for (const auto &c : g.parts)
                b += wkbSizeOf(c, z);
            return b;
        }
    }
}

// max-file-size accounting decoded from the reference: 20 per feature,
// WKB byte size for the geometry, and per-field costs by declared type
// (scalar Integer 15, Integer64 25, Real/Date 19, Time 21, DateTime 38,
// String 8+bytes; lists 8 plus scalar-minus-4 per element). A part
// starts charged 65536 (or the on-disk size when resuming an existing
// part) and closes once the accumulator reaches max-file-size. A null
// or unset value in a String field makes the reference SIGSEGV (strlen
// on a null pointer in its estimator); -1 signals the caller to
// reproduce that crash.
long long partFeatEstimate(const OgrLayer &lyr, const OgrFeature &f)
{
    auto strBytes = [](const JVal &j) -> long long
    {
        switch (j.type)
        {
            case JVal::STRING:
                return (long long)j.s.size();
            case JVal::INT:
                return (long long)strPrintf("%lld", j.i).size();
            case JVal::DOUBLE:
                return (long long)strPrintf("%.15g", j.d).size();
            case JVal::BOOL:
                return j.b ? 4 : 5;
            case JVal::ARRAY:
            case JVal::OBJECT:
                // String(JSON) storage keeps the spaced serialization
                return (long long)ogrJsonSpacedSerialize(j).size();
            default:
                return 0;
        }
    };
    long long est = 20;
    if (f.hasGeom)
        est += wkbSizeOf(f.geom, false);
    for (size_t i = 0; i < lyr.fields.size(); ++i)
    {
        const OgrFieldDefn &fd = lyr.fields[i];
        const OgrFieldValue *v =
            i < f.values.size() ? &f.values[i] : nullptr;
        bool isNull = !v || !v->set || v->v.type == JVal::NUL;
        switch (fd.type)
        {
            case OFTInteger:
                est += 15;
                break;
            case OFTInteger64:
                est += 25;
                break;
            case OFTReal:
            case OFTDate:
                est += 19;
                break;
            case OFTTime:
                est += 21;
                break;
            case OFTDateTime:
                est += 38;
                break;
            case OFTString:
                if (isNull)
                    return -1;
                est += 8 + strBytes(v->v);
                break;
            case OFTIntegerList:
            case OFTInteger64List:
            case OFTRealList:
            case OFTStringList:
            {
                est += 8;
                if (isNull || v->v.type != JVal::ARRAY)
                    break;
                for (const auto &el : v->v.arr)
                {
                    if (fd.type == OFTIntegerList)
                        est += 11;
                    else if (fd.type == OFTInteger64List)
                        est += 21;
                    else if (fd.type == OFTRealList)
                        est += 15;
                    else
                        est += 4 + strBytes(el);
                }
                break;
            }
            default:
                break;
        }
    }
    return est;
}

// percent-encode charset measured from the reference: control bytes,
// space, '%', '/', ':', '=', '>', '\\' and all >= 0x80 (DEL passes)
std::string sanitizeToken(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (c < 0x20 || c == ' ' || c == '%' || c == '/' || c == ':' ||
            c == '=' || c == '>' || c == '\\' || c >= 0x80)
            out += strPrintf("%%%02X", c);
        else
            out += (char)c;
    }
    return out;
}

std::string fieldKeyString(const OgrFieldDefn &fd, const OgrFeature &f,
                           size_t idx)
{
    const JVal &v = f.values[idx].v;
    if (fd.type == OFTInteger || fd.type == OFTInteger64)
    {
        long long n = v.type == JVal::INT      ? v.i
                      : v.type == JVal::DOUBLE ? (long long)v.d
                      : v.type == JVal::BOOL   ? (v.b ? 1 : 0)
                      : strtoll(v.s.c_str(), nullptr, 10);
        return strPrintf("%lld", n);
    }
    switch (v.type)
    {
        case JVal::STRING:
            return v.s;
        case JVal::INT:
            return strPrintf("%lld", v.i);
        case JVal::DOUBLE:
            return strPrintf("%.15g", v.d);
        case JVal::BOOL:
            return v.b ? "1" : "0";
        default:
            return "";
    }
}

bool parseMaxFileSize(const std::string &s, long long &bytes)
{
    const char *p = s.c_str();
    char *endp = nullptr;
    double num = strtod(p, &endp);
    if (endp == p)
        return false;
    while (*endp == ' ')
        ++endp;
    std::string suf = endp;
    double mult = 1;
    if (suf.empty())
        mult = 1;
    else if (strEqualNoCase(suf, "K") || strEqualNoCase(suf, "KB"))
        mult = 1024;
    else if (strEqualNoCase(suf, "M") || strEqualNoCase(suf, "MB"))
        mult = 1024.0 * 1024;
    else if (strEqualNoCase(suf, "G") || strEqualNoCase(suf, "GB"))
        mult = 1024.0 * 1024 * 1024;
    else if (strEqualNoCase(suf, "T") || strEqualNoCase(suf, "TB"))
        mult = 1024.0 * 1024 * 1024 * 1024;
    else if (suf == "B")
        mult = 1;
    else
        return false;
    if (num < 0)
        return false;
    bytes = (long long)(num * mult);
    return true;
}

const char *kGeojsonLcoKeys[] = {
    "WRITE_BBOX",     "COORDINATE_PRECISION",
    "SIGNIFICANT_FIGURES", "NATIVE_DATA",
    "NATIVE_MEDIA_TYPE",   "RFC7946",
    "WRAPDATELINE",   "WRITE_NAME",
    "DESCRIPTION",    "ID_FIELD",
    "ID_TYPE",        "ID_GENERATE",
    "WRITE_NON_FINITE_VALUES",
    "AUTODETECT_JSON_STRINGS", "FOREIGN_MEMBERS_FEATURE",
    "FOREIGN_MEMBERS_COLLECTION"};
const char *kShapefileLcoKeys[] = {
    "SHPT",          "2GB_LIMIT",  "ENCODING",
    "RESIZE",        "SPATIAL_INDEX", "DBF_DATE_LAST_UPDATE",
    "AUTO_REPACK",   "DBF_EOF_CHAR"};

bool partLcoSupported(const std::string &driver, const std::string &key)
{
    if (driver == "GeoJSON")
    {
        for (const char *k : kGeojsonLcoKeys)
            if (strEqualNoCase(key, k))
                return true;
        return false;
    }
    if (driver == "ESRI Shapefile")
    {
        for (const char *k : kShapefileLcoKeys)
            if (strEqualNoCase(key, k))
                return true;
        return false;
    }
    return false;
}

struct PartBuf
{
    std::string path;
    std::string dir;
    long long count = 0;
    long long bytes = 0;
    bool appendExisting = false;
    int shpType = -1;
    std::vector<const OgrFeature *> feats;
};

int vectorPartitionHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.list("input").empty() ? "" : r.list("input")[0];
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    std::vector<std::string> fieldsReq = r.list("field");
    std::string scheme = r.str("scheme");
    bool flat = strEqualNoCase(scheme, "flat");
    bool omit = r.flag("omit-partitioned-field");
    bool skipErrors = r.flag("skip-errors");
    bool fOverwrite = r.flag("overwrite");
    bool fAppend = r.flag("append");
    long long featLimit = 0;
    if (r.get("feature-limit"))
        featLimit = strtoll(r.str("feature-limit").c_str(), nullptr, 10);
    long long maxSize = 0;
    if (r.get("max-file-size"))
        parseMaxFileSize(r.str("max-file-size"), maxSize);

    std::string format = r.str("output-format");
    std::string driver;
    if (!format.empty())
    {
        if (strEqualNoCase(format, "stream"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "partition: Invalid value for argument "
                        "'output-format'. Driver 'stream' does not "
                        "exist.");
            handlerPrintUsage();
            return 1;
        }
        std::string ferr = vectorOutputDriverResolve(format, driver);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "partition: " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(true, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "partition: " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }

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

    if (driver.empty())
        driver = ds->driverShort == "ESRI Shapefile" ? "ESRI Shapefile"
                 : ds->driverShort == "GeoJSONSeq"   ? "GeoJSONSeq"
                                                     : "GeoJSON";
    std::string ext;
    if (driver == "GeoJSON")
        ext = ".json";
    else if (driver == "GeoJSONSeq")
        ext = ".geojsonl";
    else if (driver == "ESRI Shapefile")
        ext = ".shp";
    else
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "partition: Output driver has no known file extension");
        return 1;
    }
    bool gjOut = driver == "GeoJSON" || driver == "GeoJSONSeq";
    bool shpOut = driver == "ESRI Shapefile";

    bool outExists = fileExistsIx(output);
    if (outExists && !fOverwrite && !fAppend)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "partition: '" + output +
                        "' already exists. Specify --overwrite or "
                        "--append");
        return 1;
    }
    if (outExists && fOverwrite)
    {
        rmTreeIx(output);
        outExists = false;
    }
    if (!outExists)
        mkdir(output.c_str(), 0755);

    std::string pattern = r.str("pattern");
    if (pattern.empty())
        pattern = flat ? (fieldsReq.empty() ? "{LAYER_NAME}_%010d"
                                            : "{LAYER_NAME}_{FIELD_VALUE}_"
                                              "%010d")
                       : "part_%010d";

    std::vector<std::string> co = r.list("creation-option");
    std::vector<std::string> lcoAll = r.list("layer-creation-option");
    std::vector<std::string> lcoPass;
    std::vector<std::string> lcoUnsupported;
    for (const auto &kv : lcoAll)
    {
        std::string key = kv.substr(0, kv.find('='));
        if (partLcoSupported(driver, key))
            lcoPass.push_back(kv);
        else
            lcoUnsupported.push_back(key);
    }

    long long total = 0;
    for (const auto &l : ds->layers)
        total += (long long)l.features.size();

    TermProgress tp;
    bool bar = !quiet;
    long long written = 0;

    struct FlushItem
    {
        PartBuf *buf;
        const OgrLayer *lyr;
        std::vector<int> partFieldIdx;
    };
    std::vector<std::unique_ptr<PartBuf>> allParts;
    std::vector<FlushItem> flushOrder;

    auto instantiate = [&](const std::string &layerToken,
                           const std::string &valueToken, long long n)
    {
        char buf[2048];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
        snprintf(buf, sizeof(buf), pattern.c_str(), n);
#pragma GCC diagnostic pop
        std::string pat(buf);
        auto replAll = [](std::string &s, const std::string &from,
                          const std::string &to)
        {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos)
            {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replAll(pat, "{LAYER_NAME}", layerToken);
        replAll(pat, "{FIELD_VALUE}", valueToken);
        return pat;
    };

    auto flushPart = [&](const FlushItem &it) -> int
    {
        const OgrLayer &src = *it.lyr;
        auto pds = std::make_unique<OgrDataset>();
        pds->path = it.buf->path;
        pds->driverShort = "MEM";
        OgrLayer pl;
        pl.name = src.name;
        pl.geomType = src.geomType;
        pl.geomHasZ = src.geomHasZ;
        pl.geomHasM = src.geomHasM;
        pl.hasGeomField = src.hasGeomField;
        pl.hasSrs = src.hasSrs;
        pl.srs = src.srs;
        std::vector<int> keep;
        for (size_t i = 0; i < src.fields.size(); ++i)
        {
            bool dropped = false;
            if (omit)
                for (int pi : it.partFieldIdx)
                    if (pi == (int)i)
                        dropped = true;
            if (!dropped)
            {
                keep.push_back((int)i);
                pl.fields.push_back(src.fields[i]);
            }
        }
        for (const OgrFeature *sf : it.buf->feats)
        {
            OgrFeature f;
            f.fid = sf->fid;
            f.hasGeom = sf->hasGeom;
            f.geom = sf->geom;
            for (int ki : keep)
                f.values.push_back(ki < (int)sf->values.size()
                                       ? sf->values[ki]
                                       : OgrFieldValue());
            pl.features.push_back(std::move(f));
        }
        vectorLayerRecomputeExtent(pl);
        pds->layers.push_back(std::move(pl));
        // append-mode runs write through update-opened datasets, which
        // drop the source FIDs even for freshly created parts
        bool fid = gjOut && !fAppend;
        if (fid)
            g_gjForceFidIds = true;
        int rc = runConvertDelegate(
            std::move(pds), input, it.buf->path, driver, {},
            it.buf->appendExisting ? std::vector<std::string>() : lcoPass,
            it.buf->appendExisting, false, "", "partition");
        if (fid)
            g_gjForceFidIds = false;
        return rc;
    };

    auto flushAll = [&]() -> int
    {
        int rc = 0;
        for (const auto &it : flushOrder)
            if (flushPart(it))
                rc = 1;
        return rc;
    };

    // on the estimator SIGSEGV path the reference leaves rotated-away
    // parts complete (closed at rotation) while still-open parts hold
    // only what their writers flushed: GeoJSON/GeoJSONSeq the whole
    // 4096-byte-block prefix of the byte stream written so far (the
    // GeoJSON footer "\n]\n}\n" lands only at close), shapefile a DBF
    // lagging one record with a zero record count in its header and
    // 0-byte SHP/SHX bodies; resumed parts rewrite in place at close,
    // so they stay untouched
    auto materializeCrash =
        [&](const std::map<std::string, PartBuf *> &groups)
    {
        std::set<PartBuf *> openSet;
        for (const auto &g : groups)
            openSet.insert(g.second);
        for (const auto &it : flushOrder)
        {
            if (!openSet.count(it.buf))
            {
                flushPart(it);
                continue;
            }
            if (it.buf->appendExisting)
                continue;
            if (shpOut)
            {
                bool none = it.buf->feats.empty();
                if (!none)
                    it.buf->feats.pop_back();
                flushPart(it);
                std::string base = it.buf->path;
                if (base.size() > 4)
                    base = base.substr(0, base.size() - 4);
                (void)!truncate((base + ".shp").c_str(), 0);
                (void)!truncate((base + ".shx").c_str(), 0);
                if (none)
                {
                    // the DBF header lands with the first record write
                    (void)!truncate((base + ".dbf").c_str(), 0);
                }
                else
                {
                    FILE *df = fopen((base + ".dbf").c_str(), "r+b");
                    if (df)
                    {
                        unsigned char zero[4] = {0, 0, 0, 0};
                        fseek(df, 4, SEEK_SET);
                        fwrite(zero, 1, 4, df);
                        fclose(df);
                    }
                }
            }
            else
            {
                flushPart(it);
                struct stat st;
                if (stat(it.buf->path.c_str(), &st) == 0)
                {
                    long long streamLen = (long long)st.st_size;
                    if (driver == "GeoJSON")
                        streamLen -= 5;
                    if (streamLen < 0)
                        streamLen = 0;
                    (void)!truncate(it.buf->path.c_str(),
                                    (streamLen / 4096) * 4096);
                }
            }
        }
    };

    long long headerBase = 65536;

    for (const auto &lyr : ds->layers)
    {
        std::string layerToken = sanitizeToken(lyr.name);
        std::string layerDir = output;
        if (!flat)
        {
            layerDir += "/" + layerToken;
            mkdir(layerDir.c_str(), 0755);
        }
        std::vector<int> partFieldIdx;
        std::vector<std::string> fieldUserNames;
        for (const auto &fq : fieldsReq)
        {
            int found = -1;
            for (size_t i = 0; i < lyr.fields.size(); ++i)
                if (lyr.fields[i].name == fq)
                {
                    found = (int)i;
                    break;
                }
            if (found < 0)
                for (size_t i = 0; i < lyr.fields.size(); ++i)
                    if (strEqualNoCase(lyr.fields[i].name, fq))
                    {
                        found = (int)i;
                        break;
                    }
            if (found < 0)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "partition: Cannot find field '" + fq +
                                "' in layer '" + lyr.name + "'");
                flushAll();
                return 1;
            }
            int t = lyr.fields[found].type;
            if (t != OFTString && t != OFTInteger && t != OFTInteger64)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "partition: Field '" + fq +
                                "' not valid for partitioning. Only fields "
                                "of type String, Integer or Integer64, or "
                                "geometry fields, are accepted");
                flushAll();
                return 1;
            }
            partFieldIdx.push_back(found);
            fieldUserNames.push_back(fq);
        }

        std::map<std::string, PartBuf *> groups;
        std::map<std::string, long long> groupPartNo;

        for (const auto &feat : lyr.features)
        {
            std::string groupDir = layerDir;
            std::string valueToken;
            for (size_t fi = 0; fi < partFieldIdx.size(); ++fi)
            {
                int idx = partFieldIdx[fi];
                bool isNull = idx >= (int)feat.values.size() ||
                              !feat.values[idx].set ||
                              feat.values[idx].v.type == JVal::NUL;
                std::string tok;
                if (flat)
                {
                    tok = isNull ? "__NULL__"
                          : fieldKeyString(lyr.fields[idx], feat, idx)
                                    .empty()
                              ? "__EMPTY__"
                              : sanitizeToken(fieldKeyString(
                                    lyr.fields[idx], feat, idx));
                    if (fi)
                        valueToken += "_";
                    valueToken += tok;
                }
                else
                {
                    tok = isNull ? "__HIVE_DEFAULT_PARTITION__"
                                 : sanitizeToken(fieldKeyString(
                                       lyr.fields[idx], feat, idx));
                    groupDir += "/" + sanitizeToken(fieldUserNames[fi]) +
                                "=" + tok;
                }
            }
            std::string groupKey = flat ? valueToken : groupDir;

            auto git = groups.find(groupKey);
            PartBuf *cur = git == groups.end() ? nullptr : git->second;
            bool needNew =
                !cur || (featLimit && cur->count >= featLimit) ||
                (maxSize && cur->bytes >= maxSize);

            // the shapefile pre-check runs against the part the feature
            // would join; a rotated part re-pins its type from this
            // feature, so only same-part mismatches can fail
            if (shpOut && !needNew && cur && feat.hasGeom)
            {
                int want = cur->shpType;
                if (want < 0)
                {
                    int t = shpTypeForGeomProbe(feat.geom.type,
                                                feat.geom.hasZ, false);
                    if (t >= 0)
                        cur->shpType = t;
                }
                else
                {
                    std::string msg =
                        shpGeomMismatchError(want, feat.geom);
                    if (!msg.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
                        if (skipErrors)
                        {
                            cplErrorStr(CE_Warning, CPLE_AppDefined,
                                        "partition: Cannot insert feature "
                                        "-1");
                            continue;
                        }
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "partition: Cannot insert feature -1");
                        flushAll();
                        return 1;
                    }
                }
            }

            if (needNew)
            {
                long long n;
                auto npIt = groupPartNo.find(groupKey);
                if (npIt == groupPartNo.end())
                {
                    if (!flat)
                    {
                        // build the hive directory chain lazily
                        std::string acc;
                        size_t start = 0;
                        while (start <= groupDir.size())
                        {
                            size_t slash = groupDir.find('/', start);
                            std::string seg =
                                slash == std::string::npos
                                    ? groupDir.substr(start)
                                    : groupDir.substr(start,
                                                      slash - start);
                            acc += (acc.empty() ? "" : "/") + seg;
                            if (!acc.empty())
                                mkdir(acc.c_str(), 0755);
                            if (slash == std::string::npos)
                                break;
                            start = slash + 1;
                        }
                    }
                    n = 1;
                    if (fAppend)
                    {
                        while (fileExistsIx(
                            (flat ? output : groupDir) + "/" +
                            instantiate(layerToken, valueToken, n) + ext))
                            ++n;
                        if (n > 1)
                            --n;  // resume in the last existing part
                    }
                }
                else
                    n = npIt->second + 1;

                std::string dir = flat ? output : groupDir;
                std::string path =
                    dir + "/" + instantiate(layerToken, valueToken, n) +
                    ext;
                {
                    // a pattern with subdirectories fails at part-create
                    // time and aborts the whole run before anything is
                    // flushed
                    std::string parent = path;
                    size_t sl = parent.find_last_of('/');
                    parent = sl == std::string::npos ? "."
                                                     : parent.substr(0, sl);
                    if (!isDirIx(parent))
                    {
                        if (driver == "GeoJSON")
                        {
                            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                        "Failed to create GeoJSON "
                                        "datasource: " +
                                            path + ": " + path +
                                            ": No such file or directory");
                            cplErrorStr(CE_Failure, CPLE_AppDefined,
                                        "partition: Cannot create dataset "
                                        "'" +
                                            path + "'");
                        }
                        else if (driver == "GeoJSONSeq")
                        {
                            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                        "Failed to create " + path + ": " +
                                            path +
                                            ": No such file or directory");
                            cplErrorStr(CE_Failure, CPLE_AppDefined,
                                        "partition: Cannot create dataset "
                                        "'" +
                                            path + "'");
                        }
                        else
                            cplErrorStr(CE_Failure, CPLE_AppDefined,
                                        "Failed to create file " + path +
                                            ": No such file or directory");
                        return 1;
                    }
                }
                bool appendExisting = fAppend && fileExistsIx(path);
                long long c = 0;
                long long b = 0;
                while (appendExisting)
                {
                    // an existing part may already be full: skip ahead
                    cplPushQuietHandler();
                    std::string perr;
                    auto pexist = openVectorDataset(path, perr, {});
                    cplPopHandler();
                    c = pexist && !pexist->layers.empty()
                            ? (long long)pexist->layers[0].features.size()
                            : 0;
                    b = fileSizeIx(path);
                    if (!((featLimit && c >= featLimit) ||
                          (maxSize && b >= maxSize)))
                        break;
                    ++n;
                    path = dir + "/" +
                           instantiate(layerToken, valueToken, n) + ext;
                    appendExisting = fAppend && fileExistsIx(path);
                    c = 0;
                    b = 0;
                }
                {
                    auto nb = std::make_unique<PartBuf>();
                    nb->path = path;
                    nb->dir = dir;
                    nb->count = appendExisting ? c : 0;
                    nb->bytes = appendExisting ? (b > 0 ? b : 0)
                                               : headerBase;
                    nb->appendExisting = appendExisting;
                    cur = nb.get();
                    allParts.push_back(std::move(nb));
                }
                groups[groupKey] = cur;
                groupPartNo[groupKey] = n;
                if (!cur->appendExisting)
                {
                    for (const auto &kv : co)
                        cplErrorStr(CE_Warning, CPLE_NotSupported,
                                    "driver " + driver +
                                        " does not support creation "
                                        "option " +
                                        kv.substr(0, kv.find('=')));
                    for (const auto &key : lcoUnsupported)
                        cplErrorStr(CE_Warning, CPLE_NotSupported,
                                    "dataset " + cur->path +
                                        " does not support layer creation "
                                        "option " +
                                        key);
                }

                if (shpOut && !cur->appendExisting)
                {
                    // list fields fail at part-create time: the part
                    // keeps the fields created before the failure (plus
                    // the 100-byte SHP/SHX headers written at close) and
                    // the whole run aborts, flushing earlier parts
                    int failIdx = -1;
                    for (size_t i = 0;
                         failIdx < 0 && i < lyr.fields.size(); ++i)
                    {
                        if (omit)
                        {
                            bool dropped = false;
                            for (int pi : partFieldIdx)
                                if (pi == (int)i)
                                    dropped = true;
                            if (dropped)
                                continue;
                        }
                        int t = lyr.fields[i].type;
                        if (t == OFTIntegerList ||
                            t == OFTInteger64List || t == OFTRealList ||
                            t == OFTStringList)
                            failIdx = (int)i;
                    }
                    if (failIdx >= 0)
                    {
                        OgrLayer skel;
                        skel.name = lyr.name;
                        skel.geomType = lyr.geomType;
                        skel.geomHasZ = lyr.geomHasZ;
                        skel.geomHasM = lyr.geomHasM;
                        skel.hasGeomField = lyr.hasGeomField;
                        skel.hasSrs = lyr.hasSrs;
                        skel.srs = lyr.srs;
                        for (int i = 0; i < failIdx; ++i)
                        {
                            if (omit)
                            {
                                bool dropped = false;
                                for (int pi : partFieldIdx)
                                    if (pi == i)
                                        dropped = true;
                                if (dropped)
                                    continue;
                            }
                            skel.fields.push_back(lyr.fields[i]);
                        }
                        auto pds = std::make_unique<OgrDataset>();
                        pds->path = cur->path;
                        pds->driverShort = "MEM";
                        pds->layers.push_back(std::move(skel));
                        runConvertDelegate(std::move(pds), input,
                                           cur->path, driver, {}, lcoPass,
                                           false, false, "", "partition");
                        const char *tn =
                            lyr.fields[failIdx].type == OFTIntegerList
                                ? "IntegerList"
                            : lyr.fields[failIdx].type ==
                                    OFTInteger64List
                                ? "Integer64List"
                            : lyr.fields[failIdx].type == OFTRealList
                                ? "RealList"
                                : "StringList";
                        cplErrorStr(CE_Failure, CPLE_NotSupported,
                                    std::string("Can't create fields of "
                                                "type ") +
                                        tn + " on shapefile layers.");
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "partition: Cannot create field '" +
                                        lyr.fields[failIdx].name + "'");
                        flushAll();
                        return 1;
                    }
                }
                if (shpOut)
                {
                    int t = shpTypeForGeomProbe(lyr.geomType, lyr.geomHasZ,
                                                lyr.geomHasM);
                    cur->shpType = t;
                    if (cur->shpType < 0 && feat.hasGeom)
                    {
                        int ft = shpTypeForGeomProbe(feat.geom.type,
                                                     feat.geom.hasZ,
                                                     false);
                        if (ft >= 0)
                            cur->shpType = ft;
                    }
                }
                flushOrder.push_back({cur, &lyr, partFieldIdx});
                if (shpOut && cur->shpType >= 0 && feat.hasGeom)
                {
                    std::string msg =
                        shpGeomMismatchError(cur->shpType, feat.geom);
                    if (!msg.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
                        if (skipErrors)
                        {
                            cplErrorStr(CE_Warning, CPLE_AppDefined,
                                        "partition: Cannot insert feature "
                                        "-1");
                            continue;
                        }
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "partition: Cannot insert feature -1");
                        flushAll();
                        return 1;
                    }
                }
            }

            long long featBytes = 0;
            if (maxSize)
            {
                featBytes = partFeatEstimate(lyr, feat);
                if (featBytes < 0)
                {
                    materializeCrash(groups);
                    raise(SIGSEGV);
                }
            }

            cur->feats.push_back(&feat);
            cur->count++;
            cur->bytes += featBytes;
            ++written;
            if (bar && total > 0)
                tp.update((double)written / (double)total);
        }
    }

    int rc = flushAll();
    return rc;
}

}  // namespace

void registerVectorIndexHandlers()
{
    registerHandler("vector_index", vectorIndexHandler);
    registerHandler("vector_partition", vectorPartitionHandler);
    registerArgValueCheck(
        "vector_index",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName != "dst-crs")
                return "";
            bool ok = false;
            Srs::fromCliInput(value, ok, true);
            if (!ok)
                return "Invalid value for 'dst-crs' argument";
            return "";
        });
    registerArgValueCheck(
        "vector_partition",
        [](const std::string &argName,
           const std::string &value) -> std::string {
            if (argName == "pattern" &&
                value.find('%') == std::string::npos)
                return "\x05Missing '%' character in pattern";
            if (argName == "max-file-size")
            {
                long long bytes = 0;
                if (!parseMaxFileSize(value, bytes))
                    return "\x05Invalid value for max-file-size";
                if (bytes < 1024 * 1024)
                    return "\x05max-file-size should be at least one MB";
            }
            return "";
        });
    registerPostValidator(
        "vector_index",
        [](const CmdSpec &, ParseResult &r, bool) -> bool {
            bool bad = false;
            std::string of = r.str("output-format");
            std::string out = r.str("output");
            if (strEqualNoCase(of, "GDALG") ||
                (of.empty() &&
                 strEndsWith(strToLower(out), ".gdalg.json")))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "index: GDALG output is not supported");
                bad = true;
            }
            if (r.get("source-crs-format") &&
                !r.get("source-crs-field-name"))
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "index: Option 'source-crs-name' must be "
                            "specified when 'source-crs-format' is "
                            "specified");
                bad = true;
            }
            return bad;
        });
    registerPostValidator(
        "vector_partition",
        [](const CmdSpec &, ParseResult &r, bool) -> bool {
            if (strEqualNoCase(r.str("output-format"), "GDALG"))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "partition: GDALG output is not supported.");
                return true;
            }
            if (r.list("field").empty() && !r.get("feature-limit") &&
                !r.get("max-file-size"))
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "partition: When 'fields' argument is not "
                            "specified, 'feature-limit' and/or "
                            "'max-file-size' must be specified");
                return true;
            }
            return false;
        });
}
