#include "cpl.h"
#include "util.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

static std::string g_lastErrorMsg;
static int g_lastErrorNo = 0;
static CPLErrClass g_lastErrorType = CE_None;
static std::map<std::string, std::string> g_config;

static long g_errorSeq = 0;
static int g_quietDepth = 0;
static int g_debugMuteDepth = 0;
static std::vector<bool> g_quietMuteStack;

void cplPushQuietHandler(bool muteDebug)
{
    ++g_quietDepth;
    g_quietMuteStack.push_back(muteDebug);
    if (muteDebug)
        ++g_debugMuteDepth;
}

void cplPopHandler()
{
    if (g_quietDepth > 0)
        --g_quietDepth;
    if (!g_quietMuteStack.empty())
    {
        if (g_quietMuteStack.back() && g_debugMuteDepth > 0)
            --g_debugMuteDepth;
        g_quietMuteStack.pop_back();
    }
}

bool cplQuietActive()
{
    return g_quietDepth > 0;
}

int cplSuspendQuiet()
{
    int d = g_quietDepth;
    g_quietDepth = 0;
    return d;
}

void cplRestoreQuiet(int depth)
{
    g_quietDepth = depth;
}

static void emit(CPLErrClass eclass, int num, const std::string &msg)
{
    if (g_quietDepth > 0)
    {
        if (eclass == CE_Failure || eclass == CE_Fatal)
        {
            g_lastErrorMsg = msg;
            g_lastErrorNo = num;
            g_lastErrorType = eclass;
            ++g_errorSeq;
        }
        return;
    }
    if (eclass == CE_Failure || eclass == CE_Fatal)
    {
        g_lastErrorMsg = msg;
        g_lastErrorNo = num;
        g_lastErrorType = eclass;
        ++g_errorSeq;
        fprintf(stderr, "ERROR %d: %s\n", num, msg.c_str());
    }
    else if (eclass == CE_Warning)
    {
        g_lastErrorType = eclass;
        ++g_errorSeq;
        fprintf(stderr, "Warning %d: %s\n", num, msg.c_str());
    }
    fflush(stderr);
}

long cplErrorSeq()
{
    return g_errorSeq;
}

void cplError(CPLErrClass eclass, int num, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *buf = nullptr;
    if (vasprintf(&buf, fmt, ap) < 0)
        buf = nullptr;
    va_end(ap);
    emit(eclass, num, buf ? buf : "");
    free(buf);
}

void cplErrorStr(CPLErrClass eclass, int num, const std::string &msg)
{
    emit(eclass, num, msg);
}

std::string cplGetLastErrorMsg()
{
    return g_lastErrorMsg;
}

int cplGetLastErrorNo()
{
    return g_lastErrorNo;
}

void cplResetLastError()
{
    g_lastErrorMsg.clear();
    g_lastErrorNo = 0;
    g_lastErrorType = CE_None;
}

CPLErrClass cplGetLastErrorType()
{
    return g_lastErrorType;
}

void configSet(const std::string &key, const std::string &value)
{
    g_config[key] = value;
}

std::string configGet(const std::string &key, const std::string &def)
{
    auto it = g_config.find(key);
    if (it != g_config.end())
        return it->second;
    const char *env = getenv(key.c_str());
    if (env)
        return env;
    return def;
}

bool configIsSet(const std::string &key)
{
    return g_config.count(key) || getenv(key.c_str()) != nullptr;
}

bool configTestBool(const std::string &key, bool def)
{
    std::string v = configGet(key, "");
    if (v.empty())
        return def;
    return !(v == "NO" || v == "no" || v == "FALSE" || v == "false" ||
             v == "0" || v == "OFF" || v == "off");
}

bool cplDebugEnabled(const std::string &category)
{
    if (g_debugMuteDepth > 0)
        return false;
    if (!configIsSet("CPL_DEBUG"))
        return false;
    std::string v = configGet("CPL_DEBUG");
    // an empty value enables all categories, like ON
    if (v.empty() || strEqualNoCase(v, "ON") || strEqualNoCase(v, "YES") ||
        strEqualNoCase(v, "TRUE") || v == "1")
        return true;
    if (strEqualNoCase(v, "NO") || strEqualNoCase(v, "OFF") ||
        strEqualNoCase(v, "FALSE") || v == "0")
        return false;
    return strToLower(v).find(strToLower(category)) != std::string::npos;
}

void cplDebug(const std::string &category, const std::string &msg)
{
    if (!cplDebugEnabled(category))
        return;
    fprintf(stderr, "%s: %s\n", category.c_str(), msg.c_str());
    fflush(stderr);
}

std::string cplDebugPtr()
{
    static unsigned long long addr = 0x58d2374f5610ULL;
    addr += 0x39f0;
    return strPrintf("0x%llx", addr);
}

// GDALG is not a registered driver in the reference build (GDAL_SKIP=GDALG
// warns)
static const char *kRegisteredDrivers[] = {
    "GTiff",   "COG",        "VRT",      "MEM",
    "GeoJSON", "GeoJSONSeq", "ESRIJSON", "TopoJSON", "ESRI Shapefile"};

static std::vector<std::string> gdalSkipTokens()
{
    std::vector<std::string> toks;
    std::string v = configGet("GDAL_SKIP", "");
    // a comma anywhere switches to comma-only splitting, which is the
    // only way to skip space-bearing names like "ESRI Shapefile"
    bool commaMode = v.find(',') != std::string::npos;
    std::string cur;
    for (char c : v)
    {
        if (commaMode ? c == ',' : (c == ' ' || c == '\t'))
        {
            if (!cur.empty())
                toks.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty())
        toks.push_back(cur);
    return toks;
}

bool gdalSkipHas(const std::string &driverName)
{
    for (const auto &t : gdalSkipTokens())
        if (strEqualNoCase(t, driverName))
            return true;
    return false;
}

void gdalSkipStartupWarnings()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (const auto &t : gdalSkipTokens())
    {
        bool known = false;
        for (const char *d : kRegisteredDrivers)
            if (strEqualNoCase(t, d))
                known = true;
        if (known)
            cplDebug("GDAL", "AutoSkipDriver(" + t + ")");
        else
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Unable to find driver " + t +
                            " to unload from GDAL_SKIP environment "
                            "variable.");
    }
}

static long long usablePhysicalRam()
{
    long long ram =
        (long long)sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGESIZE);
    // cgroup pseudo-files stat as size 0, so read them with stdio
    for (const char *p : {"/sys/fs/cgroup/memory.max",
                          "/sys/fs/cgroup/memory/memory.limit_in_bytes"})
    {
        FILE *f = fopen(p, "rb");
        if (!f)
            continue;
        char buf[64] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        long long v = atoll(buf);
        if (v > 0 && v < ram)
            ram = v;
        break;
    }
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        (long long)rl.rlim_cur < ram)
        ram = (long long)rl.rlim_cur;
    return ram;
}

long long gdalDefaultCacheMax()
{
    std::string v = configGet("GDAL_CACHEMAX", "");
    if (v.empty())
        v = "5%";
    if (v.find('%') != std::string::npos)
        return usablePhysicalRam() / 100 * atoll(v.c_str());
    long long n = atoll(v.c_str());
    if (n < 100000)
        n *= 1024 * 1024;
    return n;
}

void gdalDebugCacheMaxOnce()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    cplDebug("GDAL", strPrintf("GDAL_CACHEMAX = %lld MB",
                               gdalDefaultCacheMax() / (1024 * 1024)));
}

bool gdalPamEnabled()
{
    if (configTestBool("GDAL_PAM_ENABLED", true))
        return true;
    static bool noted = false;
    if (!noted)
    {
        noted = true;
        cplDebug("GDAL", "PAM is disabled. Further messages of this type "
                         "will be suppressed.");
    }
    return false;
}
