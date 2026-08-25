// `gdal completion <words...>`: shell completion candidates for the word
// being typed.  Candidates are space-joined with spaces inside a candidate
// escaped by backslash; no trailing newline.
#include "complete.h"
#include "embedded.h"
#include "engine.h"
#include "json.h"
#include "ogr.h"
#include "proj_min.h"
#include "spec.h"
#include "util.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

const JVal &completeData()
{
    static JVal data = [] {
        bool ok = false;
        JVal v = JVal::parse(embGet("misc/complete.json"), &ok);
        return v;
    }();
    return data;
}

std::string escTok(const std::string &s)
{
    std::string out;
    for (char c : s)
    {
        if (c == ' ')
            out += '\\';
        out += c;
    }
    return out;
}

std::string joinEsc(const std::vector<std::string> &toks)
{
    std::string out;
    for (size_t i = 0; i < toks.size(); ++i)
    {
        if (i)
            out += ' ';
        out += escTok(toks[i]);
    }
    return out;
}

bool ciEqual(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

std::string hintFor(const std::string &desc)
{
    return "** " + escTok("\xc2\xa0"
                          "description: " +
                          desc);
}

// tms_NZTM2000.json warning choreography: emitted (once per instantiation of
// the raster tile algorithm) when GDAL_DATA is not set
int g_tileWarn = 0;

void markTileWarn(int n)
{
    g_tileWarn += n;
}

void flushTileWarn()
{
    if (getenv("GDAL_DATA"))
        return;
    for (int i = 0; i < g_tileWarn; ++i)
        fprintf(stderr, "Warning 3: Cannot find tms_NZTM2000.json "
                        "(GDAL_DATA is not defined)\n");
}

// ----- CRS catalogs (via PROJ) -------------------------------------------

const char *const kAuthorities[] = {"EPSG", "ESRI", "IAU_2015",
                                    "IGNF", "NKG",  "OGC"};

std::string authorityList(const std::string &specials)
{
    std::string out = specials;
    for (const char *a : kAuthorities)
    {
        if (!out.empty())
            out += ' ';
        out += a;
        out += ':';
    }
    return out;
}

bool crsCodes(const std::string &auth, const std::string &prefix,
              std::string &out)
{
    int n = 0;
    PROJ_CRS_INFO **list =
        proj_get_crs_info_list_from_database(nullptr, auth.c_str(), nullptr,
                                             &n);
    if (!list)
        return false;
    out.clear();
    size_t count = 0;
    std::string lastCode;
    for (int i = 0; i < n; ++i)
    {
        const PROJ_CRS_INFO *c = list[i];
        if (c->deprecated)
            continue;
        if (!prefix.empty() &&
            strncmp(c->code, prefix.c_str(), prefix.size()) != 0)
            continue;
        const char *suf = "";
        if (c->type == PJ_TYPE_GEOGRAPHIC_2D_CRS)
            suf = " (geographic 2D)";
        else if (c->type == PJ_TYPE_GEOGRAPHIC_3D_CRS)
            suf = " (geographic 3D)";
        else if (c->type == PJ_TYPE_GEOCENTRIC_CRS)
            suf = " (geocentric)";
        ++count;
        lastCode = c->code;
        if (!out.empty())
            out += ' ';
        out += escTok(std::string(c->code) + " -- " + c->name + suf);
    }
    proj_crs_info_list_destroy(list);
    if (count == 0)
        return false;
    if (count == 1)
        out = escTok(lastCode);
    return true;
}

// The specials are prepended unfiltered; typing one of them exactly ends
// completion.  Authority names appear only for an empty word (or, without
// specials, as the universal fallback).
std::string crsComplete(const std::string &specials, const std::string &val)
{
    std::vector<std::string> spec;
    {
        std::string cur;
        for (char c : specials + " ")
        {
            if (c == ' ')
            {
                if (!cur.empty())
                    spec.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
    }
    for (const auto &s : spec)
        if (val == s)
            return "";
    auto withSpecials = [&](const std::string &rest) {
        std::string sp = joinEsc(spec);
        if (sp.empty())
            return rest;
        if (rest.empty())
            return sp;
        return sp + " " + rest;
    };
    auto noMatch = [&]() {
        return spec.empty() ? authorityList("") : joinEsc(spec);
    };
    if (val.empty())
        return withSpecials(authorityList(""));
    std::string out;
    size_t colon = val.find(':');
    if (colon != std::string::npos)
    {
        std::string auth = val.substr(0, colon);
        for (const char *a : kAuthorities)
            if (ciEqual(auth, a))
            {
                if (crsCodes(a, val.substr(colon + 1), out))
                    return withSpecials(out);
                return noMatch();
            }
        return noMatch();
    }
    for (const char *a : kAuthorities)
        if (ciEqual(val, a))
        {
            if (crsCodes(a, "", out))
                return withSpecials(out);
            return noMatch();
        }
    return noMatch();
}

// ----- filename completion ------------------------------------------------

enum class FileFilter
{
    All,
    Raster,
    Vector,
    Both,
    Multidim,
    RasterVectorMultidim
};

bool extMatches(FileFilter f, const std::string &name)
{
    if (f == FileFilter::All)
        return true;
    size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = strToLower(name.substr(dot + 1));
    bool raster = ext == "tif" || ext == "tiff" || ext == "vrt";
    bool vector = ext == "shp" || ext == "dbf" || ext == "shz" ||
                  ext == "json" || ext == "geojson" || ext == "geojsonl" ||
                  ext == "geojsons";
    bool multidim = ext == "vrt";
    switch (f)
    {
        case FileFilter::Raster:
            return raster;
        case FileFilter::Vector:
            return vector;
        case FileFilter::Both:
        case FileFilter::RasterVectorMultidim:
            return raster || vector;
        case FileFilter::Multidim:
            return multidim;
        default:
            return true;
    }
}

void fileScan(FileFilter filter, const std::string &prefix, bool withDirs,
              std::vector<std::string> &toks)
{
    size_t slash = prefix.rfind('/');
    std::string dirPart =
        slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
    std::string base =
        slash == std::string::npos ? prefix : prefix.substr(slash + 1);
    std::string scanDir = dirPart.empty() ? "." : dirPart;
    DIR *d = opendir(scanDir.c_str());
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
    {
        std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        if (!base.empty() && !strStartsWith(name, base))
            continue;
        std::string full = dirPart + name;
        struct stat st;
        bool isDir = false;
        if (stat(full.c_str(), &st) == 0)
            isDir = S_ISDIR(st.st_mode);
        if (isDir)
        {
            if (withDirs)
                toks.push_back(full + "/");
        }
        else if (extMatches(filter, name))
            toks.push_back(full);
    }
    closedir(d);
}

// a word that already names an existing candidate file yields no further
// suggestions
void dropExact(std::vector<std::string> &toks, const std::string &prefix)
{
    for (const auto &t : toks)
        if (t == prefix && !t.empty() && t.back() != '/')
        {
            toks.clear();
            return;
        }
}

std::string fileComplete(FileFilter filter, const std::string &prefix)
{
    std::vector<std::string> toks;
    fileScan(filter, prefix, true, toks);
    dropExact(toks, prefix);
    return joinEsc(toks);
}

// raster pass (with directories) followed by a vector-only pass, as used by
// pipeline read dataset completion.  When completing an option value each
// empty pass falls back: the raster pass to the argument's description hint,
// the vector pass to the full unfiltered listing (directories included);
// positional words get no fallbacks.
std::string fileCompleteTwoPass(const std::string &prefix,
                                const std::string &rasterHint,
                                bool withFallbacks)
{
    std::vector<std::string> toks;
    fileScan(FileFilter::Raster, prefix, true, toks);
    dropExact(toks, prefix);
    std::string out = joinEsc(toks);
    if (toks.empty() && withFallbacks && !rasterHint.empty())
        out = rasterHint;
    std::vector<std::string> vtoks;
    fileScan(FileFilter::Vector, prefix, false, vtoks);
    dropExact(vtoks, prefix);
    if (vtoks.empty() && withFallbacks)
        fileScan(FileFilter::Vector, "", true, vtoks);
    std::string vout = joinEsc(vtoks);
    if (!out.empty() && !vout.empty())
        out += ' ';
    return out + vout;
}

FileFilter filterFromTypes(const std::vector<std::string> &types)
{
    bool raster = false, vector = false, multidim = false;
    for (const auto &t : types)
    {
        if (t == "raster")
            raster = true;
        else if (t == "vector")
            vector = true;
        else if (t == "multidim_raster")
            multidim = true;
    }
    if (raster && vector)
        return FileFilter::RasterVectorMultidim;
    if (raster)
        return FileFilter::Raster;
    if (vector)
        return FileFilter::Vector;
    if (multidim)
        return FileFilter::Multidim;
    return FileFilter::All;
}

// ----- driver option catalogs ---------------------------------------------

std::string extDriver(const std::string &path, const std::string &family)
{
    size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return "";
    std::string ext = strToLower(path.substr(dot + 1));
    if (family == "raster" || family == "mdim")
    {
        if (ext == "tif" || ext == "tiff")
            return "GTiff";
        if (ext == "vrt")
            return "VRT";
        return "";
    }
    if (ext == "shp" || ext == "dbf" || ext == "shz")
        return "ESRI Shapefile";
    if (ext == "json" || ext == "geojson")
        return "GeoJSON";
    if (ext == "geojsonl" || ext == "geojsons")
        return "GeoJSONSeq";
    return "";
}

// kind: "co", "lco" or "oo"; family: "raster"/"vector"/"mdim".
// Returns false if no catalog entry exists for the driver.
bool driverOptComplete(const std::string &family, const std::string &kind,
                       const std::string &driver, const std::string &val,
                       std::string &out, bool warnCog = true)
{
    if (driver.empty())
        return false;
    std::string fam = family == "mdim" ? "raster" : family;
    const JVal *cat = completeData().get("drvopts");
    if (!cat)
        return false;
    const JVal *bykind = cat->get(fam + "_" + kind);
    if (!bykind)
        return false;
    const JVal *ent = nullptr;
    for (const auto &kv : bykind->obj)
        if (ciEqual(kv.first, driver))
            ent = &kv.second;
    if (!ent)
        return false;
    if (warnCog && ciEqual(driver, "COG") && kind == "co")
        markTileWarn(1);
    size_t eq = val.find('=');
    if (eq != std::string::npos)
    {
        std::string key = val.substr(0, eq);
        const JVal *values = ent->get("values");
        if (values)
        {
            const JVal *v = values->get(key);
            if (v)
            {
                out = v->s;
                return true;
            }
        }
    }
    out = ent->getString("keys");
    return true;
}

// ----- option universes ---------------------------------------------------

std::string cmdFamily(const std::string &id)
{
    if (strStartsWith(id, "raster_"))
        return "raster";
    if (strStartsWith(id, "vector_"))
        return "vector";
    if (strStartsWith(id, "mdim_"))
        return "mdim";
    if (strStartsWith(id, "dataset_"))
        return "dataset";
    return "";
}

std::string universe(const std::string &key)
{
    const JVal *u = completeData().get("universes");
    return u ? u->getString(key) : "";
}

const JVal *cmdRec(const std::string &cid)
{
    const JVal *c = completeData().get("cmds");
    return c ? c->get(cid) : nullptr;
}

const JVal *cmdArgRec(const std::string &cid, const std::string &argName)
{
    const JVal *c = cmdRec(cid);
    if (!c)
        return nullptr;
    const JVal *a = c->get("args");
    return a ? a->get(argName) : nullptr;
}

// Only a handful of captured static value lists are prefix-filtered by the
// reference (their autocomplete functions filter; most return the full list
// and let the shell filter).
std::string staticValueList(const std::string &list, const std::string &an,
                            const std::string &val, const std::string &desc)
{
    bool filtered = an == "geometry-type" || an == "field-type" ||
                    an == "src-field-type";
    if (!filtered || val.empty())
        return list;
    std::vector<std::string> keep;
    std::string cur;
    for (char c : list + " ")
    {
        if (c == ' ')
        {
            if (!cur.empty() && strStartsWith(cur, val))
                keep.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    if (keep.empty())
        return hintFor(desc);
    std::string out;
    for (const auto &t : keep)
    {
        if (!out.empty())
            out += ' ';
        out += t;
    }
    return out;
}

FileFilter filterFromName(const std::string &fk)
{
    if (fk == "raster")
        return FileFilter::Raster;
    if (fk == "vector")
        return FileFilter::Vector;
    if (fk == "both")
        return FileFilter::Both;
    if (fk == "mdim")
        return FileFilter::Multidim;
    return FileFilter::All;
}

// ----- tree-mode state -----------------------------------------------------

struct TreeCtx
{
    const CmdSpec *node = nullptr;
    const ArgSpec *pending = nullptr;
    bool pendingConfig = false;
    bool configUsed = false;  // a --config KEY=VALUE was fully consumed;
                              // the reference then only offers the top list
    std::vector<std::string> positionals;
    std::map<std::string, std::string> optValues;  // canonical name -> value
};

const ArgSpec *findArg(const CmdSpec *node, const std::string &name)
{
    for (const auto &a : node->args)
    {
        if (ciEqual(a.name, name))
            return &a;
        for (const auto &al : a.aliases)
            if (ciEqual(al, name))
                return &a;
        for (const auto &al : a.hiddenAliases)
            if (ciEqual(al, name))
                return &a;
    }
    for (const auto &a : node->args)
        for (const auto &s : a.shorts)
            if (s == name)
                return &a;
    return nullptr;
}

std::vector<std::string> optionUniverse(const CmdSpec *node)
{
    std::vector<std::string> toks;
    for (const auto &a : node->args)
    {
        if (a.kind == "output_arguments")
            continue;
        toks.push_back("--" + a.name);
    }
    if (node->id == "ROOT")
    {
        toks.push_back("--config");
        toks.push_back("--help");
        toks.push_back("--json-usage");
        toks.push_back("--version");
    }
    std::sort(toks.begin(), toks.end());
    return toks;
}

// positional-arg helpers
std::vector<const ArgSpec *> positionalArgs(const CmdSpec *node)
{
    std::vector<const ArgSpec *> out;
    for (const auto &a : node->args)
        if (a.positional >= 0)
            out.push_back(&a);
    std::sort(out.begin(), out.end(),
              [](const ArgSpec *a, const ArgSpec *b) {
                  return a->positional < b->positional;
              });
    return out;
}

const ArgSpec *currentPositional(const CmdSpec *node, size_t consumed)
{
    for (const ArgSpec *a : positionalArgs(node))
    {
        size_t cap = a->isList()
                         ? SIZE_MAX
                         : (a->maxCount > 1 ? (size_t)a->maxCount : 1);
        if (cap == SIZE_MAX || consumed < cap)
            return a;
        consumed -= cap;
    }
    return nullptr;
}

// completing a vector positional with a directory input makes GDAL try a
// shapefile-directory open, whose failures leak onto stderr
void emitShapefileDirErrors(const std::string &inPath)
{
    if (inPath.empty())
        return;
    std::string dir = inPath;
    while (dir.size() > 1 && dir.back() == '/')
        dir.pop_back();
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    std::vector<std::string> shps;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
    {
        std::string n = e->d_name;
        if (n.size() > 4 && strEndsWith(strToLower(n), ".shp"))
            shps.push_back(n);
    }
    closedir(d);
    std::sort(shps.begin(), shps.end());
    for (const std::string &n : shps)
    {
        std::string base = n.substr(0, n.size() - 4);
        std::string shp = dir + "/" + n;
        std::string shx = dir + "/" + base + ".shx";
        struct stat shpSt, shxSt;
        if (stat(shp.c_str(), &shpSt) != 0)
            continue;
        if (stat(shx.c_str(), &shxSt) != 0)
        {
            fprintf(stderr,
                    "ERROR 4: Unable to open %s/%s.shx or %s/%s.SHX. Set "
                    "SHAPE_RESTORE_SHX config option to YES to restore or "
                    "create it.\n",
                    dir.c_str(), base.c_str(), dir.c_str(), base.c_str());
        }
        else if (shpSt.st_size > 0 && shxSt.st_size < 100)
        {
            fprintf(stderr, "ERROR 4: .shx file is unreadable, or corrupt.\n");
        }
        else
            continue;
        fprintf(stderr,
                "ERROR 4: Failed to open file %s.It may be corrupt or "
                "read-only file accessed in update mode.\n",
                shp.c_str());
    }
}

// commands whose unbounded input truly accepts several datasets fill
// positionals right to left, so a lone word lands on the output
bool r2lCmd(const std::string &id)
{
    return id == "raster_calc" || id == "raster_footprint" ||
           id == "raster_index" || id == "raster_mosaic" ||
           id == "raster_stack" || id == "vector_concat" ||
           id == "vector_index" || strStartsWith(id, "vector_grid_");
}

// assign already-typed positional words to args; returns value for the arg
// with the given name ("" when absent)
std::string positionalValueFor(const TreeCtx &ctx, const std::string &name)
{
    auto args = positionalArgs(ctx.node);
    size_t wi = 0;
    for (size_t ai = 0; ai < args.size() && wi < ctx.positionals.size(); ++ai)
    {
        const ArgSpec *a = args[ai];
        size_t cap = a->isList()
                         ? SIZE_MAX
                         : (a->maxCount > 1 ? (size_t)a->maxCount : 1);
        size_t take = cap;
        if (cap == SIZE_MAX)
        {
            size_t reserve = 0;
            for (size_t aj = ai + 1; aj < args.size(); ++aj)
                reserve += 1;
            size_t avail = ctx.positionals.size() - wi;
            take = avail > reserve ? avail - reserve : 0;
            if (a->required && take == 0 && avail > 0 &&
                !r2lCmd(ctx.node->id))
                take = 1;
        }
        size_t end = wi + take;
        if (end > ctx.positionals.size())
            end = ctx.positionals.size();
        if (ciEqual(a->name, name) && end > wi)
            return ctx.positionals[end - 1];
        wi = end;
    }
    return "";
}

std::string argContextValue(const TreeCtx &ctx, const std::string &name)
{
    auto it = ctx.optValues.find(name);
    if (it != ctx.optValues.end())
        return it->second;
    return positionalValueFor(ctx, name);
}

std::string valueComplete(const TreeCtx &ctx, const ArgSpec *arg,
                          const std::string &val, bool positional = false)
{
    const CmdSpec *node = ctx.node;
    const std::string &an = arg->name;
    std::string fam = cmdFamily(node->id);
    if (an == "creation-option" || an == "layer-creation-option")
    {
        bool lco = an == "layer-creation-option";
        // creation options follow the output family, which differs from
        // the command family for rasterizing/vectorizing verbs
        std::string cofam = fam;
        if (lco)
            cofam = "vector";
        else if (node->id == "vector_rasterize" ||
                 strStartsWith(node->id, "vector_grid_"))
            cofam = "raster";
        else if (node->id == "raster_footprint" ||
                 node->id == "raster_polygonize" ||
                 node->id == "raster_contour")
            cofam = "vector";
        if (node->id == "raster_overview_add")
        {
            std::string ds = strToLower(argContextValue(ctx, "input"));
            if (strEndsWith(ds, ".tif") || strEndsWith(ds, ".tiff"))
                return "LOCATION= COMPRESS= BLOCKSIZE= NUM_THREADS= "
                       "PREDICTOR= JPEG_QUALITY= JPEGTABLESMODE= ZLEVEL= "
                       "ZSTD_LEVEL= WEBP_LEVEL= INTERLEAVE= PHOTOMETRIC= "
                       "BIGTIFF= ALPHA=";
        }
        std::string drv = argContextValue(ctx, "output-format");
        if (drv.empty())
            drv = extDriver(argContextValue(ctx, "output"), cofam);
        std::string out;
        if (driverOptComplete(cofam, lco ? "lco" : "co", drv, val, out))
            return out;
        const JVal *rec = cmdArgRec(node->id, an);
        if (rec && rec->getString("k") == "static")
            return rec->getString("v");
        return hintFor(arg->description);
    }
    if (an == "open-option")
    {
        std::string drv = argContextValue(ctx, "input-format");
        if (drv.empty())
        {
            std::string in = argContextValue(ctx, "input");
            if (in.empty())
                in = argContextValue(ctx, "filename");
            if (in.empty())
                in = argContextValue(ctx, "dataset");
            drv = extDriver(in, fam);
        }
        std::string out;
        if (driverOptComplete(fam, "oo", drv, val, out))
            return out;
        const JVal *rec = cmdArgRec(node->id, an);
        if (rec && rec->getString("k") == "static")
            return rec->getString("v");
        return hintFor(arg->description);
    }
    if (!positional && (an == "input-layer" || an == "layer"))
    {
        std::string in = argContextValue(ctx, "input");
        if (in.empty())
            in = argContextValue(ctx, "dataset");
        if (in.empty())
            in = argContextValue(ctx, "filename");
        if (!in.empty())
        {
            std::string lerr;
            auto ds = openVectorDataset(in, lerr, {});
            if (ds)
            {
                std::vector<std::string> names;
                for (const auto &l : ds->layers)
                    names.push_back(l.name);
                return joinEsc(names);
            }
        }
    }
    const JVal *rec = cmdArgRec(node->id, an);
    if (rec)
    {
        std::string k = rec->getString("k");
        if (k == "files")
        {
            std::string res =
                fileComplete(filterFromName(rec->getString("v")), val);
            if (res.empty() && !positional)
                return hintFor(arg->description);
            return res;
        }
        if (!positional)
        {
            if (k == "empty")
                return "";
            if (k == "static")
                return arg->choices.empty()
                           ? staticValueList(rec->getString("v"), an, val,
                                             arg->description)
                           : rec->getString("v");
            if (k == "config")
                return completeData().getString("configkeys");
            if (k == "crs")
                return crsComplete(rec->getString("v"), val);
        }
    }
    bool crsName = an == "crs" || (an.size() > 4 &&
                                   an.compare(an.size() - 4, 4, "-crs") == 0);
    if (crsName && (arg->type == "string" || arg->type == "string_list"))
    {
        if (positional)
            return "";
        std::string specials;
        if ((node->id == "raster_create" || node->id == "raster_edit" ||
             node->id == "vector_edit") &&
            an == "crs")
            specials = "none";
        if (node->id == "raster_pixel-info" && an == "position-crs")
            specials = "pixel dataset";
        return crsComplete(specials, val);
    }
    if (!arg->choices.empty())
    {
        if (positional)
            return "";
        std::vector<std::string> c = arg->choices;
        std::sort(c.begin(), c.end());
        return joinEsc(c);
    }
    if (an == "output-format" || an == "input-format" || an == "format")
    {
        if (positional)
            return "";
        if (fam == "raster" || fam == "vector" || fam == "mdim")
            return universe(fam + (an == "input-format" ? "_in" : "_out"));
        if (fam == "dataset" && an == "format")
            return universe("dataset_format");
        if (node->id == "convert" && an == "output-format")
            return hintFor(arg->description);
        return "";
    }
    if (arg->isDataset())
    {
        if (arg->positional < 0)
            return hintFor(arg->description);
        return fileComplete(filterFromTypes(arg->datasetType), val);
    }
    if (arg->type == "string" || arg->type == "string_list")
    {
        if (an == "filename" || an == "source" || an == "destination")
            return fileComplete(FileFilter::All, val);
    }
    if (arg->isBool())
        return "";
    return positional ? "" : hintFor(arg->description);
}

// ----- pipeline-mode state -------------------------------------------------

struct PipeCtx
{
    std::string entry;  // pipeline / raster_pipeline / vector_pipeline
    bool firstSegment = true;
    bool sawEntryOpt = false;  // option before the first step name kills
                               // step completion entirely
    std::string step;
    std::string pending;  // canonical option name expecting a value
    std::string readPath;
    std::map<std::string, std::string> stepValues;
};

const JVal *pipeEntryData(const std::string &entry)
{
    const JVal *p = completeData().get("pipeline");
    return p ? p->get(entry) : nullptr;
}

bool pipeExactStep(const PipeCtx &pc, const std::string &word);

std::string pipeFlavor(const PipeCtx &pc)
{
    std::string probe =
        pc.readPath.empty() ? "" : convertDispatchProbe(pc.readPath);
    if (probe == "raster" && pc.entry != "vector_pipeline")
        return "ras";
    if (probe == "vector" && pc.entry != "raster_pipeline")
        return "vec";
    return "gen";
}

const CmdSpec *pipeStepSpec(const PipeCtx &pc, const std::string &flavor,
                            bool preferVector)
{
    const Spec &spec = Spec::instance();
    std::string first = preferVector ? "vector_" : "raster_";
    std::string second = preferVector ? "raster_" : "vector_";
    const CmdSpec *c = spec.findById(first + pc.step);
    if (!c)
        c = spec.findById(second + pc.step);
    (void)flavor;
    return c;
}

bool pipeOnlyValueOpt(const std::string &name)
{
    static const std::set<std::string> v = {
        "input",           "input-format",
        "open-option",     "input-layer",
        "output",          "output-format",
        "creation-option", "layer-creation-option",
        "output-open-option", "output-layer",
        "tee-pipeline",    "command"};
    return v.count(name) != 0;
}

std::string pipeCanonical(const PipeCtx &pc, const std::string &flavor,
                          const std::string &name, bool &takesValue,
                          const ArgSpec **argOut)
{
    static const std::map<std::string, std::string> aliases = {
        {"of", "output-format"},  {"f", "output-format"},
        {"if", "input-format"},   {"co", "creation-option"},
        {"lco", "layer-creation-option"}, {"oo", "open-option"},
        {"i", "input"},           {"o", "output"},
        {"l", "input-layer"},     {"format", "output-format"},
        {"nln", "output-layer"}};
    *argOut = nullptr;
    const CmdSpec *spec = pipeStepSpec(pc, flavor, flavor == "vec");
    if (spec)
    {
        const ArgSpec *a = findArg(spec, name);
        if (a)
        {
            *argOut = a;
            takesValue = !a->isBool();
            return a->name;
        }
    }
    std::string canon = name;
    auto it = aliases.find(name);
    if (it != aliases.end())
        canon = it->second;
    takesValue = pipeOnlyValueOpt(canon);
    return canon;
}

// isValueWord: completing a separate value word rather than the bare option
std::string pipeStepValue(const PipeCtx &pc, const std::string &flavor,
                          const std::string &canonIn, const std::string &val,
                          bool isValueWord)
{
    if (pc.step.empty())
        return "";
    bool optForm = !canonIn.empty();
    std::string canon = canonIn;
    if (isValueWord && canon == "input" && pc.step == "read")
        canon.clear();  // value of read --input behaves like its positional
    const JVal *ent = pipeEntryData(pc.entry);
    if (!ent)
        return "";
    if (!pipeExactStep(pc, pc.step))
        return "";
    if (!pc.firstSegment && pc.step == "read" &&
        (canon.empty() || canon == "input"))
    {
        FileFilter f = FileFilter::Both;
        std::string desc = "Input raster or vector datasets";
        if (pc.entry == "raster_pipeline")
        {
            f = FileFilter::Raster;
            desc = "Input raster datasets";
        }
        else if (pc.entry == "vector_pipeline" || flavor == "vec")
        {
            f = FileFilter::Vector;
            if (pc.entry == "vector_pipeline")
                desc = "Input vector datasets";
        }
        std::string res = fileComplete(f, val);
        return (res.empty() && optForm) ? hintFor(desc) : res;
    }
    std::string family = flavor == "vec" ? "vector" : "raster";
    const ArgSpec *arg = nullptr;
    bool takes = false;
    pipeCanonical(pc, flavor, canon.empty() ? "input" : canon, takes, &arg);
    auto ctxVal = [&](const std::string &n) {
        auto it = pc.stepValues.find(n);
        return it == pc.stepValues.end() ? std::string() : it->second;
    };
    // driver-dependent option catalogs take precedence over captured hints
    bool warnCog = flavor != "gen";
    std::string otherFamily = family == "vector" ? "raster" : "vector";
    if (canon == "creation-option" || canon == "layer-creation-option")
    {
        std::string drv = ctxVal("output-format");
        if (drv.empty())
            drv = extDriver(ctxVal("output"), family);
        if (drv.empty())
            drv = extDriver(ctxVal("output"), otherFamily);
        std::string out;
        std::string kind = canon == "creation-option" ? "co" : "lco";
        if (driverOptComplete(family, kind, drv, val, out, warnCog))
            return out;
        if (driverOptComplete(otherFamily, kind, drv, val, out, warnCog))
            return out;
    }
    else if (canon == "open-option")
    {
        std::string drv = ctxVal("input-format");
        if (drv.empty())
        {
            std::string in = ctxVal("input");
            if (in.empty())
                in = pc.readPath;
            drv = extDriver(in, family);
            if (drv.empty())
                drv = extDriver(in, otherFamily);
        }
        std::string out;
        if (driverOptComplete(family, "oo", drv, val, out, warnCog))
            return out;
        if (driverOptComplete(otherFamily, "oo", drv, val, out, warnCog))
            return out;
    }
    if (optForm && (canon == "input-layer" || canon == "layer"))
    {
        std::string in = ctxVal("input");
        if (in.empty())
            in = pc.readPath;
        if (!in.empty())
        {
            std::string lerr;
            auto ds = openVectorDataset(in, lerr, {});
            if (ds)
            {
                std::vector<std::string> names;
                for (const auto &l : ds->layers)
                    names.push_back(l.name);
                return joinEsc(names);
            }
        }
    }
    const JVal *vals = nullptr;
    if (pc.firstSegment)
    {
        const JVal *fv = ent->get("firstvals");
        if (fv)
            vals = fv->get(pc.step);
    }
    if (!vals)
    {
        const JVal *ov = ent->get("optvals");
        if (ov)
        {
            for (const char *fl : {flavor.c_str(), "gen", "ras", "vec"})
            {
                const JVal *f = ov->get(fl);
                if (f)
                    vals = f->get(pc.step);
                if (vals)
                    break;
            }
        }
        if (!vals && pc.step == "read")
        {
            const JVal *other = pipeEntryData(
                (pc.entry == "vector_pipeline" || flavor == "vec")
                    ? "vector_pipeline"
                    : "raster_pipeline");
            const JVal *fv = other ? other->get("firstvals") : nullptr;
            if (fv)
                vals = fv->get("read");
        }
        if (!vals && !pc.firstSegment)
        {
            const JVal *fv = ent->get("firstvals");
            if (fv)
                vals = fv->get(pc.step);
        }
    }
    const JVal *rec = nullptr;
    if (vals)
        rec = vals->get(canon.empty() ? "" : "--" + canon);
    if (rec)
    {
        std::string k = rec->getString("k");
        if (k == "empty")
            return "";
        if (k == "static")
        {
            std::string list = rec->getString("v");
            if (canon == "geometry-type" || canon == "field-type" ||
                canon == "src-field-type")
                return staticValueList(list, canon, val,
                                       arg ? arg->description : "");
            return list;
        }
        if (k == "config")
            return completeData().getString("configkeys");
        if (k == "crs")
            return crsComplete(rec->getString("v"), val);
        if (k == "files")
        {
            std::string fk = rec->getString("v");
            if (fk == "both" && pc.firstSegment)
            {
                if (flavor == "ras")
                {
                    std::string res = fileComplete(FileFilter::Raster, val);
                    return (res.empty() && optForm)
                               ? hintFor("Input raster datasets")
                               : res;
                }
                if (flavor == "vec")
                {
                    std::string res = fileComplete(FileFilter::Vector, val);
                    return (res.empty() && optForm)
                               ? hintFor("Input vector datasets")
                               : res;
                }
                return fileCompleteTwoPass(
                    val, hintFor("Input raster datasets"), optForm);
            }
            std::string res = fileComplete(filterFromName(fk), val);
            if (res.empty() && optForm && arg)
                return hintFor(arg->description);
            return res;
        }
        return "";
    }
    if (canon == "creation-option")
        return hintFor(arg ? arg->description : "Creation option");
    if (canon == "layer-creation-option")
        return hintFor(arg ? arg->description : "Layer creation option");
    if (canon == "open-option")
        return hintFor(arg ? arg->description : "Open options");
    if (!canon.empty() && arg)
    {
        TreeCtx fake;
        static CmdSpec dummy;
        const Spec &spec = Spec::instance();
        const CmdSpec *sspec =
            spec.findById((family == "vector" ? "vector_" : "raster_") +
                          pc.step);
        if (!sspec)
            sspec = spec.findById((family == "vector" ? "raster_"
                                                      : "vector_") +
                                  pc.step);
        fake.node = sspec ? sspec : &dummy;
        return valueComplete(fake, arg, val);
    }
    if (canon == "input")
    {
        // the hidden per-step --input completes datasets of the step's
        // own family; the tee step is typeless
        const Spec &spec = Spec::instance();
        std::string family2;
        if (spec.findById("raster_" + pc.step))
            family2 = "raster";
        else if (spec.findById("vector_" + pc.step))
            family2 = "vector";
        else if (pc.step != "tee")
        {
            if (pc.entry == "raster_pipeline" || flavor == "ras")
                family2 = "raster";
            else if (pc.entry == "vector_pipeline" || flavor == "vec")
                family2 = "vector";
        }
        FileFilter f = FileFilter::Both;
        std::string desc = "Input  datasets";
        if (family2 == "raster")
        {
            f = FileFilter::Raster;
            desc = "Input raster datasets";
        }
        else if (family2 == "vector")
        {
            f = FileFilter::Vector;
            desc = "Input vector datasets";
        }
        if (family2.empty())
            return hintFor(desc);
        std::string res = fileComplete(f, val);
        return res.empty() ? hintFor(desc) : res;
    }
    return "";
}

std::string pipeOptList(const PipeCtx &pc, const std::string &flavor)
{
    const JVal *ent = pipeEntryData(pc.entry);
    if (!ent)
        return "";
    if (pc.firstSegment)
    {
        const JVal *fo = ent->get("firstopts");
        return fo ? fo->getString(pc.step) : "";
    }
    const JVal *ov = ent->get("opts");
    if (ov)
    {
        for (const char *fl : {flavor.c_str(), "gen", "ras", "vec"})
        {
            const JVal *f = ov->get(fl);
            if (f)
            {
                std::string s = f->getString(pc.step);
                if (!s.empty())
                    return s;
            }
        }
    }
    if (pc.step == "read")
    {
        const JVal *other = pipeEntryData(
            (pc.entry == "vector_pipeline" || flavor == "vec")
                ? "vector_pipeline"
                : "raster_pipeline");
        const JVal *fo = other ? other->get("firstopts") : nullptr;
        if (fo)
        {
            std::string s = fo->getString("read");
            if (!s.empty())
                return s;
        }
    }
    const JVal *fo = ent->get("firstopts");
    return fo ? fo->getString(pc.step) : "";
}

std::string pipeStepList(const PipeCtx &pc, const std::string &flavor)
{
    const JVal *ent = pipeEntryData(pc.entry);
    if (!ent)
        return "";
    if (pc.firstSegment && pc.step.empty())
        return ent->getString("first");
    const JVal *mid = ent->get("mid");
    if (!mid)
        return "";
    const JVal *fl = mid->get(flavor);
    if (!fl)
        fl = mid->get("gen");
    return fl ? fl->s : "";
}

bool isPipelineNode(const std::string &id)
{
    return id == "pipeline" || id == "raster_pipeline" ||
           id == "vector_pipeline";
}

// true when word names any step known to this entry (first steps or the mid
// list of any flavor); selecting such a step yields no suggestions
bool pipeExactStep(const PipeCtx &pc, const std::string &word)
{
    const JVal *ent = pipeEntryData(pc.entry);
    if (!ent)
        return false;
    auto inList = [&](const std::string &s) {
        std::string cur;
        for (char c : s + " ")
        {
            if (c == ' ')
            {
                if (cur == word)
                    return true;
                cur.clear();
            }
            else
                cur += c;
        }
        return false;
    };
    if (inList(ent->getString("first")))
        return true;
    const JVal *mid = ent->get("mid");
    if (mid)
        for (const auto &kv : mid->obj)
            if (inList(kv.second.s))
                return true;
    return false;
}

}  // namespace

int runCompletion(const std::vector<std::string> &words)
{
    const Spec &spec = Spec::instance();
    std::string result;

    // words[0] is the program name and always skipped.
    std::vector<std::string> w(words.begin() + 1, words.end());

    TreeCtx tc;
    tc.node = spec.findById("ROOT");
    PipeCtx pc;
    bool pipeMode = false;

    auto enterPipeIfNeeded = [&]() {
        if (!pipeMode && isPipelineNode(tc.node->id))
        {
            pipeMode = true;
            pc.entry = tc.node->id;
        }
    };

    // ---- phase 1: consume all words except the last ----
    for (size_t i = 0; i + 1 < w.size(); ++i)
    {
        const std::string &word = w[i];
        enterPipeIfNeeded();
        if (pipeMode)
        {
            if (word == "!")
            {
                if (pc.step == "tile" && !pc.firstSegment &&
                    pc.entry != "vector_pipeline")
                    markTileWarn(1);
                pc.step.clear();
                pc.firstSegment = false;
                pc.pending.clear();
                pc.stepValues.clear();
                continue;
            }
            // a pending option swallows whatever comes next,
            // option-looking words included
            if (!pc.pending.empty())
            {
                pc.stepValues[pc.pending] = word;
                if (pc.pending == "input" && pc.step == "read" &&
                    pc.readPath.empty())
                    pc.readPath = word;
                pc.pending.clear();
                continue;
            }
            if (word.size() > 1 && word[0] == '-')
            {
                if (pc.step.empty() && pc.firstSegment)
                    pc.sawEntryOpt = true;
                std::string name = word.substr(word[1] == '-' ? 2 : 1);
                std::string val;
                size_t eq = name.find('=');
                bool hasEq = eq != std::string::npos;
                if (hasEq)
                {
                    val = name.substr(eq + 1);
                    name = name.substr(0, eq);
                }
                bool takes = false;
                const ArgSpec *arg = nullptr;
                std::string flavor = pipeFlavor(pc);
                std::string canon =
                    pipeCanonical(pc, flavor, name, takes, &arg);
                bool known = true;
                if (!pc.step.empty())
                {
                    // inside a step only its listed options (plus the
                    // hidden per-step --input) take values; anything
                    // else is inert
                    known = canon == "input";
                    if (!known)
                    {
                        std::string toklist = pipeOptList(pc, flavor);
                        std::string cur;
                        for (char c : toklist + " ")
                        {
                            if (c == ' ')
                            {
                                if (cur == "--" + canon)
                                    known = true;
                                cur.clear();
                            }
                            else
                                cur += c;
                        }
                    }
                    if (canon == "input")
                        takes = true;
                }
                if (!known)
                    continue;
                if (hasEq)
                    pc.stepValues[canon] = val;
                else if (takes)
                    pc.pending = canon;
                continue;
            }
            if (pc.step.empty())
            {
                pc.step = word;
                continue;
            }
            if (pc.step == "read" && pc.readPath.empty())
                pc.readPath = word;
            else if (pc.step == "write")
                pc.stepValues["output"] = word;
            continue;
        }
        // tree mode
        bool optWord = word.size() > 1 && word[0] == '-';
        if (tc.pendingConfig && !optWord)
        {
            tc.pendingConfig = false;
            tc.configUsed = true;
            continue;
        }
        if (tc.pending && !optWord)
        {
            tc.optValues[tc.pending->name] = word;
            tc.pending = nullptr;
            continue;
        }
        if (optWord)
        {
            tc.pendingConfig = false;
            tc.pending = nullptr;
            std::string name = word.substr(word[1] == '-' ? 2 : 1);
            std::string val;
            size_t eq = name.find('=');
            bool hasEq = eq != std::string::npos;
            if (hasEq)
            {
                val = name.substr(eq + 1);
                name = name.substr(0, eq);
            }
            if (ciEqual(name, "config"))
            {
                if (!hasEq)
                    tc.pendingConfig = true;
                else
                    tc.configUsed = true;
                continue;
            }
            const ArgSpec *arg = findArg(tc.node, name);
            if (!arg)
                continue;
            if (hasEq)
                tc.optValues[arg->name] = val;
            else if (!arg->isBool())
                tc.pending = arg;
            continue;
        }
        if (!tc.node->leaf())
        {
            bool isRoot = tc.node->id == "ROOT";
            std::string childId =
                isRoot ? word : tc.node->id + "_" + word;
            const CmdSpec *child = spec.findById(childId);
            if (child)
            {
                tc.node = child;
                continue;
            }
        }
        tc.positionals.push_back(word);
    }

    enterPipeIfNeeded();

    // ---- phase 2: complete the final word ----
    const std::string lw = w.empty() ? "" : w.back();

    if (w.empty())
    {
        result = "";
        const CmdSpec *root = spec.findById("ROOT");
        std::vector<std::string> subs = root->subNames;
        result = joinEsc(subs);
        printf("%s", result.c_str());
        return 0;
    }

    if (tc.configUsed)
    {
        printf("%s", joinEsc(spec.findById("ROOT")->subNames).c_str());
        return 0;
    }

    if (pipeMode)
    {
        std::string flavor = pipeFlavor(pc);
        bool warnEntry = pc.entry != "vector_pipeline";
        auto splitToks = [](const std::string &s) {
            std::vector<std::string> toks;
            std::string cur;
            for (char c : s + " ")
            {
                if (c == ' ')
                {
                    if (!cur.empty())
                        toks.push_back(cur);
                    cur.clear();
                }
                else
                    cur += c;
            }
            return toks;
        };
        if (pc.step == "tile" && warnEntry && lw != "!")
            markTileWarn(pc.firstSegment ? 1 : 2);
        if (lw == "!")
        {
            if (pc.step.empty() && pc.firstSegment)
            {
                if (warnEntry)
                    markTileWarn(1);
                result = "";
            }
            else
            {
                if (pc.step == "tile" && !pc.firstSegment && warnEntry)
                    markTileWarn(1);
                pc.step.clear();
                pc.firstSegment = false;
                if (warnEntry)
                    markTileWarn(1);
                result = pipeStepList(pc, flavor);
            }
        }
        else if (!pc.pending.empty() && !(!lw.empty() && lw[0] == '-'))
        {
            result = pipeStepValue(pc, flavor, pc.pending, lw, true);
        }
        else if (!lw.empty() && lw[0] == '-')
        {
            if (pc.step.empty())
            {
                if (warnEntry && !pc.sawEntryOpt)
                    markTileWarn(1);
                result = "";
            }
            else
            {
                std::string name = lw;
                while (!name.empty() && name[0] == '-')
                    name.erase(0, 1);
                std::string val;
                size_t eq = name.find('=');
                bool hasEq = eq != std::string::npos;
                if (hasEq)
                {
                    val = name.substr(eq + 1);
                    name = name.substr(0, eq);
                }
                bool takes = false;
                const ArgSpec *arg = nullptr;
                std::string canon =
                    pipeCanonical(pc, flavor, name, takes, &arg);
                std::vector<std::string> toks =
                    splitToks(pipeOptList(pc, flavor));
                bool exact = false;
                for (const auto &t : toks)
                    if (t == "--" + canon)
                        exact = true;
                if (exact && !name.empty())
                {
                    if (takes || hasEq)
                        result = pipeStepValue(pc, flavor, canon, val, false);
                    else
                        result = "";
                }
                else
                {
                    std::vector<std::string> match;
                    for (const auto &t : toks)
                        if (strStartsWith(t, lw))
                            match.push_back(t);
                    result = joinEsc(match);
                }
            }
        }
        else if (pc.step.empty())
        {
            if (pc.sawEntryOpt && pc.firstSegment)
                result = "";
            else
            {
                std::string list = pipeStepList(pc, flavor);
                std::vector<std::string> toks = splitToks(list);
                bool exact = false;
                for (const auto &t : toks)
                    if (t == lw)
                        exact = true;
                if (!exact && !pc.firstSegment)
                    exact = pipeExactStep(pc, lw);
                if (exact)
                {
                    if (lw == "tile" && warnEntry)
                        markTileWarn(3);
                    result = "";
                }
                else
                {
                    if (warnEntry)
                        markTileWarn(1);
                    if (pc.firstSegment)
                    {
                        std::vector<std::string> match;
                        for (const auto &t : toks)
                            if (strStartsWith(t, lw))
                                match.push_back(t);
                        result = joinEsc(match);
                    }
                    else
                        result = list;
                }
            }
        }
        else
        {
            result = pipeStepValue(pc, flavor, "", lw, true);
        }
        flushTileWarn();
        printf("%s", result.c_str());
        return 0;
    }

    // tree mode completion
    if (tc.node->id == "raster_tile")
        markTileWarn(1);
    bool lwOpt = !lw.empty() && lw[0] == '-';
    if (tc.pendingConfig && !lwOpt)
    {
        result = (!lw.empty() && lw.back() == '=')
                     ? ""
                     : completeData().getString("configkeys");
    }
    else if (tc.pending && !lwOpt)
    {
        result = valueComplete(tc, tc.pending, lw);
    }
    else if (lwOpt)
    {
        std::string name = lw;
        while (!name.empty() && name[0] == '-')
            name.erase(0, 1);
        std::string val;
        size_t eq = name.find('=');
        bool hasEq = eq != std::string::npos;
        if (hasEq)
        {
            val = name.substr(eq + 1);
            name = name.substr(0, eq);
        }
        if (!name.empty() && ciEqual(name, "config"))
        {
            result = (hasEq && !val.empty() && val.back() == '=')
                         ? ""
                         : completeData().getString("configkeys");
        }
        else
        {
            const ArgSpec *arg =
                name.empty() ? nullptr : findArg(tc.node, name);
            if (arg)
            {
                if (hasEq || !arg->isBool())
                    result = valueComplete(tc, arg, val);
                else
                    result = "";
            }
            else if (!name.empty() &&
                     (ciEqual(name, "help") || ciEqual(name, "json-usage") ||
                      ciEqual(name, "version") || ciEqual(name, "drivers") ||
                      ciEqual(name, "help-doc")))
            {
                result = "";
            }
            else
            {
                std::vector<std::string> toks;
                const JVal *cr = cmdRec(tc.node->id);
                std::string captured = cr ? cr->getString("opts") : "";
                if (!captured.empty())
                {
                    std::string cur;
                    for (char c : captured + " ")
                    {
                        if (c == ' ')
                        {
                            if (!cur.empty())
                                toks.push_back(cur);
                            cur.clear();
                        }
                        else
                            cur += c;
                    }
                }
                else
                    toks = optionUniverse(tc.node);
                bool exact = false;
                for (const auto &t : toks)
                    if (t == lw)
                        exact = true;
                if (exact && !hasEq)
                    result = "";
                else
                {
                    std::vector<std::string> match;
                    for (const auto &t : toks)
                        if (strStartsWith(t, lw))
                            match.push_back(t);
                    result = joinEsc(match);
                }
            }
        }
    }
    else if (!tc.node->leaf())
    {
        bool isRoot = tc.node->id == "ROOT";
        std::string childId = isRoot ? lw : tc.node->id + "_" + lw;
        const CmdSpec *child = spec.findById(childId);
        if (child)
        {
            if (isPipelineNode(child->id))
            {
                if (child->id != "vector_pipeline")
                    markTileWarn(1);
            }
            else if (!child->leaf())
            {
                if (child->id == "raster")
                    markTileWarn(1);
            }
            else if (child->id == "raster_tile")
                markTileWarn(1);
            const JVal *cr = cmdRec(child->id);
            result = cr ? cr->getString("bare") : "";
        }
        else
        {
            if (tc.node->id == "raster")
                markTileWarn(1);
            if (tc.node->id == "ROOT")
                result = joinEsc(tc.node->subNames);
            else
            {
                const JVal *cr = cmdRec(tc.node->id);
                result = cr ? cr->getString("bare")
                            : joinEsc(tc.node->subNames);
            }
        }
    }
    else
    {
        if (cmdFamily(tc.node->id) == "vector" && tc.positionals.size() == 1)
            emitShapefileDirErrors(tc.positionals[0]);
        // an empty completion falls through to the following positional,
        // but never past a dataset-taking one
        auto plist = positionalArgs(tc.node);
        const ArgSpec *cur =
            currentPositional(tc.node, tc.positionals.size());
        size_t start = 0;
        while (start < plist.size() && plist[start] != cur)
            ++start;
        result = "";
        for (size_t i = start; cur && i < plist.size(); ++i)
        {
            result = valueComplete(tc, plist[i], lw, true);
            if (!result.empty())
                break;
            const JVal *prec = cmdArgRec(tc.node->id, plist[i]->name);
            if (prec && prec->getString("k") == "files")
                break;
        }
    }

    flushTileWarn();
    printf("%s", result.c_str());
    return 0;
}
