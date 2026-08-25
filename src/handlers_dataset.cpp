#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "jsonwriter.h"
#include "progress.h"
#include "tiff.h"
#include "util.h"
#include "xml_min.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace
{

bool pathExists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool isDirectory(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string dirNameOf(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

std::string baseNameOf(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string stemOf(const std::string &base)
{
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

std::string extOf(const std::string &base)
{
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? "" : base.substr(dot + 1);
}

std::vector<std::string> listDir(const std::string &p)
{
    std::vector<std::string> out;
    DIR *d = opendir(p.c_str());
    if (!d)
        return out;
    while (struct dirent *e = readdir(d))
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        out.push_back(e->d_name);
    }
    closedir(d);
    return out;
}

bool isGeoJsonType(const std::string &v)
{
    static const char *types[] = {
        "Feature",         "FeatureCollection", "Point",
        "LineString",      "Polygon",           "MultiPoint",
        "MultiLineString", "MultiPolygon",      "GeometryCollection"};
    for (const char *t : types)
        if (v == t)
            return true;
    return false;
}

bool looksLikeGeoJson(const std::string &head)
{
    size_t b = head.find_first_not_of(" \t\r\n");
    if (b == std::string::npos || (head[b] != '{' && head[b] != '['))
        return false;
    size_t pos = 0;
    while ((pos = head.find("\"type\"", pos)) != std::string::npos)
    {
        size_t q = pos + 6;
        while (q < head.size() &&
               (head[q] == ' ' || head[q] == '\t' || head[q] == '\r' ||
                head[q] == '\n'))
            ++q;
        if (q < head.size() && head[q] == ':')
        {
            ++q;
            while (q < head.size() &&
                   (head[q] == ' ' || head[q] == '\t' || head[q] == '\r' ||
                    head[q] == '\n'))
                ++q;
            if (q < head.size() && head[q] == '"')
            {
                size_t end = head.find('"', q + 1);
                if (end != std::string::npos &&
                    isGeoJsonType(head.substr(q + 1, end - q - 1)))
                    return true;
            }
        }
        pos += 6;
    }
    return false;
}

bool looksLikeDbf(const std::string &path)
{
    std::string lower = strToLower(extOf(baseNameOf(path)));
    if (lower != "dbf")
        return false;
    std::string content;
    if (!readFileToString(path, content) || content.size() < 32)
        return false;
    unsigned char v = (unsigned char)content[0];
    switch (v)
    {
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x7b:
        case 0x82:
        case 0x83:
        case 0x87:
        case 0x8b:
        case 0x8e:
        case 0xcb:
        case 0xf5:
        case 0xfb:
            return true;
        default:
            return false;
    }
}

// returns short driver name or "" when unrecognized
std::string identifyDriverName(const std::string &path)
{
    if (strStartsWith(path, "GTIFF_DIR:"))
    {
        std::string rest = path.substr(10);
        if (strStartsWith(rest, "off:"))
            rest = rest.substr(4);
        size_t colon = rest.find(':');
        if (colon == std::string::npos)
            return "";
        std::string file = rest.substr(colon + 1);
        return TiffFile::identify(file) ? "GTiff" : "";
    }
    if (isDirectory(path))
    {
        for (const auto &e : listDir(path))
        {
            std::string ext = strToLower(extOf(e));
            if (ext == "shp" || ext == "dbf")
                return "ESRI Shapefile";
        }
        return "";
    }
    if (TiffFile::identify(path))
        return "GTiff";
    if (looksLikeDbf(path))
        return "ESRI Shapefile";
    std::string content;
    if (readFileToString(path, content))
    {
        std::string head = content.substr(0, 6000);
        if (strStartsWith(head, "<VRTDataset"))
            return "VRT";
        if (head.size() >= 4 && (unsigned char)head[0] == 0 &&
            (unsigned char)head[1] == 0 && (unsigned char)head[2] == 0x27 &&
            (unsigned char)head[3] == 0x0a)
            return "ESRI Shapefile";
        if (looksLikeGeoJson(head))
            return "GeoJSON";
    }
    return "";
}

struct VrtInfo
{
    bool parsed = false;
    bool hasSrs = false;
    bool hasGt = false;
    std::vector<std::string> files;  // vrt itself first, then sources
    // per band, list of (source path, source band)
    std::vector<std::vector<std::pair<std::string, int>>> bandSources;
};

void collectVrtSources(const XmlNode &n, const std::string &vrtDir,
                       std::vector<std::pair<std::string, int>> &out)
{
    if (n.name == "SimpleSource" || n.name == "ComplexSource" ||
        n.name == "AveragedSource" || n.name == "KernelFilteredSource" ||
        n.name == "NoDataFromMaskSource")
    {
        const XmlNode *fn = n.child("SourceFilename");
        if (fn)
        {
            std::string p = fn->text;
            if (fn->attr("relativeToVRT") == "1" && !vrtDir.empty())
                p = vrtDir + "/" + p;
            int band = 1;
            if (const XmlNode *sb = n.child("SourceBand"))
                band = atoi(sb->text.c_str());
            out.emplace_back(p, band);
        }
    }
    for (const auto &c : n.children)
        collectVrtSources(c, vrtDir, out);
}

VrtInfo parseVrtInfo(const std::string &path)
{
    VrtInfo v;
    std::string content;
    if (!readFileToString(path, content))
        return v;
    XmlNode root;
    if (!xmlParse(content, root) || root.name != "VRTDataset")
        return v;
    v.parsed = true;
    v.hasSrs = root.child("SRS") != nullptr;
    v.hasGt = root.child("GeoTransform") != nullptr;
    v.files.push_back(path);
    std::string vrtDir = dirNameOf(path);
    for (const auto &c : root.children)
    {
        if (c.name != "VRTRasterBand")
            continue;
        std::vector<std::pair<std::string, int>> srcs;
        collectVrtSources(c, vrtDir, srcs);
        for (const auto &s : srcs)
        {
            bool seen = false;
            for (const auto &f : v.files)
                if (f == s.first)
                    seen = true;
            if (!seen)
                v.files.push_back(s.first);
        }
        v.bandSources.push_back(std::move(srcs));
    }
    return v;
}

std::vector<std::string> shapefileFileList(const std::string &path)
{
    std::vector<std::string> out;
    out.push_back(path);
    std::string dir = dirNameOf(path);
    std::string stem = stemOf(baseNameOf(path));
    static const char *exts[] = {"shp", "shx", "dbf", "prj", "cpg",
                                 "qix", "sbn", "sbx"};
    std::string selfExt = strToLower(extOf(baseNameOf(path)));
    for (const char *e : exts)
    {
        if (selfExt == e)
            continue;
        std::string cand =
            (dir.empty() ? stem : dir + "/" + stem) + "." + e;
        if (pathExists(cand))
            out.push_back(cand);
    }
    return out;
}

struct DetailInfo
{
    bool opened = false;
    std::string layout;
    std::vector<std::string> files;
    bool hasCrs = false, hasGt = false, hasOvr = false;
};

DetailInfo detailFor(const std::string &path, const std::string &driver)
{
    DetailInfo d;
    if (driver == "GTiff")
    {
        cplPushQuietHandler();
        std::string err;
        auto ds = openRaster(path, err);
        cplPopHandler();
        if (!ds)
            return d;
        d.opened = true;
        d.files = ds->files;
        if (const std::string *v = ds->getMd("IMAGE_STRUCTURE", "LAYOUT"))
            d.layout = *v;
        d.hasCrs = ds->hasSrs;
        d.hasGt = ds->hasGT;
        for (const auto &f : d.files)
            if (strEndsWith(strToLower(f), ".ovr"))
                d.hasOvr = true;
    }
    else if (driver == "VRT")
    {
        VrtInfo v = parseVrtInfo(path);
        if (!v.parsed)
            return d;
        d.opened = true;
        d.files = v.files;
        d.hasCrs = v.hasSrs;
        d.hasGt = v.hasGt;
    }
    else if (driver == "GeoJSON")
    {
        d.opened = true;
        d.files.push_back(path);
        d.hasCrs = true;
    }
    else if (driver == "ESRI Shapefile" && !isDirectory(path))
    {
        d.opened = true;
        d.files = shapefileFileList(path);
        std::string dir = dirNameOf(path);
        std::string stem = stemOf(baseNameOf(path));
        std::string prj =
            (dir.empty() ? stem : dir + "/" + stem) + ".prj";
        d.hasCrs = pathExists(prj);
    }
    return d;
}

struct IdentifyEntry
{
    std::string path;
    std::string driver;  // empty = unrecognized
    bool detailedOpened = false;
    DetailInfo detail;
};

struct IdentifyCtx
{
    bool detailed = false;
    bool reportFailures = false;
    bool recurse = false;
    bool force = false;
    bool toFile = false;
    bool jsonOut = false;
    bool textFile = false;
    std::string textBuf;
    std::vector<IdentifyEntry> entries;
};

void emitEntry(IdentifyCtx &ctx, const std::string &path,
               const std::string &driver)
{
    IdentifyEntry e;
    e.path = path;
    e.driver = driver;
    if (ctx.detailed && !driver.empty())
    {
        e.detail = detailFor(path, driver);
        e.detailedOpened = e.detail.opened;
    }
    if (ctx.textFile || (!ctx.toFile && !ctx.jsonOut))
    {
        std::string line;
        if (!driver.empty())
        {
            line = path + ": " + driver;
            if (e.detailedOpened)
            {
                if (!e.detail.layout.empty())
                    line += ", layout=" + e.detail.layout;
                if (e.detail.files.size() > 1)
                    line += ", has side-car files";
                if (e.detail.hasCrs)
                    line += ", has CRS";
                if (e.detail.hasGt)
                    line += ", has geotransform";
                if (e.detail.hasOvr)
                    line += ", has overview(s)";
            }
            line += "\n";
        }
        else if (ctx.reportFailures)
            line = path + ": unrecognized\n";
        if (ctx.textFile)
            ctx.textBuf += line;
        else
            fputs(line.c_str(), stdout);
    }
    if (driver.empty() && !ctx.reportFailures)
        return;
    ctx.entries.push_back(std::move(e));
}

void identifyProcess(IdentifyCtx &ctx, const std::string &path)
{
    std::string driver = identifyDriverName(path);
    // GDAL_SKIP'd drivers are unregistered: their files identify as
    // nothing (same rendering as unrecognized)
    if (!driver.empty() && gdalSkipHas(driver))
        driver.clear();
    bool dir = !strStartsWith(path, "GTIFF_DIR:") && isDirectory(path);
    if (!driver.empty())
    {
        emitEntry(ctx, path, driver);
        if (dir && ctx.force)
            for (const auto &c : listDir(path))
                identifyProcess(ctx, path + "/" + c);
        return;
    }
    if (dir && (ctx.recurse || ctx.force))
    {
        emitEntry(ctx, path, "");
        for (const auto &c : listDir(path))
            identifyProcess(ctx, path + "/" + c);
        return;
    }
    emitEntry(ctx, path, "");
}

std::string geoJsonFeature(const IdentifyEntry &e)
{
    std::string f = "{\"type\":\"Feature\",\"properties\":{";
    f += "\"filename\":\"" + JsonStreamWriter::escape(e.path) + "\"";
    if (!e.driver.empty())
        f += ",\"driver\":\"" + JsonStreamWriter::escape(e.driver) + "\"";
    if (e.detailedOpened)
    {
        if (!e.detail.layout.empty())
            f += ",\"layout\":\"" +
                 JsonStreamWriter::escape(e.detail.layout) + "\"";
        f += ",\"file_list\":[";
        for (size_t i = 0; i < e.detail.files.size(); ++i)
        {
            if (i)
                f += ",";
            f += "\"" + JsonStreamWriter::escape(e.detail.files[i]) + "\"";
        }
        f += "]";
        f += std::string(",\"has_crs\":") +
             (e.detail.hasCrs ? "true" : "false");
        f += std::string(",\"has_geotransform\":") +
             (e.detail.hasGt ? "true" : "false");
        f += std::string(",\"has_overview\":") +
             (e.detail.hasOvr ? "true" : "false");
    }
    f += "},\"geometry\":null}";
    return f;
}

std::string buildGeoJsonOutput(const std::vector<IdentifyEntry> &entries,
                               const std::string &layer)
{
    std::string out = "{\n\"type\": \"FeatureCollection\",\n\"name\": \"" +
                      JsonStreamWriter::escape(layer) + "\",\n";
    out += "\"features\": [\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        out += geoJsonFeature(entries[i]);
        out += i + 1 < entries.size() ? ",\n" : "\n";
    }
    out += "]\n}\n";
    return out;
}

std::string buildJsonArray(const std::vector<IdentifyEntry> &entries)
{
    if (entries.empty())
        return "[]";
    std::string out = "[\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const IdentifyEntry &e = entries[i];
        out += "  {\n";
        out += "    \"name\": \"" + JsonStreamWriter::escape(e.path) +
               "\",\n";
        out += "    \"driver\": ";
        out += e.driver.empty()
                   ? "null"
                   : "\"" + JsonStreamWriter::escape(e.driver) + "\"";
        if (e.detailedOpened)
        {
            if (!e.detail.layout.empty())
                out += ",\n    \"layout\": \"" +
                       JsonStreamWriter::escape(e.detail.layout) + "\"";
            out += ",\n    \"file_list\": [\n";
            for (size_t j = 0; j < e.detail.files.size(); ++j)
            {
                out += "      \"" +
                       JsonStreamWriter::escape(e.detail.files[j]) + "\"";
                out += j + 1 < e.detail.files.size() ? ",\n" : "\n";
            }
            out += "    ]";
            if (e.detail.hasCrs)
                out += ",\n    \"has_crs\": true";
            if (e.detail.hasGt)
                out += ",\n    \"has_geotransform\": true";
            if (e.detail.hasOvr)
                out += ",\n    \"has_overview\": true";
        }
        out += "\n  }";
        out += i + 1 < entries.size() ? ",\n" : "\n";
    }
    out += "]";
    return out;
}

std::string buildDbfOutput(const std::vector<IdentifyEntry> &entries)
{
    std::string d;
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    uint32_t nrec = (uint32_t)entries.size();
    d.push_back((char)0x03);
    d.push_back((char)lt.tm_year);
    d.push_back((char)(lt.tm_mon + 1));
    d.push_back((char)lt.tm_mday);
    for (int i = 0; i < 4; ++i)
        d.push_back((char)((nrec >> (8 * i)) & 0xff));
    uint16_t hdrSize = 97, recSize = 161;
    d.push_back((char)(hdrSize & 0xff));
    d.push_back((char)(hdrSize >> 8));
    d.push_back((char)(recSize & 0xff));
    d.push_back((char)(recSize >> 8));
    while (d.size() < 29)
        d.push_back('\0');
    d.push_back((char)0x57);  // ANSI language driver id
    d.push_back('\0');
    d.push_back('\0');
    const char *names[2] = {"filename", "driver"};
    for (const char *nm : names)
    {
        std::string field(nm);
        field.resize(11, '\0');
        d += field;
        d.push_back('C');
        for (int i = 0; i < 4; ++i)
            d.push_back('\0');
        d.push_back((char)80);
        d.push_back('\0');
        for (int i = 0; i < 14; ++i)
            d.push_back('\0');
    }
    d.push_back((char)0x0D);
    for (const auto &e : entries)
    {
        d.push_back(' ');
        std::string fn = e.path.substr(0, 80);
        fn.resize(80, ' ');
        d += fn;
        std::string dr = e.driver.substr(0, 80);
        dr.resize(80, ' ');
        d += dr;
    }
    d.push_back((char)0x1A);
    return d;
}

struct DriverEntry
{
    const char *name;
    bool vector;
    bool vectorCreate;
};

const DriverEntry kDrivers[] = {
    {"GTiff", false, false},        {"COG", false, false},
    {"VRT", false, false},          {"MEM", true, true},
    {"GNMFile", false, false},      {"GNMDatabase", false, false},
    {"ESRI Shapefile", true, true}, {"GeoJSON", true, true},
    {"GeoJSONSeq", true, true},     {"ESRIJSON", true, false},
    {"TopoJSON", true, false},      {"GDALG", false, false},
};

const DriverEntry *findDriver(const std::string &name)
{
    for (const auto &d : kDrivers)
        if (strEqualNoCase(name, d.name))
            return &d;
    return nullptr;
}

int datasetIdentifyHandler(const CmdSpec &, ParseResult &r)
{
    IdentifyCtx ctx;
    ctx.detailed = r.flag("detailed");
    ctx.reportFailures = r.flag("report-failures");
    ctx.recurse = r.flag("recursive");
    ctx.force = r.flag("force-recursive");
    std::string output = r.str("output");
    ctx.toFile = r.get("output") != nullptr;

    std::string format;
    std::string runtimeDriver;
    if (r.get("output-format") && r.str("output-format") == "json")
        ctx.jsonOut = true;
    else if (r.get("output-format") &&
             (strEqualNoCase(r.str("output-format"), "json") ||
              strEqualNoCase(r.str("output-format"), "text")))
    {
        // passes value validation like the lowercase builtins, but the
        // runtime driver lookup is case-sensitive
        if (r.str("output-format") != "text")
            runtimeDriver = r.str("output-format");
    }
    else if (r.get("output-format"))
    {
        std::string f = r.str("output-format");
        const DriverEntry *de = findDriver(f);
        if (de && strEqualNoCase(f, "GDALG"))
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "identify: GDALG output is not supported.");
            handlerPrintUsage();
            return 1;
        }
        if (!de)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: Invalid value for argument "
                        "'output-format'. Driver '" +
                            f + "' does not exist.");
            handlerPrintUsage();
            return 1;
        }
        if (!de->vector)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: Invalid value for argument "
                        "'output-format'. Driver '" +
                            f +
                            "' does not expose the required 'DCAP_VECTOR' "
                            "capability.");
            handlerPrintUsage();
            return 1;
        }
        if (!de->vectorCreate)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: Invalid value for argument "
                        "'output-format'. Driver '" +
                            f + "' does not have write support.");
            handlerPrintUsage();
            return 1;
        }
        format = de->name;
    }

    bool textOut =
        r.get("output-format") && r.str("output-format") == "text";

    if (!ctx.toFile &&
        (!runtimeDriver.empty() || (!format.empty() && format != "MEM")))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "identify: 'output' argument must be specified for "
                    "non-text or non-json output");
        return 1;
    }
    if (format == "MEM")
        ctx.toFile = true;
    ctx.textFile = ctx.toFile && textOut;

    if (ctx.toFile && format.empty() && !ctx.jsonOut && !textOut &&
        runtimeDriver.empty())
    {
        std::string ext = strToLower(extOf(baseNameOf(output)));
        if (ext == "json" || ext == "geojson")
            format = "GeoJSON";
        else if (ext == "dbf" || ext == "shp")
            format = "ESRI Shapefile";
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: Cannot guess driver for " + output);
            return 1;
        }
    }

    if (ctx.toFile && format != "MEM" && pathExists(output) &&
        !r.flag("overwrite"))
    {
        const char *kind =
            identifyDriverName(output).empty() ? "File" : "Dataset";
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    std::string("identify: ") + kind + " '" + output +
                        "' already exists. You may specify the --overwrite "
                        "option.");
        handlerPrintUsage();
        return 1;
    }

    if (!runtimeDriver.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "identify: Driver " + runtimeDriver + " does not exist");
        return 1;
    }

    for (const auto &in : r.list("filename"))
        identifyProcess(ctx, in);

    if (ctx.textFile)
    {
        if (!writeStringToFile(output, ctx.textBuf))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: cannot write " + output);
            return 1;
        }
        return 0;
    }

    if (ctx.jsonOut)
    {
        std::string out = buildJsonArray(ctx.entries);
        if (ctx.toFile)
        {
            if (!writeStringToFile(output, out))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "identify: cannot write " + output);
                return 1;
            }
        }
        else
            fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }

    if (!ctx.toFile || format == "MEM")
        return 0;

    std::string layer =
        r.get("output-layer") ? r.str("output-layer") : std::string();

    if (format == "GeoJSON")
    {
        if (!writeStringToFile(
                output,
                buildGeoJsonOutput(ctx.entries,
                                   layer.empty() ? "output" : layer)))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: cannot write " + output);
            return 1;
        }
        return 0;
    }
    if (format == "GeoJSONSeq")
    {
        std::string out;
        for (const auto &e : ctx.entries)
            out += geoJsonFeature(e) + "\n";
        if (!writeStringToFile(output, out))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "identify: cannot write " + output);
            return 1;
        }
        return 0;
    }

    // ESRI Shapefile: a null-geometry layer materializes as a lone .dbf
    std::string target = output;
    std::string ext = strToLower(extOf(baseNameOf(output)));
    if (ext == "dbf" || ext == "shp")
    {
        if (ext == "shp")
            target = output.substr(0, output.size() - 4) + ".dbf";
        if (pathExists(target) && r.flag("overwrite"))
            remove(target.c_str());
    }
    else
    {
        if (!isDirectory(output))
            mkdir(output.c_str(), 0755);
        std::string stem = layer.empty() ? stemOf(baseNameOf(output)) : layer;
        target = output + "/" + stem + ".dbf";
    }
    if (!writeStringToFile(target, buildDbfOutput(ctx.entries)))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "identify: cannot write " + output);
        return 1;
    }
    return 0;
}

// file list of a dataset for copy/rename/delete; empty on failure with
// errors already reported
bool datasetFileList(const std::string &path, const std::string &driver,
                     std::vector<std::string> &files)
{
    if (driver == "GTiff")
    {
        std::string err;
        auto ds = openRaster(path, err);
        if (!ds)
        {
            if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + path +
                                "' not recognized as being in a supported "
                                "file format.");
            return false;
        }
        // file-level dataset ops never scan the directory chain, so the
        // lazy re-read diagnostics stay unreplayed
        files = ds->files;
        return true;
    }
    if (driver == "VRT")
    {
        VrtInfo v = parseVrtInfo(path);
        if (!v.parsed)
        {
            files = {path};
            return true;
        }
        files = v.files;
        return true;
    }
    if (driver == "ESRI Shapefile" && !isDirectory(path))
    {
        files = shapefileFileList(path);
        return true;
    }
    files = {path};
    return true;
}

bool copyFile(const std::string &src, const std::string &dst)
{
    std::string content;
    if (!readFileToString(src, content))
        return false;
    return writeStringToFile(dst, content);
}

int copyRenameCommon(ParseResult &r, bool isRename)
{
    const char *alg = isRename ? "rename" : "copy";
    std::string src = r.str("source");
    std::string dst = r.str("destination");

    if (r.get("format"))
    {
        std::string f = r.str("format");
        if (!findDriver(f) || strEqualNoCase(f, "GDALG"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        std::string(alg) +
                            ": Invalid value for argument 'format'. "
                            "Driver '" +
                            f + "' does not exist.");
            handlerPrintUsage();
            return 1;
        }
    }

    if (pathExists(dst))
    {
        if (!r.flag("overwrite"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        std::string(alg) + ": Dataset '" + dst +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
        std::string dstDriver = identifyDriverName(dst);
        std::vector<std::string> dfiles{dst};
        if (dstDriver == "VRT")
            dfiles = {dst};
        else if (!dstDriver.empty())
        {
            cplPushQuietHandler();
            if (!datasetFileList(dst, dstDriver, dfiles))
                dfiles = {dst};
            cplPopHandler();
        }
        for (const auto &f : dfiles)
            remove(f.c_str());
    }

    std::string driver = identifyDriverName(src);
    if (driver.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "No identifiable driver for " + src + ".");
        return 1;
    }

    std::vector<std::string> files;
    if (!datasetFileList(src, driver, files))
        return 1;

    std::string srcBase = baseNameOf(src), dstBase = baseNameOf(dst);
    std::string srcStem = stemOf(srcBase), dstStem = stemOf(dstBase);
    std::string srcExt = extOf(srcBase), dstExt = extOf(dstBase);
    std::string dstDir = dirNameOf(dst);

    if (files.size() > 1 && !strEqualNoCase(srcExt, dstExt))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Unable to copy/rename fileset due to irregular "
                    "filename correspondence.");
        return 1;
    }

    std::vector<std::pair<std::string, std::string>> moves;
    for (const auto &f : files)
    {
        std::string fb = baseNameOf(f);
        std::string to;
        if (fb == srcBase)
            to = dst;
        else if (strStartsWith(fb, srcStem + "."))
            to = (dstDir.empty() ? std::string() : dstDir + "/") + dstStem +
                 fb.substr(srcStem.size());
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unable to copy/rename fileset due irregular "
                        "basenames.");
            return 1;
        }
        moves.emplace_back(f, to);
    }

    for (const auto &m : moves)
    {
        if (isRename)
        {
            if (pathExists(m.second))
                remove(m.second.c_str());
            if (rename(m.first.c_str(), m.second.c_str()) != 0)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            std::string(alg) + ": Cannot rename " +
                                m.first + " to " + m.second);
                return 1;
            }
        }
        else if (!copyFile(m.first, m.second))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        std::string(alg) + ": Cannot copy " + m.first +
                            " to " + m.second);
            return 1;
        }
    }
    return 0;
}

int datasetCopyHandler(const CmdSpec &, ParseResult &r)
{
    return copyRenameCommon(r, false);
}

int datasetRenameHandler(const CmdSpec &, ParseResult &r)
{
    return copyRenameCommon(r, true);
}

int datasetDeleteHandler(const CmdSpec &, ParseResult &r)
{
    if (r.get("format"))
    {
        std::string f = r.str("format");
        if (!findDriver(f) || strEqualNoCase(f, "GDALG"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "delete: Invalid value for argument 'format'. "
                        "Driver '" +
                            f + "' does not exist.");
            handlerPrintUsage();
            return 1;
        }
    }
    for (const auto &path : r.list("filename"))
    {
        std::string driver = identifyDriverName(path);
        if (driver.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "No identifiable driver for " + path + ".");
            return 1;
        }
        std::vector<std::string> files;
        if (driver == "VRT")
            files = {path};  // the VRT driver never deletes source files
        else if (!datasetFileList(path, driver, files))
            return 1;
        for (const auto &f : files)
            remove(f.c_str());
    }
    return 0;
}

int datasetCheckHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.str("input");
    bool quiet = r.flag("quiet");

    OpenOptions oo;
    oo.allowedDrivers = r.list("input-format");
    for (const auto &kv : r.list("open-option"))
    {
        size_t eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
        oo.raw.emplace_back(key, val);
    }

    std::string driver = identifyDriverName(input);
    TermProgress tp;

    if (driver == "GeoJSON" || driver == "ESRI Shapefile")
    {
        if (!quiet)
            tp.update(1.0);
        return 0;
    }

    if (driver == "VRT")
    {
        VrtInfo v = parseVrtInfo(input);
        if (!v.parsed)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported file "
                            "format.");
            handlerPrintUsage();
            return 1;
        }
        size_t nb = v.bandSources.size();
        for (size_t b = 0; b < nb; ++b)
        {
            for (const auto &s : v.bandSources[b])
            {
                cplPushQuietHandler();
                std::string err;
                auto sds = openRaster(s.first, err, {});
                cplPopHandler();
                if (!sds)
                    continue;
                std::vector<uint8_t> buf;
                if (!sds->readBandRawStrict(s.second, buf))
                    return 1;
            }
            if (!quiet)
                tp.update((double)(b + 1) / (double)nb);
        }
        return 0;
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
    if (ds->driverShort == "GTiff")
        cplDebug("GTiff", "ScanDirectories()");
    ds->replayDeferred();
    if (ds->openHadErrors)
        return 1;

    size_t nb = ds->bands.size();
    for (size_t b = 0; b < nb; ++b)
    {
        std::vector<uint8_t> buf;
        if (!ds->readBandRawStrict((int)b + 1, buf))
            return 1;
        if (!quiet)
            tp.update((double)(b + 1) / (double)nb);
    }
    debugCloseDataset(*ds);
    return 0;
}

}  // namespace

// GDALDriver::QuietDelete emulation used by convert-like --overwrite
void overwriteDeleteFileset(const std::string &path)
{
    cplPushQuietHandler();
    std::string driver = identifyDriverName(path);
    cplPopHandler();
    if (isDirectory(path))
    {
        if (driver.empty())
            return;
        // shapefile directory: layer filesets go, stray files stay
        DIR *d = opendir(path.c_str());
        if (d)
        {
            std::vector<std::string> victims;
            while (struct dirent *e = readdir(d))
            {
                std::string n = e->d_name;
                std::string ext = strToLower(extOf(n));
                if (ext == "shp" || ext == "shx" || ext == "dbf" ||
                    ext == "prj" || ext == "cpg" || ext == "qix" ||
                    ext == "sbn" || ext == "sbx")
                    victims.push_back(path + "/" + n);
            }
            closedir(d);
            for (const auto &f : victims)
                remove(f.c_str());
        }
        rmdir(path.c_str());
        return;
    }
    cplPushQuietHandler();
    std::vector<std::string> files{path};
    if (!driver.empty() && driver != "VRT")
        if (!datasetFileList(path, driver, files) || files.empty())
            files = {path};
    cplPopHandler();
    for (const auto &f : files)
        remove(f.c_str());
}

std::string outputExistsKind(const std::string &path)
{
    cplPushQuietHandler();
    std::string driver = identifyDriverName(path);
    cplPopHandler();
    if (!driver.empty())
        return "Dataset";
    return isDirectory(path) ? "Directory" : "File";
}

void registerDatasetHandlers()
{
    registerHandler("dataset_identify", datasetIdentifyHandler);
    registerHandler("dataset_copy", datasetCopyHandler);
    registerHandler("dataset_rename", datasetRenameHandler);
    registerHandler("dataset_delete", datasetDeleteHandler);
    registerHandler("dataset_check", datasetCheckHandler);
}
