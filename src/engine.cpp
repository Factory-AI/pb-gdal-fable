#include <sys/wait.h>
#include <unistd.h>

#include "engine.h"
#include <set>
#include "complete.h"
#include "cpl.h"
#include "dataset.h"
#include "embedded.h"
#include "ogr.h"
#include "util.h"
#include "vsi.h"

#include <sys/stat.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::map<std::string, Handler> &preValidatorMap()
{
    static std::map<std::string, Handler> m;
    return m;
}

void registerPreValidator(const std::string &id, Handler h)
{
    preValidatorMap()[id] = std::move(h);
}

static std::map<std::string, ArgCheck> &argCheckMap()
{
    static std::map<std::string, ArgCheck> m;
    return m;
}

void registerArgCheck(const std::string &id, ArgCheck f)
{
    argCheckMap()[id] = std::move(f);
}

static std::map<std::string, ArgValueCheck> &argValueCheckMap()
{
    static std::map<std::string, ArgValueCheck> m;
    return m;
}

void registerArgValueCheck(const std::string &id, ArgValueCheck f)
{
    argValueCheckMap()[id] = std::move(f);
}

static std::map<std::string, PostValidator> &postValidatorMap()
{
    static std::map<std::string, PostValidator> m;
    return m;
}

void registerPostValidator(const std::string &id, PostValidator f)
{
    postValidatorMap()[id] = std::move(f);
}

static std::map<std::string, Handler> &handlerMap()
{
    static std::map<std::string, Handler> m;
    return m;
}

void registerHandler(const std::string &id, Handler h)
{
    handlerMap()[id] = h;
}

bool g_infoAttempt = false;
int g_infoAttemptFd = -1;

Handler findHandler(const std::string &id)
{
    auto it = handlerMap().find(id);
    return it == handlerMap().end() ? Handler() : it->second;
}

bool g_pipelineMode = false;
bool g_dashStdout = false;
bool g_pipelineConcatInfo = false;
bool g_pipelineStreamInfo = false;
bool g_pipelineInfoDriverKeep = false;
bool g_pipelineReadLayerFilter = false;
std::vector<std::string> g_pipelineInfoStepLayers;
bool g_pipelineHasMidWrite = false;

bool geosStubVectorCmd(const std::string &id)
{
    return id == "vector_buffer" || id == "vector_simplify" ||
           id == "vector_convex-hull" || id == "vector_concave-hull" ||
           id == "vector_make-valid" || id == "vector_dissolve" ||
           id == "vector_check-coverage" || id == "vector_clean-coverage" ||
           id == "vector_simplify-coverage" ||
           id == "vector_check-geometry" || id == "vector_layer-algebra";
}

bool convertLikeOutputCmd(const std::string &id)
{
    return id == "raster_convert" || id == "vector_convert" ||
           id == "raster_select" || id == "raster_calc" ||
           id == "raster_reclassify" || id == "raster_set-type" ||
           id == "raster_scale" || id == "raster_unscale" ||
           id == "raster_clip" || id == "vector_sql" ||
           id == "vector_clip" || id == "vector_combine" ||
           id == "raster_proximity" || id == "raster_sieve" ||
           id == "raster_fill-nodata" || id == "raster_neighbors" ||
           id == "raster_color-map" || id == "raster_nodata-to-alpha" ||
           id == "raster_rgb-to-palette" || id == "vector_index" ||
           id == "raster_index" ||
           id == "mdim_convert" || id == "mdim_mosaic" ||
           id == "raster_clean-collar" ||
           strStartsWith(id, "vector_grid_") || geosStubVectorCmd(id);
}

namespace
{
struct DrvCaps
{
    const char *name;
    bool ras;
    bool vec;
};
const DrvCaps kDrvCaps[] = {
    {"GTiff", true, false},          {"COG", true, false},
    {"VRT", true, false},            {"MEM", true, true},
    {"GNMFile", false, false},       {"GNMDatabase", false, false},
    {"ESRI Shapefile", false, true}, {"GeoJSON", false, true},
    {"GeoJSONSeq", false, true},     {"ESRIJSON", false, true},
    {"TopoJSON", false, true}};
}  // namespace

bool knownDriverCaps(const std::string &drv, bool &ras, bool &vec)
{
    for (const auto &d : kDrvCaps)
        if (strEqualNoCase(drv, d.name))
        {
            ras = d.ras;
            vec = d.vec;
            return true;
        }
    return false;
}

void memoryDriverDeprecationWarnOnce()
{
    static bool warned = false;
    if (warned)
        return;
    warned = true;
    cplErrorStr(CE_Warning, CPLE_AppDefined,
                "DeprecationWarning: 'Memory' driver is deprecated since "
                "GDAL 3.11. Use 'MEM' onwards. Further messages of this "
                "type will be suppressed.");
}

std::string inputFormatCapError(bool wantVector, const std::string &drv)
{
    if (strEqualNoCase(drv, "Memory"))
    {
        memoryDriverDeprecationWarnOnce();
        return "";
    }
    const DrvCaps *found = nullptr;
    for (const auto &d : kDrvCaps)
        if (strEqualNoCase(drv, d.name))
            found = &d;
    if (!found)
        return "Invalid value for argument 'input-format'. Driver '" + drv +
               "' does not exist.";
    if (wantVector && !found->vec)
        return "Invalid value for argument 'input-format'. Driver '" + drv +
               "' does not expose the required 'DCAP_VECTOR' capability.";
    if (!wantVector && !found->ras)
        return "Invalid value for argument 'input-format'. Driver '" + drv +
               "' does not expose the required 'DCAP_RASTER' capability.";
    return "";
}
bool g_pipelineCommitted = false;
bool g_pipelineFailProgressForce = false;
bool g_pipelineBarStdout = false;
bool g_pipelineWriteBarAtEnd = false;
bool g_pipelineDeferredFail = false;
std::string g_pipelineGdalgCli;
std::string g_pipelineStepPrefix;
std::function<int(std::unique_ptr<RasterDatasetBase> &)>
    g_pipelineTailMaterialize;
int g_pipelineTotalSteps = 0;
std::string g_pipelineDemVrtVerb;

std::string suggestOptionName(const std::string &name,
                              const std::vector<std::string> &candidates)
{
    if (name.size() < 3)
        return "";
    std::set<std::string> sorted(candidates.begin(), candidates.end());
    std::string best;
    size_t bestDist = std::string::npos;
    for (const auto &c : sorted)
    {
        size_t dist = osaDistance(name, c);
        if (dist < bestDist)
        {
            best = c;
            bestDist = dist;
        }
        else if (dist == bestDist)
            best.clear();
    }
    if (!best.empty() && bestDist <= (best.size() >= 5 ? 2u : 1u))
        return best;
    return "";
}

std::string convertMsgPrefix()
{
    return g_pipelineStepPrefix.empty() ? "convert" : g_pipelineStepPrefix;
}

namespace
{

[[maybe_unused]] const char *kTmsWarning =
    "Cannot find tms_NZTM2000.json (GDAL_DATA is not defined)";
const char *kTmsWarningLine =
    "Warning 3: Cannot find tms_NZTM2000.json (GDAL_DATA is not defined)\n";
const char *kTmsBadFileLine = "ERROR 1: Expected type = TileMatrixSetType\n";

void emitOut(const std::string &s)
{
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

void emitErr(const std::string &s)
{
    fwrite(s.data(), 1, s.size(), stderr);
    fflush(stderr);
}

bool fileExists(const std::string &path)
{
    if (vsiIsVirtual(path))
        return vsiExists(path);
    struct stat sb;
    return stat(path.c_str(), &sb) == 0;
}

bool fileReadable(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f)
        fclose(f);
    return f != nullptr;
}

// GDAL data file finder: GDAL_DATA config first, then current directory.
std::string findDataFile(const std::string &basename)
{
    if (configIsSet("GDAL_DATA"))
    {
        std::string p = configGet("GDAL_DATA") + "/" + basename;
        if (fileReadable(p))
            return p;
    }
    std::string p = "./" + basename;
    if (fileReadable(p))
        return p;
    return std::string();
}

// Diagnostics emitted whenever the driver list / raster tile machinery is
// instantiated: complaints about the tms_NZTM2000.json data file.
}  // namespace
std::string tmsDiagnosticLine()
{
    std::string found = findDataFile("tms_NZTM2000.json");
    if (!found.empty())
        return kTmsBadFileLine;  // any found file fails TileMatrixSetType
    if (configIsSet("GDAL_DATA"))
        return std::string();
    return kTmsWarningLine;
}
namespace
{

// Verbatim .err captures contain the tms warning line as recorded in the
// default environment. Adjust them for the current environment.
std::string adjustErrCapture(const std::string &captured)
{
    if (captured.empty())
        return captured;
    std::string repl = tmsDiagnosticLine();
    if (repl == kTmsWarningLine)
        return captured;
    std::string out;
    size_t start = 0;
    while (start < captured.size())
    {
        size_t nl = captured.find('\n', start);
        if (nl == std::string::npos)
            nl = captured.size() - 1;
        std::string line = captured.substr(start, nl - start + 1);
        if (line == kTmsWarningLine)
            out += repl;
        else
            out += line;
        start = nl + 1;
    }
    return out;
}

struct Invocation
{
    const CmdSpec *cmd = nullptr;
    std::vector<std::string> invokedPath;  // names as typed (aliases kept)

    std::string canonicalCli() const
    {
        std::string s = "gdal";
        for (const auto &p : cmd->path)
            s += " " + p;
        return s;
    }
    std::string invokedCli() const
    {
        std::string s = "gdal";
        for (const auto &p : invokedPath)
            s += " " + p;
        return s;
    }
    std::string algName() const
    {
        return cmd->path.empty() ? "gdal" : cmd->path.back();
    }

    std::string usageOverride;
    // generic `gdal info` forwarding: mutex violations surface as a
    // "positional not expected" error on the input token
    std::string dispatchedPositional;
    bool dispatchedInfo = false;
    // generic `gdal convert` re-dispatch: the injected --input counts
    // as consumed, so positionals skip the input slot entirely
    bool dispatchedConvert = false;

    std::string usageLine() const
    {
        if (!usageOverride.empty())
            return usageOverride;
        std::string line = cmd->usageLine;
        std::string canon = canonicalCli();
        std::string inv = invokedCli();
        size_t pos = line.find(canon);
        if (pos != std::string::npos && canon != inv)
            line = line.substr(0, pos) + inv + line.substr(pos + canon.size());
        return line;
    }

    std::string helpText() const
    {
        std::string text = cmd->helpText();
        std::string canon = canonicalCli();
        std::string inv = invokedCli();
        if (canon != inv)
        {
            size_t nl = text.find('\n');
            std::string first = text.substr(0, nl);
            size_t pos = first.find(canon);
            if (pos != std::string::npos)
                text = first.substr(0, pos) + inv +
                       first.substr(pos + canon.size()) + text.substr(nl);
        }
        return text;
    }

    std::string jusageText() const
    {
        std::string text = cmd->jusageText();
        if (!cmd->path.empty() && !invokedPath.empty() &&
            cmd->path.back() != invokedPath.back())
        {
            std::string canonLine = "    \"" + cmd->path.back() + "\"\n";
            std::string invLine = "    \"" + invokedPath.back() + "\"\n";
            size_t fp = text.find("\"full_path\":[");
            if (fp != std::string::npos)
            {
                size_t end = text.find(']', fp);
                size_t pos = text.find(canonLine, fp);
                if (pos != std::string::npos && pos < end)
                    text = text.substr(0, pos) + invLine +
                           text.substr(pos + canonLine.size());
            }
        }
        return text;
    }
};

[[noreturn]] void doHelp(const Invocation &inv)
{
    emitErr(adjustErrCapture(inv.cmd->helpErr()));
    emitOut(inv.helpText());
    exit(0);
}

// --help-doc renders the same text minus the trailing "For more
// details" / provisional-interface epilogue
[[noreturn]] void doHelpDoc(const Invocation &inv)
{
    emitErr(adjustErrCapture(inv.cmd->helpErr()));
    std::string text = inv.helpText();
    size_t pos = text.find("\n\nFor more details, consult ");
    if (pos != std::string::npos)
        text = text.substr(0, pos + 1);
    emitOut(text);
    exit(0);
}

[[noreturn]] void doJsonUsage(const Invocation &inv)
{
    emitErr(adjustErrCapture(inv.cmd->jusageErr()));
    emitOut(inv.jusageText());
    exit(0);
}

void printLeafUsageError(const Invocation &inv)
{
    if (g_infoAttempt)
        return;
    emitErr(inv.usageLine() + "\n");
    emitErr("Try '" + inv.invokedCli() + " --help' for help.\n");
}

void printNonLeafUsageError(const CmdSpec &cmd)
{
    emitErr(adjustErrCapture(cmd.errUsage()));
}

std::string metavarOf(const ArgSpec &a)
{
    if (!a.display.empty())
    {
        size_t lt = a.display.find('<');
        size_t gt = a.display.rfind('>');
        if (lt != std::string::npos && gt != std::string::npos && gt > lt)
            return a.display.substr(lt + 1, gt - lt - 1);
    }
    if (!a.metavar.empty())
    {
        std::string m = a.metavar;
        if (m.front() == '<' && m.back() == '>')
            return m.substr(1, m.size() - 2);
        return m;
    }
    return strToUpper(a.name);
}

// ------------------------------------------------------------------
// general command line processing (before algorithm dispatch)
// ------------------------------------------------------------------

const char *kConfigNoKeyValue =
    "--config option given without a key=value argument.";
const char *kConfigNoKeyAndValue =
    "--config option given without a key and value argument.";

// First pass: set config options early. Empirically: aborts the process when
// --config is the last token; on key-without-value it reports once and stops
// scanning, letting the second pass report again and abort.
void earlyConfigScan(const std::vector<std::string> &args)
{
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] != "--config")
            continue;
        if (i + 1 >= args.size())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, kConfigNoKeyValue);
            exit(1);
        }
        const std::string &next = args[i + 1];
        size_t eq = next.find('=');
        if (eq != std::string::npos)
        {
            configSet(next.substr(0, eq), next.substr(eq + 1));
            ++i;
        }
        else if (i + 2 < args.size())
        {
            configSet(next, args[i + 2]);
            i += 2;
        }
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, kConfigNoKeyAndValue);
            return;
        }
    }
}

std::vector<std::string> tokenizeOptfile(const std::string &content)
{
    std::vector<std::string> toks;
    size_t i = 0;
    const size_t n = content.size();
    while (i < n)
    {
        while (i < n && (content[i] == ' ' || content[i] == '\t' ||
                         content[i] == '\r' || content[i] == '\n'))
            ++i;
        if (i >= n)
            break;
        if (content[i] == '#')
        {
            while (i < n && content[i] != '\n')
                ++i;
            continue;
        }
        std::string tok;
        while (i < n && content[i] != ' ' && content[i] != '\t' &&
               content[i] != '\r' && content[i] != '\n')
        {
            if (content[i] == '"' || content[i] == '\'')
            {
                char q = content[i];
                ++i;
                while (i < n && content[i] != q)
                    tok += content[i++];
                if (i < n)
                    ++i;
            }
            else
                tok += content[i++];
        }
        toks.push_back(tok);
    }
    return toks;
}

[[noreturn]] void emulateCrashSegv()
{
    fflush(nullptr);
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
    _exit(139);
}

[[noreturn]] void emulateCrashDoubleFree()
{
    fflush(nullptr);
    emitErr("double free or corruption (fasttop)\n");
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(134);
}

// Second pass: full general option processing; returns remaining args.
std::vector<std::string> generalPass(std::vector<std::string> args)
{
    std::vector<std::string> out;
    bool fromOptfile = false;
    std::vector<std::pair<std::string, bool>> work;
    for (auto &a : args)
        work.emplace_back(a, false);

    for (size_t i = 0; i < work.size(); ++i)
    {
        const std::string &a = work[i].first;
        fromOptfile = work[i].second;
        if (a == "--version")
        {
            if (fromOptfile)
                emulateCrashSegv();
            emitOut(embGet("misc/version.out"));
            exit(0);
        }
        if (a == "--license")
        {
            if (fromOptfile)
                emulateCrashSegv();
            std::string lic = findDataFile("LICENSE.TXT");
            if (!lic.empty())
            {
                FILE *f = fopen(lic.c_str(), "rb");
                std::string content;
                if (f)
                {
                    char buf[65536];
                    size_t r;
                    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
                        content.append(buf, r);
                    fclose(f);
                }
                emitOut(content + "\n");
            }
            else
            {
                emitOut(
                    "GDAL/OGR is released under the MIT license.\n"
                    "The LICENSE.TXT distributed with GDAL/OGR should\n"
                    "contain additional details.\n\n");
            }
            exit(0);
        }
        if (a == "--formats")
        {
            if (fromOptfile)
            {
                emitErr(adjustErrCapture(embGet("misc/formats.err")));
                emulateCrashDoubleFree();
            }
            emitErr(adjustErrCapture(embGet("misc/formats.err")));
            emitOut(embGet("misc/formats.out"));
            exit(0);
        }
        if (a == "--config")
        {
            if (i + 1 >= work.size())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined, kConfigNoKeyValue);
                exit(1);
            }
            const std::string &next = work[i + 1].first;
            if (next.find('=') != std::string::npos)
            {
                ++i;
            }
            else if (i + 2 < work.size())
            {
                i += 2;
            }
            else
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined, kConfigNoKeyAndValue);
                exit(1);
            }
            continue;
        }
        if (a == "--debug")
        {
            if (i + 1 >= work.size())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "--debug option given without debug level.");
                exit(1);
            }
            configSet("CPL_DEBUG", work[i + 1].first);
            ++i;
            continue;
        }
        if (a == "--optfile")
        {
            if (i + 1 >= work.size())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "--optfile option given without filename.");
                exit(1);
            }
            const std::string fname = work[i + 1].first;
            FILE *f = fopen(fname.c_str(), "rb");
            if (!f)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Unable to open optfile '" + fname +
                                "'.\nNo such file or directory");
                exit(1);
            }
            std::string content;
            char buf[65536];
            size_t r;
            while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
                content.append(buf, r);
            fclose(f);
            std::vector<std::string> toks = tokenizeOptfile(content);
            std::vector<std::pair<std::string, bool>> nw(
                work.begin(), work.begin() + i);
            for (auto &t : toks)
                nw.emplace_back(t, true);
            nw.insert(nw.end(), work.begin() + i + 2, work.end());
            work = std::move(nw);
            --i;
            continue;
        }
        out.push_back(a);
    }
    return out;
}

// ------------------------------------------------------------------
// leaf argument parsing
// ------------------------------------------------------------------

// KEY=VALUE list values tokenize on unescaped, unquoted commas with quotes
// and escapes preserved; if any token lacks '=', the whole raw string is a
// single value
static std::vector<std::string> kvListSplit(const std::string &s)
{
    std::vector<std::string> toks;
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size())
        {
            cur += c;
            cur += s[++i];
            continue;
        }
        if (c == '"')
        {
            inStr = !inStr;
            cur += c;
            continue;
        }
        if (c == ',' && !inStr)
        {
            if (!cur.empty())
                toks.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty())
        toks.push_back(cur);
    bool allEq = !toks.empty();
    for (const auto &t : toks)
    {
        size_t eq = t.find('=');
        if (eq == std::string::npos || t.find('=', eq + 1) != std::string::npos)
            allEq = false;
    }
    if (!allEq)
    {
        toks.clear();
        toks.push_back(s);
    }
    return toks;
}

struct LeafParser
{
    const Invocation &inv;
    const CmdSpec &cmd;
    ParseResult &result;
    bool datasetOpenFailed = false;
    std::map<std::string, int> optCount;
    std::vector<std::string> ignoredConfigValues;
    // positional-count diagnostics wait for the per-argument value checks
    std::string pendingPosErr;
    int pendingPosErrCls = CPLE_AppDefined;
    // excess-positional flavor outranks the list count ceiling check
    bool pendingPosErrExcess = false;
    // update-family output opened during validation; closed (with its
    // debug trace) after the usage block
    std::unique_ptr<OgrDataset> validationTarget;

    LeafParser(const Invocation &invIn, ParseResult &r)
        : inv(invIn), cmd(*invIn.cmd), result(r)
    {
        for (const auto &a : cmd.args)
        {
            ArgValue v;
            v.spec = &a;
            r.byName[a.name] = v;
        }
    }

    [[noreturn]] void failUsage()
    {
        printLeafUsageError(inv);
        if (validationTarget)
        {
            // directory targets only materialize their layers (and any
            // buffered per-layer notes) when the dataset closes
            ogrFlushPendingDebug(*validationTarget);
            vectorDebugClose(*validationTarget);
        }
        exit(1);
    }

    [[noreturn]] void errUsage(int num, const std::string &msg)
    {
        cplErrorStr(CE_Failure, num, inv.algName() + ": " + msg);
        failUsage();
    }

    void setValue(const ArgSpec &a, const std::string &rawValue,
                  const std::string &displayName)
    {
        if (!result.byName[a.name].set)
            result.order.push_back(a.name);
        ArgValue &v = result.byName[a.name];
        // packed fixed-count lists (bbox, window) reject repetition just
        // like scalars; open-ended packed lists (gcp) accumulate
        if (v.set && (!a.isList() ||
                      (a.packed && a.minCount >= 0 &&
                       a.minCount == a.maxCount)))
        {
            std::string disp = displayName;
            if (disp.rfind("--", 0) != 0 && !disp.empty() && disp[0] == '-')
                disp = disp.substr(1);
            errUsage(CPLE_IllegalArg,
                     strPrintf("Argument '%s' has already been specified.",
                               disp.c_str()));
        }
        std::vector<std::string> vals;
        bool packedList = a.isList() && a.packed;
        if (packedList)
            vals = strSplit(rawValue, ',');
        else if (a.type == "string_list" && a.metavar == "<KEY>=<VALUE>" &&
                 optCount[a.name] <= 1)
            vals = kvListSplit(rawValue);
        else
            vals.push_back(rawValue);
        bool rangeBad = false;
        // short options drop their dash in value-parse diagnostics
        // ("argument 's'"), long ones keep both ("--target-values")
        std::string dispName = displayName;
        if (dispName.rfind("--", 0) != 0 && !dispName.empty() &&
            dispName[0] == '-')
            dispName = dispName.substr(1);
        for (const auto &val : vals)
        {
            // packed numeric lists report the whole raw value with the
            // "list of" wording
            const std::string &shown = packedList ? rawValue : val;
            const char *listOf = packedList ? "list of " : "";
            if (a.type == "integer" || a.type == "integer_list")
            {
                char *endp = nullptr;
                errno = 0;
                strtoll(val.c_str(), &endp, 10);
                if (val.empty() || *endp != '\0' || errno == ERANGE)
                {
                    errUsage(CPLE_IllegalArg,
                             strPrintf("Expected %sinteger value for "
                                       "argument '%s', but got '%s'.",
                                       listOf, dispName.c_str(),
                                       shown.c_str()));
                }
            }
            else if (a.type == "real" || a.type == "real_list")
            {
                char *endp = nullptr;
                strtod(val.c_str(), &endp);
                if (val.empty() || *endp != '\0' || numLooksHex(val))
                {
                    errUsage(CPLE_IllegalArg,
                             strPrintf("Expected %sreal value for argument "
                                       "'%s', but got '%s'.",
                                       listOf, dispName.c_str(),
                                       shown.c_str()));
                }
            }
            if (a.metavar == "<KEY>=<VALUE>" &&
                val.find('=') == std::string::npos)
            {
                errUsage(CPLE_AppDefined,
                         strPrintf("Invalid value for argument '%s'. "
                                   "<KEY>=<VALUE> expected",
                                   a.name.c_str()));
            }
            if (!a.isList() && choicesEnforced(a) && !choiceValueOk(a, val))
            {
                emitChoiceError(a, val);
                exit(1);
            }
            if (checkRange(a, val))
                rangeBad = true;
            auto vit = argValueCheckMap().find(cmd.id);
            if (vit != argValueCheckMap().end())
            {
                std::string msg = vit->second(a.name, val);
                if (!msg.empty())
                {
                    // leading \x05 marks an illegal-argument class check
                    if (msg[0] == '\x05')
                        errUsage(CPLE_IllegalArg, msg.substr(1));
                    // \x06: illegal-argument without the verb prefix
                    if (msg[0] == '\x06')
                    {
                        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                    msg.substr(1));
                        failUsage();
                    }
                    // \x07: not-supported class with the verb prefix
                    if (msg[0] == '\x07')
                        errUsage(CPLE_NotSupported, msg.substr(1));
                    errUsage(CPLE_AppDefined, msg);
                }
            }
            v.values.push_back(val);
        }
        if (rangeBad)
            failUsage();
        v.set = true;
        // vector info: setting --limit also marks --features as specified
        if (cmd.id == "vector_info" && a.name == "limit" &&
            !result.byName["features"].set)
        {
            const ArgSpec *f = cmd.findByName("features");
            if (f)
                setValue(*f, "true", "--features");
        }
        // --overwrite-layer also marks --update as specified (the
        // reference's argument action): a later explicit --update trips
        // the duplicate check; the stored value stays "false" so the
        // handlers' update semantics remain keyed to the explicit flag
        if (a.name == "overwrite-layer" && cmd.findByName("update"))
        {
            ArgValue &u = result.byName["update"];
            if (!u.set)
            {
                u.set = true;
                u.values = {"false"};
                result.order.push_back("update");
            }
        }
        // index --append has the same argument action
        if ((cmd.id == "vector_index" || cmd.id == "raster_index") &&
            a.name == "append")
        {
            ArgValue &u = result.byName["update"];
            if (!u.set)
            {
                u.set = true;
                u.values = {"false"};
                result.order.push_back("update");
            }
        }
    }

    bool checkRange(const ArgSpec &a, const std::string &val)
    {
        if (a.type != "integer" && a.type != "integer_list" &&
            a.type != "real" && a.type != "real_list")
            return false;
        double d = strtod(val.c_str(), nullptr);
        auto fmtBound = [](double b)
        {
            return b == static_cast<long long>(b)
                       ? strPrintf("%lld", static_cast<long long>(b))
                       : strPrintf("%g", b);
        };
        bool anyBad = false;
        if (a.hasMin)
        {
            bool bad = a.minIncluded ? d < a.minVal : d <= a.minVal;
            if (bad)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("Value of argument '%s' is %s, but "
                                      "should be %s %s",
                                      a.name.c_str(), val.c_str(),
                                      a.minIncluded ? ">=" : ">",
                                      fmtBound(a.minVal).c_str()));
                anyBad = true;
            }
        }
        if (a.hasMax)
        {
            bool bad = a.maxIncluded ? d > a.maxVal : d >= a.maxVal;
            if (bad)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("Value of argument '%s' is %s, but "
                                      "should be %s %s",
                                      a.name.c_str(), val.c_str(),
                                      a.maxIncluded ? "<=" : "<",
                                      fmtBound(a.maxVal).c_str()));
                anyBad = true;
            }
        }
        return anyBad;
    }

    void parse(std::vector<std::string> tokens)
    {
        std::vector<std::string> positional;
        if (tokens.size() == 1 && tokens[0] == "help")
            doHelp(inv);
        // KEY=VALUE list values only tokenize on commas when the option
        // appears exactly once on the command line
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const std::string &tok = tokens[i];
            const ArgSpec *a = nullptr;
            if (strStartsWith(tok, "--") && tok.size() > 2)
            {
                std::string body = tok.substr(2);
                size_t eq = body.find('=');
                std::string name =
                    eq == std::string::npos ? body : body.substr(0, eq);
                if (name != "config")
                    a = cmd.findLong(name);
                if (a && !a->isBool() && eq == std::string::npos)
                    ++i;
            }
            else if (tok.size() == 2 && tok[0] == '-' &&
                     !(tok[1] >= '0' && tok[1] <= '9') && tok[1] != '.')
            {
                a = cmd.findShort(tok.substr(1));
                if (a && !a->isBool())
                    ++i;
            }
            if (a)
                ++optCount[a->name];
        }
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            const std::string &tok = tokens[i];
            if (tok == "--help" || tok == "-h")
                doHelp(inv);
            if (tok == "--help-doc")
                doHelpDoc(inv);
            if (tok == "--json-usage")
                doJsonUsage(inv);
            if (strStartsWith(tok, "--") && tok.size() > 2)
            {
                std::string body = tok.substr(2);
                size_t eq = body.find('=');
                std::string name =
                    eq == std::string::npos ? body : body.substr(0, eq);
                bool hasValue = eq != std::string::npos;
                std::string inlineValue =
                    hasValue ? body.substr(eq + 1) : std::string();
                if (name == "config")
                {
                    // --config KEY=VALUE (space form) was consumed by the
                    // general pass; only the inline form reaches here, and
                    // its values are ignored with a warning.
                    if (hasValue)
                        ignoredConfigValues.push_back(inlineValue);
                    else if (i + 1 < tokens.size())
                        ignoredConfigValues.push_back(tokens[++i]);
                    continue;
                }
                const ArgSpec *a = cmd.findLong(name);
                if (!a)
                {
                    std::vector<std::string> cands = {"help", "help-doc",
                                                      "json-usage", "config"};
                    for (const auto &ar : cmd.args)
                    {
                        cands.push_back(ar.name);
                        for (const auto &al : ar.aliases)
                            cands.push_back(al);
                        for (const auto &al : ar.hiddenAliases)
                            cands.push_back(al);
                    }
                    std::string sug = suggestOptionName(name, cands);
                    if (!sug.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                    inv.algName() +
                                        strPrintf(": Option '--%s' is "
                                                  "unknown. Do you mean "
                                                  "'--%s'?",
                                                  name.c_str(), sug.c_str()));
                        exit(1);
                    }
                    errUsage(CPLE_IllegalArg,
                             strPrintf("Option '--%s' is unknown.",
                                       name.c_str()));
                }
                std::string displayName = "--" + name;
                if (a->isBool())
                {
                    if (hasValue)
                    {
                        const std::string &lv = inlineValue;
                        if (lv != "true" && lv != "false")
                        {
                            errUsage(
                                CPLE_IllegalArg,
                                strPrintf("Invalid value '%s' for boolean "
                                          "argument '%s'. Should be "
                                          "'true' or 'false'.",
                                          inlineValue.c_str(),
                                          displayName.c_str()));
                        }
                        setValue(*a, lv, displayName);
                    }
                    else
                        setValue(*a, "true", displayName);
                }
                else
                {
                    std::string value;
                    if (hasValue)
                        value = inlineValue;
                    else
                    {
                        if (i + 1 >= tokens.size())
                        {
                            errUsage(CPLE_IllegalArg,
                                     strPrintf("Option '%s' requires a value.",
                                               tok.c_str()));
                        }
                        value = tokens[++i];
                    }
                    setValue(*a, value, displayName);
                }
            }
            else if (tok == "--")
            {
                errUsage(CPLE_IllegalArg, "Option '--' is unknown.");
            }
            else if (tok.size() >= 2 && tok[0] == '-' &&
                     !(tok[1] >= '0' && tok[1] <= '9') && tok[1] != '.')
            {
                std::string chars = tok.substr(1);
                auto shortUnknown = [&](const std::string &sn) {
                    std::string msg =
                        strPrintf("Short name option '%s' is unknown.",
                                  sn.c_str());
                    if (cmd.findLong(chars))
                    {
                        msg += strPrintf(" Do you mean '--%s' (with leading "
                                         "double dash) ?",
                                         chars.c_str());
                        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                    inv.algName() + ": " + msg);
                        exit(1);
                    }
                    errUsage(CPLE_IllegalArg, msg);
                };
                if (chars.size() == 1)
                {
                    const ArgSpec *a = cmd.findShort(chars);
                    if (!a)
                        shortUnknown(chars);
                    std::string displayName = "-" + chars;
                    if (a->isBool())
                        setValue(*a, "true", displayName);
                    else
                    {
                        if (i + 1 >= tokens.size())
                        {
                            errUsage(CPLE_IllegalArg,
                                     strPrintf("Option '%s' requires a value.",
                                               tok.c_str()));
                        }
                        setValue(*a, tokens[++i], displayName);
                    }
                }
                else
                {
                    for (char c : chars)
                    {
                        std::string sn(1, c);
                        const ArgSpec *a = cmd.findShort(sn);
                        if (!a)
                            shortUnknown(sn);
                        if (!a->isBool())
                        {
                            errUsage(CPLE_IllegalArg,
                                     strPrintf("Invalid argument '%s'. "
                                               "Option '%s' is not a "
                                               "boolean option.",
                                               tok.c_str(), sn.c_str()));
                        }
                        setValue(*a, "true", "-" + sn);
                    }
                }
            }
            else
            {
                positional.push_back(tok);
            }
        }

        assignPositionals(positional);
        validate();

        if (!ignoredConfigValues.empty())
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        inv.algName() + ": Configuration options passed with "
                                        "the 'config' argument are ignored");
        }
    }

    void assignPositionals(std::vector<std::string> &positional)
    {
        std::vector<const ArgSpec *> posArgs;
        for (const auto &a : cmd.args)
            if (a.positional >= 0)
                posArgs.push_back(&a);
        std::sort(posArgs.begin(), posArgs.end(),
                  [](const ArgSpec *x, const ArgSpec *y)
                  { return x->positional < y->positional; });

        // wording quirk: when an unset numeric positional immediately
        // follows another unset positional, shortfalls report "Not enough
        // positional values." instead of naming the argument
        bool notEnoughWording = false;
        for (size_t k = 1; k < posArgs.size(); ++k)
        {
            const ArgSpec &a = *posArgs[k];
            if (result.byName[a.name].set ||
                result.byName[posArgs[k - 1]->name].set)
                continue;
            if (a.type == "real" || a.type == "integer" ||
                a.type == "real_list" || a.type == "integer_list")
                notEnoughWording = true;
        }

        const long long kUnbounded = 2147483647;
        // overflow past a leading list positional followed by two or more
        // pending positionals re-targets the arg after the list: the
        // reference reports it as a repeated specification (values are
        // never assigned, so tail parse checks stay silent)
        {
            std::vector<const ArgSpec *> unfilled;
            for (const ArgSpec *pa : posArgs)
                if (!result.byName[pa->name].set)
                    unfilled.push_back(pa);
            long long cap = 0;
            bool open = false;
            for (const ArgSpec *pa : unfilled)
            {
                if (pa->isList() && pa->maxCount != 1)
                {
                    if (pa->minCount == pa->maxCount && pa->minCount > 0)
                        cap += pa->minCount;
                    else
                        open = true;
                }
                else
                    cap += 1;
            }
            if (!open && unfilled.size() >= 3 && unfilled[0]->isList() &&
                (long long)positional.size() > cap)
            {
                pendingPosErr =
                    strPrintf("Argument '%s' has already been specified.",
                              unfilled[1]->name.c_str());
                pendingPosErrCls = CPLE_IllegalArg;
                return;
            }
        }
        size_t vi = 0;
        size_t ai = 0;
        while (vi < positional.size() && ai < posArgs.size())
        {
            const ArgSpec &a = *posArgs[ai];
            // single-value slots already set through their option name
            // are skipped; exact-count list slots (dataset lists) still
            // consume values, and the count ceiling reports the double
            // specification afterwards. The generic convert dispatch
            // injects --input, which counts as consumed either way.
            if (result.byName[a.name].set &&
                (!a.isList() || inv.dispatchedConvert))
            {
                ++ai;
                continue;
            }
            size_t take = 1;
            if (a.isList() && a.maxCount != 1)
            {
                if (a.minCount == a.maxCount && a.minCount > 0)
                {
                    take = static_cast<size_t>(a.minCount);
                    if (positional.size() - vi < take)
                    {
                        // positional-count errors defer until the
                        // per-argument value checks have run
                        pendingPosErr =
                            notEnoughWording
                                ? "Not enough positional values."
                                : strPrintf("Positional arguments starting "
                                            "at '%s' have not been "
                                            "specified.",
                                            metavarOf(a).c_str());
                        return;
                    }
                }
                else
                {
                    long long needLater = 0;
                    for (size_t aj = ai + 1; aj < posArgs.size(); ++aj)
                    {
                        // later positional slots stay reserved even when
                        // already set through their option name
                        const ArgSpec &b = *posArgs[aj];
                        if (b.isList() && b.maxCount != 1)
                            needLater +=
                                b.minCount > 0 && b.minCount != kUnbounded
                                    ? b.minCount
                                    : 0;
                        else
                            needLater += 1;
                    }
                    long long avail =
                        static_cast<long long>(positional.size() - vi) -
                        needLater;
                    take = avail > 0 ? static_cast<size_t>(avail) : 0;
                }
            }
            for (size_t k = 0; k < take && vi < positional.size(); ++k)
                setValue(a, positional[vi++], a.name);
            ++ai;
        }
        if (vi < positional.size())
        {
            pendingPosErr =
                strPrintf("Positional values starting at '%s' are not "
                          "expected.",
                          positional[vi].c_str());
            pendingPosErrExcess = true;
            return;
        }
        while (ai < posArgs.size() && result.byName[posArgs[ai]->name].set)
            ++ai;
        if (ai < posArgs.size())
        {
            const ArgSpec &a = *posArgs[ai];
            bool needsValue = a.isList() && a.maxCount != 1
                                  ? (a.minCount > 0 && a.required)
                                  : a.required;
            if (needsValue)
            {
                pendingPosErr =
                    notEnoughWording
                        ? "Not enough positional values."
                        : strPrintf("Positional arguments starting at '%s' "
                                    "have not been specified.",
                                    metavarOf(a).c_str());
                return;
            }
        }
    }

    // GDALAlgorithm-style output dataset processing at validation time:
    // refusal when the output exists without --overwrite, QuietDelete with
    // it, update-family output opening
    void processConvertOutputArgs()
    {
        if (!convertLikeOutputCmd(cmd.id))
            return;
        // a mutual-exclusion conflict (reported later) suppresses the
        // dataset processing: no update-family open, no refusal probe.
        // grid and clean-collar stack the exists refusal with their
        // mutex reports instead
        if (!strStartsWith(cmd.id, "vector_grid_") &&
            cmd.id != "raster_clean-collar")
            for (const auto &a : cmd.args)
            {
                if (!result.byName[a.name].set || a.mutex.empty())
                    continue;
                for (const auto &b : cmd.args)
                {
                    if (&b == &a)
                        break;
                    if (b.mutex == a.mutex && result.byName[b.name].set)
                    {
                        // index consumes the overwrite deletion while the
                        // output argument is processed, before the
                        // exclusion report fires
                        if (cmd.id == "raster_index" &&
                            result.flag("overwrite") &&
                            !result.flag("append"))
                        {
                            const ArgValue &ov = result.byName["output"];
                            if (ov.set && !ov.values.empty() &&
                                fileExists(ov.values[0]))
                            {
                                std::string kind =
                                    outputExistsKind(ov.values[0]);
                                if (kind == "Dataset")
                                    overwriteDeleteFileset(ov.values[0]);
                                else if (kind != "Directory")
                                    unlink(ov.values[0].c_str());
                            }
                        }
                        return;
                    }
                }
            }
        bool ow = result.flag("overwrite");
        bool fAppend = result.flag("append");
        bool fUpdate = result.flag("update");
        bool fOverLayer = result.flag("overwrite-layer");
        bool fUpsert = result.flag("upsert");
        bool fam = fAppend || fUpdate || fOverLayer || fUpsert;
        std::string of = result.str("output-format");
        if (strEqualNoCase(of, "MEM") || strEqualNoCase(of, "Memory") ||
            strEqualNoCase(of, "stream"))
            return;
        for (const auto &a : cmd.args)
        {
            if ((a.kind != "output_arguments" &&
                 a.kind != "input_output_arguments") ||
                !a.isDataset())
                continue;
            const ArgValue &v = result.byName[a.name];
            if (!v.set || v.values.empty())
                continue;
            // index defers its exists refusal until after the missing
            // required-input report (mirrors the concat sequencing)
            if (cmd.id == "raster_index" && !result.byName["input"].set)
                continue;
            const std::string &val = v.values[0];
            bool exists = fileExists(val);
            if (cmd.id == "vector_layer-algebra" &&
                (strEqualNoCase(of, "GDALG") ||
                 (of.empty() &&
                  strEndsWith(strToLower(val), ".gdalg.json"))))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "layer-algebra: GDALG output is not supported");
                datasetOpenFailed = true;
                continue;
            }
            if (cmd.id == "vector_convert" && datasetOpenFailed &&
                (fUpdate || fUpsert || fOverLayer))
            {
                // the input already failed, so the handler never runs;
                // the update-family output still opens during validation
                if (!exists)
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(val));
                else
                {
                    std::string terr;
                    validationTarget =
                        openVectorDataset(val, terr, {}, {}, false);
                    if (!validationTarget)
                    {
                        if (outputExistsKind(val) == "Directory")
                            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                        val + ": Is a directory");
                        else if (terr != "reported")
                            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                        "`" + val +
                                            "' not recognized as being in "
                                            "a supported file format.");
                    }
                }
                continue;
            }
            if (cmd.id == "raster_clean-collar" && fUpdate)
            {
                // update opens the output during validation
                if (!exists)
                {
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(val));
                    datasetOpenFailed = true;
                }
                else
                {
                    std::string oerr;
                    cplPushQuietHandler();
                    auto probe = openRaster(val, oerr);
                    cplPopHandler();
                    if (!probe)
                    {
                        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                    "`" + val +
                                        "' not recognized as being in a "
                                        "supported file format.");
                        datasetOpenFailed = true;
                    }
                }
                continue;
            }
            if ((cmd.id == "vector_sql" || cmd.id == "vector_clip" ||
                 cmd.id == "vector_combine" ||
                 (cmd.id == "vector_index" && !fAppend) ||
                 (cmd.id == "raster_index" && !fAppend) ||
                 geosStubVectorCmd(cmd.id)) &&
                (fUpdate || fUpsert || fOverLayer))
            {
                // update-family opens the output during validation
                if (!exists)
                {
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                datasetMissingMessage(val));
                    datasetOpenFailed = true;
                }
                else if (outputExistsKind(val) == "Directory")
                {
                    cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                val + ": Is a directory");
                    datasetOpenFailed = true;
                }
                else if (geosStubVectorCmd(cmd.id) ||
                         cmd.id == "vector_sql" ||
                         cmd.id == "vector_clip" ||
                         cmd.id == "vector_combine" ||
                         cmd.id == "vector_index" ||
                         cmd.id == "raster_index")
                {
                    std::string oerr;
                    cplPushQuietHandler();
                    auto probe = openVectorDataset(val, oerr, {});
                    cplPopHandler();
                    if (!probe)
                    {
                        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                                    "`" + val +
                                        "' not recognized as being in a "
                                        "supported file format.");
                        datasetOpenFailed = true;
                    }
                }
                continue;
            }
            if (!exists)
                continue;
            // calc --append rewrites the whole output: it deletes the
            // existing file up front like --overwrite does
            bool calcAppend = cmd.id == "raster_calc" && fAppend;
            if (!ow && !fam)
            {
                std::string opts = "--overwrite";
                auto hasArg = [&](const char *n)
                {
                    for (const auto &b : cmd.args)
                        if (b.name == n)
                            return true;
                    return false;
                };
                if (hasArg("overwrite-layer"))
                    opts += "/--overwrite-layer";
                if (hasArg("append"))
                    opts += "/--append";
                if (hasArg("update"))
                    opts += "/--update";
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            inv.algName() + ": " + outputExistsKind(val) +
                                " '" + val +
                                "' already exists. You may specify the " +
                                opts + " option.");
                datasetOpenFailed = true;
            }
            else if ((ow && !fam) || calcAppend)
            {
                std::string kind = outputExistsKind(val);
                if (kind == "Directory")
                {
                    cplErrorStr(
                        CE_Failure, CPLE_AppDefined,
                        inv.algName() + ": Directory '" + val +
                            "' already exists, but is not recognized as a "
                            "valid GDAL dataset. Please manually delete it "
                            "before retrying");
                    datasetOpenFailed = true;
                }
                else if (kind == "Dataset")
                {
                    // the reference holds the opened input across the
                    // deletion; our handlers re-read from disk, so when
                    // the output IS an input file the deletion is
                    // deferred to the handler (post-open)
                    bool sameAsInput = false;
                    struct stat so{};
                    if (cmd.id != "raster_calc" &&
                        stat(val.c_str(), &so) == 0)
                        for (const auto &b : cmd.args)
                        {
                            if (!b.isDataset() ||
                                b.kind != "input_arguments")
                                continue;
                            const ArgValue &bv = result.byName[b.name];
                            if (!bv.set)
                                continue;
                            for (const auto &ival : bv.values)
                            {
                                struct stat si{};
                                if (stat(ival.c_str(), &si) == 0 &&
                                    si.st_dev == so.st_dev &&
                                    si.st_ino == so.st_ino)
                                    sameAsInput = true;
                            }
                        }
                    if (!sameAsInput)
                        overwriteDeleteFileset(val);
                }
            }
        }
    }

    // numeric arguments never get their declared choices enforced
    // (rgb-to-palette --bit-depth accepts any integer)
    static bool choicesEnforced(const ArgSpec &a)
    {
        return !a.choices.empty() && a.type != "integer" &&
               a.type != "real" && a.type != "integer_list" &&
               a.type != "real_list";
    }

    static bool choiceValueOk(const ArgSpec &a, const std::string &val)
    {
        for (const auto &c : a.choices)
            if (strToLower(c) == strToLower(val))
                return true;
        if (strToLower(val) == "byte")
            for (const auto &c : a.choices)
                if (c == "UInt8")  // legacy GDAL name accepted
                    return true;
        if (strToLower(val) == "near")
            for (const auto &c : a.choices)
                if (c == "nearest")  // hidden resampling alias
                    return true;
        return false;
    }

    void emitChoiceError(const ArgSpec &a, const std::string &val)
    {
        std::string list;
        for (size_t k = 0; k < a.choices.size(); ++k)
        {
            if (k)
                list += ", ";
            list += "'" + a.choices[k] + "'";
        }
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    strPrintf("Invalid value '%s' for string argument "
                              "'%s'. Should be one among %s.",
                              val.c_str(), a.name.c_str(), list.c_str()));
    }

    // deferred choices enforcement for LIST args only: the reference
    // validates scalar choices at value-parse time but screens list
    // choices after the whole command line is parsed (parse-time checks
    // like ranges, driver names and the verb pre-validators outrank
    // them); the first list arg in declaration order with any invalid
    // value reports EVERY invalid value it carries, then the process
    // exits without a usage block
    void validateChoices()
    {
        for (const auto &a : cmd.args)
        {
            if (!a.isList() || !choicesEnforced(a))
                continue;
            const ArgValue &v = result.byName[a.name];
            if (!v.set)
                continue;
            bool bad = false;
            for (const auto &val : v.values)
            {
                if (choiceValueOk(a, val))
                    continue;
                emitChoiceError(a, val);
                bad = true;
            }
            if (bad)
                exit(1);
        }
    }

    void validate()
    {
        for (const auto &a : cmd.args)
        {
            if (!a.isDataset())
                continue;
            ArgValue &v = result.byName[a.name];
            if (!v.set)
                continue;
            for (auto &val : v.values)
                if (val == "-")
                {
                    if (a.kind == "input_arguments")
                        val = "/vsistdin/";
                    else
                    {
                        val = "/vsistdout/";
                        g_dashStdout = true;
                    }
                }
        }
        for (const auto &a : cmd.args)
        {
            const ArgValue &v = result.byName[a.name];
            if (!v.set)
                continue;
            if (a.isList())
            {
                long long cnt = (long long)v.values.size();
                // an excess-positional error outranks the count
                // ceiling (input swallows the spare value first)
                if (a.minCount >= 0 && a.minCount == a.maxCount &&
                    cnt != a.minCount && !pendingPosErrExcess)
                {
                    errUsage(CPLE_AppDefined,
                             strPrintf("%lld value%s been specified for "
                                       "argument '%s', whereas exactly "
                                       "%lld %s expected.",
                                       cnt, cnt == 1 ? " has" : "s have",
                                       a.name.c_str(), a.minCount,
                                       a.minCount == 1 ? "was" : "were"));
                }
            }
            auto ait = argCheckMap().find(cmd.id);
            if (ait != argCheckMap().end())
            {
                int rc = ait->second(a.name, result);
                if (rc)
                    exit(rc);
            }
        }
        {
            auto pit = preValidatorMap().find(cmd.id);
            if (pit != preValidatorMap().end())
            {
                int rc = pit->second(cmd, result);
                if (rc)
                    exit(rc);
            }
        }
        validateChoices();
        if (!pendingPosErr.empty())
            errUsage(pendingPosErrCls, pendingPosErr);
        // @<filename> indirection (sql/where style args): the value is
        // replaced by the file content; a missing file aborts before any
        // dataset checks
        for (const auto &a : cmd.args)
        {
            if (a.metavar.find("|@<filename>") == std::string::npos)
                continue;
            ArgValue &v = result.byName[a.name];
            if (!v.set)
                continue;
            for (auto &val : v.values)
            {
                if (val.empty() || val[0] != '@')
                    continue;
                std::string content;
                if (!readFileToString(val.substr(1), content))
                {
                    cplErrorStr(CE_Failure, CPLE_FileIO,
                                "Cannot open file '" + val.substr(1) +
                                    "'");
                    failUsage();
                }
                val = content;
            }
        }

        // pixel-info and as-features validate band minimums before their
        // inputs are probed, so a bad band outranks a missing dataset
        if (cmd.id == "raster_pixel-info" ||
            cmd.id == "raster_as-features")
        {
            const ArgValue &bv = result.byName["band"];
            if (bv.set)
                for (const auto &sv : bv.values)
                    if (atoi(sv.c_str()) < 1)
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "Value of 'band' should greater or "
                                    "equal to 1.");
                        failUsage();
                    }
        }
        for (const auto &a : cmd.args)
        {
            // generic `gdal convert` probes the input itself and reports
            // nothing when it cannot be opened; concat and calc validate
            // their inputs at run time without usage output
            if (cmd.id == "convert" || cmd.id == "vector_concat" ||
                cmd.id == "raster_calc")
                break;
            if (cmd.id == "raster_zonal-stats" && a.name == "zones")
                zonalStatsValidateOutput(result, datasetOpenFailed);
            if (!a.isDataset() || a.kind != "input_arguments")
                continue;
            // mask datasets of the pixel verbs are opened during their
            // validation stack (after the output-exists refusal), not by
            // this generic pass
            if (a.name == "mask" && postValidatorMap().count(cmd.id))
                continue;
            // pansharpen opens its spectral datasets at run time: their
            // failures skip the usage block entirely
            if (cmd.id == "raster_pansharpen" && a.name == "spectral")
                continue;
            bool wantsDataset = false;
            for (const auto &f : a.inputFlags)
                if (f == "dataset")
                    wantsDataset = true;
            if (!wantsDataset)
                continue;
            ArgValue &v = result.byName[a.name];
            if (!v.set)
                continue;
            bool wantsMultidim = false;
            for (const auto &t : a.datasetType)
                if (t == "multidim_raster")
                    wantsMultidim = true;
            for (const auto &val : v.values)
            {
                if (val.rfind("GTIFF_DIR:", 0) == 0)
                    continue;
                std::string openErr;
                if (!fileExists(val))
                {
                    // filesystem-specific failures (curl, credentials)
                    // are fully reported by the VSI layer
                    if (!vsiMissingPrelude(val))
                    {
                        datasetOpenFailed = true;
                        continue;
                    }
                    openErr = datasetMissingMessage(val);
                }
                else if (wantsMultidim)
                {
                    std::string mdErr;
                    if (mdimValidationOpen(val, mdErr))
                        continue;
                    if (!mdErr.empty())
                    {
                        // structural parse failures of a multidim VRT
                        // are reported verbatim, not as open errors
                        cplErrorStr(CE_Failure, CPLE_AppDefined, mdErr);
                        datasetOpenFailed = true;
                        continue;
                    }
                    bool classicOk = false;
                    for (const auto &t : a.datasetType)
                        if (t == "raster" &&
                            datasetIdentify(val, {"raster"}))
                            classicOk = true;
                    if (classicOk)
                        continue;
                    openErr = "`" + val +
                              "' not recognized as being in a "
                              "supported file format.";
                }
                else if (!datasetIdentify(val, a.datasetType))
                    openErr = "`" + val +
                              "' not recognized as being in a "
                              "supported file format.";
                if (openErr.empty())
                    continue;
                cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                datasetOpenFailed = true;
                // set-field-type dereferences its failed input open:
                // error text without usage, then SIGSEGV
                if (cmd.id == "vector_set-field-type")
                {
                    fflush(stdout);
                    fflush(stderr);
                    raise(SIGSEGV);
                }
                // --append re-opens the input while wiring the output:
                // the same failure is reported three times in total and
                // no usage block follows
                if (convertLikeOutputCmd(cmd.id) && result.flag("append") &&
                    !result.flag("overwrite"))
                {
                    if (cmd.id == "vector_convert")
                    {
                        auto ov = result.byName.find("output");
                        if (ov != result.byName.end() && ov->second.set &&
                            !ov->second.values.empty() &&
                            fileExists(ov->second.values[0]))
                        {
                            std::string terr;
                            validationTarget = openVectorDataset(
                                ov->second.values[0], terr, {}, {}, false);
                        }
                    }
                    cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                    cplErrorStr(CE_Failure, CPLE_OpenFailed, openErr);
                    if (validationTarget)
                    {
                        ogrFlushPendingDebug(*validationTarget);
                        vectorDebugClose(*validationTarget);
                    }
                    exit(1);
                }
            }
        }

        processConvertOutputArgs();

        // mdim convert forbids -group with -array: after the exists
        // refusal, the conflict is reported and the half-built translate
        // options are then dereferenced
        if (cmd.id == "mdim_convert" && !datasetOpenFailed &&
            result.get("group") && result.get("array"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Argument '-group <group_spec>' not allowed with "
                        "'-array <array_spec>'");
            fflush(stdout);
            fflush(stderr);
            signal(SIGSEGV, SIG_DFL);
            raise(SIGSEGV);
        }

        // mutual exclusions: every set arg reports its first prior
        // conflict; all reports precede the single usage block and the
        // missing-required reports. vector create sequences these
        // reports itself, after its output-exists check and before its
        // combined refusal line; the info verbs enforce theirs handler
        // side, after the missing-required reports and the post
        // validators
        bool mutexFired = false;
        auto runMutexPass = [&]()
        {
            for (const auto &a : cmd.args)
            {
                const ArgValue &v = result.byName[a.name];
                if (!v.set || a.mutex.empty())
                    continue;
                for (const auto &b : cmd.args)
                {
                    if (&b == &a)
                        break;
                    if (b.mutex == a.mutex && result.byName[b.name].set)
                    {
                        if (inv.dispatchedInfo &&
                            !inv.dispatchedPositional.empty())
                            errUsage(
                                CPLE_AppDefined,
                                strPrintf("Positional values starting at "
                                          "'%s' are not expected.",
                                          inv.dispatchedPositional.c_str()));
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            inv.algName() +
                                strPrintf(": Argument '%s' is mutually "
                                          "exclusive with '%s'.",
                                          a.name.c_str(), b.name.c_str()));
                        mutexFired = true;
                        break;
                    }
                }
            }
        };
        // footprint's post validator emits its exclusions itself
        // (between the output checks and the GDALG refusal) and unsets
        // the reporting arg, so its engine pass must run after it
        bool lateMutex = cmd.id == "vector_create" ||
                         cmd.id == "vector_info" ||
                         cmd.id == "raster_info" ||
                         cmd.id == "raster_footprint";
        if (!lateMutex)
            runMutexPass();
        bool missingRequired = false;
        for (const auto &a : cmd.args)
        {
            const ArgValue &v = result.byName[a.name];
            if (a.required && !v.set)
            {
                // a required member of a mutual-exclusion group is
                // satisfied by any set member of that group
                if (!a.mutex.empty())
                {
                    bool groupSet = false;
                    for (const auto &b : cmd.args)
                        if (b.mutex == a.mutex &&
                            result.byName[b.name].set)
                            groupSet = true;
                    if (groupSet)
                        continue;
                }
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            inv.algName() +
                                strPrintf(": Required argument '%s' has not "
                                          "been specified.",
                                          a.name.c_str()));
                missingRequired = true;
            }
        }
        {
            auto pv = postValidatorMap().find(cmd.id);
            if (pv != postValidatorMap().end() &&
                pv->second(cmd, result, datasetOpenFailed))
                datasetOpenFailed = true;
        }
        if (lateMutex && cmd.id != "vector_create")
            runMutexPass();
        if (cmd.id == "vector_info")
        {
            // empty string values count as unset for this pairing
            auto nonEmpty = [&](const char *name) {
                auto it = result.byName.find(name);
                return it != result.byName.end() && it->second.set &&
                       !it->second.values.empty() &&
                       !it->second.values[0].empty();
            };
            if (nonEmpty("sql") && nonEmpty("where"))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            inv.algName() + ": Option 'sql' and 'where' "
                                            "are mutually exclusive");
                mutexFired = true;
            }
        }
        if (missingRequired &&
            (cmd.id == "vector_concat" || cmd.id == "raster_index"))
        {
            const ArgValue &out = result.byName["output"];
            bool ovw = false;
            for (const char *fl : {"overwrite", "append", "update",
                                   "overwrite-layer", "upsert"})
            {
                auto it2 = result.byName.find(fl);
                if (it2 != result.byName.end() && it2->second.set)
                    ovw = true;
            }
            if (out.set && !out.values.empty() && !ovw &&
                fileExists(out.values[0]))
            {
                bool isDs = datasetIdentify(out.values[0],
                                            {"raster", "vector"});
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            inv.algName() + ": " +
                                (isDs ? "Dataset" : "File") + " '" +
                                out.values[0] +
                                "' already exists. You may specify the "
                                "--overwrite/--overwrite-layer/--append/"
                                "--update option.");
            }
        }
        if (mutexFired)
            failUsage();
        if (missingRequired || datasetOpenFailed)
        {
            // the info handlers enforce this mutex after open; validation
            // failures still surface it before the usage block
            if (cmd.id == "raster_info" || cmd.id == "vector_info")
            {
                auto of = result.byName.find("output-format");
                auto cf = result.byName.find("crs-format");
                if (of != result.byName.end() && of->second.set &&
                    !of->second.values.empty() &&
                    strEqualNoCase(of->second.values[0], "json") &&
                    cf != result.byName.end() && cf->second.set &&
                    !cf->second.values.empty() &&
                    !strEqualNoCase(cf->second.values[0], "AUTO"))
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                inv.algName() +
                                    ": 'crs-format' cannot be set when "
                                    "'format' is set to 'json'");
            }
            failUsage();
        }
    }
};

}  // namespace
std::string g_handlerUsageText;
std::string g_handlerInvokedCli;
std::string g_infoDispatchOpenUsage;
namespace
{

// generic `gdal info`: determine raster/vector from the input and forward
// the whole command line to the matching sub-command; may exit
void genericInfoRedirect(Invocation &inv, const std::vector<std::string> &tokens)
{
    const Spec &spec = Spec::instance();
    const CmdSpec *subs[3] = {spec.findById("raster_info"),
                              spec.findById("vector_info"),
                              spec.findById("mdim_info")};
    const CmdSpec &generic = *inv.cmd;
    bool hasForward = false;
    std::string firstForwardTok;
    std::string firstPositional;
    std::vector<std::string> inputs;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::string &tok = tokens[i];
        if (tok == "--help" || tok == "-h" || tok == "--help-doc" ||
            tok == "--json-usage")
            return;
        if (strStartsWith(tok, "--") && tok.size() > 2)
        {
            std::string body = tok.substr(2);
            size_t eq = body.find('=');
            std::string name =
                eq == std::string::npos ? body : body.substr(0, eq);
            bool hasValue = eq != std::string::npos;
            if (name == "config")
            {
                if (!hasValue && i + 1 < tokens.size())
                    ++i;
                continue;
            }
            if (const ArgSpec *g = generic.findLong(name))
            {
                std::string value;
                if (hasValue)
                    value = body.substr(eq + 1);
                else if (!g->isBool() && i + 1 < tokens.size())
                    value = tokens[++i];
                if (g->isDataset())
                    inputs.push_back(value);
                continue;
            }
            const ArgSpec *sub = nullptr;
            for (const CmdSpec *sc : subs)
                if (sc && (sub = sc->findLong(name)) != nullptr)
                    break;
            hasForward = true;
            if (firstForwardTok.empty())
                firstForwardTok = tok;
            if (sub && !sub->isBool() && !hasValue &&
                i + 1 < tokens.size())
                ++i;
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' &&
            !(tok[1] >= '0' && tok[1] <= '9') && tok[1] != '.')
        {
            std::string chars = tok.substr(1);
            if (chars.size() == 1)
            {
                if (const ArgSpec *g = generic.findShort(chars))
                {
                    std::string value;
                    if (!g->isBool() && i + 1 < tokens.size())
                        value = tokens[++i];
                    if (g->isDataset())
                        inputs.push_back(value);
                    continue;
                }
                const ArgSpec *sub = nullptr;
                for (const CmdSpec *sc : subs)
                    if (sc && (sub = sc->findShort(chars)) != nullptr)
                        break;
                hasForward = true;
                if (firstForwardTok.empty())
                    firstForwardTok = tok;
                if (sub && !sub->isBool() && i + 1 < tokens.size())
                    ++i;
            }
            else
            {
                for (char c : chars)
                    if (!generic.findShort(std::string(1, c)))
                    {
                        hasForward = true;
                        if (firstForwardTok.empty())
                            firstForwardTok = tok;
                        break;
                    }
            }
            continue;
        }
        inputs.push_back(tok);
        if (firstPositional.empty())
            firstPositional = tok;
    }
    if (inputs.size() >= 2)
    {
        std::string msg = strPrintf(
            "read: %d values have been specified for argument 'input', "
            "whereas exactly 1 was expected.",
            (int)inputs.size());
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        exit(1);
    }
    if (inputs.empty())
    {
        if (hasForward)
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "info: Option '" + firstForwardTok +
                            "' is unknown.");
            printLeafUsageError(inv);
            exit(1);
        }
        return;
    }
    const std::string &input = inputs[0];
    const char *sub = nullptr;
    if (infoDispatchRaster(input))
        sub = hasForward ? "raster" : nullptr;
    else if (infoDispatchVector(input))
        sub = "vector";
    else if (hasForward)
    {
        printLeafUsageError(inv);
        exit(1);
    }
    if (!sub)
        return;
    const CmdSpec *target = spec.findById(std::string(sub) + "_info");
    if (!target)
        return;
    // dataset-open failures inside the dispatched sub-command report the
    // generic `gdal info` usage, unlike argument-level errors
    g_infoDispatchOpenUsage = inv.usageLine() + "\nTry '" +
                              inv.invokedCli() + " --help' for help.\n";
    inv.cmd = target;
    inv.invokedPath = {"info", sub};
    inv.usageOverride = strPrintf("Usage: gdal info %s [OPTIONS]", sub);
    inv.dispatchedInfo = true;
    inv.dispatchedPositional = firstPositional;
}

// generic `gdal convert`: open the input to choose raster/vector and
// re-dispatch the command line to the sub-command; may exit
void genericConvertRedirect(Invocation &inv,
                            std::vector<std::string> &tokens)
{
    const Spec &spec = Spec::instance();
    const CmdSpec *subs[2] = {spec.findById("raster_convert"),
                              spec.findById("vector_convert")};
    const CmdSpec &generic = *inv.cmd;
    bool hasForward = false;
    bool subUnknownOpt = false;
    bool inputFormatGiven = false;
    std::string namedInput;
    std::string namedOutput;
    size_t namedInputTok = (size_t)-1;
    size_t namedInputValTok = (size_t)-1;
    std::vector<std::pair<std::string, size_t>> positionals;
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::string &tok = tokens[i];
        if (tok == "--help" || tok == "-h" || tok == "--help-doc" ||
            tok == "--json-usage")
            return;
        if (strStartsWith(tok, "--") && tok.size() > 2)
        {
            std::string body = tok.substr(2);
            size_t eq = body.find('=');
            std::string name =
                eq == std::string::npos ? body : body.substr(0, eq);
            bool hasValue = eq != std::string::npos;
            if (name == "config")
            {
                if (!hasValue && i + 1 < tokens.size())
                    ++i;
                continue;
            }
            const ArgSpec *g = generic.findLong(name);
            const ArgSpec *sub = nullptr;
            for (const CmdSpec *sc : subs)
                if (sc && (sub = sc->findLong(name)) != nullptr)
                    break;
            if (!g)
                hasForward = true;
            if (!g && !sub)
            {
                subUnknownOpt = true;
                continue;
            }
            const ArgSpec &a = g ? *g : *sub;
            if (a.name == "input-format")
                inputFormatGiven = true;
            std::string value;
            size_t valTok = (size_t)-1;
            if (hasValue)
                value = body.substr(eq + 1);
            else if (!a.isBool() && i + 1 < tokens.size())
            {
                valTok = i + 1;
                value = tokens[++i];
            }
            if (a.name == "input")
            {
                namedInput = value;
                namedInputTok = hasValue ? i : i - 1;
                namedInputValTok = valTok;
            }
            else if (a.name == "output")
                namedOutput = value;
            continue;
        }
        if (tok.size() >= 2 && tok[0] == '-' &&
            !(tok[1] >= '0' && tok[1] <= '9') && tok[1] != '.')
        {
            std::string chars = tok.substr(1);
            if (chars.size() == 1)
            {
                const ArgSpec *g = generic.findShort(chars);
                const ArgSpec *sub = nullptr;
                for (const CmdSpec *sc : subs)
                    if (sc && (sub = sc->findShort(chars)) != nullptr)
                        break;
                if (!g)
                    hasForward = true;
                if (!g && !sub)
                {
                    subUnknownOpt = true;
                    continue;
                }
                const ArgSpec &a = g ? *g : *sub;
                if (a.name == "input-format")
                    inputFormatGiven = true;
                std::string value;
                size_t valTok = (size_t)-1;
                if (!a.isBool() && i + 1 < tokens.size())
                {
                    valTok = i + 1;
                    value = tokens[++i];
                }
                if (a.name == "input")
                {
                    namedInput = value;
                    namedInputTok = i - 1;
                    namedInputValTok = valTok;
                }
                else if (a.name == "output")
                    namedOutput = value;
            }
            else
            {
                for (char c : chars)
                    if (!generic.findShort(std::string(1, c)))
                    {
                        hasForward = true;
                        break;
                    }
            }
            continue;
        }
        positionals.emplace_back(tok, i);
    }

    // without a resolvable output the generic parse reports the missing
    // positional itself, before any dispatch
    size_t needPos = namedInput.empty() ? 1 : 0;
    if (namedOutput.empty())
        ++needPos;
    if (positionals.size() < needPos)
        return;

    std::string input = namedInput;
    size_t inputPosTok = (size_t)-1;
    std::string subName;
    if (!input.empty())
        subName = convertDispatchProbe(input);
    else
        for (const auto &p : positionals)
        {
            subName = convertDispatchProbe(p.first);
            if (!subName.empty())
            {
                input = p.first;
                inputPosTok = p.second;
                break;
            }
        }
    if (subName.empty())
    {
        if (hasForward)
        {
            emitErr(inv.usageLine() + "\nTry '" + inv.invokedCli() +
                    " --help' for help.\n");
            exit(1);
        }
        return;
    }

    const CmdSpec *target = spec.findById(subName + "_convert");
    if (!target)
        return;
    inv.cmd = target;
    inv.invokedPath = {"convert", subName};
    inv.usageOverride =
        "Usage: gdal convert " + subName + " [OPTIONS] <INPUT> <OUTPUT>";

    // positionals left once the input is taken out
    std::vector<std::string> rest;
    for (const auto &p : positionals)
        if (p.second != inputPosTok)
            rest.push_back(p.first);

    bool fire = false;
    std::string firePos;
    if (subName == "raster" && inputFormatGiven)
    {
        fire = true;
        firePos = rest.size() >= 2 && inputPosTok == (size_t)-1
                      ? rest[1]
                      : (!rest.empty() ? rest[0] : namedOutput);
    }
    else if (!subUnknownOpt)
    {
        if (!namedOutput.empty() && !rest.empty())
        {
            fire = true;
            firePos = rest[0];
        }
        else if (namedOutput.empty() && rest.size() >= 2)
        {
            fire = true;
            firePos = inputPosTok == (size_t)-1 ? rest[1] : rest[0];
        }
    }
    if (fire)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "convert: Positional values starting at '" + firePos +
                        "' are not expected.");
        emitErr(inv.usageLine() + "\nTry '" + inv.invokedCli() +
                " --help' for help.\n");
        exit(1);
    }

    std::vector<std::string> newTokens = {"--input", input};
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        if (i == inputPosTok || i == namedInputTok ||
            i == namedInputValTok)
            continue;
        newTokens.push_back(tokens[i]);
    }
    tokens = std::move(newTokens);
    inv.dispatchedConvert = true;
}

[[noreturn]] void runLeaf(Invocation inv, std::vector<std::string> tokens)
{
    if (inv.cmd->id == "info")
        genericInfoRedirect(inv, tokens);
    if (inv.cmd->id == "convert")
        genericConvertRedirect(inv, tokens);
    if (inv.cmd->id == "raster_tile")
    {
        // help/usage output is replayed from captures that already carry
        // the tms diagnostic; emitting here would double it
        bool helpTok = tokens.size() == 1 && tokens[0] == "help";
        for (const auto &t : tokens)
        {
            if (t == "--")
                break;
            if (t == "--help" || t == "-h" || t == "--help-doc" ||
                t == "--json-usage")
            {
                helpTok = true;
                break;
            }
        }
        if (!helpTok)
            emitErr(tmsDiagnosticLine());
    }
    if (inv.cmd->id == "pipeline" || inv.cmd->id == "raster_pipeline" ||
        inv.cmd->id == "vector_pipeline")
        pipelineLeaf(*inv.cmd,
                     inv.usageLine() + "\nTry '" + inv.invokedCli() +
                         " --help' for help.\n",
                     inv.invokedCli(), tokens);
    g_handlerUsageText = inv.usageLine() + "\nTry '" + inv.invokedCli() +
                         " --help' for help.\n";
    g_handlerInvokedCli = inv.invokedCli();
    ParseResult result;
    LeafParser parser(inv, result);
    parser.parse(std::move(tokens));
    auto it = handlerMap().find(inv.cmd->id);
    if (it == handlerMap().end())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    inv.algName() +
                        ": this command is not implemented in this build");
        exit(1);
    }
    if (g_infoAttemptFd >= 0)
    {
        ssize_t n = write(g_infoAttemptFd, "x", 1);
        (void)n;
    }
    exit(it->second(*inv.cmd, result));
}

// subcommand aliases: parent id -> alias -> canonical child name
const std::map<std::string, std::map<std::string, std::string>> &aliasMap()
{
    static const std::map<std::string, std::map<std::string, std::string>> m =
        {
            {"ROOT", {{"translate", "convert"}}},
            {"dataset",
             {{"cp", "copy"},
              {"ren", "rename"},
              {"mv", "rename"},
              {"rm", "delete"},
              {"remove", "delete"}}},
            {"vsi",
             {{"cp", "copy"},
              {"ls", "list"},
              {"mv", "move"},
              {"ren", "move"},
              {"rename", "move"},
              {"rm", "delete"},
              {"rmdir", "delete"},
              {"del", "delete"}}},
            {"raster",
             {{"neighbours", "neighbors"},
              {"warp", "reproject"},
              {"translate", "convert"}}},
            {"vector", {{"translate", "convert"}}},
        };
    return m;
}

size_t levenshtein(const std::string &a, const std::string &b)
{
    std::vector<size_t> prev(b.size() + 1), curr(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i)
    {
        curr[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
        {
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1,
                                prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[b.size()];
}

// Returns best unique suggestion within distance 2, or empty.
std::string suggestSubcommand(const CmdSpec &parent, const std::string &name)
{
    std::string best;
    size_t bestDist = 3;
    bool tie = false;
    for (const auto &cand : parent.subNames)
    {
        size_t d = levenshtein(name, cand);
        if (d < bestDist)
        {
            bestDist = d;
            best = cand;
            tie = false;
        }
        else if (d == bestDist)
            tie = true;
    }
    if (bestDist <= 2 && !tie)
        return best;
    return std::string();
}

const ArgSpec *infoUnionFind(const std::string &n, bool shortName)
{
    const Spec &spec = Spec::instance();
    for (const char *id : {"info", "raster_info", "vector_info"})
        if (const CmdSpec *c = spec.findById(id))
            if (const ArgSpec *a =
                    shortName ? c->findShort(n) : c->findLong(n))
                return a;
    return nullptr;
}

// forks a `gdal info <rest>` attempt; exits when it succeeds or when the
// leaf handler ran, returns when the attempt failed during parsing
void infoAttemptOnce(const std::vector<std::string> &rest)
{
    int fds[2];
    if (pipe(fds) != 0)
        std::exit(1);
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        g_infoAttempt = true;
        g_infoAttemptFd = fds[1];
        Invocation iv;
        iv.cmd = Spec::instance().findById("info");
        iv.invokedPath = {"info"};
        runLeaf(iv, rest);
    }
    close(fds[1]);
    int st = 0;
    waitpid(pid, &st, 0);
    char b = 0;
    bool ran = read(fds[0], &b, 1) == 1;
    close(fds[0]);
    if (WIFEXITED(st) && (ran || WEXITSTATUS(st) == 0))
        std::exit(WEXITSTATUS(st));
}

// emulates the "gdal <FILE>" info attempt the reference makes for
// unrecognized root invocations whose arguments contain existing
// datasets. Successful attempts exit; returns 1 when the caller should
// stop with exit code 1, 0 when it should continue with its own error
// report. Callers gate on at least one existing argument.
int infoAttemptEmulate(const std::vector<std::string> &rest)
{
    size_t nExist = 0;
    std::string firstExisting;
    for (const auto &a2 : rest)
        if (fileExists(a2))
        {
            ++nExist;
            if (firstExisting.empty())
                firstExisting = a2;
        }
    std::vector<std::string> pos;
    std::string unknownTail, ranShortName;
    for (size_t k = 0; k < rest.size(); ++k)
    {
        const std::string &t2 = rest[k];
        if (strStartsWith(t2, "--") && t2.size() > 2)
        {
            std::string name = t2.substr(2);
            size_t eq = name.find('=');
            bool hasEq = eq != std::string::npos;
            if (hasEq)
                name = name.substr(0, eq);
            const ArgSpec *a = infoUnionFind(name, false);
            if (!a)
            {
                unknownTail =
                    strPrintf("Option '--%s' is unknown.", name.c_str());
                break;
            }
            if (!a->isBool() && !hasEq)
            {
                if (k + 1 >= rest.size())
                    ranShortName = "--" + name;
                ++k;
            }
        }
        else if (strStartsWith(t2, "-") && t2.size() > 1)
        {
            const ArgSpec *a = infoUnionFind(t2.substr(1, 1), true);
            if (!a)
            {
                unknownTail = strPrintf("Short name option '%s' is "
                                        "unknown.",
                                        t2.substr(1, 1).c_str());
                break;
            }
            if (!a->isBool())
            {
                if (k + 1 >= rest.size())
                    ranShortName = t2.substr(0, 2);
                ++k;
            }
        }
        else
            pos.push_back(t2);
    }
    if (!unknownTail.empty())
    {
        for (size_t k = 0; k < nExist; ++k)
            cplErrorStr(CE_Failure, CPLE_IllegalArg, "info: " + unknownTail);
        return 0;
    }
    if (!ranShortName.empty())
    {
        for (size_t k = 0; k < nExist; ++k)
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        strPrintf("info: Expected value for argument "
                                  "'%s', but ran short of tokens",
                                  ranShortName.c_str()));
        return 0;
    }
    if (pos.size() >= 2)
    {
        if (pos.size() == nExist &&
            convertDispatchProbe(firstExisting) != "raster")
        {
            for (size_t k = 0; k < nExist; ++k)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("read: %zu values have been "
                                      "specified for argument 'input', "
                                      "whereas exactly 1 was expected.",
                                      nExist));
            return 1;
        }
        for (size_t k = 0; k < nExist; ++k)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("info: Positional values starting at "
                                  "'%s' are not expected.",
                                  pos[0].c_str()));
        return 0;
    }
    for (size_t k = 0; k < nExist; ++k)
        infoAttemptOnce(rest);
    return 0;
}

// `gdal mdim --drivers`: the only non-leaf argument mode outside root
[[noreturn]] void mdimDriversMode(Invocation &inv, const CmdSpec &node,
                                  const std::vector<std::string> &args,
                                  size_t i)
{
    for (; i < args.size(); ++i)
    {
        const std::string &tok = args[i];
        if (tok == "--help" || tok == "-h")
            doHelp(inv);
        if (tok == "--help-doc")
            doHelpDoc(inv);
        if (tok == "--json-usage")
            doJsonUsage(inv);
        if (tok == "--drivers")
            continue;
        if (strStartsWith(tok, "-") && tok.size() > 1)
        {
            if (strStartsWith(tok, "--"))
            {
                std::string name = tok.substr(2);
                size_t eq = name.find('=');
                if (eq != std::string::npos)
                    name = name.substr(0, eq);
                std::string sug = suggestOptionName(
                    name, {"help", "help-doc", "json-usage", "config",
                           "drivers"});
                if (!sug.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                strPrintf("%s: Option '--%s' is unknown. "
                                          "Do you mean '--%s'?",
                                          node.path.back().c_str(),
                                          name.c_str(), sug.c_str()));
                    exit(1);
                }
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("%s: Option '%s' is unknown.",
                                      node.path.back().c_str(),
                                      tok.c_str()));
            }
            else
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("%s: Short name option '%s' is "
                                      "unknown.",
                                      node.path.back().c_str(),
                                      tok.substr(1, 1).c_str()));
            printNonLeafUsageError(node);
            exit(1);
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Positional values starting at '%s' are "
                              "not expected.",
                              node.path.back().c_str(), tok.c_str()));
        printNonLeafUsageError(node);
        exit(1);
    }
    emitErr(tmsDiagnosticLine());
    emitOut(mdimDriversJson());
    exit(0);
}

[[noreturn]] void rootArgMode(const CmdSpec &root,
                              const std::vector<std::string> &args, size_t i)
{
    Invocation inv{&root, {}};
    bool drivers = false;
    bool configIgnored = false;
    const size_t i0 = i;
    for (; i < args.size(); ++i)
    {
        const std::string &tok = args[i];
        if (tok == "--help" || tok == "-h")
            doHelp(inv);
        if (tok == "--help-doc")
            doHelpDoc(inv);
        if (tok == "--json-usage")
            doJsonUsage(inv);
        if (tok == "--drivers")
        {
            drivers = true;
            continue;
        }
        if (strStartsWith(tok, "--config="))
        {
            configIgnored = true;
            continue;
        }
        if (tok == "--config")
        {
            if (i + 1 < args.size())
            {
                configIgnored = true;
                ++i;
                continue;
            }
            configIgnored = true;
            continue;
        }
        if (strStartsWith(tok, "-") && tok.size() > 1)
        {
            if (strStartsWith(tok, "--"))
            {
                std::string name = tok.substr(2);
                size_t eq = name.find('=');
                if (eq != std::string::npos)
                    name = name.substr(0, eq);
                std::string sug = suggestOptionName(
                    name, {"help", "help-doc", "json-usage", "config",
                           "drivers", "version"});
                if (!sug.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                strPrintf("gdal: Option '--%s' is unknown. "
                                          "Do you mean '--%s'?",
                                          name.c_str(), sug.c_str()));
                    exit(1);
                }
            }
            std::vector<std::string> rest(args.begin() + i, args.end());
            bool anyExist = false;
            for (const auto &a2 : rest)
                if (fileExists(a2))
                    anyExist = true;
            if (anyExist && infoAttemptEmulate(rest))
                exit(1);
            if (!strStartsWith(tok, "--"))
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("gdal: Short name option '%s' is "
                                      "unknown.",
                                      tok.substr(1, 1).c_str()));
            else
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("gdal: Option '%s' is unknown.",
                                      tok.c_str()));
            printNonLeafUsageError(root);
            exit(1);
        }
        {
            std::vector<std::string> rest;
            for (size_t k = i0; k < args.size(); ++k)
            {
                const std::string &t2 = args[k];
                if (t2 == "--config")
                {
                    ++k;
                    continue;
                }
                if (strStartsWith(t2, "--config="))
                    continue;
                rest.push_back(t2);
            }
            bool anyExist = false;
            for (const auto &a2 : rest)
                if (fileExists(a2))
                    anyExist = true;
            if (anyExist && infoAttemptEmulate(rest))
                exit(1);
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("gdal: Positional values starting at '%s' are "
                              "not expected.",
                              tok.c_str()));
        printNonLeafUsageError(root);
        exit(1);
    }
    if (configIgnored)
    {
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "gdal: Configuration options passed with the 'config' "
                    "argument are ignored");
    }
    if (drivers)
    {
        emitErr(adjustErrCapture(embGet("misc/drivers.err")));
        emitOut(embGet("misc/drivers.out"));
    }
    exit(0);
}

}  // namespace

void handlerPrintUsage()
{
    emitErr(g_handlerUsageText);
}

const std::string &handlerInvokedCli()
{
    return g_handlerInvokedCli;
}

void handlerAppendUsageSub(const std::string &sub)
{
    std::string oldCli = g_handlerInvokedCli;
    std::string newCli = oldCli + " " + sub;
    size_t pos = 0;
    while ((pos = g_handlerUsageText.find(oldCli, pos)) !=
           std::string::npos)
    {
        g_handlerUsageText.replace(pos, oldCli.size(), newCli);
        pos += newCli.size();
    }
    g_handlerInvokedCli = newCli;
}

int runGdalMain(int argc, char **argv)
{
    registerAllHandlers();
    std::vector<std::string> rawArgs(argv + 1, argv + argc);

    if (!rawArgs.empty() && rawArgs[0] == "completion" && rawArgs.size() >= 2)
        return runCompletion(
            std::vector<std::string>(rawArgs.begin() + 1, rawArgs.end()));

    earlyConfigScan(rawArgs);
    if (configIsSet("GDAL_SKIP"))
        gdalSkipStartupWarnings();
    std::vector<std::string> args = generalPass(std::move(rawArgs));

    const Spec &spec = Spec::instance();
    const CmdSpec *cur = spec.findById("ROOT");
    Invocation inv{cur, {}};

    size_t i = 0;
    while (true)
    {
        bool isRoot = cur->path.empty();
        if (i >= args.size())
        {
            std::string msg = isRoot ? "gdal: Missing command name."
                                     : cur->path.back() +
                                           ": Missing subcommand name.";
            cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
            printNonLeafUsageError(*cur);
            return 1;
        }
        const std::string &tok = args[i];
        if (tok == "--help" || tok == "-h")
            doHelp(inv);
        if (tok == "--help-doc")
            doHelpDoc(inv);
        if (tok == "--json-usage")
            doJsonUsage(inv);
        if (strStartsWith(tok, "-") && tok.size() > 1)
        {
            if (!isRoot && cur->id == "mdim" && tok == "--drivers")
                mdimDriversMode(inv, *cur, args, i);
            if (isRoot)
                rootArgMode(*cur, args, i);
            if (strStartsWith(tok, "--"))
            {
                std::string name = tok.substr(2);
                size_t eq = name.find('=');
                if (eq != std::string::npos)
                    name = name.substr(0, eq);
                std::string sug = suggestOptionName(
                    name, {"help", "help-doc", "json-usage", "config",
                           "drivers"});
                if (!sug.empty())
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                strPrintf("%s: Option '--%s' is unknown. "
                                          "Do you mean '--%s'?",
                                          cur->path.back().c_str(),
                                          name.c_str(), sug.c_str()));
                    return 1;
                }
            }
            if (!strStartsWith(tok, "--"))
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("%s: Short name option '%s' is "
                                      "unknown.",
                                      cur->path.back().c_str(),
                                      tok.substr(1, 1).c_str()));
            else
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("%s: Option '%s' is unknown.",
                                      cur->path.back().c_str(),
                                      tok.c_str()));
            printNonLeafUsageError(*cur);
            return 1;
        }

        if (tok == "help" && i + 1 == args.size())
            doHelp(inv);

        std::string canonical = tok;
        auto pit = aliasMap().find(cur->id);
        if (pit != aliasMap().end())
        {
            auto ait = pit->second.find(tok);
            if (ait != pit->second.end())
                canonical = ait->second;
        }
        std::string childId =
            isRoot ? canonical : cur->id + "_" + canonical;
        const CmdSpec *child = spec.findById(childId);
        if (child)
        {
            cur = child;
            inv.cmd = child;
            inv.invokedPath.push_back(tok);
            ++i;
            if (cur->leaf())
                runLeaf(inv, std::vector<std::string>(args.begin() + i,
                                                      args.end()));
            continue;
        }
        if (isRoot && tok == "read" && i + 1 < args.size())
        {
            // 'gdal read ...' pipeline alias; parse failures fall back to
            // the unknown-command report below
            pipelineAliasRun(
                std::vector<std::string>(args.begin() + i + 1, args.end()));
        }
        if (isRoot && fileExists(tok) && i + 1 == args.size())
        {
            inv.cmd = spec.findById("info");
            inv.invokedPath = {"info"};
            runLeaf(inv, std::vector<std::string>(args.begin() + i,
                                                  args.end()));
        }
        if (isRoot && i + 1 < args.size() && tok != "read")
        {
            // the "gdal <FILE>" info shortcut also swallows unknown
            // commands whose arguments contain existing datasets
            std::vector<std::string> rest(args.begin() + i, args.end());
            bool anyExist = false;
            for (const auto &a2 : rest)
                if (fileExists(a2))
                    anyExist = true;
            if (anyExist && infoAttemptEmulate(rest))
                return 1;
        }
        std::string suggestion = suggestSubcommand(*cur, tok);
        if (!suggestion.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Algorithm '%s' is unknown. Do you mean "
                                  "'%s'?",
                                  tok.c_str(), suggestion.c_str()));
            return 1;
        }
        std::string msg =
            (isRoot ? std::string("gdal") : cur->path.back()) +
            ": Unknown command: '" + tok + "'";
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        printNonLeafUsageError(*cur);
        return 1;
    }
}
