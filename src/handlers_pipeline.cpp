#include "cpl.h"
#include "dataset.h"
#include "embedded.h"
#include "engine.h"
#include "ogr.h"

extern std::unique_ptr<OgrDataset> g_convertSourceOverride;
extern bool g_convertLayerWriteFailed;
#include "json.h"
#include "jsonc.h"
#include "progress.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "vsi.h"

#include <array>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

enum PipeType
{
    P_GENERIC,
    P_RASTER,
    P_VECTOR
};

const std::set<std::string> &stepNames(PipeType t)
{
    static const std::set<std::string> ras = {
        "read",          "calc",       "mosaic",
        "stack",         "aspect",     "blend",
        "clip",          "color-map",  "create",
        "edit",          "external",   "fill-nodata",
        "hillshade",     "materialize", "neighbors",
        "neighbours",    "nodata-to-alpha", "overview",
        "pansharpen",    "proximity",  "reclassify",
        "reproject",     "resize",     "rgb-to-palette",
        "roughness",     "scale",      "select",
        "set-type",      "sieve",      "slope",
        "tee",           "tpi",        "tri",
        "unscale",       "update",     "viewshed",
        "compare",       "info",       "tile",
        "write"};
    static const std::set<std::string> vec = {
        "read",         "concat",       "buffer",
        "check-coverage", "check-geometry", "clean-coverage",
        "clip",         "combine",      "concave-hull",
        "convex-hull",  "create",       "dissolve",
        "edit",         "explode-collections", "external",
        "filter",       "limit",        "make-point",
        "make-valid",   "materialize",  "rename-layer",
        "reproject",    "segmentize",   "select",
        "set-field-type", "set-geom-type", "simplify",
        "simplify-coverage", "sort",    "sql",
        "swap-xy",      "tee",          "update",
        "export-schema", "info",        "partition",
        "write"};
    static const std::set<std::string> gen = [] {
        std::set<std::string> s = ras;
        s.insert(vec.begin(), vec.end());
        for (const char *extra :
             {"as-features", "contour", "footprint", "grid", "pixel-info",
              "polygonize", "rasterize", "zonal-stats"})
            s.insert(extra);
        return s;
    }();
    return t == P_RASTER ? ras : t == P_VECTOR ? vec : gen;
}

// vector transform steps sharing the OGRSQL/select engine and the
// geometry verbs applied at the same dataset-mutate point
bool vectorVerbStepName(const std::string &n)
{
    return n == "filter" || n == "select" || n == "sql" ||
           n == "swap-xy" || n == "segmentize" || n == "make-point" ||
           n == "explode-collections" || n == "set-geom-type" ||
           n == "edit" || n == "rename-layer" || n == "clip" ||
           n == "combine" || n == "set-field-type";
}

// geometry steps parseable in full but hitting the no-GEOS wall when
// the chain executes (the GDALG terminal serializes them cleanly)
bool geosWallStepName(const std::string &n)
{
    return n == "buffer" || n == "simplify" || n == "convex-hull" ||
           n == "concave-hull" || n == "make-valid" ||
           n == "dissolve" || n == "check-coverage" ||
           n == "clean-coverage" || n == "simplify-coverage" ||
           n == "check-geometry";
}

bool firstStepAllowed(PipeType t, const std::string &n)
{
    static const std::set<std::string> ras = {"read",     "calc",  "create",
                                              "external", "mosaic", "stack"};
    static const std::set<std::string> vec = {"read", "concat", "create",
                                              "external"};
    static const std::set<std::string> gen = {
        "read", "calc", "concat", "create", "external", "mosaic", "stack"};
    const auto &s = t == P_RASTER ? ras : t == P_VECTOR ? vec : gen;
    return s.count(n) != 0;
}

const char *firstStepMsg(PipeType t)
{
    switch (t)
    {
        case P_RASTER:
            return "pipeline: First step should be 'read', 'calc', "
                   "'create', 'external', 'mosaic' or 'stack'";
        case P_VECTOR:
            return "pipeline: First step should be 'read', 'concat', "
                   "'create' or 'external'";
        default:
            return "pipeline: First step should be 'read', 'calc', "
                   "'concat', 'create', 'external', 'mosaic', 'read' or "
                   "'stack'";
    }
}

bool lastStepAllowed(PipeType t, const std::string &n)
{
    static const std::set<std::string> ras = {
        "write", "compare", "external", "info", "tee", "tile", "update"};
    static const std::set<std::string> vec = {
        "write", "create", "export-schema", "external",
        "info",  "partition", "tee"};
    static const std::set<std::string> gen = {
        "write", "compare", "create", "export-schema", "external",
        "info",  "partition", "tee",  "tile",          "update"};
    const auto &s = t == P_RASTER ? ras : t == P_VECTOR ? vec : gen;
    return s.count(n) != 0;
}

const char *lastStepMsg(PipeType t)
{
    switch (t)
    {
        case P_RASTER:
            return "pipeline: Last step should be 'write', 'compare', "
                   "'external', 'info', 'tee', 'tile' or 'update'";
        case P_VECTOR:
            return "pipeline: Last step should be 'write', 'create', "
                   "'export-schema', 'external', 'info', 'partition' or "
                   "'tee'";
        default:
            return "pipeline: Last step should be 'write', 'compare', "
                   "'create', 'export-schema', 'external', 'info', "
                   "'partition', 'tee', 'tile', 'update' or 'write'";
    }
}

enum Side
{
    S_BOTH,
    S_RAS,
    S_VEC
};

struct OptDef
{
    const char *canon;
    std::vector<const char *> longs;
    const char *shortName;
    bool takesValue;
    Side side;
    // matched like longs but never offered as suggestion candidates
    std::vector<const char *> hiddenLongs;
};

const std::vector<OptDef> &readDefs()
{
    static const std::vector<OptDef> d = {
        {"help", {"help"}, "h", false, S_BOTH},
        {"json-usage", {"json-usage"}, nullptr, false, S_BOTH},
        {"help-doc", {"help-doc"}, nullptr, false, S_BOTH},
        {"config", {"config"}, nullptr, true, S_BOTH},
        {"input", {"input"}, "i", true, S_BOTH},
        {"input-layer", {"layer", "input-layer"}, "l", true, S_VEC},
        {"input-format", {"if", "input-format"}, nullptr, true, S_BOTH},
        {"open-option", {"oo", "open-option"}, nullptr, true, S_BOTH},
    };
    return d;
}

const std::vector<OptDef> &writeDefs()
{
    static const std::vector<OptDef> d = {
        {"help", {"help"}, "h", false, S_BOTH},
        {"json-usage", {"json-usage"}, nullptr, false, S_BOTH},
        {"help-doc", {"help-doc"}, nullptr, false, S_BOTH},
        {"config", {"config"}, nullptr, true, S_BOTH},
        {"output", {"output"}, "o", true, S_BOTH},
        {"of", {"of", "format", "output-format"}, "f", true, S_BOTH},
        {"co", {"co", "creation-option"}, nullptr, true, S_BOTH},
        {"lco", {"lco", "layer-creation-option"}, nullptr, true, S_VEC},
        {"overwrite", {"overwrite"}, nullptr, false, S_BOTH},
        {"append", {"append"}, nullptr, false, S_BOTH},
        {"update", {"update"}, nullptr, false, S_VEC},
        {"overwrite-layer", {"overwrite-layer"}, nullptr, false, S_VEC},
        {"upsert", {"upsert"}, nullptr, false, S_VEC},
        {"skip-errors", {"skip-errors"}, nullptr, false, S_VEC},
        {"no-create-empty-layers",
         {"no-create-empty-layers"},
         nullptr,
         false,
         S_VEC},
        {"output-layer", {"output-layer"}, "l", true, S_VEC, {"nln"}},
        {"output-open-option",
         {"output-oo", "output-open-option"},
         nullptr,
         true,
         S_VEC},
    };
    return d;
}

const std::vector<OptDef> &infoDefs()
{
    static const std::vector<OptDef> d = {
        {"help", {"help"}, "h", false, S_BOTH},
        {"json-usage", {"json-usage"}, nullptr, false, S_BOTH},
        {"help-doc", {"help-doc"}, nullptr, false, S_BOTH},
        {"config", {"config"}, nullptr, true, S_BOTH},
        {"of", {"of", "format", "output-format"}, "f", true, S_BOTH},
        {"min-max", {"mm", "min-max"}, nullptr, false, S_RAS},
        {"stats", {"stats"}, nullptr, false, S_RAS},
        {"approx-stats", {"approx-stats"}, nullptr, false, S_RAS},
        {"hist", {"hist"}, nullptr, false, S_RAS},
        {"no-gcp", {"no-gcp"}, nullptr, false, S_RAS},
        {"no-md", {"no-md"}, nullptr, false, S_RAS},
        {"no-ct", {"no-ct"}, nullptr, false, S_RAS},
        {"no-fl", {"no-fl"}, nullptr, false, S_RAS},
        {"checksum", {"checksum"}, nullptr, false, S_RAS},
        {"list-mdd",
         {"list-metadata-domains", "list-mdd"},
         nullptr,
         false,
         S_RAS},
        {"no-nodata", {"no-nodata"}, nullptr, false, S_RAS},
        {"no-mask", {"no-mask"}, nullptr, false, S_RAS},
        {"mdd", {"mdd", "metadata-domain"}, nullptr, true, S_RAS},
        {"subdataset", {"subdataset"}, nullptr, true, S_RAS},
        {"input-layer", {"layer", "input-layer"}, "l", true, S_VEC},
        {"input-format", {"if", "input-format"}, nullptr, true, S_VEC},
        {"open-option", {"oo", "open-option"}, nullptr, true, S_VEC},
        {"features", {"features"}, nullptr, false, S_VEC},
        {"summary", {"summary"}, nullptr, false, S_VEC},
        {"limit", {"limit"}, nullptr, true, S_VEC},
        {"sql", {"sql"}, nullptr, true, S_VEC},
        {"where", {"where"}, nullptr, true, S_VEC},
        {"fid", {"fid"}, nullptr, true, S_VEC},
        {"dialect", {"dialect"}, nullptr, true, S_VEC},
        {"crs-format", {"crs-format"}, nullptr, true, S_BOTH},
    };
    return d;
}

struct StepData
{
    std::string name;
    std::vector<std::string> tokens;
    std::map<std::string, std::vector<std::string>> v;
    std::vector<std::string> pos;
    bool usedVecOnly = false;
    bool usedRasOnly = false;
    // raster->vector transition step (contour/polygonize/footprint) in a
    // generic pipeline; sideVec marks steps running on the vector side
    // of such a transition. transRas is the vector->raster rasterize
    // transition; sideRas marks steps on its raster side
    bool trans = false;
    bool sideVec = false;
    bool transRas = false;
    bool sideRas = false;
    // select exists on both sides: a generic pipeline defers its parse
    // until the evolving-type walk knows the input side
    bool selDual = false;

    bool has(const std::string &k) const { return v.count(k) != 0; }
    std::string str(const std::string &k) const
    {
        auto it = v.find(k);
        return it == v.end() || it->second.empty() ? "" : it->second[0];
    }
    std::vector<std::string> list(const std::string &k) const
    {
        auto it = v.find(k);
        return it == v.end() ? std::vector<std::string>() : it->second;
    }
};

struct PipeCtx
{
    PipeType type = P_GENERIC;
    std::string usage;       // full usage failure block
    std::string invokedCli;  // e.g. "gdal pipeline"
    bool alias = false;
    bool quiet = false;
    // single-token pipelines report failures without the usage block
    bool noUsage = false;
    // generic pipeline resolved by the read-dispatch open probe; an
    // info step that consumed a vector-only option then renders with
    // the leaf vector-info json default
    bool genericOrigin = false;
    // a step name that exists only in the vector registry fixes the
    // pipeline as vector at parse: info keeps the text default
    bool vecFixedByName = false;
};

// parse failure: prints pending usage unless alias mode
constexpr int kParseFail = -1;
constexpr int kHelpPassthrough = -2;

int pipeFail(const PipeCtx &c)
{
    if (!c.alias && !c.noUsage)
        fwrite(c.usage.data(), 1, c.usage.size(), stderr);
    return kParseFail;
}

int pipeFailMsg(const PipeCtx &c, int cls, const std::string &msg)
{
    cplErrorStr(CE_Failure, cls, msg);
    return pipeFail(c);
}

bool fileExistsP(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// options whose values must be skipped when scanning step tokens
const OptDef *findDef(const std::vector<OptDef> &defs, const std::string &l,
                      bool isShort)
{
    for (const auto &d : defs)
    {
        if (isShort)
        {
            if (d.shortName && l == d.shortName)
                return &d;
            continue;
        }
        for (const char *cand : d.longs)
            if (l == cand)
                return &d;
        for (const char *cand : d.hiddenLongs)
            if (l == cand)
                return &d;
    }
    return nullptr;
}

std::string inputFormatError(PipeType t, const std::string &drv);

const std::vector<OptDef> &concatDefs()
{
    static const std::vector<OptDef> d = {
        {"help", {"help"}, "h", false, S_BOTH},
        {"json-usage", {"json-usage"}, nullptr, false, S_BOTH},
        {"help-doc", {"help-doc"}, nullptr, false, S_BOTH},
        {"config", {"config"}, nullptr, true, S_BOTH},
        {"input", {"input"}, "i", true, S_BOTH},
        {"input-layer", {"layer", "input-layer"}, "l", true, S_VEC},
        {"input-format", {"if", "input-format"}, nullptr, true, S_BOTH},
        {"open-option", {"oo", "open-option"}, nullptr, true, S_BOTH},
        {"mode", {"mode"}, nullptr, true, S_VEC},
        {"output-layer", {"output-layer"}, nullptr, true, S_VEC, {"nln"}},
        {"source-layer-field-name", {"source-layer-field-name"}, nullptr,
         true, S_VEC},
        {"source-layer-field-content", {"source-layer-field-content"},
         nullptr, true, S_VEC},
        {"field-strategy", {"field-strategy"}, nullptr, true, S_VEC},
        {"src-crs", {"src-crs"}, "s", true, S_VEC},
        {"dst-crs", {"dst-crs"}, "d", true, S_VEC},
    };
    return d;
}

// arguments a transform step never exposes even though the leaf spec
// declares them
// raster->vector transition steps executable in a generic pipeline;
// the remaining transition names (as-features, zonal-stats, rasterize,
// grid, pixel-info) stay behind the build wall
bool transStepName(const std::string &n)
{
    return n == "contour" || n == "polygonize" || n == "footprint";
}

const std::set<std::string> &stepExcludedArgs(bool vec)
{
    static const std::set<std::string> raster = {
        "output",    "output-format", "creation-option", "overwrite",
        "append",    "help",          "open-option",     "input-format",
        "dataset",   "auxiliary",     "stats",           "approx-stats",
        "hist"};
    static const std::set<std::string> vector = {
        "output",          "output-format",
        "creation-option", "layer-creation-option",
        "overwrite",       "append",
        "update",          "overwrite-layer",
        "upsert",          "output-layer",
        "skip-errors",     "help",
        "open-option",     "input-format",
        "output-open-option", "input-layer",
        "no-create-empty-layers"};
    return vec ? vector : raster;
}

// spec-driven parsing for transform steps (set-type/scale/reproject/...)
int parseSpecStep(const PipeCtx &c, StepData &st, const CmdSpec *cs,
                  bool vec)
{
    if (!cs)
        return 0;
    const std::set<std::string> &excludedBase = stepExcludedArgs(vec);
    // the sql step keeps --output-layer: its result layers are named
    // there, not on the write step
    auto isExcluded = [&](const std::string &n) {
        if (st.trans)
        {
            static const std::set<std::string> transExcl = {
                "input",         "output",
                "output-format", "creation-option",
                "layer-creation-option", "output-open-option",
                "open-option",   "input-format",
                "overwrite",     "update",
                "overwrite-layer", "append",
                "upsert",        "skip-errors",
                "help",          "dataset"};
            return transExcl.count(n) != 0;
        }
        if (st.name == "sql" && n == "output-layer")
            return false;
        // the rasterize transition keeps its own layer selector: the
        // step consumes the vector input directly, but never the
        // update-mode accumulation flag
        if (st.transRas && n == "input-layer")
            return false;
        if (st.transRas && n == "add")
            return true;
        if (st.name == "rename-layer" &&
            (n == "output-layer" || n == "input-layer"))
            return false;
        if (st.name == "export-schema" &&
            (n == "input-layer" || n == "input-format" ||
             n == "open-option"))
            return false;
        // update's output is the step product; the hidden update flag
        // stays reachable too
        if (st.name == "update" &&
            (n == "output" || n == "output-layer" ||
             n == "output-open-option" || n == "input-layer" ||
             n == "update"))
            return false;
        // the create step keeps the whole leaf surface (its output is
        // the step product, not the write step's)
        if (st.name == "create")
            return false;
        return excludedBase.count(n) != 0;
    };
    auto findArg = [&](const std::string &n,
                       bool isShort) -> const ArgSpec * {
        for (const auto &a : cs->args)
        {
            if (isExcluded(a.name))
                continue;
            if (isShort)
            {
                for (const auto &s : a.shorts)
                    if (s == n)
                        return &a;
            }
            else
            {
                if (a.name == n)
                    return &a;
                for (const auto &al : a.aliases)
                    if (al == n)
                        return &a;
            }
        }
        return nullptr;
    };
    std::set<std::string> seen;
    auto assign = [&](const ArgSpec &a, const std::string &value,
                      const std::string &disp) -> int {
        if (seen.count(a.name) &&
            (!a.isList() || (a.packed && a.minCount >= 0 &&
                             a.minCount == a.maxCount)))
            return pipeFailMsg(c, CPLE_IllegalArg,
                               st.name + ": Argument '" + disp +
                                   "' has already been specified.");
        seen.insert(a.name);
        if (!a.isBool())
        {
            bool packedList = a.isList() && a.packed;
            std::vector<std::string> parts;
            if (packedList)
                parts = strSplit(value, ',');
            else
                parts.push_back(value);
            const char *listOf = packedList ? "list of " : "";
            if (a.type == "integer" || a.type == "integer_list")
            {
                for (const auto &part : parts)
                {
                    char *endp = nullptr;
                    errno = 0;
                    strtoll(part.c_str(), &endp, 10);
                    if (part.empty() || *endp != '\0' || errno == ERANGE)
                        return pipeFailMsg(
                            c, CPLE_IllegalArg,
                            st.name + ": Expected " + listOf +
                                "integer value for argument '" + disp +
                                "', but got '" + value + "'.");
                }
            }
            else if (a.type == "real" || a.type == "real_list")
            {
                for (const auto &part : parts)
                {
                    char *endp = nullptr;
                    strtod(part.c_str(), &endp);
                    if (part.empty() || *endp != '\0' ||
                        numLooksHex(part))
                        return pipeFailMsg(
                            c, CPLE_IllegalArg,
                            st.name + ": Expected " + listOf +
                                "real value for argument '" + disp +
                                "', but got '" + value + "'.");
                }
            }
            if (!a.choices.empty())
            {
                bool found = false;
                for (const auto &ch : a.choices)
                    if (strToLower(ch) == strToLower(value))
                        found = true;
                if (!found && strToLower(value) == "byte")
                    for (const auto &ch : a.choices)
                        if (ch == "UInt8")
                            found = true;
                if (!found && strToLower(value) == "near")
                    for (const auto &ch : a.choices)
                        if (ch == "nearest")
                            found = true;
                if (!found)
                {
                    std::string list;
                    for (size_t k = 0; k < a.choices.size(); ++k)
                    {
                        if (k)
                            list += ", ";
                        list += "'" + a.choices[k] + "'";
                    }
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                "Invalid value '" + value +
                                    "' for string argument '" + a.name +
                                    "'. Should be one among " + list + ".");
                    return kParseFail;
                }
            }
            if (a.hasMin || a.hasMax)
            {
                for (const auto &part : parts)
                {
                    double d = strtod(part.c_str(), nullptr);
                    auto fmtBound = [](double b)
                    {
                        return b == static_cast<long long>(b)
                                   ? strPrintf("%lld",
                                               static_cast<long long>(b))
                                   : strPrintf("%g", b);
                    };
                    bool bad = false;
                    if (a.hasMin &&
                        (a.minIncluded ? d < a.minVal : d <= a.minVal))
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_IllegalArg,
                            strPrintf("Value of argument '%s' is %s, but "
                                      "should be %s %s",
                                      a.name.c_str(), part.c_str(),
                                      a.minIncluded ? ">=" : ">",
                                      fmtBound(a.minVal).c_str()));
                        bad = true;
                    }
                    if (a.hasMax &&
                        (a.maxIncluded ? d > a.maxVal : d >= a.maxVal))
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_IllegalArg,
                            strPrintf("Value of argument '%s' is %s, but "
                                      "should be %s %s",
                                      a.name.c_str(), part.c_str(),
                                      a.maxIncluded ? "<=" : "<",
                                      fmtBound(a.maxVal).c_str()));
                        bad = true;
                    }
                    if (bad)
                        return pipeFail(c);
                }
            }
            if ((st.name == "scale" || rasterDemStepName(st.name) ||
                 st.trans) &&
                a.name == "band")
            {
                bool anyLow = false;
                for (const auto &part : strSplit(value, ','))
                    if (atoi(part.c_str()) < 1)
                        anyLow = true;
                if (anyLow)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Value of 'band' should greater or equal "
                                "to 1.");
                    return pipeFail(c);
                }
            }
            if ((st.name == "clip" &&
                 (a.name == "bbox-crs" || a.name == "geometry-crs")) ||
                ((st.name == "reproject" || st.name == "make-point") &&
                 (a.name == "src-crs" || a.name == "dst-crs")) ||
                (st.name == "edit" && a.name == "crs" && value != "none" &&
                 value != "null"))
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       st.name + ": Invalid value for '" +
                                           a.name + "' argument");
            }
            if ((st.name == "set-geom-type" || st.name == "edit" ||
                 st.name == "create") &&
                a.name == "geometry-type")
            {
                int gt;
                bool gz, gm;
                if (!ogrGeomTypeFromWktName(value, gt, gz, gm))
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       st.name +
                                           ": Invalid geometry "
                                           "type '" +
                                           value + "'");
            }
            if (st.name == "create" && a.name == "crs")
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "create: Invalid value for 'crs' "
                                       "argument");
            }
            if (st.name == "create" && a.name == "field")
            {
                std::string err = vectorCreateFieldDefError(value);
                if (!err.empty())
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "create: " + err);
            }
            if (st.name == "rename-layer" &&
                a.name == "replacement-character" && value.size() > 1)
                return pipeFailMsg(
                    c, CPLE_IllegalArg,
                    "Value of argument 'replacement-character' is '" +
                        value + "', but should have no more than 1 "
                                "character");
            if (st.name == "edit" && a.name == "nodata")
            {
                double d;
                if (value != "none" && value != "nan" && value != "inf" &&
                    value != "-inf" && !editNodataParse(value, d))
                    return pipeFailMsg(
                        c, CPLE_IllegalArg,
                        "edit: Value of 'nodata' should be 'none', a "
                        "numeric value, 'nan', 'inf' or '-inf'");
            }
            if (st.name == "export-schema" && a.name == "input-format")
            {
                std::string err = inputFormatError(P_VECTOR, value);
                if (!err.empty())
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       st.name + ": " + err);
            }
            if (st.name == "set-field-type" &&
                (a.name == "field-type" || a.name == "src-field-type"))
            {
                int tt, ss;
                if (!vectorFieldTypeNameParse(value, tt, ss))
                    return pipeFailMsg(
                        c, CPLE_AppDefined,
                        "set-field-type: Invalid value for argument '" +
                            a.name + "': '" + value + "'");
            }
        }
        st.v[a.name].push_back(a.isBool() && value.empty() ? "true"
                                                           : value);
        return 0;
    };
    for (size_t i = 0; i < st.tokens.size(); ++i)
    {
        const std::string &tok = st.tokens[i];
        if (tok == "--")
            return pipeFailMsg(c, CPLE_IllegalArg,
                               st.name + ": Option '--' is unknown.");
        if (strStartsWith(tok, "--") && tok.size() > 2)
        {
            std::string body = tok.substr(2);
            size_t eq = body.find('=');
            std::string name =
                eq == std::string::npos ? body : body.substr(0, eq);
            if (name == "config")
            {
                if (eq == std::string::npos && i + 1 < st.tokens.size())
                    ++i;
                continue;
            }
            if (name == "json-usage" || name == "help-doc")
                continue;
            const ArgSpec *a = findArg(name, false);
            if (!a && st.name == "edit" && name == "input")
            {
                // hidden step-input argument (spec names the leaf's
                // positional 'dataset' instead); triggers the open probe
                // and the exec-time step-input rejection
                std::string value;
                if (eq != std::string::npos)
                    value = body.substr(eq + 1);
                else if (i + 1 < st.tokens.size())
                    value = st.tokens[++i];
                st.v["input"].push_back(value);
                continue;
            }
            if (!a)
            {
                std::vector<std::string> cands = {"help", "help-doc",
                                                  "json-usage", "config"};
                for (const auto &aa : cs->args)
                {
                    // quiet stays accepted but hidden from suggestions
                    if (isExcluded(aa.name) || aa.name == "quiet")
                        continue;
                    cands.push_back(aa.name);
                    for (const auto &al : aa.aliases)
                        cands.push_back(al);
                }
                std::string sug = suggestOptionName(name, cands);
                if (!sug.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                st.name + ": Option '--" + name +
                                    "' is unknown. Do you mean '--" + sug +
                                    "'?");
                    return kParseFail;
                }
                return pipeFailMsg(c, CPLE_IllegalArg,
                                   st.name + ": Option '--" + name +
                                       "' is unknown.");
            }
            std::string value;
            if (!a->isBool())
            {
                if (eq != std::string::npos)
                    value = body.substr(eq + 1);
                else if (i + 1 < st.tokens.size())
                    value = st.tokens[++i];
                else
                    return pipeFailMsg(c, CPLE_IllegalArg,
                                       st.name + ": Expected value for "
                                                 "argument '--" +
                                           name +
                                           "', but ran short of tokens");
            }
            else if (eq != std::string::npos)
            {
                std::string lv = body.substr(eq + 1);
                if (lv != "true" && lv != "false")
                    return pipeFailMsg(
                        c, CPLE_IllegalArg,
                        st.name + ": Invalid value '" +
                            body.substr(eq + 1) +
                            "' for boolean argument '--" + name +
                            "'. Should be 'true' or 'false'.");
                value = lv;
            }
            int rc = assign(*a, value, "--" + name);
            if (rc)
                return rc;
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' &&
            !isdigit((unsigned char)tok[1]) && tok[1] != '.' &&
            tok[1] != '-')
        {
            std::string sn = tok.substr(1, 1);
            const ArgSpec *a = findArg(sn, true);
            if (!a)
                return pipeFailMsg(c, CPLE_IllegalArg,
                                   st.name + ": Short name option '" + sn +
                                       "' is unknown.");
            std::string value;
            if (!a->isBool())
            {
                if (i + 1 < st.tokens.size())
                    value = st.tokens[++i];
                else
                    return pipeFailMsg(c, CPLE_IllegalArg,
                                       st.name + ": Expected value for "
                                                 "argument '" +
                                           sn +
                                           "', but ran short of tokens");
            }
            int rc = assign(*a, value, sn);
            if (rc)
                return rc;
            continue;
        }
        st.pos.push_back(tok);
    }
    if (!st.pos.empty())
    {
        // select and sql keep step-level positionals (field list /
        // statements); segmentize, buffer and the simplify pair take
        // exactly one; every other step rejects them
        const char *posArg = !vec ? (st.name == "select" ? "band"
                                                         : nullptr)
                             : st.name == "select"
                                 ? "fields"
                                 : st.name == "sql" ? "sql" : nullptr;
        const char *onePos = st.name == "segmentize" ? "max-length"
                             : st.name == "buffer"   ? "distance"
                             : st.name == "simplify" ||
                                     st.name == "simplify-coverage"
                                 ? "tolerance"
                                 : nullptr;
        if (vec && onePos && !st.v.count(onePos))
        {
            const ArgSpec *ml = cs->findByName(onePos);
            if (ml)
            {
                int rc = assign(*ml, st.pos[0], onePos);
                if (rc)
                    return rc;
            }
            st.pos.erase(st.pos.begin());
        }
        if (vec && (st.name == "create" || st.name == "update") &&
            !st.pos.empty() && !st.v.count("output"))
        {
            st.v["output"].push_back(st.pos[0]);
            st.pos.erase(st.pos.begin());
        }
        if (posArg)
        {
            for (const auto &p : st.pos)
                st.v[posArg].push_back(p);
            st.pos.clear();
        }
        else if (!st.pos.empty())
            return pipeFailMsg(c, CPLE_AppDefined,
                               st.name + ": Positional values starting "
                                         "at '" +
                                   st.pos[0] + "' are not expected.");
    }
    // the SQLITE gate fires at value parse, ahead of the list checks
    if (vec && st.name == "combine" && st.v.count("add-extra-fields"))
    {
        for (const auto &raw : st.v["add-extra-fields"])
        {
            std::string canon = raw;
            for (const char *ch :
                 {"no", "sometimes-identical", "always-identical"})
                if (strEqualNoCase(raw, ch))
                    canon = ch;
            if (canon != "no")
                return pipeFailMsg(c, CPLE_NotSupported,
                                   "combine: The SQLITE driver must be "
                                   "available for add-extra-fields=" +
                                       canon);
        }
    }
    for (const auto &a : cs->args)
    {
        if (isExcluded(a.name))
            continue;
        auto it = st.v.find(a.name);
        if (it == st.v.end() || it->second.empty())
            continue;
        std::vector<std::string> parts;
        for (const auto &raw : it->second)
            for (const auto &p : strSplit(raw, ','))
                parts.push_back(p);
        if (a.isList() && a.minCount >= 0 && a.minCount == a.maxCount &&
            (long long)parts.size() != a.minCount)
        {
            long long cnt = (long long)parts.size();
            return pipeFailMsg(
                c, CPLE_AppDefined,
                strPrintf("%s: %lld value%s been specified for argument "
                          "'%s', whereas exactly %lld were expected.",
                          st.name.c_str(), cnt,
                          cnt == 1 ? " has" : "s have", a.name.c_str(),
                          a.minCount));
        }
        if (st.name == "clip" && a.name == "window" && parts.size() == 4)
        {
            long long w = strtoll(parts[2].c_str(), nullptr, 10);
            long long h = strtoll(parts[3].c_str(), nullptr, 10);
            if (w <= 0 || h <= 0)
                return pipeFailMsg(
                    c, CPLE_AppDefined,
                    "Value of 'window' should be col,line,width,height "
                    "with width > 0 and height > 0");
        }
        if ((st.name == "clip" || st.name == "edit" ||
             st.name == "filter") &&
            a.name == "bbox" && parts.size() == 4)
        {
            double x0 = atof(parts[0].c_str());
            double y0 = atof(parts[1].c_str());
            double x1 = atof(parts[2].c_str());
            double y1 = atof(parts[3].c_str());
            // NaN corners fail these comparisons too
            if (!(x0 <= x1) || !(y0 <= y1))
                return pipeFailMsg(
                    c, CPLE_AppDefined,
                    "Value of 'bbox' should be xmin,ymin,xmax,ymax with "
                    "xmin <= xmax and ymin <= ymax");
        }
        if (st.name == "select" && a.name == "fields")
        {
            for (size_t x = 0; x < parts.size(); ++x)
                for (size_t y = x + 1; y < parts.size(); ++y)
                    if (parts[x] == parts[y])
                        return pipeFailMsg(c, CPLE_AppDefined,
                                           "'fields' must be a list of "
                                           "unique values.");
        }
        if (st.name == "combine" && a.name == "group-by")
        {
            for (size_t x = 0; x < parts.size(); ++x)
                for (size_t y = x + 1; y < parts.size(); ++y)
                    if (parts[x] == parts[y])
                        return pipeFailMsg(c, CPLE_AppDefined,
                                           "'group-by' must be a list of "
                                           "unique values.");
        }
        if (st.name == "edit" &&
            (a.name == "scale" || a.name == "offset"))
        {
            for (const auto &p : parts)
            {
                size_t eq = p.find('=');
                std::string val = p;
                if (eq != std::string::npos &&
                    cplValueType(p.substr(0, eq)) == 1)
                    val = p.substr(eq + 1);
                if (cplValueType(val) == 0)
                    return pipeFailMsg(
                        c, CPLE_IllegalArg,
                        strPrintf("edit: Invalid value '%s' for '%s'",
                                  p.c_str(), a.name.c_str()));
            }
        }
        if (st.name == "edit" &&
            (a.name == "metadata" || a.name == "layer-metadata"))
        {
            for (const auto &raw : it->second)
                if (raw.find('=') == std::string::npos)
                    return pipeFailMsg(
                        c, CPLE_AppDefined,
                        "edit: Invalid value for argument '" + a.name +
                            "'. <KEY>=<VALUE> expected");
        }
        if (st.name == "edit" && a.name == "gcp")
        {
            for (const auto &raw : it->second)
            {
                // only a lone @file value bypasses literal validation
                if (it->second.size() == 1 && raw.size() > 1 &&
                    raw[0] == '@')
                    continue;
                std::vector<std::string> gp = strSplit(raw, ',');
                bool ok = gp.size() == 4 || gp.size() == 5;
                for (size_t gi = 0; ok && gi < gp.size(); ++gi)
                    if (cplValueType(gp[gi]) == 0)
                        ok = false;
                if (!ok)
                    return pipeFailMsg(c, CPLE_IllegalArg,
                                       "edit: Bad format for " + raw);
            }
        }
    }
    // the pairing check is a parse-time consistency error (usage block
    // in multi-token mode), unlike the layer-existence checks at exec
    if (vec && st.name == "rename-layer" &&
        st.v.count("input-layer") && !st.v.count("output-layer"))
        return pipeFailMsg(c, CPLE_AppDefined,
                           "rename-layer: Argument output-layer must be "
                           "specified when input-layer is specified");
    // @<filename> indirection (where/sql style step args): the value is
    // replaced by the file content at parse time
    for (const auto &a : cs->args)
    {
        if (!vec || !vectorVerbStepName(st.name))
            break;
        if (a.metavar.find("|@<filename>") == std::string::npos ||
            isExcluded(a.name))
            continue;
        auto it = st.v.find(a.name);
        if (it == st.v.end())
            continue;
        for (auto &val : it->second)
        {
            if (val.empty() || val[0] != '@')
                continue;
            std::string content;
            if (!readFileToString(val.substr(1), content))
                return pipeFailMsg(c, CPLE_FileIO,
                                   "Cannot open file '" + val.substr(1) +
                                       "'");
            val = content;
        }
    }
    // the step-input open probe and required-argument checks run later,
    // after the read step's own open
    return 0;
}

int parseRasterStep(const PipeCtx &c, StepData &st)
{
    const CmdSpec *cs = Spec::instance().findById("raster_" + st.name);
    if (!cs)
        return 0;
    int rc = parseSpecStep(c, st, cs, false);
    if (rc)
        return rc;
    if (st.name == "select")
    {
        if (!rasterSelectBandTokensValid(st.list("band")))
            return pipeFailMsg(c, CPLE_AppDefined,
                               "Invalid band specification.");
        if (!rasterSelectMaskTokenValid(st.str("mask")))
            return pipeFailMsg(c, CPLE_AppDefined,
                               "Invalid mask band specification.");
    }
    // reproject exists on both sides: a generic pipeline resolving to
    // vector re-parses it with the vector spec after dispatch
    if (c.type == P_GENERIC && st.name != "reproject")
        st.usedRasOnly = true;
    return 0;
}

int parseStep(const PipeCtx &c, StepData &st, bool isFirst,
              bool afterTrans = false)
{
    const std::vector<OptDef> *defs = nullptr;
    if (st.name == "read")
        defs = &readDefs();
    else if (st.name == "write")
        defs = &writeDefs();
    else if (st.name == "info")
        defs = &infoDefs();
    else if (st.name == "concat" && isFirst)
        defs = &concatDefs();
    else if (c.type == P_GENERIC && transStepName(st.name))
    {
        st.trans = true;
        return parseSpecStep(
            c, st, Spec::instance().findById("raster_" + st.name), false);
    }
    else if (c.type == P_GENERIC && st.name == "rasterize")
    {
        st.transRas = true;
        int rc = parseSpecStep(
            c, st, Spec::instance().findById("vector_rasterize"), true);
        if (rc)
            return rc;
        // the step keeps the leaf's parse-time validators: they fire
        // before any write pre-check or gdalg serialization
        for (const auto &bv : st.list("band"))
            for (const auto &p : strSplit(bv, ','))
                if (atoi(p.c_str()) < 1)
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "Value of 'band' should greater "
                                       "or equal to 1.");
        if (st.has("crs"))
        {
            bool ok = false;
            Srs s = Srs::fromCliInput(st.str("crs"), ok, true);
            if (!ok || !s.valid())
                return pipeFailMsg(c, CPLE_AppDefined,
                                   "rasterize: Invalid value for 'crs' "
                                   "argument");
        }
        return 0;
    }
    else if (c.type == P_GENERIC && afterTrans &&
             (st.name == "reproject" || st.name == "update" ||
              vectorVerbStepName(st.name) || geosWallStepName(st.name)))
        return parseSpecStep(
            c, st, Spec::instance().findById("vector_" + st.name), true);
    else if (c.type == P_VECTOR &&
             (st.name == "reproject" || st.name == "export-schema" ||
              st.name == "create" || st.name == "update" ||
              vectorVerbStepName(st.name) ||
              geosWallStepName(st.name)))
        return parseSpecStep(
            c, st, Spec::instance().findById("vector_" + st.name), true);
    else if (c.type == P_GENERIC &&
             (st.name == "filter" || st.name == "sql" ||
              st.name == "swap-xy" || st.name == "segmentize" ||
              st.name == "make-point" ||
              st.name == "explode-collections" ||
              st.name == "set-geom-type" || st.name == "rename-layer" ||
              st.name == "combine" || st.name == "export-schema" ||
              geosWallStepName(st.name)))
    {
        // vector-only steps: a raster dispatch later reports the
        // vector-input mismatch
        st.usedVecOnly = true;
        return parseSpecStep(
            c, st, Spec::instance().findById("vector_" + st.name), true);
    }
    else if (c.type == P_GENERIC && st.name == "select")
    {
        st.selDual = true;
        return 0;
    }
    else if (c.type != P_VECTOR && rasterTailStepKnown(st.name))
    {
        if (c.type == P_GENERIC &&
            (st.name == "reproject" || st.name == "edit" ||
             st.name == "clip"))
        {
            // try the raster variant silently; on failure the step is
            // vector-only and the vector parse reports the errors
            PipeCtx quietCtx = c;
            quietCtx.alias = true;
            cplPushQuietHandler();
            int rc = parseSpecStep(
                quietCtx, st,
                Spec::instance().findById("raster_" + st.name), false);
            cplPopHandler();
            if (!rc)
                return 0;
            st.v.clear();
            st.pos.clear();
            st.usedVecOnly = true;
            return parseSpecStep(
                c, st, Spec::instance().findById("vector_" + st.name),
                true);
        }
        return parseRasterStep(c, st);
    }
    else
        return 0;
    for (size_t i = 0; i < st.tokens.size(); ++i)
    {
        const std::string &tok = st.tokens[i];
        if (strStartsWith(tok, "--") && tok.size() > 2)
        {
            std::string body = tok.substr(2);
            size_t eq = body.find('=');
            std::string name =
                eq == std::string::npos ? body : body.substr(0, eq);
            const OptDef *d = findDef(*defs, name, false);
            if (d && c.type == P_RASTER && d->side == S_VEC)
                d = nullptr;
            if (d && c.type == P_VECTOR && d->side == S_RAS)
                d = nullptr;
            if (!d)
            {
                std::vector<std::string> cands = {"help", "help-doc",
                                                  "json-usage", "config"};
                for (const auto &dd : *defs)
                {
                    if (c.type == P_RASTER && dd.side == S_VEC)
                        continue;
                    if (c.type == P_VECTOR && dd.side == S_RAS)
                        continue;
                    for (const char *cand : dd.longs)
                        cands.push_back(cand);
                    for (const char *cand : dd.hiddenLongs)
                        cands.push_back(cand);
                }
                std::string sug = suggestOptionName(name, cands);
                if (!sug.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                st.name + ": Option '--" + name +
                                    "' is unknown. Do you mean '--" + sug +
                                    "'?");
                    return kParseFail;
                }
                return pipeFailMsg(c, CPLE_IllegalArg,
                                   st.name + ": Option '--" + name +
                                       "' is unknown.");
            }
            if (d->side == S_VEC)
                st.usedVecOnly = true;
            if (d->side == S_RAS)
                st.usedRasOnly = true;
            std::string value;
            if (d->takesValue)
            {
                if (eq != std::string::npos)
                    value = body.substr(eq + 1);
                else if (i + 1 < st.tokens.size())
                    value = st.tokens[++i];
                else
                    return pipeFailMsg(c, CPLE_IllegalArg,
                                       st.name + ": Expected value for "
                                                 "argument '--" +
                                           name +
                                           "', but ran short of tokens");
            }
            else
                value = "true";
            st.v[d->canon].push_back(value);
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' && !isdigit((unsigned char)tok[1]) &&
            tok[1] != '.' && tok[1] != '-')
        {
            std::string sn = tok.substr(1, 1);
            const OptDef *d = findDef(*defs, sn, true);
            if (d && c.type == P_RASTER && d->side == S_VEC)
                d = nullptr;
            if (d && c.type == P_VECTOR && d->side == S_RAS)
                d = nullptr;
            if (!d)
                return pipeFailMsg(c, CPLE_IllegalArg,
                                   st.name + ": Short name option '" + sn +
                                       "' is unknown.");
            if (d->side == S_VEC)
                st.usedVecOnly = true;
            if (d->side == S_RAS)
                st.usedRasOnly = true;
            std::string value = "true";
            if (d->takesValue)
            {
                if (i + 1 < st.tokens.size())
                    value = st.tokens[++i];
                else
                    return pipeFailMsg(c, CPLE_IllegalArg,
                                       st.name + ": Expected value for "
                                                 "argument '" +
                                           sn +
                                           "', but ran short of tokens");
            }
            st.v[d->canon].push_back(value);
            continue;
        }
        st.pos.push_back(tok);
    }
    // positional assignment and excess detection
    size_t used = 0;
    if (st.name == "read" && isFirst && !st.has("input") &&
        !st.pos.empty())
    {
        st.v["input"].push_back(st.pos[0]);
        used = 1;
    }
    else if (st.name == "concat")
    {
        for (const auto &pv : st.pos)
            st.v["input"].push_back(pv);
        used = st.pos.size();
    }
    else if (st.name == "write" && !st.has("output") && !st.pos.empty())
    {
        st.v["output"].push_back(st.pos[0]);
        used = 1;
    }
    if (st.pos.size() > used)
        return pipeFailMsg(c, CPLE_AppDefined,
                           st.name + ": Positional values starting at '" +
                               st.pos[used] + "' are not expected.");
    if (st.name == "read" && isFirst && !st.has("input"))
        return pipeFailMsg(c, CPLE_AppDefined,
                           "read: Positional arguments starting at 'INPUT' "
                           "have not been specified.");
    if (st.name == "concat" && !st.has("input"))
        return pipeFailMsg(c, CPLE_AppDefined,
                           "concat: Positional arguments starting at "
                           "'INPUTS' have not been specified.");
    if (st.name == "concat" && st.has("mode"))
    {
        const std::string &m = st.str("mode");
        if (m != "merge-per-layer-name" && m != "stack" && m != "single")
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "Invalid value '" + m +
                            "' for string argument 'mode'. Should be one "
                            "among 'merge-per-layer-name', 'stack', "
                            "'single'.");
            return kParseFail;
        }
    }
    if (st.name == "write" && !st.has("output"))
        return pipeFailMsg(c, CPLE_AppDefined,
                           "write: Positional arguments starting at "
                           "'OUTPUT' have not been specified.");
    if (st.name == "info" && st.has("of"))
    {
        const std::string &f = st.str("of");
        if (f != "json" && f != "text")
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "Invalid value '" + f +
                            "' for string argument 'output-format'. Should "
                            "be one among 'json', 'text'.");
            return kParseFail;
        }
    }
    if ((st.name == "read" || st.name == "concat" ||
         st.name == "info") &&
        st.has("input-format"))
        for (const auto &f : st.list("input-format"))
        {
            std::string err = inputFormatError(
                st.name == "read" ? c.type : P_VECTOR, f);
            if (!err.empty())
                return pipeFailMsg(c, CPLE_AppDefined,
                                   st.name + ": " + err);
        }
    return 0;
}

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

std::string joinComma(const std::vector<std::string> &v)
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

std::string buildGdalgReadCli(const PipeCtx &c, const StepData &read)
{
    if (read.name == "concat")
    {
        std::string cli = c.invokedCli + " concat";
        if (read.has("input-format"))
            cli += " --input-format " +
                   joinComma(read.list("input-format"));
        if (read.has("open-option"))
            cli +=
                " --open-option " + joinComma(read.list("open-option"));
        for (const auto &v : read.list("input"))
            cli += " --input " + v;
        if (read.has("input-layer"))
            cli += " --input-layer " +
                   joinComma(read.list("input-layer"));
        if (read.has("mode"))
            cli += " --mode " + read.str("mode");
        if (read.has("output-layer"))
            cli += " --output-layer " + read.str("output-layer");
        if (read.has("source-layer-field-name"))
            cli += " --source-layer-field-name " +
                   read.str("source-layer-field-name");
        if (read.has("source-layer-field-content"))
            cli += " --source-layer-field-content " +
                   read.str("source-layer-field-content");
        if (read.has("field-strategy"))
            cli += " --field-strategy " + read.str("field-strategy");
        if (read.has("src-crs"))
            cli += " --src-crs " + read.str("src-crs");
        if (read.has("dst-crs"))
            cli += " --dst-crs " + read.str("dst-crs");
        return cli;
    }
    std::string cli = c.invokedCli + " read";
    if (read.has("input-format"))
        cli += " --input-format " + joinComma(read.list("input-format"));
    if (read.has("open-option"))
        cli += " --open-option " + joinComma(read.list("open-option"));
    cli += " --input " + read.str("input");
    if (read.has("input-layer"))
        cli += " --input-layer " + joinComma(read.list("input-layer"));
    return cli;
}

// vector-side single 'read' step parse quirk: a positional is reported as
// unexpected at pipeline level, option-only forms fail silently
int vectorSingleReadQuirk(const PipeCtx &c, const std::vector<std::string> &tk)
{
    const std::vector<OptDef> &defs = readDefs();
    for (size_t i = 1; i < tk.size(); ++i)
    {
        const std::string &tok = tk[i];
        if (strStartsWith(tok, "--") && tok.size() > 2)
        {
            std::string body = tok.substr(2);
            size_t eq = body.find('=');
            std::string name =
                eq == std::string::npos ? body : body.substr(0, eq);
            const OptDef *d = findDef(defs, name, false);
            if (d && d->takesValue && eq == std::string::npos &&
                i + 1 < tk.size())
                ++i;
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' &&
            !isdigit((unsigned char)tok[1]) && tok[1] != '.' && tok[1] != '-')
        {
            const OptDef *d = findDef(defs, tok.substr(1, 1), true);
            if (d && d->takesValue && i + 1 < tk.size())
                ++i;
            continue;
        }
        return pipeFailMsg(c, CPLE_AppDefined,
                           "pipeline: Positional values starting at '" +
                               tok + "' are not expected.");
    }
    if (tk.size() > 1)
        return pipeFail(c);  // options only: silent usage
    return pipeFailMsg(c, CPLE_AppDefined,
                       "pipeline: At least 2 steps must be provided");
}

// resolve the read-step input value with a light scan (used before the
// full step parse, for dispatch and the single-step paths)
std::string quickReadInput(const std::vector<std::string> &tk)
{
    const std::vector<OptDef> &defs = readDefs();
    std::string named;
    std::string firstPos;
    for (size_t i = 1; i < tk.size(); ++i)
    {
        const std::string &tok = tk[i];
        if (strStartsWith(tok, "--") && tok.size() > 2)
        {
            std::string body = tok.substr(2);
            size_t eq = body.find('=');
            std::string name =
                eq == std::string::npos ? body : body.substr(0, eq);
            const OptDef *d = findDef(defs, name, false);
            std::string value;
            if (d && d->takesValue)
            {
                if (eq != std::string::npos)
                    value = body.substr(eq + 1);
                else if (i + 1 < tk.size())
                    value = tk[++i];
            }
            if (d && std::string(d->canon) == "input" && named.empty())
                named = value;
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' &&
            !isdigit((unsigned char)tok[1]) && tok[1] != '.' && tok[1] != '-')
        {
            const OptDef *d = findDef(defs, tok.substr(1, 1), true);
            std::string value;
            if (d && d->takesValue && i + 1 < tk.size())
                value = tk[++i];
            if (d && std::string(d->canon) == "input" && named.empty())
                named = value;
            continue;
        }
        if (firstPos.empty())
            firstPos = tok;
    }
    return named.empty() ? firstPos : named;
}

bool readHasVectorOnlyOpt(const std::vector<std::string> &tk)
{
    for (size_t i = 1; i < tk.size(); ++i)
    {
        const std::string &tok = tk[i];
        if (tok == "-l" || tok == "--layer" || tok == "--input-layer" ||
            strStartsWith(tok, "--layer=") ||
            strStartsWith(tok, "--input-layer=") || tok == "--if" ||
            tok == "--input-format" || strStartsWith(tok, "--if=") ||
            strStartsWith(tok, "--input-format="))
            return true;
    }
    return false;
}

// empty result means valid; otherwise the error message body
std::string inputFormatError(PipeType t, const std::string &drv)
{
    return inputFormatCapError(t != P_RASTER, drv);
}

// composes the transform-step wrappers applied by the convert/info
// handlers at their materialize point; failures print their own error
// (progress line handled here, usage never)
void installTailMaterialize(const PipeCtx &c,
                            const std::vector<const StepData *> &transforms,
                            bool qEff, bool progressOnError,
                            bool presentVrt)
{
    if (transforms.empty())
        return;
    g_pipelineDemVrtVerb.clear();
    for (const StepData *t : transforms)
        if (rasterDemStepName(t->name))
        {
            g_pipelineDemVrtVerb = t->name;
            break;
        }
    std::vector<StepData> copies;
    for (const StepData *t : transforms)
        copies.push_back(*t);
    g_pipelineTailMaterialize =
        [copies, qEff, progressOnError,
         presentVrt](std::unique_ptr<RasterDatasetBase> &d) -> int {
        for (size_t k = 0; k < copies.size(); ++k)
        {
            if (!copies[k].has("input"))
                continue;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("pipeline: Step nr %zu (%s) does not "
                                  "use input dataset from previous step",
                                  k + 1, copies[k].name.c_str()));
            return 1;
        }
        for (const StepData &t : copies)
        {
            int rc = rasterTailApplyPipeStep(t.name, t.v, d);
            if (rc)
            {
                if (progressOnError && !qEff)
                    printProgress();
                return 1;
            }
        }
        if (presentVrt && !d->inMemoryVrtCopy)
        {
            std::string srcPath = d->path;
            presentAsTranslatedVrt(*d);
            // a chain of wrappers nests a nameless VRT inside another,
            // losing the source file association; a single wrapper lists
            // only the source dataset name (never its own file list)
            if (copies.size() > 1)
                d->files.clear();
            else if (!srcPath.empty())
                d->files = {srcPath};
        }
        return 0;
    };
    (void)c;
}

// canonical serialization of a consumed non-terminal write step for the
// GDALG command line of a later terminal write
std::string writeStepEcho(const StepData &w)
{
    std::string s = " ! write";
    if (w.has("of"))
        s += " --output-format " + w.str("of");
    s += " --output " + w.str("output");
    for (const auto &v : w.list("co"))
        s += " --creation-option " + v;
    for (const auto &v : w.list("lco"))
        s += " --layer-creation-option " + v;
    if (w.has("output-layer"))
        s += " --output-layer " + w.str("output-layer");
    for (const char *fl : {"overwrite", "append", "update",
                           "overwrite-layer", "upsert", "skip-errors"})
        if (w.has(fl))
            s += std::string(" --") + fl;
    return s;
}

// GDALG command-line serialization wraps values holding spaces or quotes
std::string stepGq(const std::string &v)
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

// canonical spec-ordered " ! <step> --args" echo for transition steps
// (contour/polygonize/footprint): long names, normalized numerics,
// comma-joined lists, quiet and I/O plumbing omitted
std::string transStepEcho(const StepData &st)
{
    std::string s = " ! " + st.name;
    const CmdSpec *cs = Spec::instance().findById(
        (st.transRas ? "vector_" : "raster_") + st.name);
    if (!cs)
        return s;
    static const std::set<std::string> skip = {
        "input",         "output",
        "output-format", "creation-option",
        "layer-creation-option", "output-open-option",
        "open-option",   "input-format",
        "overwrite",     "update",
        "overwrite-layer", "append",
        "upsert",        "skip-errors",
        "help",          "dataset",
        "quiet"};
    for (const auto &a : cs->args)
    {
        if (skip.count(a.name) || !st.has(a.name))
            continue;
        if (a.isBool())
        {
            if (st.str(a.name) != "false")
                s += " --" + a.name;
            continue;
        }
        if (a.isList())
        {
            std::vector<std::string> parts;
            for (const auto &raw : st.list(a.name))
                for (const auto &p : strSplit(raw, ','))
                {
                    if (a.type == "real_list")
                        parts.push_back(strPrintf(
                            "%.17g", strtod(p.c_str(), nullptr)));
                    else if (a.type == "integer_list")
                        parts.push_back(strPrintf(
                            "%lld", strtoll(p.c_str(), nullptr, 10)));
                    else
                        parts.push_back(p);
                }
            s += " --" + a.name + " " + stepGq(joinComma(parts));
        }
        else if (a.type == "real")
            s += " --" + a.name + " " +
                 strPrintf("%.17g",
                           strtod(st.str(a.name).c_str(), nullptr));
        else if (a.type == "integer")
            s += " --" + a.name + " " +
                 strPrintf("%lld",
                           strtoll(st.str(a.name).c_str(), nullptr, 10));
        else
            s += " --" + a.name + " " + stepGq(st.str(a.name));
    }
    return s;
}

// canonical " ! <step> --args" echo for vector transform steps
std::string vectorStepEcho(const StepData &st)
{
    if (st.name == "reproject")
        return vectorReprojectStepEcho(st.v);
    std::string s = " ! " + st.name;
    std::string v;
    if (st.name == "filter")
    {
        if (!(v = st.str("active-layer")).empty())
            s += " --active-layer " + stepGq(v);
        if (st.has("bbox"))
        {
            std::string joined;
            for (const auto &raw : st.list("bbox"))
                for (const auto &p : strSplit(raw, ','))
                {
                    if (!joined.empty())
                        joined += ",";
                    joined += strPrintf("%.17g",
                                        strtod(p.c_str(), nullptr));
                }
            s += " --bbox " + joined;
        }
        if (!(v = st.str("where")).empty())
            s += " --where " + stepGq(v);
        if (st.has("update-extent"))
            s += " --update-extent";
    }
    else if (st.name == "select")
    {
        if (!(v = st.str("active-layer")).empty())
            s += " --active-layer " + stepGq(v);
        if (st.has("fields"))
            s += " --fields " + stepGq(joinComma(st.list("fields")));
        if (st.has("exclude"))
            s += " --exclude";
        if (st.has("ignore-missing-fields"))
            s += " --ignore-missing-fields";
    }
    else if (st.name == "sql")
    {
        for (const auto &q : st.list("sql"))
            s += " --sql " + stepGq(q);
        if (st.has("output-layer"))
            s += " --output-layer " +
                 stepGq(joinComma(st.list("output-layer")));
        if (!(v = st.str("dialect")).empty())
            s += " --dialect " + stepGq(v);
    }
    else if (st.name == "swap-xy" || st.name == "segmentize" ||
             st.name == "explode-collections" ||
             st.name == "set-geom-type")
    {
        if (!(v = st.str("active-layer")).empty())
            s += " --active-layer " + stepGq(v);
        if (!(v = st.str("active-geometry")).empty())
            s += " --active-geometry " + stepGq(v);
        if (st.name == "segmentize")
            s += " --max-length " +
                 strPrintf("%.17g",
                           strtod(st.str("max-length").c_str(), nullptr));
        else if (st.name == "explode-collections")
        {
            if (!(v = st.str("geometry-type")).empty())
                s += " --geometry-type " + stepGq(v);
            if (st.has("skip-on-type-mismatch"))
                s += " --skip-on-type-mismatch";
        }
        else if (st.name == "set-geom-type")
        {
            if (st.has("layer-only"))
                s += " --layer-only";
            if (st.has("feature-only"))
                s += " --feature-only";
            if (!(v = st.str("geometry-type")).empty())
                s += " --geometry-type " + stepGq(v);
            if (st.has("multi"))
                s += " --multi";
            if (st.has("single"))
                s += " --single";
            if (st.has("linear"))
                s += " --linear";
            if (st.has("curve"))
                s += " --curve";
            if (!(v = st.str("dim")).empty())
                s += " --dim " + stepGq(v);
            if (st.has("skip"))
                s += " --skip";
        }
    }
    else if (st.name == "clip")
    {
        if (!(v = st.str("active-layer")).empty())
            s += " --active-layer " + stepGq(v);
        if (st.has("bbox"))
        {
            std::string joined;
            for (const auto &raw : st.list("bbox"))
                for (const auto &p : strSplit(raw, ','))
                {
                    if (!joined.empty())
                        joined += ",";
                    joined += strPrintf("%.17g",
                                        strtod(p.c_str(), nullptr));
                }
            s += " --bbox " + joined;
        }
        if (!(v = st.str("bbox-crs")).empty())
            s += " --bbox-crs " + stepGq(v);
        if (!(v = st.str("geometry")).empty())
            s += " --geometry " + stepGq(v);
        if (!(v = st.str("geometry-crs")).empty())
            s += " --geometry-crs " + stepGq(v);
        if (!(v = st.str("like")).empty())
            s += " --like " + stepGq(v);
        if (!(v = st.str("like-sql")).empty())
            s += " --like-sql " + stepGq(v);
        if (!(v = st.str("like-layer")).empty())
            s += " --like-layer " + stepGq(v);
        if (!(v = st.str("like-where")).empty())
            s += " --like-where " + stepGq(v);
    }
    else if (st.name == "make-point")
    {
        s += " --x " + stepGq(st.str("x"));
        s += " --y " + stepGq(st.str("y"));
        if (!(v = st.str("z")).empty())
            s += " --z " + stepGq(v);
        if (!(v = st.str("m")).empty())
            s += " --m " + stepGq(v);
        if (!(v = st.str("dst-crs")).empty())
            s += " --dst-crs " + stepGq(v);
    }
    else if (st.name == "edit")
    {
        if (!(v = st.str("active-layer")).empty())
            s += " --active-layer " + stepGq(v);
        if (!(v = st.str("geometry-type")).empty())
            s += " --geometry-type " + stepGq(v);
        if (!(v = st.str("crs")).empty())
            s += " --crs " + stepGq(v);
        auto listEcho = [&](const char *key) {
            std::string joined;
            for (const auto &raw : st.list(key))
                for (const auto &p : strSplit(raw, ','))
                {
                    if (!joined.empty())
                        joined += ",";
                    joined += stepGq(p);
                }
            return joined;
        };
        for (const auto &m : st.list("metadata"))
            s += " --metadata " + stepGq(m);
        if (st.has("unset-metadata"))
            s += " --unset-metadata " + listEcho("unset-metadata");
        for (const auto &m : st.list("layer-metadata"))
            s += " --layer-metadata " + stepGq(m);
        if (st.has("unset-layer-metadata"))
            s += " --unset-layer-metadata " +
                 listEcho("unset-layer-metadata");
        if (st.has("unset-fid"))
            s += " --unset-fid";
    }
    else if (st.name == "combine")
    {
        if (st.has("group-by"))
        {
            std::string joined;
            for (const auto &raw : st.list("group-by"))
                for (const auto &p : strSplit(raw, ','))
                {
                    if (!joined.empty())
                        joined += ",";
                    joined += stepGq(p);
                }
            s += " --group-by " + joined;
        }
        if (st.has("keep-nested"))
            s += " --keep-nested";
        if (st.has("add-extra-fields"))
            s += " --add-extra-fields " + stepGq(st.str("add-extra-fields"));
    }
    else if (st.name == "set-field-type")
    {
        if (!(v = st.str("active-layer")).empty())
            s += " --active-layer " + stepGq(v);
        if (!(v = st.str("field-name")).empty())
            s += " --field-name " + stepGq(v);
        if (!(v = st.str("src-field-type")).empty())
            s += " --src-field-type " + stepGq(v);
        if (!(v = st.str("field-type")).empty())
            s += " --field-type " + stepGq(v);
    }
    else if (st.name == "update")
    {
        if (!(v = st.str("input-layer")).empty())
            s += " --input-layer " + stepGq(v);
        s += " --output " + stepGq(st.str("output"));
        for (const auto &m : st.list("output-open-option"))
            s += " --output-open-option " + stepGq(m);
        if (!(v = st.str("output-layer")).empty())
            s += " --output-layer " + stepGq(v);
        if (st.has("update") && st.str("update") != "false")
            s += " --update";
        if (!(v = st.str("mode")).empty())
            s += " --mode " + stepGq(v);
        for (const auto &k : st.list("key"))
            s += " --key " + stepGq(k);
    }
    else if (st.name == "rename-layer")
    {
        if (!(v = st.str("input-layer")).empty())
            s += " --input-layer " + stepGq(v);
        if (!(v = st.str("output-layer")).empty())
            s += " --output-layer " + stepGq(v);
        if (st.has("ascii"))
            s += " --ascii";
        if (st.has("lower-case"))
            s += " --lower-case";
        if (st.has("filename-compatible"))
            s += " --filename-compatible";
        if (st.has("reserved-characters"))
            s += " --reserved-characters " +
                 stepGq(st.str("reserved-characters"));
        if (st.has("replacement-character"))
            s += " --replacement-character " +
                 stepGq(st.str("replacement-character"));
        if (st.has("max-length"))
            s += " --max-length " + st.str("max-length");
    }
    else if (geosWallStepName(st.name))
    {
        // spec-order generic echo (only explicitly set arguments)
        const CmdSpec *cs =
            Spec::instance().findById("vector_" + st.name);
        if (cs)
            for (const auto &a : cs->args)
            {
                if (a.name == "input")
                    continue;
                auto it = st.v.find(a.name);
                if (it == st.v.end() || it->second.empty())
                    continue;
                if (a.isBool())
                {
                    if (it->second[0] == "true")
                        s += " --" + a.name;
                    continue;
                }
                auto fmt = [&](const std::string &p) {
                    return (a.type == "real" || a.type == "real_list")
                               ? strPrintf("%.17g",
                                           strtod(p.c_str(), nullptr))
                               : stepGq(p);
                };
                std::string joined;
                if (a.isList())
                    for (const auto &raw : it->second)
                        for (const auto &p : strSplit(raw, ','))
                        {
                            if (!joined.empty())
                                joined += ",";
                            joined += fmt(p);
                        }
                else
                    joined = fmt(it->second.back());
                s += " --" + a.name + " " + joined;
            }
    }
    return s;
}

// applies one vector verb transform step to the chained dataset
int vectorVerbApplyPipeStep(const StepData &t, OgrDataset &d)
{
    if (t.name == "filter")
    {
        double bb[4] = {0, 0, 0, 0};
        bool hasBbox = t.has("bbox");
        if (hasBbox)
        {
            std::vector<std::string> parts;
            for (const auto &raw : t.list("bbox"))
                for (const auto &p : strSplit(raw, ','))
                    parts.push_back(p);
            for (size_t i = 0; i < 4 && i < parts.size(); ++i)
                bb[i] = strtod(parts[i].c_str(), nullptr);
        }
        return vectorFilterApplyStep(d, t.str("where"),
                                     t.str("active-layer"), {}, hasBbox,
                                     bb);
    }
    if (t.name == "select")
    {
        std::vector<std::string> fields;
        for (const auto &raw : t.list("fields"))
            for (const auto &p : strSplit(raw, ','))
                fields.push_back(p);
        return vectorSelectApplyStep(d, fields, t.has("exclude"),
                                     t.has("ignore-missing-fields"),
                                     t.str("active-layer"), {});
    }
    if (t.name == "sql")
    {
        std::vector<std::string> names;
        for (const auto &raw : t.list("output-layer"))
            for (const auto &p : strSplit(raw, ','))
                names.push_back(p);
        return vectorSqlApplyStep(d, t.list("sql"), names,
                                  t.str("dialect"), "sql");
    }
    if (t.name == "clip")
    {
        VectorClipOpts o;
        o.hasBbox = t.has("bbox");
        if (o.hasBbox)
        {
            std::vector<std::string> parts;
            for (const auto &raw : t.list("bbox"))
                for (const auto &p : strSplit(raw, ','))
                    parts.push_back(p);
            for (size_t i = 0; i < 4 && i < parts.size(); ++i)
                o.bbox[i] = strtod(parts[i].c_str(), nullptr);
        }
        o.bboxCrs = t.str("bbox-crs");
        o.geometry = t.str("geometry");
        o.geometryCrs = t.str("geometry-crs");
        o.like = t.str("like");
        o.hasLike = t.has("like");
        o.likeSql = t.str("like-sql");
        o.likeLayer = t.str("like-layer");
        o.likeWhere = t.str("like-where");
        o.activeLayer = t.str("active-layer");
        return vectorClipApplyStep(d, o, {});
    }
    if (t.name == "combine")
    {
        VectorCombineOpts o;
        for (const auto &raw : t.list("group-by"))
            for (const auto &p : strSplit(raw, ','))
                o.groupBy.push_back(p);
        o.keepNested = t.has("keep-nested");
        return vectorCombineApplyStep(d, o, {});
    }
    if (t.name == "swap-xy")
        return vectorSwapXyApplyStep(d, t.str("active-geometry"),
                                     t.str("active-layer"), {});
    if (t.name == "segmentize")
        return vectorSegmentizeApplyStep(
            d, strtod(t.str("max-length").c_str(), nullptr),
            t.str("active-geometry"), t.str("active-layer"), {});
    if (t.name == "make-point")
        return vectorMakePointApplyStep(d, t.str("x"), t.str("y"),
                                        t.str("z"), t.str("m"),
                                        t.str("dst-crs"), {});
    if (t.name == "explode-collections")
        return vectorExplodeApplyStep(d, t.str("geometry-type"),
                                      t.has("skip-on-type-mismatch"),
                                      t.str("active-geometry"),
                                      t.str("active-layer"), {});
    if (t.name == "set-geom-type")
    {
        SetGeomTypeOpts o;
        o.geomType = t.str("geometry-type");
        o.multi = t.has("multi");
        o.single = t.has("single");
        o.linear = t.has("linear");
        o.curve = t.has("curve");
        o.dim = t.str("dim");
        o.layerOnly = t.has("layer-only");
        o.featureOnly = t.has("feature-only");
        o.skip = t.has("skip");
        o.activeGeom = t.str("active-geometry");
        o.activeLayer = t.str("active-layer");
        return vectorSetGeomTypeApplyStep(d, o);
    }
    if (t.name == "edit")
    {
        VectorEditOpts o;
        o.geomType = t.str("geometry-type");
        o.crs = t.str("crs");
        o.metadata = t.list("metadata");
        for (const auto &raw : t.list("unset-metadata"))
            for (const auto &p : strSplit(raw, ','))
                o.unsetMetadata.push_back(p);
        o.layerMetadata = t.list("layer-metadata");
        for (const auto &raw : t.list("unset-layer-metadata"))
            for (const auto &p : strSplit(raw, ','))
                o.unsetLayerMetadata.push_back(p);
        o.unsetFid = t.has("unset-fid");
        o.activeLayer = t.str("active-layer");
        return vectorEditApplyStep(d, o);
    }
    if (t.name == "rename-layer")
    {
        VectorRenameLayerOpts o;
        o.inputLayer = t.str("input-layer");
        o.outputLayer = t.str("output-layer");
        o.ascii = t.has("ascii");
        o.lowerCase = t.has("lower-case");
        o.fnCompat = t.has("filename-compatible");
        o.reserved = t.str("reserved-characters");
        o.hasReplacement = t.has("replacement-character");
        o.replacement = t.str("replacement-character");
        if (t.has("max-length"))
            o.maxLength =
                strtoll(t.str("max-length").c_str(), nullptr, 10);
        return vectorRenameLayerApplyStep(d, o);
    }
    return 0;
}

// eager application of a mixed transform chain. Streaming attribute
// filters push down through warped layers to the base layer, so a
// where-only filter behind reproject hops reports the extent of the
// grid-sampled reprojection of the base layer's filtered extent; the
// eager equivalent runs pure-where filters before the hops. select and
// sql outputs are opaque to push-down and bound the reordering; bbox
// and --update-extent evaluate in place.
int vectorChainApply(const std::vector<const StepData *> &transforms,
                     OgrDataset &d, bool initOpaque = false,
                     bool featuresNeeded = true, bool extentNeeded = true)
{
    auto pureWhere = [](const StepData &t) {
        return t.name == "filter" && !t.has("bbox") &&
               !t.has("update-extent");
    };
    // any step other than the filter/reproject pair bounds the
    // push-down reordering
    auto orderBoundary = [](const std::string &nm) {
        return nm != "filter" && nm != "reproject";
    };
    std::vector<const StepData *> order;
    size_t i = 0, n = transforms.size();
    while (i < n)
    {
        size_t j = i;
        while (j < n && orderBoundary(transforms[j]->name) == false)
            ++j;
        for (size_t k = i; k < j; ++k)
            if (pureWhere(*transforms[k]))
                order.push_back(transforms[k]);
        for (size_t k = i; k < j; ++k)
            if (!pureWhere(*transforms[k]))
                order.push_back(transforms[k]);
        if (j < n)
            order.push_back(transforms[j]);
        i = j + 1;
    }

    // 0 = fresh (a first where recomputes the extent), 1 = filtered,
    // 2 = opaque output (a later where keeps the source extent);
    // layers recaptured from a middle write are exhausted streams whose
    // stored extent survives filtering
    std::vector<char> state(d.layers.size(), initOpaque ? 2 : 0);
    auto selected = [](const StepData &t, const OgrLayer &l) {
        std::string a = t.str("active-layer");
        return a.empty() || l.name == a;
    };
    // combine materializes its input: a preceding clip's deferred
    // per-feature errors replay at the combine pull instead of the
    // terminal's, and the layer failure is defused (the empty layer
    // writes cleanly). A non-summary info terminal reaches the combine
    // twice unless a materializing step (select/sql/edit/clip) sits
    // between them; every other terminal reaches it once.
    size_t lastCombine = order.size();
    for (size_t k = 0; k < order.size(); ++k)
        if (order[k]->name == "combine")
            lastCombine = k;
    int savedEmitPulls = g_vectorClipEmitPulls;
    // set-field-type: a contiguous leading run of steps after a GeoJSON
    // read is absorbed into the read as one schema-override group; any
    // other position runs SetFrom per step. Warning replay counts how
    // often the terminal facet decodes the producing stream:
    //   write     raw pull x1 (0 for stream/unresolvable outputs)
    //   summary   never
    //   info      1 + rescanning follower (filter/edit/combine/clip)
    //             - materializing select follower (cast); SetFrom's
    //             materialized stream is pulled twice, minus select
    //   features  one more listing pull
    //   fid       the listing pull plus the named feature's messages,
    //             unless a rescanner (cast) or combine (SetFrom)
    //             breaks direct feature access
    enum SftFacet
    {
        SFT_WRITE,
        SFT_SUMMARY,
        SFT_PLAIN,
        SFT_FEATURES,
        SFT_FID
    };
    SftFacet sftFacet =
        savedEmitPulls < 0
            ? SFT_SUMMARY
            : savedEmitPulls == 0
                  ? SFT_WRITE
                  : g_vectorInfoFid >= 0
                        ? SFT_FID
                        : savedEmitPulls >= 3 ? SFT_FEATURES : SFT_PLAIN;
    size_t castEnd = 0;
    if (d.driverShort == "GeoJSON" && !d.capturedStream && !initOpaque)
        while (castEnd < transforms.size() &&
               transforms[castEnd]->name == "set-field-type")
            ++castEnd;
    g_vectorSftAllCast = castEnd > 0 && castEnd == transforms.size();
    auto sftOpts = [](const StepData &t) {
        VectorSetFieldTypeOpts o;
        o.hasFieldName = t.has("field-name");
        o.fieldName = t.str("field-name");
        o.hasSrcType = t.has("src-field-type");
        o.srcTypeName = t.str("src-field-type");
        o.hasDstType = t.has("field-type");
        o.dstTypeName = t.str("field-type");
        o.activeLayer = t.str("active-layer");
        o.hasActiveLayer = !o.activeLayer.empty();
        return o;
    };
    auto sftEmit = [&](size_t after, bool cast, int &emit,
                       long long &xf) {
        bool rescan = false, sel = false, comb = false;
        bool selThenComb = false, combThenSel = false;
        for (size_t j = after; j < transforms.size(); ++j)
        {
            const std::string &nm = transforms[j]->name;
            if (nm == "filter" || nm == "edit" || nm == "combine" ||
                nm == "clip")
                rescan = true;
            if (nm == "select")
            {
                if (comb)
                    combThenSel = true;
                sel = true;
            }
            if (nm == "combine")
            {
                if (sel)
                    selThenComb = true;
                comb = true;
            }
        }
        emit = 0;
        xf = -1;
        // a SetFrom step followed by select-then-combine goes fully
        // silent; combine-then-select flattens every facet to one pull
        if (!cast && selThenComb)
            return;
        if (!cast && combThenSel)
        {
            emit = sftFacet == SFT_SUMMARY
                       ? 0
                       : sftFacet == SFT_WRITE ? g_convertWritePulls : 1;
            return;
        }
        int pinfo = cast ? 1 + (rescan ? 1 : 0) - (sel ? 1 : 0)
                         : 2 - (sel ? 1 : 0);
        if (pinfo < 0)
            pinfo = 0;
        switch (sftFacet)
        {
            case SFT_WRITE:
                emit = g_convertWritePulls;
                break;
            case SFT_SUMMARY:
                break;
            case SFT_PLAIN:
                emit = pinfo;
                break;
            case SFT_FEATURES:
                emit = pinfo + 1 - (!cast && comb ? 1 : 0);
                break;
            case SFT_FID:
                emit = cast ? pinfo + 1 : pinfo;
                if (cast ? !rescan : !comb)
                    xf = g_vectorInfoFid;
                break;
        }
        if (emit < 0)
            emit = 0;
    };
    bool castGroupDone = false;
    int updDone = 0;
    for (size_t oi = 0; oi < order.size(); ++oi)
    {
        const StepData *t = order[oi];
        // update-flow natives: any step but reproject drops the
        // whole-document replay; select and sql drop the per-feature
        // merge too
        if (t->name != "update")
            for (OgrLayer &l : d.layers)
            {
                if (t->name != "reproject")
                    l.gjUpdateFlow = false;
                if (t->name == "select" || t->name == "sql")
                    l.gjNativeMerge = false;
            }
        if (geosWallStepName(t->name))
        {
            const std::string &n = t->name;
            if (n == "check-coverage" || n == "simplify-coverage")
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            n + ": " + n +
                                " requires GDAL to be built against "
                                "version 3.12 or later of the GEOS "
                                "library.");
            else if (n == "clean-coverage")
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            n + ": " + n +
                                " requires GDAL to be built against "
                                "version 3.14 or later of the GEOS "
                                "library.");
            else if (n == "check-geometry")
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            n + ": " + n +
                                " requires GDAL to be built against "
                                "the GEOS library.");
            else
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            n + ": This algorithm is only supported "
                                "for builds against GEOS");
            return 1;
        }
        if (lastCombine < order.size())
            g_vectorClipEmitPulls =
                oi < lastCombine ? 0 : savedEmitPulls;
        if (t->name == "combine" && g_convertClipPending.active)
        {
            int times = 1;
            if (savedEmitPulls > 0)
            {
                times = 2;
                for (size_t k = oi + 1; k < order.size(); ++k)
                {
                    const std::string &nm = order[k]->name;
                    if (nm == "select" || nm == "sql" || nm == "edit" ||
                        nm == "clip")
                        times = 1;
                }
            }
            for (int rep = 0; rep < times; ++rep)
                for (const auto &pl : g_convertClipPending.layers)
                    convertClipEmitLayerErrors(pl);
            g_convertClipPending = ConvertClipPending();
        }
        if (t->name == "update")
        {
            const std::string outPath = t->str("output");
            bool updMode =
                !(t->has("update") && t->str("update") == "false");
            if (!updMode)
            {
                if (fileExistsP(outPath))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "update: Dataset '" + outPath +
                                    "' already exists. You may specify "
                                    "the --overwrite/--update option.");
                    handlerPrintUsage();
                    g_pipelineMutateSilentFail = true;
                    return 1;
                }
                raise(SIGSEGV);
            }
            std::unique_ptr<OgrDataset> tgt;
            std::string err;
            if (!strEndsWith(strToLower(outPath), ".gdalg.json"))
                tgt = openVectorDataset(outPath, err, {});
            else if (!fileExistsP(outPath))
                err = "missing";
            if (!tgt)
            {
                struct stat stt;
                if (err == "missing")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(outPath));
                else if (err != "reported" &&
                         stat(outPath.c_str(), &stt) == 0 &&
                         S_ISDIR(stt.st_mode))
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                outPath + ": Is a directory");
                else if (err != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + outPath +
                                    "' not recognized as being in a "
                                    "supported file format.");
                handlerPrintUsage();
                g_pipelineMutateSilentFail = true;
                return 1;
            }
            VectorUpdateOpts o;
            o.hasInputLayer = t->has("input-layer");
            o.inputLayer = t->str("input-layer");
            o.hasOutputLayer = t->has("output-layer");
            o.outputLayer = t->str("output-layer");
            o.mode = t->has("mode") ? t->str("mode") : "merge";
            o.keys = t->list("key");
            // the downstream dataset's NATIVE_DATA domain reflects the
            // pre-update document (original bbox values, crs foreign
            // members)
            std::string natDoc;
            bool natAny = false;
            if (!tgt->layers.empty() && tgt->layers[0].gjRoot &&
                tgt->layers[0].gjRoot->type == JVal::OBJECT)
            {
                JVal nat;
                nat.type = JVal::OBJECT;
                for (const auto &kv : tgt->layers[0].gjRoot->obj)
                    if (kv.first != "type" && kv.first != "features")
                        nat.obj.push_back(kv);
                natDoc = ogrJsonSpacedSerialize(nat);
                natAny = true;
            }
            if (vectorUpdateRun(d, std::move(tgt), outPath, o, true,
                                false))
                return 1;
            // each eagerly executed update advances the shared bar to
            // k/(k+1); the terminal continues from the floor tick
            ++updDone;
            if (g_convertMutateBarOk)
            {
                TermProgress tp;
                tp.update(updDone / (double)(updDone + 1));
                g_termTickFloor = tp.lastTick;
            }
            std::string err2;
            auto re = openVectorDataset(outPath, err2, {});
            if (!re)
                return 1;
            d = std::move(*re);
            for (OgrLayer &l : d.layers)
            {
                l.gjUpdateFlow = true;
                l.gjNativeMerge = true;
                if (natAny)
                    l.extraMdDomains.push_back(
                        {"NATIVE_DATA",
                         {{"NATIVE_DATA", natDoc},
                          {"NATIVE_MEDIA_TYPE",
                           "application/vnd.geo+json"}}});
            }
            state.assign(d.layers.size(), 0);
            continue;
        }
        if (t->name == "reproject")
        {
            VectorReprojectInfoNeeds needs;
            // the terminal step's pulls decide the trailing warp's work:
            // a feature-less info never surfaces per-feature transform
            // errors, a write never grid-samples the extent
            needs.extent = extentNeeded || t != order.back();
            needs.features = featuresNeeded || t != order.back();
            needs.limit = -1;
            if (vectorReprojectApply(d,
                                     {{t->str("src-crs"),
                                       t->str("dst-crs"),
                                       t->str("active-layer")}},
                                     needs))
                return 1;
            continue;
        }
        if (t->name == "set-field-type")
        {
            size_t ti = 0;
            for (size_t k = 0; k < transforms.size(); ++k)
                if (transforms[k] == t)
                {
                    ti = k;
                    break;
                }
            if (ti < castEnd)
            {
                if (castGroupDone)
                    continue;
                castGroupDone = true;
                std::vector<VectorSetFieldTypeOpts> grp;
                for (size_t k = 0; k < castEnd; ++k)
                    grp.push_back(sftOpts(*transforms[k]));
                for (const auto &o : grp)
                    if (vectorSetFieldTypeValidate(d, o, true))
                    {
                        // the cast form errors as a read open-option
                        // failure: usage follows and no write progress
                        // fakes completion
                        handlerPrintUsage();
                        g_pipelineMutateSilentFail = true;
                        return 1;
                    }
                int emit;
                long long xf;
                sftEmit(castEnd, true, emit, xf);
                vectorSetFieldTypeCastGroup(d, grp, emit, xf);
                continue;
            }
            VectorSetFieldTypeOpts o = sftOpts(*t);
            if (vectorSetFieldTypeValidate(d, o, false))
                return 1;
            int emit;
            long long xf;
            sftEmit(ti + 1, false, emit, xf);
            vectorSetFieldTypeConvert(d, o, false, emit, xf);
            continue;
        }
        std::vector<char> pre = state;
        std::vector<char> snapHas;
        std::vector<std::array<double, 4>> snapExt;
        if (t->name == "filter")
            for (const OgrLayer &l : d.layers)
            {
                snapHas.push_back(l.hasExtent ? 1 : 0);
                snapExt.push_back({l.extent[0], l.extent[1],
                                   l.extent[2], l.extent[3]});
            }
        if (vectorVerbApplyPipeStep(*t, d))
            return 1;
        if (t->name == "sql")
        {
            state.assign(d.layers.size(), 2);
            continue;
        }
        for (size_t li = 0; li < d.layers.size(); ++li)
        {
            if (li >= state.size() || !selected(*t, d.layers[li]))
                continue;
            if (t->name == "select")
                state[li] = 2;
            else if (t->name == "filter")
            {
                bool hasWhere = !t->str("where").empty();
                bool keep = hasWhere && pre[li] != 0 &&
                            !t->has("update-extent") && !t->has("bbox");
                if (keep && li < snapHas.size())
                {
                    d.layers[li].hasExtent = snapHas[li] != 0;
                    for (int e = 0; e < 4; ++e)
                        d.layers[li].extent[e] = snapExt[li][e];
                }
                if (t->has("update-extent"))
                    vectorLayerRecomputeExtent(d.layers[li]);
                if (hasWhere && state[li] == 0)
                    state[li] = 1;
            }
        }
    }
    g_vectorClipEmitPulls = savedEmitPulls;
    return 0;
}

int runInfoStep(const PipeCtx &c, const StepData &read, const StepData &info,
                const std::string &inputOverride,
                const std::vector<const StepData *> &transforms,
                std::unique_ptr<OgrDataset> *chainDs)
{
    const Spec &spec = Spec::instance();
    const char *id = c.type == P_RASTER ? "raster_info" : "vector_info";
    const CmdSpec *cs = spec.findById(id);
    Handler h = findHandler(id);
    if (!cs || !h)
        return 1;
    ParseResult r;
    initResult(*cs, r);
    std::string input =
        inputOverride.empty() ? read.str("input") : inputOverride;
    setArg(*cs, r, "input", {input});
    if (inputOverride.empty())
    {
        if (read.has("input-format"))
            setArg(*cs, r, "if", read.list("input-format"));
        if (read.has("open-option"))
            setArg(*cs, r, "oo", read.list("open-option"));
    }
    std::vector<std::string> readSel = read.list("input-layer");
    // with transforms in play the read selection materializes here,
    // before the chain, so the transforms see the selected layer set
    bool readSelHere = c.type == P_VECTOR && !transforms.empty() &&
                       !readSel.empty();
    std::vector<std::string> layers;
    if (!readSelHere)
        layers = readSel;
    if (layers.empty() && info.has("input-layer"))
        g_pipelineInfoDriverKeep = true;
    if (!layers.empty())
    {
        // the read selection travels in 'layer'; the info step's own -l
        // fetches from the selected set afterwards
        g_pipelineReadLayerFilter = true;
        g_pipelineInfoStepLayers = info.list("input-layer");
    }
    else
        for (const auto &l : info.list("input-layer"))
            layers.push_back(l);
    if (!layers.empty())
        setArg(*cs, r, "layer", layers);
    std::string fmt = info.has("of")
                          ? info.str("of")
                          : ((c.alias || (c.genericOrigin &&
                                          !c.vecFixedByName &&
                                          info.usedVecOnly))
                                 ? "json"
                                 : "text");
    setArg(*cs, r, "of", {fmt});
    static const char *passthrough[] = {
        "min-max", "stats",    "approx-stats", "hist",     "no-gcp",
        "no-md",   "no-ct",    "no-fl",        "checksum", "list-mdd",
        "no-nodata", "no-mask", "mdd",         "subdataset", "features",
        "summary", "limit",    "sql",          "where",    "fid",
        "dialect", "crs-format"};
    for (const char *k : passthrough)
        if (info.has(k))
            setArg(*cs, r, k, info.list(k));
    if (c.type == P_VECTOR)
    {
        if (!transforms.empty())
        {
            std::unique_ptr<OgrDataset> ds;
            bool fromChain = chainDs && *chainDs;
            if (fromChain)
                ds = std::move(*chainDs);
            else
            {
                std::string err;
                ds = openVectorDataset(
                    input, err,
                    inputOverride.empty() ? read.list("input-format")
                                          : std::vector<std::string>(),
                    inputOverride.empty() ? read.list("open-option")
                                          : std::vector<std::string>());
            }
            if (!ds)
                return 1;
            if (readSelHere && !fromChain &&
                vectorReadSelectLayers(*ds, readSel))
                return 1;
            bool anyVerb = false;
            for (const StepData *t : transforms)
                if (t->name != "reproject")
                    anyVerb = true;
            if (anyVerb)
            {
                bool needF = info.has("features") || info.has("limit") ||
                             info.has("fid");
                bool summaryList =
                    info.has("summary") && layers.empty();
                // info pulls the clipped stream once per rendered
                // facet: count + extent, plus the feature listing
                g_vectorClipEmitPulls =
                    summaryList ? -1 : 2 + (needF ? 1 : 0);
                g_vectorInfoFid = info.has("fid")
                                      ? atoll(info.str("fid").c_str())
                                      : -1;
                g_vectorSftAllCast = false;
                int crc = vectorChainApply(transforms, *ds, false, needF,
                                           !summaryList);
                g_vectorClipEmitPulls = 0;
                g_vectorInfoFid = -1;
                g_pipelineMutateSilentFail = false;
                if (crc)
                    return 1;
            }
            else
            {
                std::vector<VectorReprojectStep> vsteps;
                for (const StepData *t : transforms)
                    vsteps.push_back({t->str("src-crs"),
                                      t->str("dst-crs"),
                                      t->str("active-layer")});
                VectorReprojectInfoNeeds needs;
                bool summaryList = info.has("summary") && layers.empty();
                needs.extent = !summaryList;
                bool jsonOut = fmt == "json";
                if (!summaryList)
                {
                    if (info.has("features") || info.has("limit"))
                    {
                        needs.features = true;
                        if (info.has("limit"))
                        {
                            needs.limit =
                                atoll(info.str("limit").c_str());
                            if (needs.limit <= 0)
                                needs.limit = -1;
                        }
                    }
                    else if (info.has("fid") && !jsonOut)
                    {
                        needs.hasFid = true;
                        needs.fid = atoll(info.str("fid").c_str());
                    }
                }
                needs.layerFilter = layers;
                if (vectorReprojectApply(*ds, vsteps, needs))
                    return 1;
            }
            // transform steps rebuild the layers with only the default
            // metadata domain; a pure read-absorbed cast chain keeps
            // the read dataset identity intact, unless the read's own
            // layer selection already re-materialized the set. A tail
            // update hands its reopened output through with its file
            // identity intact
            bool tailUpdate = !transforms.empty() &&
                              transforms.back()->name == "update";
            if ((!g_vectorSftAllCast || readSelHere) && !tailUpdate)
            {
                for (OgrLayer &l : ds->layers)
                    l.extraMdDomains.clear();
                g_pipelineStreamInfo = true;
            }
            g_vectorSftAllCast = false;
            g_convertSourceOverride = std::move(ds);
        }
        else if (chainDs && *chainDs)
            g_convertSourceOverride = std::move(*chainDs);
        int rc = h(*cs, r);
        g_pipelineStreamInfo = false;
        g_pipelineInfoDriverKeep = false;
        g_pipelineReadLayerFilter = false;
        g_pipelineInfoStepLayers.clear();
        g_convertSourceOverride.reset();
        return rc;
    }
    installTailMaterialize(c, transforms, true, false, true);
    int rc = h(*cs, r);
    g_pipelineInfoDriverKeep = false;
    g_pipelineReadLayerFilter = false;
    g_pipelineInfoStepLayers.clear();
    g_pipelineTailMaterialize = nullptr;
    g_pipelineDemVrtVerb.clear();
    return rc;
}

int runExportSchemaStep(const PipeCtx &c, const StepData &read,
                        const StepData &es,
                        const std::string &inputOverride,
                        const std::vector<const StepData *> &transforms,
                        std::unique_ptr<OgrDataset> *chainDs)
{
    (void)c;
    std::string input =
        inputOverride.empty() ? read.str("input") : inputOverride;
    std::unique_ptr<OgrDataset> ds;
    bool fromChain = chainDs && *chainDs;
    if (fromChain)
        ds = std::move(*chainDs);
    else
    {
        std::string err;
        ds = openVectorDataset(
            input, err,
            inputOverride.empty() ? read.list("input-format")
                                  : std::vector<std::string>(),
            inputOverride.empty() ? read.list("open-option")
                                  : std::vector<std::string>());
        if (!ds)
        {
            if (err == "missing")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            datasetMissingMessage(input));
            else if (err != "reported")
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + input +
                                "' not recognized as being in a "
                                "supported file format.");
            handlerPrintUsage();
            return 1;
        }
    }
    if (!fromChain &&
        vectorReadSelectLayers(*ds, read.list("input-layer")))
        return 1;
    if (!transforms.empty())
    {
        // schema-only terminal: clip never pulls features
        g_vectorClipEmitPulls = -1;
        int crc = vectorChainApply(transforms, *ds, false, false, false);
        g_vectorClipEmitPulls = 0;
        g_pipelineMutateSilentFail = false;
        if (crc)
            return 1;
    }
    // the step's own -l fetches layers one by one
    std::vector<std::string> sel = es.list("input-layer");
    if (!sel.empty())
    {
        std::vector<OgrLayer> out;
        for (const std::string &lf : sel)
        {
            const OgrLayer *hit = nullptr;
            for (const OgrLayer &lyr : ds->layers)
                if (lyr.name == lf)
                {
                    hit = &lyr;
                    break;
                }
            if (!hit)
                for (const OgrLayer &lyr : ds->layers)
                    if (strEqualNoCase(lyr.name, lf))
                    {
                        hit = &lyr;
                        break;
                    }
            if (!hit)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Couldn't fetch requested layer " + lf +
                                ".");
                return 1;
            }
            out.push_back(*hit);
        }
        ds->layers = std::move(out);
    }
    std::string out = vectorExportSchemaRender(*ds);
    fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

// vector create step: first-step creates from its own flags, later
// positions treat the chained dataset as the schema template
int runCreateStep(const PipeCtx &c, const StepData &read,
                  const StepData &st, const std::string &inputOverride,
                  const std::vector<const StepData *> &transforms,
                  std::unique_ptr<OgrDataset> *chainDs, bool firstStep,
                  bool terminal)
{
    g_pipelineCommitted = true;
    VectorCreateRun p;
    p.stepMode = true;
    p.terminalStep = terminal;
    p.quiet = c.quiet;
    p.output = st.str("output");
    p.format = st.str("output-format");
    p.overwrite = st.has("overwrite");
    p.update = st.has("update");
    p.overwriteLayer = st.has("overwrite-layer");
    p.layerSel = st.list("input-layer");
    p.outputLayerSet = st.has("output-layer");
    p.outputLayer = st.str("output-layer");
    p.geomTypeSet = st.has("geometry-type");
    p.geomTypeName = st.str("geometry-type");
    p.geomFieldSet = st.has("geometry-field");
    p.geomFieldName = st.str("geometry-field");
    p.crsSet = st.has("crs");
    p.crsInput = st.str("crs");
    p.fidSet = st.has("fid");
    p.fid = st.str("fid");
    p.fieldDefs = st.list("field");
    p.co = st.list("creation-option");
    p.lco = st.list("layer-creation-option");
    p.schemaSet = st.has("schema");
    p.schemaSpec = st.str("schema");
    auto failBar = [&](int rc) {
        if (terminal && !c.quiet)
        {
            TermProgress tp;
            tp.update(1.0);
        }
        return rc;
    };
    if (firstStep)
    {
        bool hasField = !p.fieldDefs.empty();
        std::vector<const char *> group;
        if (st.has("input"))
            group.push_back("input");
        if (p.schemaSet)
            group.push_back("schema");
        if (hasField)
            group.push_back("field");
        bool mutexHit = false;
        for (size_t i = 1; i < group.size(); ++i)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        std::string("create: Argument '") + group[i] +
                            "' is mutually exclusive with '" + group[0] +
                            "'.");
            mutexHit = true;
        }
        if ((st.has("input") || p.schemaSet) &&
            (p.geomTypeSet || p.geomFieldSet || p.crsSet || p.fidSet ||
             hasField))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: When --schema or --like is specified, "
                        "--geometry-field, --geometry-type, --field, "
                        "--crs and --fid options must not be specified.");
            mutexHit = true;
        }
        if (mutexHit)
            return failBar(1);
        std::string like = st.str("input");
        if (!like.empty())
        {
            std::string err;
            p.likeDs = openVectorDataset(like, err, st.list("input-format"),
                                         st.list("open-option"));
            if (!p.likeDs)
            {
                if (err == "missing")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(like));
                else if (err != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + like +
                                    "' not recognized as being in a "
                                    "supported file format.");
                return failBar(1);
            }
        }
    }
    else
    {
        // the chained dataset rides the 'input' slot: create's own
        // template/field flags are mutually exclusive with it, each
        // reporting against the group's first-set member
        bool hasField = !p.fieldDefs.empty();
        bool mutexHit = false;
        if (p.schemaSet)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Argument 'schema' is mutually exclusive "
                        "with 'input'.");
            mutexHit = true;
        }
        if (hasField)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Argument 'field' is mutually exclusive "
                        "with 'input'.");
            mutexHit = true;
        }
        if (p.geomTypeSet || p.geomFieldSet || p.crsSet || p.fidSet ||
            hasField)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: When --schema or --like is specified, "
                        "--geometry-field, --geometry-type, --field, "
                        "--crs and --fid options must not be specified.");
            mutexHit = true;
        }
        if (mutexHit)
            return failBar(1);
        std::unique_ptr<OgrDataset> ds;
        bool fromChain = chainDs && *chainDs;
        if (fromChain)
            ds = std::move(*chainDs);
        else
        {
            std::string input = inputOverride.empty() ? read.str("input")
                                                      : inputOverride;
            std::string err;
            ds = openVectorDataset(
                input, err,
                inputOverride.empty() ? read.list("input-format")
                                      : std::vector<std::string>(),
                inputOverride.empty() ? read.list("open-option")
                                      : std::vector<std::string>());
            if (!ds)
            {
                if (err == "missing")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(input));
                else if (err != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + input +
                                    "' not recognized as being in a "
                                    "supported file format.");
                return failBar(1);
            }
            if (vectorReadSelectLayers(*ds, read.list("input-layer")))
                return failBar(1);
        }
        if (!transforms.empty())
        {
            // schema-only terminal: clip never pulls features
            g_vectorClipEmitPulls = -1;
            int crc =
                vectorChainApply(transforms, *ds, false, false, false);
            g_vectorClipEmitPulls = 0;
            if (crc)
                return failBar(1);
        }
        p.schemaContent = vectorExportSchemaRender(*ds);
        p.haveSchemaContent = true;
    }
    int rc = vectorCreateCoreRun(p);
    if (rc == 0 && chainDs)
        *chainDs = std::move(p.handover);
    return rc;
}

int runWriteStep(const PipeCtx &c, const StepData &read,
                 const StepData &write, const std::string &inputOverride,
                 bool suppressProgress,
                 const std::vector<const StepData *> &transforms,
                 size_t totalSteps, const std::string &echoPrefix,
                 std::unique_ptr<OgrDataset> *chainDs)
{
    const Spec &spec = Spec::instance();
    const char *id = c.type == P_RASTER ? "raster_convert" : "vector_convert";
    const CmdSpec *cs = spec.findById(id);
    Handler h = findHandler(id);
    if (!cs || !h)
        return 1;
    ParseResult r;
    initResult(*cs, r);
    std::string input =
        inputOverride.empty() ? read.str("input") : inputOverride;
    setArg(*cs, r, "input", {input});
    if (inputOverride.empty())
    {
        if (read.has("input-format"))
            setArg(*cs, r, "if", read.list("input-format"));
        if (read.has("open-option"))
            setArg(*cs, r, "oo", read.list("open-option"));
        if (read.has("input-layer"))
            setArg(*cs, r, "layer", read.list("input-layer"));
    }
    setArg(*cs, r, "output", {write.str("output")});
    if (write.has("of"))
        setArg(*cs, r, "of", {write.str("of")});
    if (write.has("co"))
        setArg(*cs, r, "co", write.list("co"));
    if (write.has("lco"))
        setArg(*cs, r, "lco", write.list("lco"));
    for (const char *fl : {"overwrite", "append", "update",
                           "overwrite-layer", "upsert", "skip-errors"})
        if (write.has(fl))
            setArg(*cs, r, fl, {"true"});
    if (write.has("output-layer"))
        setArg(*cs, r, "output-layer", {write.str("output-layer")});
    bool stepQuiet = false;
    for (const StepData *t : transforms)
        if (t->has("quiet"))
            stepQuiet = true;
    if (c.quiet || suppressProgress || stepQuiet)
        setArg(*cs, r, "quiet", {"true"});
    g_pipelineMode = true;
    g_pipelineStepPrefix = "write";
    g_pipelineCommitted = false;
    if (echoPrefix.empty())
    {
        g_pipelineGdalgCli = buildGdalgReadCli(c, read);
        for (const StepData *t : transforms)
            g_pipelineGdalgCli += c.type == P_VECTOR
                                      ? vectorStepEcho(*t)
                                      : rasterTailStepEcho(t->name, t->v);
    }
    else
        g_pipelineGdalgCli = echoPrefix;
    g_pipelineTotalSteps = (int)totalSteps;
    if (chainDs && *chainDs)
        g_convertSourceOverride = std::move(*chainDs);
    if (c.type == P_VECTOR)
    {
        if (!transforms.empty())
        {
            bool anyVerb = false;
            for (const StepData *t : transforms)
                if (t->name != "reproject")
                    anyVerb = true;
            // these steps hide the feature count from the writer: the
            // whole bar renders after the last feature
            for (const StepData *t : transforms)
                if (t->name == "filter" ||
                    t->name == "explode-collections" ||
                    t->name == "edit" || t->name == "rename-layer" ||
                    t->name == "reproject" || t->name == "clip" ||
                    t->name == "combine")
                    g_pipelineWriteBarAtEnd = true;
            if (!anyVerb)
            {
                std::vector<VectorReprojectStep> vsteps;
                for (const StepData *t : transforms)
                    vsteps.push_back({t->str("src-crs"),
                                      t->str("dst-crs"),
                                      t->str("active-layer")});
                vectorReprojectInstall(vsteps);
            }
            else
            {
                std::vector<StepData> copies;
                for (const StepData *t : transforms)
                    copies.push_back(*t);
                g_convertDatasetMutate =
                    [copies](OgrDataset &d) -> int {
                    std::vector<const StepData *> ptrs;
                    for (const StepData &t : copies)
                        ptrs.push_back(&t);
                    return vectorChainApply(ptrs, d, false, true, false);
                };
            }
        }
    }
    else
        installTailMaterialize(c, transforms,
                               c.quiet || suppressProgress || stepQuiet,
                               true, false);
    int rc = h(*cs, r);
    vectorReprojectUninstall();
    g_convertDatasetMutate = nullptr;
    g_convertSourceOverride.reset();
    g_convertClipPending = ConvertClipPending();
    g_pipelineWriteBarAtEnd = false;
    g_pipelineMode = false;
    g_pipelineStepPrefix.clear();
    g_pipelineGdalgCli.clear();
    g_pipelineTailMaterialize = nullptr;
    g_pipelineDemVrtVerb.clear();
    g_pipelineTotalSteps = 0;
    return rc;
}

int runTransRasTerminal(PipeCtx &c, StepData &read,
                        std::vector<StepData> &steps, size_t transIdx,
                        const std::vector<const StepData *> &preT,
                        const std::vector<const StepData *> &postT,
                        bool terminalGdalg, const std::string &fullEcho,
                        std::unique_ptr<OgrDataset> srcOverride);

// pre-verb write choreography of a rasterize transition terminal: the
// raster writer vocabulary and the exists check run before the verb
int transRasWriteChecks(const StepData &term, bool &isStream)
{
    const std::string of = term.str("of");
    const std::string output = term.str("output");
    std::string drv;
    if (!of.empty())
    {
        static const char *allowed[] = {"GTiff", "COG", "VRT", "MEM",
                                        "stream"};
        for (const char *a : allowed)
            if (strEqualNoCase(of, a))
                drv = a;
        if (drv.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "write: Invalid value for argument "
                        "'output-format'. Driver '" +
                            of + "' does not exist.");
            handlerPrintUsage();
            return 1;
        }
    }
    isStream = strEqualNoCase(drv, "stream");
    bool memLike = strEqualNoCase(drv, "MEM") || isStream;
    if (!memLike && fileExistsP(output) && !term.has("overwrite") &&
        !term.has("append"))
    {
        bool isDs = false;
        {
            std::string e2;
            cplPushQuietHandler();
            auto d2 = openVectorDataset(output, e2, {});
            cplPopHandler();
            isDs = d2 != nullptr;
        }
        if (!isDs)
            isDs = datasetIdentify(output, {"raster"});
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    std::string("write: ") + (isDs ? "Dataset" : "File") +
                        " '" + output +
                        "' already exists. You may specify the "
                        "--overwrite/--append option.");
        handlerPrintUsage();
        return 1;
    }
    return 0;
}

// executes a generic-pipeline raster->vector transition: raster tail
// transforms feed the verb handler, which owns the write choreography;
// pipeline-level write pre-checks and the progress bar live here
int runTransTerminal(PipeCtx &c, StepData &read,
                     std::vector<StepData> &steps, size_t transIdx,
                     const std::vector<const StepData *> &preT,
                     const std::vector<const StepData *> &postT,
                     bool terminalGdalg, const std::string &fullEcho)
{
    const Spec &spec = Spec::instance();
    const StepData &tr = steps[transIdx];
    StepData &term = steps.back();
    PipeCtx cv = c;
    cv.type = P_VECTOR;
    if (terminalGdalg)
    {
        for (size_t i = 1; i + 1 < steps.size(); ++i)
        {
            const StepData &st = steps[i];
            if (st.trans || st.transRas ||
                (st.name == "filter" &&
                             st.has("update-extent")) ||
                st.name == "update" || st.name == "check-coverage" ||
                st.name == "clean-coverage" ||
                st.name == "simplify-coverage")
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "pipeline: Step " + st.name +
                                " is not natively streaming compatible, "
                                "and may cause significant processing "
                                "time at opening");
        }
        std::unique_ptr<OgrDataset> empty(new OgrDataset());
        return runWriteStep(cv, read, term, "", true, {}, steps.size(),
                            fullEcho, &empty);
    }
    bool isInfo = term.name == "info" || term.name == "export-schema";
    bool isStream = false;
    bool delOnFail = false;
    bool quietAll = c.quiet || term.has("quiet") || tr.has("quiet");
    for (const StepData *t : preT)
        if (t->has("quiet"))
            quietAll = true;
    for (const StepData *t : postT)
        if (t->has("quiet"))
            quietAll = true;
    bool barOk = !isInfo && !quietAll &&
                 !(term.str("output") == "/vsistdout/" && !g_dashStdout);
    // a later rasterize step re-transitions to raster: the verb output
    // is captured in memory and the raster tail runs after it
    size_t rasIdx = 0;
    for (size_t i = transIdx + 1; i + 1 < steps.size(); ++i)
        if (steps[i].transRas)
        {
            rasIdx = i;
            break;
        }
    if (rasIdx && !isInfo)
    {
        bool subStream = false;
        int prc = transRasWriteChecks(term, subStream);
        if (prc)
            return prc;
    }
    if (!isInfo && !rasIdx)
    {
        const std::string of = term.str("of");
        const std::string output = term.str("output");
        std::string drv;
        if (!of.empty())
        {
            // the transition write only reaches the verbs' own writer
            // vocabulary, not the full vector output driver set
            static const char *allowed[] = {"GeoJSON", "GeoJSONSeq",
                                            "ESRI Shapefile", "MEM",
                                            "Memory", "stream"};
            for (const char *a : allowed)
                if (strEqualNoCase(of, a))
                    drv = a;
            if (strEqualNoCase(of, "Memory"))
                drv = "MEM";
            if (drv.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "write: Invalid value for argument "
                            "'output-format'. Driver '" + of +
                                "' does not exist.");
                handlerPrintUsage();
                return 1;
            }
        }
        isStream = strEqualNoCase(drv, "stream");
        bool fOw = term.has("overwrite");
        bool fApp = term.has("append");
        bool fUpd = term.has("update");
        bool fOwl = term.has("overwrite-layer");
        bool fUps = term.has("upsert");
        bool memLike = strEqualNoCase(drv, "MEM") ||
                       strEqualNoCase(drv, "Memory") ||
                       strEqualNoCase(drv, "stream");
        // a failing delegate leaves no trace of the destination: the
        // fresh file it began is removed, and --overwrite has already
        // dropped the previous one
        delOnFail = !memLike && (!fileExistsP(output) || fOw);
        if (fUpd || fUps || fOwl)
        {
            if (!fileExistsP(output))
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            datasetMissingMessage(output));
                handlerPrintUsage();
                return 1;
            }
            std::string terr;
            auto tgt = openVectorDataset(output, terr, {}, {}, false);
            if (!tgt)
            {
                if (outputExistsKind(output) == "Directory")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                output + ": Is a directory");
                else if (terr != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + output +
                                    "' not recognized as being in a "
                                    "supported file format.");
                handlerPrintUsage();
                return 1;
            }
        }
        else if (!memLike && fileExistsP(output) && !fOw && !fApp)
        {
            bool isDs = false;
            {
                std::string e2;
                cplPushQuietHandler();
                auto d2 = openVectorDataset(output, e2, {});
                cplPopHandler();
                isDs = d2 != nullptr;
            }
            if (!isDs)
                isDs = datasetIdentify(output, {"raster"});
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        std::string("write: ") +
                            (isDs ? "Dataset" : "File") + " '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--overwrite-layer/--append/"
                            "--update option.");
            handlerPrintUsage();
            return 1;
        }
    }
    // post-validator equivalents run before the verb: the handlers are
    // invoked directly, outside the engine validation path
    {
        std::string derr;
        cplPushQuietHandler();
        auto ds = openRaster(read.str("input"), derr);
        cplPopHandler();
        if (ds)
        {
            int nb = (int)ds->bands.size();
            auto rangeFail = [&](void) {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Value of 'band' should be greater or "
                              "equal than 1 and less or equal than %d.",
                              tr.name.c_str(), nb));
                if (barOk)
                    printProgress();
                return 1;
            };
            if (tr.name == "contour" || tr.name == "polygonize")
            {
                long long b =
                    tr.has("band")
                        ? strtoll(tr.str("band").c_str(), nullptr, 10)
                        : 1;
                if (b >= 1 && b > nb)
                    return rangeFail();
            }
            else if (tr.name == "footprint")
            {
                for (const auto &bs : tr.list("band"))
                    if (atoi(bs.c_str()) > nb)
                        return rangeFail();
                if (tr.has("overview"))
                {
                    int idx = atoi(tr.str("overview").c_str());
                    int n = (int)ds->dispOverviews().size();
                    if (n == 0)
                    {
                        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                    tr.name +
                                        ": Source dataset has no "
                                        "overviews. Argument 'overview' "
                                        "should not be specified.");
                        if (barOk)
                            printProgress();
                        return 1;
                    }
                    if (idx >= n)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_IllegalArg,
                            strPrintf("%s: Source dataset has only %d "
                                      "overview levels. 'overview' value "
                                      "should be strictly lower than "
                                      "this number.",
                                      tr.name.c_str(), n));
                        if (barOk)
                            printProgress();
                        return 1;
                    }
                }
            }
        }
    }
    const CmdSpec *cs = spec.findById("raster_" + tr.name);
    Handler h = findHandler("raster_" + tr.name);
    if (!cs || !h)
        return 1;
    ParseResult r;
    initResult(*cs, r);
    auto forceResultArg = [](ParseResult &pr, const std::string &n,
                             const std::vector<std::string> &vals) {
        ArgValue &v = pr.byName[n];
        v.set = true;
        v.values = vals;
        pr.order.push_back(n);
    };
    setArg(*cs, r, "input", {read.str("input")});
    if (read.has("input-format"))
        setArg(*cs, r, "if", read.list("input-format"));
    if (read.has("open-option"))
        setArg(*cs, r, "oo", read.list("open-option"));
    for (const auto &kv : tr.v)
    {
        if (kv.first == "output-layer" && tr.name == "polygonize" &&
            !isInfo && postT.empty())
            continue;
        const ArgSpec *a = cs->findLong(kv.first);
        if (a && a->isList())
        {
            std::vector<std::string> parts;
            for (const auto &raw : kv.second)
                for (const auto &p : strSplit(raw, ','))
                    parts.push_back(p);
            setArg(*cs, r, kv.first, parts);
        }
        else
            setArg(*cs, r, kv.first, kv.second);
    }
    if (isInfo || isStream || rasIdx)
    {
        setArg(*cs, r, "output", {""});
        setArg(*cs, r, "of", {"MEM"});
    }
    else
    {
        setArg(*cs, r, "output", {term.str("output")});
        if (term.has("of"))
        {
            std::string cof = term.str("of");
            static const char *canon[] = {"GeoJSON", "GeoJSONSeq",
                                          "ESRI Shapefile", "MEM",
                                          "stream"};
            for (const char *a : canon)
                if (strEqualNoCase(cof, a))
                    cof = a;
            if (strEqualNoCase(cof, "Memory"))
                cof = "MEM";
            setArg(*cs, r, "of", {cof});
        }
        if (term.has("co"))
            setArg(*cs, r, "co", term.list("co"));
        if (term.has("lco"))
            setArg(*cs, r, "lco", term.list("lco"));
        if (term.has("output-layer"))
            forceResultArg(r, "output-layer", {term.str("output-layer")});
        for (const char *fl : {"overwrite", "append", "update",
                               "overwrite-layer", "upsert",
                               "skip-errors"})
            if (term.has(fl))
                forceResultArg(r, fl, {"true"});
    }
    forceResultArg(r, "quiet", {"true"});
    if (!postT.empty() && !isInfo && !rasIdx)
    {
        std::vector<StepData> copies;
        for (const StepData *t : postT)
            copies.push_back(*t);
        g_convertDatasetMutate = [copies](OgrDataset &d) -> int {
            std::vector<const StepData *> ptrs;
            for (const StepData &t : copies)
                ptrs.push_back(&t);
            return vectorChainApply(ptrs, d, false, true, false);
        };
    }
    else if (!isInfo && !rasIdx && tr.name == "polygonize")
    {
        // a direct polygonize->write drops the layer name (transforms
        // in between preserve it); the writer's nameless spelling is
        // the default OGRGeoJSON source name
        g_convertDatasetMutate = [](OgrDataset &d) -> int {
            for (OgrLayer &l : d.layers)
                l.name = "OGRGeoJSON";
            return 0;
        };
    }
    g_pipelineMode = true;
    g_pipelineCommitted = false;
    g_pipelineTotalSteps = (int)steps.size();
    if (isInfo || isStream || rasIdx)
        g_pipelineTransCapture = true;
    g_transZWarnEnable = !quietAll && !isInfo && !isStream && !rasIdx;
    if (!preT.empty())
        installTailMaterialize(c, preT, true, false, false);
    int rc = h(*cs, r);
    g_pipelineTailMaterialize = nullptr;
    g_pipelineDemVrtVerb.clear();
    g_convertDatasetMutate = nullptr;
    g_convertSourceOverride.reset();
    g_pipelineTransCapture = false;
    g_transZWarnEnable = false;
    g_pipelineMode = false;
    g_pipelineStepPrefix.clear();
    g_pipelineGdalgCli.clear();
    g_pipelineTotalSteps = 0;
    if (rasIdx)
    {
        std::unique_ptr<OgrDataset> uds =
            std::move(g_pipelineTransCaptured);
        if (rc || !uds)
        {
            if (rc == 0)
                rc = 1;
            if (barOk)
                printProgress();
            return rc;
        }
        uds->path = "";
        for (OgrLayer &l : uds->layers)
            vectorLayerRecomputeExtent(l);
        std::vector<const StepData *> midV, postR;
        for (size_t i = transIdx + 1; i < rasIdx; ++i)
            midV.push_back(&steps[i]);
        for (size_t i = rasIdx + 1; i + 1 < steps.size(); ++i)
            postR.push_back(&steps[i]);
        if (!midV.empty())
        {
            int crc = vectorChainApply(midV, *uds, false, true, true);
            if (crc)
            {
                if (barOk)
                    printProgress();
                return crc;
            }
        }
        PipeCtx cSub = c;
        cSub.quiet = quietAll;
        return runTransRasTerminal(cSub, read, steps, rasIdx, {}, postR,
                                   false, fullEcho, std::move(uds));
    }
    if (isInfo)
    {
        std::unique_ptr<OgrDataset> uds =
            std::move(g_pipelineTransCaptured);
        if (rc)
            return rc;
        if (!uds)
            return 1;
        uds->path = "";
        uds->driverLong =
            "In Memory raster, vector and multidimensional raster";
        for (OgrLayer &l : uds->layers)
            vectorLayerRecomputeExtent(l);
        return term.name == "export-schema"
                   ? runExportSchemaStep(cv, read, term, "", postT, &uds)
                   : runInfoStep(cv, read, term, "", postT, &uds);
    }
    if (isStream)
        g_pipelineTransCaptured.reset();
    if (rc && delOnFail)
    {
        const std::string output = term.str("output");
        remove(output.c_str());
        std::string low = strToLower(output);
        if (strEndsWith(low, ".shp"))
        {
            std::string base = output.substr(0, output.size() - 4);
            for (const char *ext : {".shx", ".dbf", ".prj", ".cpg"})
                remove((base + ext).c_str());
        }
    }
    if (barOk)
        printProgress();
    return rc;
}

int pipelineCore(PipeCtx &c, std::vector<std::string> rawTokens);

// rasterize transition temp products live in the cwd like the
// reference's and are removed however the process ends
std::vector<std::string> g_rasterizeTempPaths;

// executes a generic-pipeline vector->raster rasterize transition: the
// verb materializes into a cwd temp GTiff and the remaining raster
// chain re-enters the pipeline engine reading from it
int runTransRasTerminal(PipeCtx &c, StepData &read,
                        std::vector<StepData> &steps, size_t transIdx,
                        const std::vector<const StepData *> &preT,
                        const std::vector<const StepData *> &postT,
                        bool terminalGdalg, const std::string &fullEcho,
                        std::unique_ptr<OgrDataset> srcOverride)
{
    const Spec &spec = Spec::instance();
    const StepData &tr = steps[transIdx];
    StepData &term = steps.back();
    if (terminalGdalg)
    {
        PipeCtx cv = c;
        cv.type = P_VECTOR;
        for (size_t i = 1; i + 1 < steps.size(); ++i)
        {
            const StepData &st = steps[i];
            if (st.trans || st.transRas ||
                (st.name == "filter" && st.has("update-extent")) ||
                st.name == "update" || st.name == "check-coverage" ||
                st.name == "clean-coverage" ||
                st.name == "simplify-coverage")
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "pipeline: Step " + st.name +
                                " is not natively streaming compatible, "
                                "and may cause significant processing "
                                "time at opening");
        }
        std::unique_ptr<OgrDataset> empty(new OgrDataset());
        return runWriteStep(cv, read, term, "", true, {}, steps.size(),
                            fullEcho, &empty);
    }
    bool isInfo = term.name == "info";
    bool quietAll = c.quiet || term.has("quiet") || tr.has("quiet");
    for (const StepData *t : preT)
        if (t->has("quiet"))
            quietAll = true;
    for (const StepData *t : postT)
        if (t->has("quiet"))
            quietAll = true;
    bool barOk = !isInfo && !quietAll &&
                 !(term.str("output") == "/vsistdout/" && !g_dashStdout);
    bool isStream = false;
    if (!isInfo)
    {
        int prc = transRasWriteChecks(term, isStream);
        if (prc)
            return prc;
    }
    const CmdSpec *cs = spec.findById("vector_rasterize");
    Handler h = findHandler("vector_rasterize");
    if (!cs || !h)
        return 1;
    ParseResult r;
    initResult(*cs, r);
    auto forceResultArg = [](ParseResult &pr, const std::string &n,
                             const std::vector<std::string> &vals) {
        ArgValue &v = pr.byName[n];
        v.set = true;
        v.values = vals;
        pr.order.push_back(n);
    };
    setArg(*cs, r, "input", {read.str("input")});
    if (read.has("input-format"))
        setArg(*cs, r, "if", read.list("input-format"));
    if (read.has("open-option"))
        setArg(*cs, r, "oo", read.list("open-option"));
    for (const auto &kv : tr.v)
    {
        const ArgSpec *a = cs->findLong(kv.first);
        if (a && a->isList())
        {
            std::vector<std::string> parts;
            for (const auto &raw : kv.second)
                for (const auto &p : strSplit(raw, ','))
                    parts.push_back(p);
            setArg(*cs, r, kv.first, parts);
        }
        else
            setArg(*cs, r, kv.first, kv.second);
    }
    static int s_rasTempCounter = 0;
    std::string temp = strPrintf("./_rasterize.tif_%d_%d", (int)getpid(),
                                 ++s_rasTempCounter);
    setArg(*cs, r, "output", {temp});
    setArg(*cs, r, "of", {"GTiff"});
    setArg(*cs, r, "co", {"TILED=YES"});
    forceResultArg(r, "quiet", {"true"});
    if (srcOverride)
        g_convertSourceOverride = std::move(srcOverride);
    else if (!preT.empty() || read.has("input-layer"))
    {
        std::string err;
        auto ds = openVectorDataset(read.str("input"), err,
                                    read.list("input-format"),
                                    read.list("open-option"));
        if (!ds)
        {
            if (barOk)
                printProgress();
            return 1;
        }
        if (vectorReadSelectLayers(*ds, read.list("input-layer")))
        {
            if (barOk)
                printProgress();
            return 1;
        }
        if (!preT.empty())
        {
            int crc = vectorChainApply(preT, *ds);
            if (crc)
            {
                if (barOk)
                    printProgress();
                return crc;
            }
        }
        g_convertSourceOverride = std::move(ds);
    }
    g_pipelineMode = true;
    g_pipelineCommitted = false;
    g_pipelineTotalSteps = (int)steps.size();
    int rc = h(*cs, r);
    g_convertSourceOverride.reset();
    g_pipelineMode = false;
    g_pipelineStepPrefix.clear();
    g_pipelineGdalgCli.clear();
    g_pipelineTotalSteps = 0;
    if (rc)
    {
        remove(temp.c_str());
        if (barOk)
            printProgress();
        return rc;
    }
    g_rasterizeTempPaths.push_back(temp);
    static bool s_cleanupReg = false;
    if (!s_cleanupReg)
    {
        s_cleanupReg = true;
        atexit([] {
            for (const auto &p : g_rasterizeTempPaths)
                remove(p.c_str());
        });
    }
    g_infoFilesHide = temp;
    if (isInfo)
    {
        bool srsRepl = false;
        for (const StepData *t : postT)
        {
            if (t->name == "reproject" ||
                (t->name == "edit" && t->has("crs")))
                srsRepl = true;
            if (t->name == "reproject")
                g_infoFilesHideDerived = true;
        }
        if (!srsRepl && g_rasterizeLastSrsSet)
        {
            g_infoSrsOverrideSet = true;
            g_infoSrsOverride = g_rasterizeLastSrs;
        }
    }
    std::vector<std::string> toks{"read", temp};
    for (const StepData *t : postT)
    {
        toks.push_back("!");
        toks.push_back(t->name);
        for (const auto &tk : t->tokens)
            toks.push_back(tk);
    }
    toks.push_back("!");
    toks.push_back(term.name);
    for (const auto &tk : term.tokens)
        toks.push_back(tk);
    PipeCtx c2 = c;
    c2.type = P_GENERIC;
    c2.quiet = quietAll;
    return pipelineCore(c2, toks);
}

struct MVal
{
    enum Kind
    {
        OBJ,
        ARR,
        STR,
        NUM,
        BOOL,
        NUL
    } kind = NUL;
    std::string text;
    std::vector<std::pair<std::string, MVal>> obj;
    std::vector<MVal> arr;
};

// mimics json-c's tokener closely enough to reproduce its error
// descriptors and offsets
struct MiniJson
{
    const std::string &s;
    size_t i = 0;
    int depth = 0;
    std::string err;
    size_t errOff = 0;

    explicit MiniJson(const std::string &t) : s(t) {}

    bool fail(const char *d, size_t off)
    {
        if (err.empty())
        {
            err = d;
            errOff = off;
        }
        return false;
    }
    bool eof() const { return i >= s.size(); }
    void ws()
    {
        while (!eof() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                          s[i] == '\r'))
            ++i;
    }
    bool literal(const char *lit, const char *desc)
    {
        for (size_t k = 0; lit[k]; ++k, ++i)
        {
            if (eof())
                return fail("continue", i);
            char lc = s[i];
            if (lc >= 'A' && lc <= 'Z')
                lc = (char)(lc - 'A' + 'a');
            if (lc != lit[k])
                return fail(desc, i);
        }
        return true;
    }
    bool str(std::string &out)
    {
        ++i;  // opening quote
        while (true)
        {
            if (eof())
                return fail("continue", i);
            char ch = s[i];
            if (ch == '"')
            {
                ++i;
                return true;
            }
            if (ch == '\\')
            {
                ++i;
                if (eof())
                    return fail("continue", i);
                char e = s[i];
                switch (e)
                {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k)
                        {
                            ++i;
                            if (eof())
                                return fail("continue", i);
                            char h = s[i];
                            unsigned v;
                            if (h >= '0' && h <= '9')
                                v = h - '0';
                            else if (h >= 'a' && h <= 'f')
                                v = h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F')
                                v = h - 'A' + 10;
                            else
                                return fail("invalid string sequence", i);
                            cp = cp * 16 + v;
                        }
                        if (cp < 0x80)
                            out += (char)cp;
                        else if (cp < 0x800)
                        {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        else
                        {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        return fail("invalid string sequence", i);
                }
                ++i;
                continue;
            }
            out += ch;
            ++i;
        }
    }
    bool value(MVal &v)
    {
        ws();
        if (eof())
            return fail("continue", i);
        char ch = s[i];
        if (ch == '{')
        {
            v.kind = MVal::OBJ;
            ++i;
            ++depth;
            while (true)
            {
                ws();
                if (eof())
                    return fail("continue", i);
                if (s[i] == '}')
                {
                    ++i;
                    --depth;
                    return true;
                }
                if (s[i] != '"')
                    return fail("quoted object property name expected", i);
                std::string key;
                if (!str(key))
                    return false;
                ws();
                if (eof())
                    return fail("continue", i);
                if (s[i] != ':')
                    return fail(
                        "object property name separator ':' expected", i);
                ++i;
                MVal mv;
                if (!value(mv))
                    return false;
                v.obj.emplace_back(key, std::move(mv));
                ws();
                if (eof())
                    return fail("continue", i);
                if (s[i] == ',')
                {
                    ++i;
                    continue;
                }
                if (s[i] == '}')
                {
                    ++i;
                    --depth;
                    return true;
                }
                return fail("object value separator ',' expected", i);
            }
        }
        if (ch == '[')
        {
            v.kind = MVal::ARR;
            ++i;
            ++depth;
            while (true)
            {
                ws();
                if (eof())
                    return fail("continue", i);
                if (s[i] == ']')
                {
                    ++i;
                    --depth;
                    return true;
                }
                MVal mv;
                if (!value(mv))
                    return false;
                v.arr.push_back(std::move(mv));
                ws();
                if (eof())
                    return fail("continue", i);
                if (s[i] == ',')
                {
                    ++i;
                    continue;
                }
                if (s[i] == ']')
                {
                    ++i;
                    --depth;
                    return true;
                }
                return fail("array value separator ',' expected", i);
            }
        }
        if (ch == '"')
        {
            v.kind = MVal::STR;
            return str(v.text);
        }
        if (ch == 't' || ch == 'T')
        {
            v.kind = MVal::BOOL;
            v.text = "true";
            return literal("true", "boolean expected");
        }
        if (ch == 'f' || ch == 'F')
        {
            v.kind = MVal::BOOL;
            v.text = "false";
            return literal("false", "boolean expected");
        }
        if (ch == 'n' || ch == 'N')
        {
            if (i + 1 < s.size() && (s[i + 1] == 'a' || s[i + 1] == 'A'))
            {
                v.kind = MVal::NUM;
                v.text = "NaN";
                return literal("nan", "null expected");
            }
            v.kind = MVal::NUL;
            return literal("null", "null expected");
        }
        if (ch == 'I' || ch == 'i')
        {
            v.kind = MVal::NUM;
            v.text = "Infinity";
            return literal("infinity", "unexpected character");
        }
        if (ch == '-' || (ch >= '0' && ch <= '9'))
        {
            v.kind = MVal::NUM;
            size_t start = i;
            if (ch == '-')
            {
                ++i;
                if (eof())
                    return fail("continue", i);
                if (s[i] == 'I' || s[i] == 'i')
                {
                    v.text = "-Infinity";
                    return literal("infinity", "unexpected character");
                }
                if (s[i] < '0' || s[i] > '9')
                    return fail("number expected", i);
            }
            bool seenDot = false, seenExp = false, afterExp = false;
            while (!eof() && (isdigit((unsigned char)s[i]) || s[i] == '.' ||
                              s[i] == 'e' || s[i] == 'E' || s[i] == '+' ||
                              s[i] == '-'))
            {
                char nc = s[i];
                if (nc == '.')
                {
                    if (seenDot || seenExp)
                        return fail("number expected", i);
                    seenDot = true;
                }
                else if (nc == 'e' || nc == 'E')
                {
                    if (seenExp)
                        return fail("number expected", i);
                    seenExp = true;
                    afterExp = true;
                    ++i;
                    continue;
                }
                else if (nc == '+' || nc == '-')
                {
                    if (!afterExp)
                        return fail("number expected", i);
                }
                afterExp = false;
                ++i;
            }
            // trailing garbage terminates a root value silently (the
            // tokener never reprocesses it) but errors inside containers
            if (depth > 0 && !eof() && s[i] != ' ' && s[i] != '\t' &&
                s[i] != '\n' && s[i] != '\r' && s[i] != ',' &&
                s[i] != '}' && s[i] != ']' && s[i] != ':')
                return fail("number expected", i);
            v.text = s.substr(start, i - start);
            return true;
        }
        return fail("unexpected character", i);
    }
};

void mvalSerialize(const MVal &v, std::string &out)
{
    switch (v.kind)
    {
        case MVal::OBJ:
        {
            if (v.obj.empty())
            {
                out += "{ }";
                return;
            }
            out += "{ ";
            for (size_t k = 0; k < v.obj.size(); ++k)
            {
                if (k)
                    out += ", ";
                out += '"';
                out += v.obj[k].first;
                out += "\": ";
                mvalSerialize(v.obj[k].second, out);
            }
            out += " }";
            return;
        }
        case MVal::ARR:
        {
            if (v.arr.empty())
            {
                out += "[ ]";
                return;
            }
            out += "[ ";
            for (size_t k = 0; k < v.arr.size(); ++k)
            {
                if (k)
                    out += ", ";
                mvalSerialize(v.arr[k], out);
            }
            out += " ]";
            return;
        }
        case MVal::STR:
            out += '"';
            out += v.text;
            out += '"';
            return;
        case MVal::NUM:
            out += v.text;
            return;
        case MVal::BOOL:
            out += v.text;
            return;
        case MVal::NUL:
            out += "null";
            return;
    }
}

int gdalgExec(PipeCtx &c, const std::string &path)
{
    std::string content;
    if (!readFileToString(path, content))
    {
        cplErrorStr(CE_Failure, CPLE_FileIO, "Cannot open file '" + path +
                                                 "'");
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    "Load json file " + path + " failed");
        return pipeFail(c);
    }
    MiniJson mj(content);
    MVal root;
    bool ok = mj.value(root);
    if (ok && root.kind == MVal::NUL)
        ok = mj.fail("continue", mj.i);
    if (!ok)
        return pipeFailMsg(c, CPLE_AppDefined,
                           strPrintf("JSON parsing error: %s (at offset "
                                     "%zu)",
                                     mj.err.c_str(), mj.errOff));
    std::string cli;
    bool cliSet = false;
    if (root.kind == MVal::OBJ)
        for (const auto &kv : root.obj)
            if (kv.first == "command_line" && kv.second.kind != MVal::NUL)
            {
                if (kv.second.kind == MVal::STR)
                    cli = kv.second.text;
                else
                    mvalSerialize(kv.second, cli);
                cliSet = true;
            }
    if (!cliSet || cli.empty())
        return pipeFailMsg(c, CPLE_AppDefined,
                           "pipeline: command_line missing in " + path);
    std::vector<std::string> toks;
    {
        std::string cur;
        for (char ch : cli)
        {
            if (isspace((unsigned char)ch))
            {
                if (!cur.empty())
                {
                    toks.push_back(cur);
                    cur.clear();
                }
            }
            else
                cur += ch;
        }
        if (!cur.empty())
            toks.push_back(cur);
    }
    if (toks.empty())
        raise(SIGSEGV);
    // only these exact prefixes are stripped, and only when followed by
    // further tokens
    auto stripPrefix = [&](std::initializer_list<const char *> pfx) {
        if (toks.size() <= pfx.size())
            return false;
        size_t i = 0;
        for (const char *p : pfx)
            if (toks[i++] != p)
                return false;
        toks.erase(toks.begin(), toks.begin() + pfx.size());
        return true;
    };
    if (!stripPrefix({"gdal", "pipeline"}))
        if (!stripPrefix({"gdal", "raster", "pipeline"}))
            stripPrefix({"gdal", "vector", "pipeline"});
    // implicit trailing write when the recorded pipeline does not end in a
    // terminal step
    std::string lastName;
    for (size_t i = 0; i < toks.size(); ++i)
        if (i == 0 || toks[i - 1] == "!")
            if (toks[i] != "!")
                lastName = toks[i];
    if (!lastStepAllowed(c.type, lastName))
    {
        toks.push_back("!");
        toks.push_back("write");
    }
    return pipelineCore(c, std::move(toks));
}

int pipelineCore(PipeCtx &c, std::vector<std::string> rawTokens)
{
    // --pipeline is resolved on the raw argv tokens, before single-token
    // re-tokenization, so an equals-form value keeps its embedded spaces
    {
        bool pipeStrSet = false, pipeStrMissing = false;
        std::string pipeStr;
        std::vector<std::string> rest;
        for (size_t i = 0; i < rawTokens.size(); ++i)
        {
            const std::string &t = rawTokens[i];
            if (t == "--pipeline")
            {
                pipeStrSet = true;
                if (i + 1 < rawTokens.size())
                    pipeStr = rawTokens[++i];
                else
                    pipeStrMissing = true;
                continue;
            }
            if (t.compare(0, 11, "--pipeline=") == 0)
            {
                pipeStrSet = true;
                pipeStr = t.substr(11);
                continue;
            }
            rest.push_back(t);
        }
        if (pipeStrSet)
        {
            bool helpish = false, havePos = false;
            std::string firstPos;
            for (const auto &t : rest)
            {
                if (t == "--help" || t == "-h" || t == "--json-usage")
                    helpish = true;
                else if (t == "-q" || t == "--quiet")
                    c.quiet = true;
                else if (t != "--progress" && !havePos)
                {
                    havePos = true;
                    firstPos = t;
                }
            }
            if (helpish)
                rawTokens = std::move(rest);
            else if (havePos)
                return pipeFailMsg(c, CPLE_AppDefined,
                                   "pipeline: Positional values starting "
                                   "at '" +
                                       firstPos + "' are not expected.");
            else if (pipeStrMissing)
                return pipeFailMsg(c, CPLE_IllegalArg,
                                   "pipeline: Expected value for argument "
                                   "'--pipeline', but ran short of "
                                   "tokens");
            else if (pipeStr.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "pipeline: 'pipeline' argument not set");
                return kParseFail;
            }
            else if (pipeStr.find(".gdalg.json") != std::string::npos)
                return gdalgExec(c, pipeStr);
            else
            {
                PipeCtx sub = c;
                sub.usage.clear();
                return pipelineCore(sub, {pipeStr});
            }
        }
    }

    // a pipeline given as one single token is re-tokenized on
    // whitespace; double quotes group (and are stripped), CSL style
    std::vector<std::string> tokens;
    if (rawTokens.size() == 1)
    {
        std::string cur;
        bool inStr = false;
        for (char ch : rawTokens[0])
        {
            if (ch == '"')
            {
                inStr = !inStr;
                continue;
            }
            if (!inStr && (ch == ' ' || ch == '\t'))
            {
                if (!cur.empty())
                {
                    tokens.push_back(cur);
                    cur.clear();
                }
            }
            else
                cur += ch;
        }
        if (!cur.empty())
            tokens.push_back(cur);
        // whitespace-bearing single-token pipelines behave like the
        // reference's re-parsed command strings: failures skip the usage
        // block and progress reaches /vsistdout/ outputs
        if (tokens.size() > 1)
        {
            c.noUsage = true;
            g_pipelineBarStdout = true;
        }
    }
    else
        tokens = std::move(rawTokens);
    // global option strip
    std::vector<std::string> tk;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::string &t = tokens[i];
        if (t == "-q" || t == "--quiet")
        {
            c.quiet = true;
            continue;
        }
        if (t == "--progress")
            continue;
        tk.push_back(t);
    }

    if (!c.alias && tk.size() == 1 && tk[0] == "help")
        return kHelpPassthrough;

    auto isAliasStrip = [](const std::string &t) {
        return t == "-h" || t == "--help" || t == "--help-doc" ||
               t == "--json-usage";
    };
    auto isHelpTok = [](const std::string &t) {
        return t == "-h" || t == "--help" || t == "--json-usage";
    };
    bool hasHelp = false;
    bool allHelpish = !tk.empty();
    std::string firstJson;
    size_t bangCount = 0;
    for (const auto &t : tk)
    {
        if (t == "--help" || t == "-h")
            hasHelp = true;
        if (!isHelpTok(t))
        {
            allHelpish = false;
            if (firstJson.empty() &&
                strEndsWith(strToLower(t), ".json"))
                firstJson = t;
        }
        if (t == "!")
            ++bangCount;
    }
    if (c.alias)
    {
        if (hasHelp && !firstJson.empty() && bangCount == 0)
            return pipeFailMsg(c, CPLE_AppDefined,
                               "pipeline: Positional values starting at '" +
                                   firstJson + "' are not expected.");
        std::vector<std::string> kept;
        for (const auto &t : tk)
            if (!isAliasStrip(t))
                kept.push_back(t);
        tk = std::move(kept);
    }
    else if (allHelpish)
        return kHelpPassthrough;
    else if (hasHelp)
    {
        const char *helpDir = c.type == P_RASTER
                                  ? "ras"
                                  : c.type == P_VECTOR ? "vec" : "gen";
        if (bangCount == 0)
        {
            if (!firstJson.empty())
                return pipeFail(c);
            std::string sname;
            for (const auto &t : tk)
                if (!isHelpTok(t))
                {
                    sname = !t.empty() && t[0] == '+' ? t.substr(1) : t;
                    break;
                }
            if (!stepNames(c.type).count(sname))
                return pipeFailMsg(c, CPLE_AppDefined,
                                   "pipeline: unknown step name: " + sname);
            if (sname == "tile" && c.type != P_VECTOR)
            {
                std::string w = tmsDiagnosticLine();
                fwrite(w.data(), 1, w.size(), stderr);
            }
            std::string h = embGet(std::string("pipelinehelp/") + helpDir +
                                   "/" + sname + ".out");
            fwrite(h.data(), 1, h.size(), stdout);
            exit(0);
        }
        // sequential: validate step names up to the step containing help
        std::string cur;
        bool curHasHelp = false;
        bool atName = true;
        for (const auto &t : tk)
        {
            if (t == "!")
            {
                if (curHasHelp)
                    break;
                cur.clear();
                curHasHelp = false;
                atName = true;
                continue;
            }
            if (atName && !isHelpTok(t))
            {
                cur = !t.empty() && t[0] == '+' ? t.substr(1) : t;
                atName = false;
                if (!stepNames(c.type).count(cur))
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "pipeline: unknown step name: " +
                                           cur);
                continue;
            }
            if (t == "--help" || t == "-h")
                curHasHelp = true;
        }
        if (curHasHelp && !cur.empty())
        {
            if (cur == "tile" && c.type != P_VECTOR)
            {
                std::string w = tmsDiagnosticLine();
                fwrite(w.data(), 1, w.size(), stderr);
            }
            std::string h = embGet(std::string("pipelinehelp/") + helpDir +
                                   "/" + cur + ".out");
            fwrite(h.data(), 1, h.size(), stdout);
            exit(0);
        }
        return kHelpPassthrough;
    }

    // a single .json positional is executed as a GDALG file
    if (tk.size() == 1 && tk[0] != "!" &&
        strEndsWith(strToLower(tk[0]), ".json"))
        return gdalgExec(c, tk[0]);

    // bracket groups (nested pipelines are not runnable in this build,
    // but their parse diagnostics are)
    {
        bool stepStart = true;
        for (size_t i = 0; i < tk.size();)
        {
            const std::string &t = tk[i];
            if (t == "[")
            {
                if (stepStart)
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "pipeline: Open bracket must be "
                                       "placed where an input dataset is "
                                       "expected");
                size_t depth = 1;
                size_t j = i + 1;
                for (; j < tk.size(); ++j)
                {
                    if (tk[j] == "[")
                        ++depth;
                    else if (tk[j] == "]" && --depth == 0)
                        break;
                }
                if (depth)
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "pipeline: Open bracket has no "
                                       "matching closing bracket");
                {
                    StepData inner;
                    bool atName = true;
                    for (size_t k = i + 1; k < j; ++k)
                    {
                        if (tk[k] == "!")
                        {
                            if (!atName)
                            {
                                int rc = parseStep(c, inner, true);
                                if (rc)
                                    return rc;
                            }
                            inner = StepData();
                            atName = true;
                            continue;
                        }
                        if (atName)
                        {
                            inner.name = !tk[k].empty() && tk[k][0] == '+'
                                             ? tk[k].substr(1)
                                             : tk[k];
                            atName = false;
                            if (!stepNames(c.type).count(inner.name))
                                return pipeFailMsg(
                                    c, CPLE_AppDefined,
                                    "pipeline: unknown step name: " +
                                        inner.name);
                        }
                        else
                            inner.tokens.push_back(
                                !tk[k].empty() && tk[k][0] == '+'
                                    ? "--" + tk[k].substr(1)
                                    : tk[k]);
                    }
                    if (!atName)
                    {
                        int rc = parseStep(c, inner, true);
                        if (rc)
                            return rc;
                    }
                }
                tk.erase(tk.begin() + i, tk.begin() + j + 1);
                continue;
            }
            if (t == "]")
                return pipeFailMsg(c, CPLE_AppDefined,
                                   "pipeline: Closing bracket found "
                                   "without matching open bracket");
            stepStart = t == "!";
            ++i;
        }
    }

    // split on '!'; '+token' is alternative syntax: a bare step name in
    // name position, '--token' anywhere else; a token carrying a space
    // can only be a value and stays literal
    std::vector<StepData> steps;
    {
        auto plusTok = [](const std::string &t) {
            return !t.empty() && t[0] == '+' &&
                   t.find(' ') == std::string::npos;
        };
        StepData cur;
        bool inStep = false;
        for (const auto &t : tk)
        {
            if (t == "!")
            {
                if (inStep)
                    steps.push_back(std::move(cur));
                cur = StepData();
                inStep = false;
                continue;
            }
            if (!inStep)
            {
                cur.name = plusTok(t) ? t.substr(1) : t;
                cur.usedVecOnly = cur.usedVecOnly || cur.name == "concat";
                inStep = true;
            }
            else
                cur.tokens.push_back(plusTok(t) ? "--" + t.substr(1) : t);
        }
        if (inStep)
            steps.push_back(std::move(cur));
    }

    // a first token that can never be a step name (it contains blanks)
    // makes the leaf parser treat it as its single positional
    if (!steps.empty() && !stepNames(c.type).count(steps[0].name) &&
        steps[0].name.find_first_of(" \t") != std::string::npos &&
        tk.size() > 1)
        return pipeFailMsg(c, CPLE_AppDefined,
                           "pipeline: Positional values starting at '" +
                               tk[1] + "' are not expected.");

    // unknown step names
    for (const auto &st : steps)
        if (!stepNames(c.type).count(st.name))
            return pipeFailMsg(c, CPLE_AppDefined,
                               "pipeline: unknown step name: " + st.name);

    if (steps.size() < 2)
    {
        if (steps.size() == 1 && steps[0].name == "read")
        {
            std::vector<std::string> full;
            full.push_back("read");
            for (const auto &t : steps[0].tokens)
                full.push_back(t);
            std::string input = quickReadInput(full);
            if (c.type == P_GENERIC)
            {
                if (input.empty())
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "pipeline: At least 2 steps must be "
                                       "provided");
                std::string sub = convertDispatchProbe(input);
                if (sub.empty() && !fileExistsP(input))
                    return pipeFailMsg(c, CPLE_OpenFailed,
                                       datasetMissingMessage(input));
                if (sub == "raster")
                {
                    if (c.alias)
                    {
                        if (!c.quiet)
                        {
                            TermProgress tp;
                            tp.update(1.0);
                        }
                        exit(0);
                    }
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       "pipeline: At least 2 steps must be "
                                       "provided");
                }
                return vectorSingleReadQuirk(c, full);
            }
            if (c.type == P_VECTOR)
                return vectorSingleReadQuirk(c, full);
        }
        // a lone create step parses at the pipeline level: a leftover
        // positional is reported, otherwise the usage block stands alone
        if (steps.size() == 1 && steps[0].name == "create" &&
            c.type != P_RASTER && !steps[0].tokens.empty())
        {
            const CmdSpec *cs = Spec::instance().findById("vector_create");
            std::string firstPos;
            const auto &tk2 = steps[0].tokens;
            for (size_t i = 0; i < tk2.size() && cs; ++i)
            {
                const std::string &t = tk2[i];
                if (t.size() >= 2 && t[0] == '-')
                {
                    std::string nm = t;
                    size_t eq = nm.find('=');
                    if (eq != std::string::npos)
                        nm = nm.substr(0, eq);
                    const ArgSpec *a = nullptr;
                    bool isShort = nm.size() == 2 && nm[1] != '-';
                    std::string bare =
                        isShort ? nm.substr(1)
                                : strStartsWith(nm, "--") ? nm.substr(2)
                                                          : nm;
                    for (const auto &as : cs->args)
                    {
                        if (isShort)
                        {
                            for (const auto &s : as.shorts)
                                if (s == bare)
                                    a = &as;
                        }
                        else
                        {
                            if (as.name == bare)
                                a = &as;
                            for (const auto &al : as.aliases)
                                if (al == bare)
                                    a = &as;
                        }
                        if (a)
                            break;
                    }
                    if (a && !a->isBool() && eq == std::string::npos)
                        ++i;
                    continue;
                }
                firstPos = t;
                break;
            }
            if (!firstPos.empty())
                return pipeFailMsg(c, CPLE_AppDefined,
                                   "pipeline: Positional values starting "
                                   "at '" +
                                       firstPos + "' are not expected.");
            return pipeFail(c);
        }
        return pipeFailMsg(c, CPLE_AppDefined,
                           "pipeline: At least 2 steps must be provided");
    }

    // a generic pipeline led by 'create' is a vector pipeline: the step
    // builds a vector dataset
    if (c.type == P_GENERIC && steps[0].name == "create")
        c.type = P_VECTOR;

    // step ordering constraints
    if (!firstStepAllowed(c.type, steps[0].name))
    {
        if (c.type != P_VECTOR)
        {
            std::string w = tmsDiagnosticLine();
            fwrite(w.data(), 1, w.size(), stderr);
        }
        return pipeFailMsg(c, CPLE_AppDefined, firstStepMsg(c.type));
    }
    for (size_t i = 1; i < steps.size(); ++i)
    {
        const std::string &n = steps[i].name;
        if (n == "read")
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pipeline: Only first step can be 'read'");
        if (n == "concat")
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pipeline: Only first step can be 'concat'");
        if (n == "create" && c.type != P_VECTOR)
            return pipeFailMsg(c, CPLE_AppDefined,
                               "pipeline: 'create' is only allowed as a "
                               "first step");
        if (n == "info" && i + 1 < steps.size())
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pipeline: Only last step can be 'info'");
        if (n == "export-schema" && i + 1 < steps.size())
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pipeline: Only last step can be "
                        "'export-schema'");
    }
    for (size_t i = 0; i + 1 < steps.size(); ++i)
        if (steps[i].name == "write" && i > 0)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "pipeline: Only last step can be 'write'");
    if (!lastStepAllowed(c.type, steps.back().name))
    {
        if (c.type != P_VECTOR)
        {
            std::string w = tmsDiagnosticLine();
            fwrite(w.data(), 1, w.size(), stderr);
        }
        return pipeFailMsg(c, CPLE_AppDefined, lastStepMsg(c.type));
    }

    // per-step argument syntax, left to right
    {
        bool seenTrans = false;
        for (size_t i = 0; i < steps.size(); ++i)
        {
            int rc = parseStep(c, steps[i], i == 0, seenTrans);
            if (rc)
                return rc;
            if (steps[i].trans)
                seenTrans = true;
            if (steps[i].transRas)
                seenTrans = false;
        }
    }

    // the reference dereferences a null usage context here
    if (!c.alias && steps[0].name == "read" &&
        (steps[0].has("json-usage") || steps[0].has("help-doc")))
        raise(SIGSEGV);

    StepData &read = steps[0];

    // generic dispatch by opening the input
    if (c.type == P_GENERIC && read.name == "concat")
        c.type = P_VECTOR;
    if (c.type == P_GENERIC)
    {
        c.genericOrigin = true;
        for (const auto &st : steps)
            if (st.name == "filter" || st.name == "sql" ||
                st.name == "swap-xy" || st.name == "segmentize" ||
                st.name == "make-point" ||
                st.name == "explode-collections" ||
                st.name == "set-geom-type" || st.name == "rename-layer")
                c.vecFixedByName = true;
        std::vector<std::string> full;
        full.push_back("read");
        for (const auto &t : read.tokens)
            full.push_back(t);
        if (readHasVectorOnlyOpt(full))
            c.type = P_VECTOR;
        else
        {
            std::string input = read.str("input");
            std::string sub = convertDispatchProbe(input);
            if (sub.empty() && !fileExistsP(input))
                return pipeFailMsg(c, CPLE_OpenFailed,
                                   datasetMissingMessage(input));
            c.type = sub == "raster" ? P_RASTER : P_VECTOR;
        }
        {
            // the pipeline type evolves across transition steps: each
            // step validates against the type its predecessor generates
            PipeType cur = c.type;
            std::string prevGen = "read";
            for (auto &st : steps)
            {
                if (st.trans)
                {
                    if (cur == P_VECTOR)
                        return pipeFailMsg(
                            c, CPLE_AppDefined,
                            "pipeline: Step '" + st.name +
                                "' expects a raster input dataset, but "
                                "previous step '" + prevGen +
                                "' generates a vector output dataset");
                    cur = P_VECTOR;
                    prevGen = st.name;
                    continue;
                }
                if (st.transRas)
                {
                    if (cur == P_RASTER)
                        return pipeFailMsg(
                            c, CPLE_AppDefined,
                            "pipeline: Step '" + st.name +
                                "' expects a vector input dataset, but "
                                "previous step '" + prevGen +
                                "' generates a raster output dataset");
                    cur = P_RASTER;
                    prevGen = st.name;
                    continue;
                }
                if (st.selDual)
                {
                    // raster-side resolution prefers the raster select:
                    // syntactically valid band tokens pick it; anything
                    // else falls back to the vector select, whose parse
                    // error (or input-type mismatch) is the one reported.
                    // On the vector side the post-walk re-parse handles
                    // the step like any other vector verb
                    st.selDual = false;
                    if (cur == P_RASTER)
                    {
                        PipeCtx quietCtx = c;
                        quietCtx.alias = true;
                        cplPushQuietHandler();
                        int rc = parseSpecStep(
                            quietCtx, st,
                            Spec::instance().findById("raster_select"),
                            false);
                        bool rasterOk =
                            !rc && st.has("band") &&
                            rasterSelectBandTokensValid(st.list("band")) &&
                            rasterSelectMaskTokenValid(st.str("mask"));
                        cplPopHandler();
                        if (!rasterOk)
                        {
                            st.v.clear();
                            st.pos.clear();
                            cplPushQuietHandler();
                            rc = parseSpecStep(
                                quietCtx, st,
                                Spec::instance().findById(
                                    "vector_select"),
                                true);
                            cplPopHandler();
                            if (rc)
                            {
                                st.v.clear();
                                st.pos.clear();
                                return parseSpecStep(
                                    c, st,
                                    Spec::instance().findById(
                                        "vector_select"),
                                    true);
                            }
                            st.usedVecOnly = true;
                        }
                    }
                }
                if (cur == P_RASTER && st.usedVecOnly)
                    return pipeFailMsg(
                        c, CPLE_AppDefined,
                        "pipeline: Step '" + st.name +
                            "' expects a vector input dataset, but "
                            "previous step '" + prevGen +
                            "' generates a raster output dataset");
                if (cur == P_VECTOR && st.usedRasOnly)
                    return pipeFailMsg(
                        c, CPLE_AppDefined,
                        "pipeline: Step '" + st.name +
                            "' expects a raster input dataset, but "
                            "previous step '" + prevGen +
                            "' generates a vector output dataset");
                if (cur == P_VECTOR && c.type == P_RASTER &&
                    &st != &steps[0] && st.name != "write" &&
                    st.name != "info" && st.name != "export-schema" &&
                    !st.usedVecOnly && rasterTailStepKnown(st.name) &&
                    st.name != "reproject" && st.name != "update" &&
                    !vectorVerbStepName(st.name) &&
                    !geosWallStepName(st.name))
                    return pipeFailMsg(
                        c, CPLE_AppDefined,
                        "pipeline: Step '" + st.name +
                            "' expects a raster input dataset, but "
                            "previous step '" + prevGen +
                            "' generates a vector output dataset");
                st.sideVec = cur == P_VECTOR && c.type == P_RASTER;
                st.sideRas = cur == P_RASTER && c.type == P_VECTOR;
            }
        }
        if (c.type == P_VECTOR)
            for (auto &st : steps)
            {
                bool cand = st.name == "reproject" ||
                            st.name == "update" ||
                            vectorVerbStepName(st.name) ||
                            geosWallStepName(st.name);
                if (!cand || st.usedVecOnly || st.sideRas)
                    continue;
                st.v.clear();
                st.pos.clear();
                int rc = parseSpecStep(
                    c, st,
                    Spec::instance().findById("vector_" + st.name), true);
                if (rc)
                    return rc;
            }
    }

    // the read step opens the input before write-side driver resolution
    std::string readDriver;
    if (read.name == "read")
    {
        const std::string input = read.str("input");
        bool okOpen = false;
        bool missing = !fileExistsP(input);
        cplPushQuietHandler();
        if (c.type == P_RASTER)
        {
            std::string err;
            OpenOptions oo;
            oo.allowedDrivers = read.list("input-format");
            okOpen = openRaster(input, err, oo) != nullptr;
        }
        else
        {
            std::string err;
            auto vds = openVectorDataset(input, err,
                                         read.list("input-format"));
            okOpen = vds != nullptr;
            if (vds)
                readDriver = vds->driverShort;
            if (!okOpen && err == "missing")
                missing = true;
        }
        cplPopHandler();
        if (!okOpen)
        {
            if (missing)
                return pipeFailMsg(c, CPLE_OpenFailed,
                                   datasetMissingMessage(input));
            return pipeFailMsg(c, CPLE_OpenFailed,
                               "`" + input +
                                   "' not recognized as being in a "
                                   "supported file format.");
        }
    }

    // a set-field-type step running directly after a GeoJSON read is
    // absorbed into the read as a schema override: its mutex/required
    // validation never runs (partial argument sets act as identity) —
    // unless the terminal is a GDALG serialization, which always
    // validates
    std::vector<char> sftCastStep(steps.size(), 0);
    {
        bool gdalgTerm = false;
        if (steps.back().name == "write")
        {
            std::string tof = steps.back().str("of");
            gdalgTerm =
                strEqualNoCase(tof, "GDALG") ||
                (tof.empty() &&
                 strEndsWith(strToLower(steps.back().str("output")),
                             ".gdalg.json"));
        }
        if (c.type == P_VECTOR && !gdalgTerm && read.name == "read" &&
            readDriver == "GeoJSON")
        {
            bool leading = true;
            for (size_t i = 1; i < steps.size(); ++i)
            {
                if (steps[i].name == "set-field-type")
                    sftCastStep[i] = leading ? 1 : 0;
                else
                    leading = false;
            }
        }
    }

    // every step validates its arguments up front, after the read open
    // but before any step runs: mutually exclusive pairs, the step-input
    // open probe (deferred failure), then required arguments
    bool stepProbeFail = false;
    for (size_t si = 0; si < steps.size(); ++si)
    {
        const auto &st = steps[si];
        const CmdSpec *cs = nullptr;
        bool vec = false;
        if (st.trans)
            cs = Spec::instance().findById("raster_" + st.name);
        else if (st.transRas)
        {
            cs = Spec::instance().findById("vector_rasterize");
            vec = true;
        }
        else if (((c.type == P_RASTER && !st.sideVec) || st.sideRas) &&
                 rasterTailStepKnown(st.name))
            cs = Spec::instance().findById("raster_" + st.name);
        else if (((c.type == P_VECTOR && !st.sideRas) || st.sideVec) &&
                 (st.name == "reproject" || st.name == "create" ||
                  st.name == "update" || vectorVerbStepName(st.name) ||
                  geosWallStepName(st.name)))
        {
            cs = Spec::instance().findById("vector_" + st.name);
            vec = true;
        }
        if (!cs)
            continue;
        if (st.name == "create")
        {
            // spec-order anchoring plus the combined refusal, all
            // accumulated ahead of the usage block
            std::vector<std::string> group;
            for (const char *n : {"input", "schema", "field"})
                if (st.v.count(n))
                    group.push_back(n);
            bool bad = false;
            for (size_t i = 1; i < group.size(); ++i)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: Argument '" + group[i] +
                                "' is mutually exclusive with '" +
                                group[0] + "'.");
                bad = true;
            }
            bool tmpl = st.v.count("input") || st.v.count("schema");
            bool extras = st.v.count("geometry-field") ||
                          st.v.count("geometry-type") ||
                          st.v.count("field") || st.v.count("crs") ||
                          st.v.count("fid");
            if (tmpl && extras)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: When --schema or --like is "
                            "specified, --geometry-field, "
                            "--geometry-type, --field, --crs and --fid "
                            "options must not be specified.");
                bad = true;
            }
            if (bad)
                return pipeFail(c);
        }
        else if (!sftCastStep[si])
            for (const auto &a : cs->args)
            {
                if (a.mutex.empty() || !st.v.count(a.name))
                    continue;
                for (const auto &b : cs->args)
                {
                    if (&b == &a)
                        break;
                    if (b.mutex == a.mutex && st.v.count(b.name))
                        return pipeFailMsg(c, CPLE_AppDefined,
                                           st.name + ": Argument '" +
                                               a.name +
                                               "' is mutually exclusive "
                                               "with '" +
                                               b.name + "'.");
                }
            }
        if (st.trans && st.name == "contour")
        {
            std::vector<std::string> toks;
            for (const auto &raw : st.list("levels"))
                for (const auto &p : strSplit(raw, ','))
                    toks.push_back(p);
            for (size_t i = 0; i < toks.size(); ++i)
                for (size_t j = i + 1; j < toks.size(); ++j)
                {
                    bool dup;
                    if (strEqualNoCase(toks[i], "MIN") ||
                        strEqualNoCase(toks[i], "MAX") ||
                        strEqualNoCase(toks[j], "MIN") ||
                        strEqualNoCase(toks[j], "MAX"))
                        dup = strEqualNoCase(toks[i], toks[j]);
                    else
                        dup = strtod(toks[i].c_str(), nullptr) ==
                              strtod(toks[j].c_str(), nullptr);
                    if (dup)
                        return pipeFailMsg(c, CPLE_AppDefined,
                                           "'levels' must be a list of "
                                           "unique values.");
                }
        }
        if (st.has("input"))
        {
            const std::string &in = st.str("input");
            bool missing = !fileExistsP(in);
            cplPushQuietHandler();
            std::string err;
            bool okOpen =
                vec ? openVectorDataset(in, err, {}) != nullptr
                    : openRaster(in, err) != nullptr;
            cplPopHandler();
            if (vec && !okOpen && err == "missing")
                missing = true;
            if (!okOpen)
            {
                if (missing)
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(in));
                else
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + in +
                                    "' not recognized as being in a "
                                    "supported file format.");
                stepProbeFail = true;
            }
        }
        // the update step opens its target already at validation (open
        // option warnings included); the chain execution only reopens
        if (vec && st.name == "update" && st.has("output"))
        {
            const std::string outPath = st.str("output");
            bool updMode =
                !(st.has("update") && st.str("update") == "false");
            if (!updMode)
            {
                if (fileExistsP(outPath))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "update: Dataset '" + outPath +
                                    "' already exists. You may specify "
                                    "the --overwrite/--update option.");
                    stepProbeFail = true;
                }
            }
            else
            {
                std::unique_ptr<OgrDataset> tgt;
                std::string terr;
                if (!strEndsWith(strToLower(outPath), ".gdalg.json"))
                    tgt = openVectorDataset(
                        outPath, terr, {},
                        st.list("output-open-option"));
                else if (!fileExistsP(outPath))
                    terr = "missing";
                if (!tgt)
                {
                    struct stat stt;
                    if (terr == "missing")
                        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                    datasetMissingMessage(outPath));
                    else if (terr != "reported" &&
                             stat(outPath.c_str(), &stt) == 0 &&
                             S_ISDIR(stt.st_mode))
                        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                    outPath + ": Is a directory");
                    else if (terr != "reported")
                        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                    "`" + outPath +
                                        "' not recognized as being in "
                                        "a supported file format.");
                    stepProbeFail = true;
                }
            }
        }
        const std::set<std::string> &excluded = stepExcludedArgs(vec);
        bool missingReq = false;
        for (const auto &a : cs->args)
            if (a.required && !sftCastStep[si] &&
                (!excluded.count(a.name) ||
                 ((st.name == "create" || st.name == "update") &&
                  a.name == "output")) &&
                a.name != "input" && !st.v.count(a.name))
            {
                // a required member of a mutually exclusive group is
                // satisfied by any set member
                if (!a.mutex.empty())
                {
                    bool groupSet = false;
                    for (const auto &b : cs->args)
                        if (b.mutex == a.mutex && st.v.count(b.name))
                            groupSet = true;
                    if (groupSet)
                        continue;
                }
                // fixed-count positionals fail at the parse layer with the
                // positional wording; variable-count ones fall through to
                // the required-argument check (raster select's BAND list
                // keeps the positional wording)
                if (a.positional >= 0 &&
                    (!a.isList() ||
                     (!vec && st.name == "select" && a.name == "band")))
                {
                    std::string mv = a.metavar;
                    if (mv.size() >= 2 && mv.front() == '<' &&
                        mv.back() == '>')
                        mv = mv.substr(1, mv.size() - 2);
                    if (mv.empty())
                    {
                        mv = strToUpper(a.name);
                    }
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                st.name + ": Positional arguments "
                                          "starting at '" +
                                    mv + "' have not been specified.");
                }
                else
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                st.name + ": Required argument '" +
                                    a.name + "' has not been specified.");
                missingReq = true;
            }
        if (missingReq)
            return pipeFail(c);
    }
    if (stepProbeFail)
        return pipeFail(c);

    // reject pipelines using steps this build cannot execute
    for (const auto &st : steps)
    {
        const std::string &n = st.name;
        if (n != "read" && n != "write" && n != "info" && n != "concat" &&
            !st.trans && !st.transRas &&
            !(((c.type == P_RASTER && !st.sideVec) || st.sideRas) &&
              rasterTailStepKnown(n)) &&
            !(((c.type == P_VECTOR && !st.sideRas) || st.sideVec) &&
              (n == "reproject" || n == "export-schema" ||
               n == "create" || n == "update" ||
               vectorVerbStepName(n) || geosWallStepName(n))))
            return pipeFailMsg(c, CPLE_AppDefined,
                               "pipeline: step '" + n +
                                   "' is not supported by this build");
    }

    // a non-terminal info step fails when the next step pulls its output
    for (size_t i = 0; i + 1 < steps.size(); ++i)
        if (steps[i].name == "info" || steps[i].name == "export-schema")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("pipeline: Step nr %zu (%s) failed to "
                                  "produce an output dataset",
                                  i, steps[i].name.c_str()));
            return 1;
        }

    std::string concatEcho;
    std::unique_ptr<OgrDataset> concatDs;
    bool concatGdalgEmpty = false;
    if (read.name == "concat")
    {
        bool stepGdalg = false;
        if (steps.back().name == "write")
        {
            std::string tof = steps.back().str("of");
            std::string tout = strToLower(steps.back().str("output"));
            stepGdalg = strEqualNoCase(tof, "GDALG") ||
                        (tof.empty() &&
                         strEndsWith(tout, ".gdalg.json"));
        }
        for (const char *crsArg : {"src-crs", "dst-crs"})
            if (read.has(crsArg))
            {
                bool ok = false;
                Srs::fromCliInput(read.str(crsArg), ok);
                if (!ok)
                    return pipeFailMsg(c, CPLE_AppDefined,
                                       std::string("concat: Invalid "
                                                   "value for '") +
                                           crsArg + "' argument");
            }
        // the layer-name/mode conflict is a run-phase check: a GDALG
        // terminal serializes without ever tripping it
        if (!stepGdalg && read.has("output-layer") &&
            (!read.has("mode") ||
             read.str("mode") == "merge-per-layer-name"))
        {
            if (!c.quiet)
            {
                TermProgress tp;
                tp.update(1.0);
            }
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "concat: 'layer-name' name argument cannot be "
                        "specified in mode=merge-per-layer-name");
            return 1;
        }
        const auto inputs = read.list("input");
        // the GDALG terminal only serializes: inputs are never opened
        concatEcho = buildGdalgReadCli(c, read);
        if (stepGdalg)
        {
            concatDs = std::make_unique<OgrDataset>();
            concatGdalgEmpty = true;
            read.name = "read";
            read.v["input"] = {""};
        }
        else
        {
        std::vector<std::unique_ptr<OgrDataset>> dss;
        for (const auto &in : inputs)
        {
            bool missing = !fileExistsP(in);
            cplPushQuietHandler();
            std::string err;
            auto ds = openVectorDataset(in, err,
                                        read.list("input-format"),
                                        read.list("open-option"));
            cplPopHandler();
            if (!ds)
            {
                if (!c.quiet)
                {
                    TermProgress tp;
                    tp.update(1.0);
                }
                if (missing || err == "missing")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(in));
                else
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + in +
                                    "' not recognized as being in a "
                                    "supported file format.");
                return 1;
            }
            dss.push_back(std::move(ds));
        }
        bool plainSingle =
            inputs.size() == 1 && !read.has("mode") &&
            !read.has("output-layer") && !read.has("src-crs") &&
            !read.has("dst-crs") && !read.has("field-strategy") &&
            !read.has("source-layer-field-name") &&
            !read.has("source-layer-field-content") &&
            read.list("input-layer").empty();
        if (plainSingle)
        {
            // a single-input concat with default options is
            // byte-identical to a plain read (verified against the
            // reference), except that the info step sees a nameless
            // in-memory dataset
            g_pipelineConcatInfo = true;
            read.name = "read";
            read.v["input"] = {inputs[0]};
        }
        else
        {
            VectorConcatOpts co;
            co.mode = read.str("mode") == "merge-per-layer-name"
                          ? ""
                          : read.str("mode");
            co.outputLayer = read.str("output-layer");
            co.slfName = read.str("source-layer-field-name");
            co.slfContent = read.str("source-layer-field-content");
            co.fieldStrategy = read.str("field-strategy");
            co.srcCrs = read.str("src-crs");
            co.dstCrs = read.str("dst-crs");
            co.srsWarnings = steps.back().name != "export-schema";
            concatDs = std::make_unique<OgrDataset>();
            concatDs->path = dss[0]->path;
            concatDs->driverShort = dss[0]->driverShort;
            concatDs->driverLong = dss[0]->driverLong;
            if (vectorConcatBuildUnion(inputs, dss,
                                       read.list("input-layer"), co,
                                       *concatDs))
            {
                bool termQ = c.quiet || steps.back().has("quiet");
                if (steps.back().name == "write" && !termQ &&
                    !(steps.back().str("output") == "/vsistdout/" &&
                      !g_dashStdout))
                {
                    TermProgress tp;
                    tp.update(1.0);
                }
                return 1;
            }
            g_pipelineConcatInfo = true;
            read.name = "read";
            read.v["input"] = {""};
        }
        }
    }

    if (c.alias)
        g_handlerUsageText = "";

    // a vector transform step fed its own input dataset refuses the chain
    // before any terminal step runs
    if (c.type == P_VECTOR)
    {
        size_t k = 0;
        for (size_t i = 1; i < steps.size(); ++i)
        {
            if (steps[i].sideRas)
                continue;
            if (!steps[i].transRas &&
                steps[i].name != "reproject" &&
                steps[i].name != "export-schema" &&
                steps[i].name != "create" &&
                steps[i].name != "update" &&
                !vectorVerbStepName(steps[i].name) &&
                !geosWallStepName(steps[i].name))
                continue;
            ++k;
            if (steps[i].has("input"))
            {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("pipeline: Step nr %zu (%s) does not use "
                              "input dataset from previous step",
                              k, steps[i].name.c_str()));
                return 1;
            }
        }
    }

    // execution: read (+ transforms and optional middle writes) +
    // final write/info; a middle write consumes the pending transforms
    // and hands its written dataset object to the next step. A GDALG
    // terminal only serializes, so middle writes never run; a stream
    // middle write is a transparent no-op.
    std::string chainInput;
    std::unique_ptr<OgrDataset> chainDs;
    std::vector<const StepData *> transforms;
    bool terminalGdalg = false;
    if (steps.back().name == "write")
    {
        std::string tof = steps.back().str("of");
        std::string tout = strToLower(steps.back().str("output"));
        terminalGdalg = strEqualNoCase(tof, "GDALG") ||
                        (tof.empty() && strEndsWith(tout, ".gdalg.json"));
    }
    std::string fullEcho =
        concatEcho.empty() ? buildGdalgReadCli(c, read) : concatEcho;
    for (size_t i = 1; i + 1 < steps.size(); ++i)
    {
        if (steps[i].name == "read" || steps[i].name == "concat")
            continue;
        if (steps[i].name == "write")
            fullEcho += writeStepEcho(steps[i]);
        else if (steps[i].trans || steps[i].transRas)
            fullEcho += transStepEcho(steps[i]);
        else
            fullEcho += (c.type == P_VECTOR && !steps[i].sideRas) ||
                                steps[i].sideVec
                            ? vectorStepEcho(steps[i])
                            : rasterTailStepEcho(steps[i].name,
                                                 steps[i].v);
    }
    {
        size_t transIdx = 0;
        for (size_t i = 1; i + 1 < steps.size(); ++i)
            if (steps[i].trans || steps[i].transRas)
            {
                transIdx = i;
                break;
            }
        if (transIdx)
        {
            std::vector<const StepData *> preT, postT;
            for (size_t i = 1; i < transIdx; ++i)
                preT.push_back(&steps[i]);
            for (size_t i = transIdx + 1; i + 1 < steps.size(); ++i)
                postT.push_back(&steps[i]);
            int trc =
                steps[transIdx].transRas
                    ? runTransRasTerminal(c, read, steps, transIdx, preT,
                                          postT, terminalGdalg, fullEcho,
                                          nullptr)
                    : runTransTerminal(c, read, steps, transIdx, preT,
                                       postT, terminalGdalg, fullEcho);
            if (trc && c.alias && !g_pipelineCommitted)
                return kParseFail;
            if (trc == 0 && g_pipelineDeferredFail)
                trc = 1;
            exit(trc);
        }
    }
    // once the first middle write completes, the reference builds the
    // whole remaining sub-pipeline: later step diagnostics land in the
    // middle of the shared progress line and later transforms consume
    // the exhausted captured stream eagerly
    bool postMid = false;
    int wTotal = steps.back().name == "write" && !terminalGdalg ? 1 : 0;
    for (size_t i = 1; i + 1 < steps.size(); ++i)
        if (steps[i].name == "write" && !terminalGdalg &&
            !strEqualNoCase(steps[i].str("of"), "stream"))
            ++wTotal;
    int wDone = 0;
    bool termQuiet = c.quiet || steps.back().has("quiet");
    std::string termOut = steps.back().str("output");
    bool prefixOk = c.type == P_VECTOR &&
                    steps.back().name == "write" && !terminalGdalg &&
                    !termQuiet &&
                    !(termOut == "/vsistdout/" && !g_dashStdout);
    if (concatDs)
    {
        chainDs = std::move(concatDs);
        // the GDALG serialization keeps transforms accumulated so the
        // streaming-compatibility warnings still fire; a materialized
        // union behaves like a completed middle write
        postMid = !concatGdalgEmpty;
    }
    if (c.type == P_VECTOR && read.name == "create")
    {
        int rc = runCreateStep(c, read, read, "", {}, &chainDs, true,
                               false);
        if (rc)
        {
            if (c.alias && !g_pipelineCommitted)
                return kParseFail;
            exit(rc);
        }
        chainInput = read.str("output");
        postMid = true;
    }
    for (size_t i = 1; i + 1 < steps.size(); ++i)
    {
        if (steps[i].name == "read" || steps[i].name == "concat")
            continue;
        if (c.type == P_VECTOR && steps[i].name == "create")
        {
            int rc = runCreateStep(c, read, steps[i], chainInput,
                                   transforms, &chainDs, false, false);
            if (rc)
            {
                if (c.alias && !g_pipelineCommitted)
                    return kParseFail;
                exit(rc);
            }
            transforms.clear();
            chainInput = steps[i].str("output");
            postMid = true;
            continue;
        }
        if ((c.type == P_RASTER && rasterTailStepKnown(steps[i].name)) ||
            (c.type == P_VECTOR &&
             (steps[i].name == "reproject" ||
              steps[i].name == "update" ||
              vectorVerbStepName(steps[i].name) ||
              geosWallStepName(steps[i].name))))
        {
            if (!postMid || !chainDs)
            {
                transforms.push_back(&steps[i]);
                continue;
            }
            g_pipelineStreamInfo = true;
            int rc = vectorChainApply({&steps[i]}, *chainDs, true);
            if (rc)
            {
                if (prefixOk)
                {
                    TermProgress tp;
                    tp.update(1.0);
                }
                if (c.alias && !g_pipelineCommitted)
                    return kParseFail;
                exit(rc);
            }
            continue;
        }
        g_pipelineHasMidWrite = true;
        if (terminalGdalg ||
            strEqualNoCase(steps[i].str("of"), "stream"))
            continue;
        g_convertCaptureWritten = true;
        g_pipelineFailProgressForce = prefixOk && !steps[i].has("quiet");
        int rc = runWriteStep(c, read, steps[i], chainInput, true,
                              transforms, steps.size(), std::string(),
                              &chainDs);
        g_pipelineFailProgressForce = false;
        g_convertCaptureWritten = false;
        if (rc)
        {
            if (c.alias && !g_pipelineCommitted)
                return kParseFail;
            exit(rc);
        }
        transforms.clear();
        chainDs = std::move(g_convertWrittenDs);
        chainInput = steps[i].str("output");
        ++wDone;
        if (!postMid && prefixOk && wTotal > 0)
        {
            // the first completed write prints its window immediately;
            // the remaining sub-pipeline builds against it, so later
            // diagnostics land right after this prefix
            TermProgress tp;
            tp.update(wDone / (double)wTotal);
            g_termTickFloor = tp.lastTick;
        }
        postMid = true;
    }
    if (wDone >= 2 && prefixOk && wTotal > 0)
    {
        // once the build survives, streaming stops at the last middle
        // write's layer-creation midpoint before the terminal step runs
        TermProgress tp;
        tp.update((wDone - 1 + 0.5) / (double)wTotal);
        g_termTickFloor = tp.lastTick;
    }
    // a GDALG terminal only serializes; steps that must materialize the
    // whole dataset warn about the deferred cost
    if (c.type == P_VECTOR && terminalGdalg)
        for (const StepData *t : transforms)
        {
            if (t->name == "filter" && t->has("update-extent"))
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "pipeline: Step filter is not natively "
                            "streaming compatible, and may cause "
                            "significant processing time at opening");
            if (t->name == "update")
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "pipeline: Step update is not natively "
                            "streaming compatible, and may cause "
                            "significant processing time at opening");
            if (t->name == "check-coverage" ||
                t->name == "clean-coverage" ||
                t->name == "simplify-coverage")
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "pipeline: Step " + t->name +
                                " is not natively streaming "
                                "compatible, and may cause significant "
                                "processing time at opening");
        }
    int rc;
    if (steps.back().name == "info")
        rc = runInfoStep(c, read, steps.back(), chainInput, transforms,
                         &chainDs);
    else if (steps.back().name == "export-schema")
        rc = runExportSchemaStep(c, read, steps.back(), chainInput,
                                 transforms, &chainDs);
    else if (c.type == P_VECTOR && steps.back().name == "create")
        rc = runCreateStep(c, read, steps.back(), chainInput, transforms,
                           &chainDs, false, true);
    else
        rc = runWriteStep(c, read, steps.back(), chainInput,
                          steps.size() > 2 && steps.back().name != "write"
                              ? true
                              : false,
                          transforms, steps.size(), fullEcho, &chainDs);
    if (rc && c.alias && !g_pipelineCommitted)
        return kParseFail;
    if (rc == 0 && g_pipelineDeferredFail)
        rc = 1;
    exit(rc);
}

}  // namespace

void pipelineLeaf(const CmdSpec &cmd, const std::string &usageFail,
                  const std::string &invokedCli,
                  const std::vector<std::string> &tokens)
{
    PipeCtx c;
    c.type = cmd.id == "raster_pipeline"
                 ? P_RASTER
                 : cmd.id == "vector_pipeline" ? P_VECTOR : P_GENERIC;
    c.usage = usageFail;
    c.invokedCli = invokedCli;
    g_handlerUsageText = usageFail;
    g_handlerInvokedCli = invokedCli;
    int rc = pipelineCore(c, tokens);
    if (rc == kHelpPassthrough)
        return;
    exit(rc == kParseFail ? 1 : rc);
}

namespace
{

// `vector concat`: single input with default options is byte-identical
// to `vector convert`; everything else rides the union engine feeding
// the convert delegate
int vectorConcatHandler(const CmdSpec &, ParseResult &r)
{
    auto inputs = r.list("input");
    std::string mode = r.str("mode");
    std::string outputLayer = r.str("output-layer");
    bool quiet = r.flag("quiet");
    bool defaultMode = mode.empty() || mode == "merge-per-layer-name";
    std::string output = r.str("output");
    std::string format = r.str("output-format");
    if (strEqualNoCase(format, "Memory"))
        memoryDriverDeprecationWarnOnce();

    bool formatOk =
        format.empty() || strEqualNoCase(format, "GeoJSON") ||
        strEqualNoCase(format, "ESRI Shapefile") ||
        strEqualNoCase(format, "MEM") || strEqualNoCase(format, "Memory") ||
        strEqualNoCase(format, "GDALG") || strEqualNoCase(format, "stream");
    bool ifOk = true;
    for (const auto &d : r.list("input-format"))
        if (!inputFormatCapError(true, d).empty())
            ifOk = false;

    bool overwriteFamily = r.flag("overwrite") || r.flag("append") ||
                           r.flag("update") || r.flag("overwrite-layer") ||
                           r.flag("upsert");
    bool memOut = strEqualNoCase(format, "MEM") ||
                  strEqualNoCase(format, "Memory") ||
                  strEqualNoCase(format, "stream");
    bool streamOut = strEqualNoCase(format, "stream");
    bool gdalgOut = strEqualNoCase(format, "GDALG") ||
                    strEndsWith(strToLower(output), ".gdalg.json");

    // GDALG only serializes: inputs never open and the run-phase
    // layer-name/mode conflict never trips
    if (gdalgOut && formatOk && ifOk)
    {
        std::string cli = handlerInvokedCli();
        if (!r.list("input-format").empty())
            cli += " --input-format " + joinComma(r.list("input-format"));
        for (const auto &v : r.list("open-option"))
            cli += " --open-option " + stepGq(v);
        for (const auto &v : inputs)
            cli += " --input " + stepGq(v);
        if (!r.list("input-layer").empty())
            cli += " --input-layer " +
                   stepGq(joinComma(r.list("input-layer")));
        if (quiet || output == "/vsistdout/")
            cli += " --quiet";
        for (const auto &v : r.list("output-open-option"))
            cli += " --output-open-option " + stepGq(v);
        for (const auto &v : r.list("creation-option"))
            cli += " --creation-option " + stepGq(v);
        for (const auto &v : r.list("layer-creation-option"))
            cli += " --layer-creation-option " + stepGq(v);
        // overwrite and update never echo: they only govern local
        // output existence, not the streamed re-execution
        if (r.flag("overwrite-layer"))
            cli += " --overwrite-layer";
        if (r.flag("append"))
            cli += " --append";
        if (r.flag("upsert"))
            cli += " --upsert";
        if (r.flag("skip-errors"))
            cli += " --skip-errors";
        if (!mode.empty())
            cli += " --mode " + mode;
        if (!outputLayer.empty())
            cli += " --output-layer " + stepGq(outputLayer);
        if (!r.str("source-layer-field-name").empty())
            cli += " --source-layer-field-name " +
                   stepGq(r.str("source-layer-field-name"));
        if (!r.str("source-layer-field-content").empty())
            cli += " --source-layer-field-content " +
                   stepGq(r.str("source-layer-field-content"));
        if (!r.str("field-strategy").empty())
            cli += " --field-strategy " + r.str("field-strategy");
        if (!r.str("src-crs").empty())
            cli += " --src-crs " + stepGq(r.str("src-crs"));
        if (!r.str("dst-crs").empty())
            cli += " --dst-crs " + stepGq(r.str("dst-crs"));
        cli += " --output-format stream --output streamed_dataset";
        JVal j;
        j.type = JVal::OBJECT;
        auto addStr = [&](const char *k, const std::string &v) {
            JVal s2;
            s2.type = JVal::STRING;
            s2.s = v;
            j.obj.emplace_back(k, std::move(s2));
        };
        addStr("type", "gdal_streamed_alg");
        addStr("command_line", cli);
        addStr("gdal_version", "3130000");
        std::string content = jsoncSerialize(j, true);
        if (!r.flag("overwrite"))
        {
            bool exists = fileExistsP(output);
            if (r.flag("append"))
            {
                if (exists)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "File '" + output +
                                    "' already exists. Specify the "
                                    "--overwrite option to overwrite "
                                    "it.");
                    return 1;
                }
            }
            else if (r.flag("update") || r.flag("overwrite-layer") ||
                     r.flag("upsert"))
            {
                // a gdalg target cannot be opened for update, so the
                // update family degrades to open-failure choreography
                if (!exists)
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(output));
                else
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + output +
                                    "' not recognized as being in a "
                                    "supported file format.");
                handlerPrintUsage();
                return 1;
            }
            else if (exists)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "concat: File '" + output +
                                "' already exists. You may specify the "
                                "--overwrite/--overwrite-layer/--append/"
                                "--update option.");
                handlerPrintUsage();
                return 1;
            }
        }
        writeStringToFile(output, content);
        return 0;
    }

    if (formatOk && ifOk && defaultMode && !outputLayer.empty())
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "concat: 'layer-name' name argument cannot be "
                    "specified in mode=merge-per-layer-name");
        return 1;
    }
    bool engineNeeded =
        formatOk && ifOk &&
        (inputs.size() >= 2 || mode == "stack" || mode == "single" ||
         !r.str("source-layer-field-name").empty() ||
         !r.str("source-layer-field-content").empty() ||
         !r.str("field-strategy").empty() || !r.str("dst-crs").empty() ||
         (r.flag("overwrite-layer") && r.flag("append")) || streamOut);
    if (!engineNeeded && inputs.size() == 1 && formatOk && ifOk &&
        !gdalgOut)
    {
        if ((r.flag("update") || r.flag("overwrite-layer") ||
             r.flag("upsert")) &&
            !r.flag("append") && !fileExistsP(output))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(output));
            handlerPrintUsage();
            return 1;
        }
        bool outBlocked = fileExistsP(output) && !overwriteFamily;
        if (!outBlocked)
        {
            cplPushQuietHandler();
            std::string err;
            auto inDs = openVectorDataset(inputs[0], err,
                                          r.list("input-format"),
                                          r.list("open-option"));
            cplPopHandler();
            if (inDs)
            {
                std::string inLayer =
                    inDs->layers.empty() ? "" : inDs->layers[0].name;
                auto lsel = r.list("input-layer");
                bool zeroMatch = !lsel.empty();
                for (const auto &l : lsel)
                    for (const auto &ly : inDs->layers)
                        if (ly.name == l)
                            zeroMatch = false;
                if (zeroMatch)
                {
                    std::string ext;
                    size_t dot = output.find_last_of('.');
                    if (dot != std::string::npos)
                        ext = strToLower(output.substr(dot + 1));
                    bool geoOut = strEqualNoCase(format, "GeoJSON") ||
                                  (format.empty() &&
                                   (ext == "json" || ext == "geojson"));
                    if (geoOut)
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "GeoJSON driver does not support "
                                    "multiple layers.");
                    else if (strEqualNoCase(format, "ESRI Shapefile") ||
                             (format.empty() &&
                              (ext == "shp" || ext == "dbf")))
                        mkdir(output.c_str(), 0755);
                    return 0;
                }
                if (r.flag("update") && !r.flag("append") &&
                    !r.flag("overwrite-layer") && !inLayer.empty())
                {
                    cplPushQuietHandler();
                    std::string e2;
                    auto outDs = openVectorDataset(output, e2, {});
                    cplPopHandler();
                    if (outDs && !outDs->layers.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "Layer " + outDs->layers[0].name +
                                        " already exists, and --append "
                                        "not specified. Consider using "
                                        "--append, or --overwrite-layer.");
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "Failed to write layer '" + inLayer +
                                        "'. Use --skip-errors to ignore "
                                        "errors and continue writing.");
                        return 0;
                    }
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "concat: this command is not implemented "
                                "in this build");
                    return 1;
                }
            }
        }
    }
    if (format.empty() && !strEndsWith(strToLower(output), ".gdalg.json"))
    {
        std::string ext;
        size_t dot = output.find_last_of('.');
        if (dot != std::string::npos)
            ext = strToLower(output.substr(dot + 1));
        if (ext != "json" && ext != "geojson" && ext != "shp" &&
            ext != "dbf")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot guess driver for " + output);
            return 0;
        }
    }
    if (engineNeeded)
    {
        if ((r.flag("update") || r.flag("overwrite-layer") ||
             r.flag("upsert")) &&
            !r.flag("append") && !fileExistsP(output))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(output));
            handlerPrintUsage();
            return 1;
        }
        std::string ext;
        size_t dot = output.find_last_of('.');
        if (dot != std::string::npos)
            ext = strToLower(output.substr(dot + 1));
        bool geoOut = strEqualNoCase(format, "GeoJSON") ||
                      (format.empty() &&
                       (ext == "json" || ext == "geojson"));
        if (!memOut && fileExistsP(output) && !overwriteFamily)
        {
            bool isDs = false;
            {
                cplPushQuietHandler();
                std::string e2;
                auto d2 = openVectorDataset(output, e2, {});
                cplPopHandler();
                isDs = d2 != nullptr;
            }
            if (!isDs)
                isDs = datasetIdentify(output, {"raster"});
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        std::string("concat: ") +
                            (isDs ? "Dataset" : "File") + " '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--overwrite-layer/--append/"
                            "--update option.");
            handlerPrintUsage();
            return 1;
        }
        std::vector<std::unique_ptr<OgrDataset>> dss;
        for (const auto &in : inputs)
        {
            std::string err;
            auto ds = openVectorDataset(in, err, r.list("input-format"),
                                        r.list("open-option"));
            if (!ds)
            {
                if (err == "missing")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(in));
                else if (err != "reported")
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                "`" + in +
                                    "' not recognized as being in a "
                                    "supported file format.");
                return 1;
            }
            dss.push_back(std::move(ds));
        }
        if (!streamOut && r.flag("overwrite-layer") && r.flag("append"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Argument '-overwrite' not allowed with "
                        "'-append'");
            return 0;
        }
        auto lsel = r.list("input-layer");
        VectorConcatOpts co;
        co.mode = mode == "merge-per-layer-name" ? "" : mode;
        co.outputLayer = outputLayer;
        co.slfName = r.str("source-layer-field-name");
        co.slfContent = r.str("source-layer-field-content");
        co.fieldStrategy = r.str("field-strategy");
        co.srcCrs = r.str("src-crs");
        co.dstCrs = r.str("dst-crs");
        co.srsWarnings = !memOut;
        co.typeWarnings = !memOut;
        auto names = vectorConcatGroupNames(inputs, dss, lsel, co);
        if (names.empty())
        {
            if (geoOut)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "GeoJSON driver does not support multiple "
                            "layers.");
            else if (strEqualNoCase(format, "ESRI Shapefile") ||
                     (format.empty() && (ext == "shp" || ext == "dbf")))
                mkdir(output.c_str(), 0755);
            return 0;
        }
        if (names.size() > 1 && geoOut)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "GeoJSON driver does not support multiple "
                        "layers.");
            return 0;
        }
        auto mergedDs = std::make_unique<OgrDataset>();
        mergedDs->path = dss[0]->path;
        mergedDs->driverShort = dss[0]->driverShort;
        mergedDs->driverLong = dss[0]->driverLong;
        if (vectorConcatBuildUnion(inputs, dss, lsel, co, *mergedDs))
            return 1;
        if (streamOut)
            return 0;
        if (r.flag("update") && !r.flag("append") &&
            !r.flag("overwrite-layer") && !r.flag("upsert"))
        {
            cplPushQuietHandler();
            std::string e2;
            auto outDs = openVectorDataset(output, e2, {});
            cplPopHandler();
            if (outDs && !outDs->layers.empty())
            {
                bool anyClash = false;
                for (const auto &ul : mergedDs->layers)
                {
                    bool found = false;
                    for (const auto &ol : outDs->layers)
                        if (ol.name == ul.name)
                            found = true;
                    if (!found)
                        continue;
                    anyClash = true;
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Layer " + ul.name +
                                    " already exists, and --append "
                                    "not specified. Consider using "
                                    "--append, or --overwrite-layer.");
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Failed to write layer '" + ul.name +
                                    "'. Use --skip-errors to ignore "
                                    "errors and continue writing.");
                }
                if (anyClash)
                    return 0;
            }
        }
        const Spec &spec2 = Spec::instance();
        const CmdSpec *cs2 = spec2.findById("vector_convert");
        Handler h2 = findHandler("vector_convert");
        if (!cs2 || !h2)
            return 1;
        ParseResult cr2;
        initResult(*cs2, cr2);
        setArg(*cs2, cr2, "input", {inputs[0]});
        setArg(*cs2, cr2, "output", {output});
        if (!format.empty())
            setArg(*cs2, cr2, "of", {format});
        if (!r.list("creation-option").empty())
            setArg(*cs2, cr2, "co", r.list("creation-option"));
        if (!r.list("layer-creation-option").empty())
            setArg(*cs2, cr2, "lco", r.list("layer-creation-option"));
        for (const char *fl : {"overwrite", "append", "update",
                               "overwrite-layer", "upsert",
                               "skip-errors"})
        {
            // once append is requested the write is a plain append;
            // overwrite/update must not reach the writer or its own
            // exclusions and update gates would fire
            if (r.flag("append") &&
                (!strcmp(fl, "overwrite") || !strcmp(fl, "update")))
                continue;
            if (r.flag(fl))
                setArg(*cs2, cr2, fl, {"true"});
        }
        if (quiet)
            setArg(*cs2, cr2, "quiet", {"true"});
        g_convertSourceOverride = std::move(mergedDs);
        g_pipelineStepPrefix = "concat";
        int rc2 = h2(*cs2, cr2);
        g_pipelineStepPrefix.clear();
        g_convertSourceOverride.reset();
        if (rc2 && g_convertLayerWriteFailed)
            rc2 = 0;
        return rc2;
    }
    const Spec &spec = Spec::instance();
    const CmdSpec *cs = spec.findById("vector_convert");
    Handler h = findHandler("vector_convert");
    if (!cs || !h)
        return 1;
    ParseResult cr;
    initResult(*cs, cr);
    setArg(*cs, cr, "input", {inputs[0]});
    setArg(*cs, cr, "output", {output});
    if (!format.empty())
        setArg(*cs, cr, "of", {format});
    if (!r.list("input-format").empty())
        setArg(*cs, cr, "if", r.list("input-format"));
    if (!r.list("open-option").empty())
        setArg(*cs, cr, "oo", r.list("open-option"));
    if (!r.list("input-layer").empty())
        setArg(*cs, cr, "layer", r.list("input-layer"));
    if (!r.list("creation-option").empty())
        setArg(*cs, cr, "co", r.list("creation-option"));
    if (!r.list("layer-creation-option").empty())
        setArg(*cs, cr, "lco", r.list("layer-creation-option"));
    for (const char *fl : {"overwrite", "append", "update",
                           "overwrite-layer", "upsert", "skip-errors"})
    {
        if (r.flag("append") &&
            (!strcmp(fl, "overwrite") || !strcmp(fl, "update")))
            continue;
        if (r.flag(fl))
            setArg(*cs, cr, fl, {"true"});
    }
    if (quiet)
        setArg(*cs, cr, "quiet", {"true"});
    g_pipelineStepPrefix = "concat";
    int rc = h(*cs, cr);
    g_pipelineStepPrefix.clear();
    if (rc && g_convertLayerWriteFailed)
        rc = 0;
    return rc;
}

}  // namespace

void registerPipelineHandlers()
{
    registerHandler("vector_concat", vectorConcatHandler);
    registerArgValueCheck(
        "vector_concat",
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

bool jsoncTokenerError(const std::string &content, std::string &desc,
                       size_t &off)
{
    MiniJson mj(content);
    MVal root;
    if (mj.value(root))
        return false;
    desc = mj.err.empty() ? "unexpected character" : mj.err;
    off = mj.err.empty() ? mj.i : mj.errOff;
    return true;
}

bool pipelineAliasRun(const std::vector<std::string> &args)
{
    const Spec &spec = Spec::instance();
    const CmdSpec *cmd = spec.findById("pipeline");
    if (!cmd)
        return false;
    PipeCtx c;
    c.type = P_GENERIC;
    c.alias = true;
    c.invokedCli = "gdal pipeline";
    c.usage = "";
    g_handlerUsageText = "";
    g_handlerInvokedCli = "gdal pipeline";
    std::vector<std::string> tokens;
    tokens.push_back("read");
    for (const auto &a : args)
        tokens.push_back(a);
    int rc = pipelineCore(c, std::move(tokens));
    if (rc == kParseFail || rc == kHelpPassthrough)
        return false;
    exit(rc);
}
