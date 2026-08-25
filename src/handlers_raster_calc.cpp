#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "progress.h"
#include "spec.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace
{

struct PrefixScope
{
    bool active;
    explicit PrefixScope(const char *name)
    {
        active = g_pipelineStepPrefix.empty();
        if (active)
            g_pipelineStepPrefix = name;
    }
    ~PrefixScope()
    {
        if (active)
            g_pipelineStepPrefix.clear();
    }
};

std::string fmt18(double d)
{
    if (std::isnan(d))
        return "nan";
    return strPrintf("%.18g", d);
}

std::string fmtG(double d)
{
    return strPrintf("%g", d);
}

std::string xmlEscA(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '&':
                r += "&amp;";
                break;
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            case '"':
                r += "&quot;";
                break;
            default:
                r += c;
        }
    }
    return r;
}

std::string xmlEscT(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '&':
                r += "&amp;";
                break;
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            default:
                r += c;
        }
    }
    return r;
}

std::string gdalgQuote(const std::string &tok)
{
    if (tok.find_first_of(" \",\\") == std::string::npos)
        return tok;
    std::string r = "\"";
    for (char c : tok)
    {
        if (c == '"' || c == '\\')
            r += '\\';
        r += c;
    }
    r += '"';
    return r;
}

std::string dirNameLocal(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

std::string relToOutput(const std::string &input, const std::string &output,
                        int &relative)
{
    std::string outDir = dirNameLocal(output);
    if (outDir.empty())
    {
        relative = !input.empty() && input[0] != '/' &&
                           input.find(':') == std::string::npos
                       ? 1
                       : 0;
        return input;
    }
    if (input.size() > outDir.size() + 1 &&
        input.compare(0, outDir.size(), outDir) == 0 &&
        input[outDir.size()] == '/')
    {
        relative = 1;
        return input.substr(outDir.size() + 1);
    }
    relative = 0;
    return input;
}

// CPLStrtod-flavoured scan: leading spaces, sign, nan/inf spellings, no hex
bool scanDouble(const char *&p, double &out, bool longInf = true)
{
    const char *q = p;
    while (*q == ' ' || *q == '\t')
        q++;
    const char *s = q;
    if (*q == '+' || *q == '-')
        q++;
    auto ciMatch = [&](const char *word) -> bool
    {
        const char *a = q;
        for (const char *w = word; *w; ++w, ++a)
            if (tolower((unsigned char)*a) != *w)
                return false;
        q = a;
        return true;
    };
    if (ciMatch("nan"))
    {
        out = std::nan("");
        p = q;
        return true;
    }
    if ((longInf && ciMatch("infinity")) || ciMatch("inf"))
    {
        out = (*s == '-') ? -HUGE_VAL : HUGE_VAL;
        p = q;
        return true;
    }
    bool any = false;
    while (isdigit((unsigned char)*q))
    {
        q++;
        any = true;
    }
    if (*q == '.')
    {
        q++;
        while (isdigit((unsigned char)*q))
        {
            q++;
            any = true;
        }
    }
    if (!any)
        return false;
    if (*q == 'e' || *q == 'E')
    {
        const char *e = q + 1;
        if (*e == '+' || *e == '-')
            e++;
        if (isdigit((unsigned char)*e))
        {
            q = e;
            while (isdigit((unsigned char)*q))
                q++;
        }
    }
    out = strtod(std::string(s, (size_t)(q - s)).c_str(), nullptr);
    p = q;
    return true;
}

bool parseFullDouble(const std::string &tok, double &out)
{
    const char *p = tok.c_str();
    if (!scanDouble(p, out))
        return false;
    return *p == '\0';
}

// interval lower bounds: full-token parse where the negative-infinity
// spelling stops after "-inf" (CPLStrtod quirk), so "-infinity" fails
// while "infinity" parses greedily
bool parseBoundFull(const std::string &tok, double &out)
{
    const char *p = tok.c_str();
    while (*p == ' ' || *p == '\t')
        p++;
    auto ciStarts = [&](const char *word) -> bool
    {
        const char *a = p;
        for (const char *w = word; *w; ++w, ++a)
            if (tolower((unsigned char)*a) != *w)
                return false;
        return true;
    };
    if (ciStarts("-inf"))
    {
        out = -HUGE_VAL;
        p += 4;
    }
    else if (!scanDouble(p, out))
        return false;
    return *p == '\0';
}

bool valueIsNodata(double v, bool hasNd, double nd)
{
    if (!hasNd)
        return false;
    return v == nd || (std::isnan(v) && std::isnan(nd));
}

bool dtypeFloating(DType t)
{
    return t == DType::Float16 || t == DType::Float32 ||
           t == DType::Float64 || t == DType::CFloat32 ||
           t == DType::CFloat64;
}

// exact representability in the output type (reclassify dest validation)
bool valueExactAs(double v, DType t)
{
    switch (t)
    {
        case DType::Float64:
        case DType::CFloat64:
            return true;
        case DType::Float32:
        case DType::CFloat32:
            if (std::isnan(v) || std::isinf(v))
                return true;
            return (double)(float)v == v;
        case DType::Float16:
            if (std::isnan(v) || std::isinf(v))
                return true;
            return (double)tailHalfToFloat(tailFloatToHalf((float)v)) == v;
        default:
            break;
    }
    if (std::isnan(v) || v != std::floor(v))
        return false;
    switch (t)
    {
        case DType::Byte:
            return v >= 0 && v <= 255;
        case DType::Int8:
            return v >= -128 && v <= 127;
        case DType::UInt16:
            return v >= 0 && v <= 65535;
        case DType::Int16:
        case DType::CInt16:
            return v >= -32768 && v <= 32767;
        case DType::UInt32:
            return v >= 0 && v <= 4294967295.0;
        case DType::Int32:
        case DType::CInt32:
            return v >= -2147483648.0 && v <= 2147483647.0;
        case DType::UInt64:
            return v >= 0 && v < 18446744073709551616.0;
        case DType::Int64:
            return v >= -9223372036854775808.0 &&
                   v < 9223372036854775808.0;
        default:
            return false;
    }
}

// ------------------------------------------------------------------
// reclassify mapping
// ------------------------------------------------------------------

enum class ReclassDst
{
    Value,
    NoData,
    PassThrough
};

struct ReclassEntry
{
    double lo = 0, hi = 0;
    bool loInc = true, hiInc = true;
    bool isNan = false;
    bool isDefault = false;
    ReclassDst dst = ReclassDst::Value;
    double dstVal = 0;
};

struct ReclassMap
{
    std::vector<ReclassEntry> entries;  // in declaration order
    bool hasNan = false, hasDefault = false;
    size_t nanIdx = 0, defIdx = 0;
};

bool ciTokenMatch(const char *&p, const char *word)
{
    const char *a = p;
    for (const char *w = word; *w; ++w, ++a)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*w))
            return false;
    if (isalnum((unsigned char)*a) || *a == '_')
        return false;
    p = a;
    return true;
}

bool exactTokenMatch(const char *&p, const char *word)
{
    const char *a = p;
    for (const char *w = word; *w; ++w, ++a)
        if (*a != *w)
            return false;
    if (isalnum((unsigned char)*a) || *a == '_')
        return false;
    p = a;
    return true;
}

// resolved destination display value used by the overlap message
double reclassDstDisplay(const ReclassEntry &e, bool hasNd, double nd)
{
    if (e.dst == ReclassDst::Value)
        return e.dstVal;
    if (e.dst == ReclassDst::NoData)
        return hasNd ? nd : 0;
    return std::nan("");
}

std::string reclassParse(const std::string &mapping, bool hasNd, double nd,
                         ReclassMap &m)
{
    std::vector<std::string> pieces;
    if (!mapping.empty())
    {
        size_t start = 0;
        for (size_t i = 0; i <= mapping.size(); ++i)
            if (i == mapping.size() || mapping[i] == ';')
            {
                pieces.push_back(mapping.substr(start, i - start));
                start = i + 1;
            }
    }
    for (const std::string &piece : pieces)
    {
        ReclassEntry e;
        const char *p = piece.c_str();
        while (*p == ' ' || *p == '\t')
            p++;
        double v;
        if (ciTokenMatch(p, "NO_DATA"))
        {
            if (!hasNd)
                return "Value mapped from NO_DATA, but NoData value is "
                       "not set";
            if (std::isnan(nd))
                e.isNan = true;
            else
                e.lo = e.hi = nd;
        }
        else if (ciTokenMatch(p, "DEFAULT"))
        {
            e.isDefault = true;
        }
        else if (*p == '[' || *p == '(')
        {
            e.loInc = *p == '[';
            p++;
            const char *comma = strchr(p, ',');
            if (!comma ||
                !parseBoundFull(std::string(p, (size_t)(comma - p)),
                                e.lo))
                return "Expected a number";
            p = comma + 1;
            if (!scanDouble(p, e.hi, false) ||
                (*p != ')' && *p != ']'))
                return "Interval must end with ')' or ']";
            e.hiInc = *p == ']';
            p++;
            if (e.lo > e.hi)
                return "Lower bound of interval must be lower or equal "
                       "to upper bound";
            if (std::isnan(e.lo) || std::isnan(e.hi))
                return "NaN is not a valid value for bounds of "
                       "interval";
        }
        else if (scanDouble(p, v))
        {
            if (std::isnan(v))
                e.isNan = true;
            else
                e.lo = e.hi = v;
        }
        else
        {
            return "Interval must start with '(' or '['";
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '=')
            return std::string("Failed to parse mapping (expected '=', "
                               "got '") +
                   *p + "')";
        p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (exactTokenMatch(p, "NO_DATA"))
        {
            if (!hasNd)
                return "Value mapped to NO_DATA, but NoData value is "
                       "not set";
            e.dst = ReclassDst::NoData;
        }
        else if (exactTokenMatch(p, "PASS_THROUGH"))
        {
            e.dst = ReclassDst::PassThrough;
        }
        else if (scanDouble(p, e.dstVal))
        {
            e.dst = ReclassDst::Value;
        }
        else
        {
            return "Failed to parse output value (expected number or "
                   "NO_DATA)";
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '\0')
            return std::string("Failed to parse mapping (expected ';' or "
                               "end of string, got '") +
                   *p + "')";
        if (e.isNan)
        {
            m.hasNan = true;
            m.nanIdx = m.entries.size();
        }
        else if (e.isDefault)
        {
            m.hasDefault = true;
            m.defIdx = m.entries.size();
        }
        m.entries.push_back(e);
    }
    // overlap detection among the numeric intervals
    std::vector<size_t> idx;
    for (size_t i = 0; i < m.entries.size(); ++i)
        if (!m.entries[i].isNan && !m.entries[i].isDefault)
            idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t a, size_t b)
                     { return m.entries[a].lo < m.entries[b].lo; });
    for (size_t k = 1; k < idx.size(); ++k)
    {
        const ReclassEntry &a = m.entries[idx[k - 1]];
        const ReclassEntry &b = m.entries[idx[k]];
        bool overlap = b.lo < a.hi ||
                       (b.lo == a.hi && a.hiInc && b.loInc);
        if (overlap)
            return strPrintf(
                "Interval from %g to %g (mapped to %g) overlaps with "
                "interval from %g to %g (mapped to %g)",
                a.lo, a.hi, reclassDstDisplay(a, hasNd, nd), b.lo, b.hi,
                reclassDstDisplay(b, hasNd, nd));
    }
    return "";
}

// explicit destination values must fit the output type exactly
std::string reclassValidateDst(const ReclassMap &m, DType t, bool hasNd,
                               double nd)
{
    for (const ReclassEntry &e : m.entries)
    {
        double v;
        if (e.dst == ReclassDst::Value)
            v = e.dstVal;
        else if (e.dst == ReclassDst::NoData)
            v = hasNd ? nd : 0;
        else
            continue;
        if (!valueExactAs(v, t))
            return strPrintf("Value %g cannot be represented as data "
                             "type %s",
                             v, dtypeName(t));
    }
    return "";
}

// 0 = mapped, 1 = unmapped (err filled)
int reclassApply(const ReclassMap &m, double v, bool hasNd, double nd,
                 double &out, std::string &err)
{
    const ReclassEntry *hit = nullptr;
    if (std::isnan(v))
    {
        if (m.hasNan)
            hit = &m.entries[m.nanIdx];
    }
    else
    {
        for (const ReclassEntry &e : m.entries)
        {
            if (e.isNan || e.isDefault)
                continue;
            bool loOk = e.loInc ? v >= e.lo : v > e.lo;
            bool hiOk = e.hiInc ? v <= e.hi : v < e.hi;
            if (loOk && hiOk)
            {
                hit = &e;
                break;
            }
        }
    }
    if (!hit && m.hasDefault)
        hit = &m.entries[m.defIdx];
    if (!hit)
    {
        err = strPrintf("Encountered value %g with no specified mapping",
                        v);
        return 1;
    }
    switch (hit->dst)
    {
        case ReclassDst::Value:
            out = hit->dstVal;
            break;
        case ReclassDst::NoData:
            out = nd;
            break;
        case ReclassDst::PassThrough:
            out = v;
            break;
    }
    (void)hasNd;
    return 0;
}

// @file expansion: separator committed before comment stripping, blank
// lines skipped outright
std::string reclassMappingFromFile(const std::string &content)
{
    std::string res;
    size_t start = 0;
    for (size_t i = 0; i <= content.size(); ++i)
    {
        if (i != content.size() && content[i] != '\n')
            continue;
        std::string line = content.substr(start, i - start);
        start = i + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (!res.empty())
            res += ';';
        size_t hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        res += line;
    }
    return res;
}

// ------------------------------------------------------------------
// calc expression parsing
// ------------------------------------------------------------------

struct CalcArg
{
    std::string key, val;
};

struct CalcExpr
{
    std::string raw;
    std::string func;
    std::vector<CalcArg> args;
};

std::string trimWs(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

CalcExpr calcParseExpr(const std::string &raw)
{
    CalcExpr e;
    e.raw = raw;
    std::vector<std::string> toks;
    std::string cur;
    for (char c : raw)
    {
        if (c == '(' || c == ')' || c == ',')
        {
            toks.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    toks.push_back(cur);
    e.func = trimWs(toks.empty() ? "" : toks[0]);
    for (size_t i = 1; i < toks.size(); ++i)
    {
        size_t eq = toks[i].find('=');
        if (eq == std::string::npos)
            continue;
        CalcArg a;
        a.key = trimWs(toks[i].substr(0, eq));
        a.val = trimWs(toks[i].substr(eq + 1));
        e.args.push_back(a);
    }
    return e;
}

const CalcArg *calcFindArg(const CalcExpr &e, const char *key)
{
    const CalcArg *hit = nullptr;
    for (const CalcArg &a : e.args)
        if (a.key == key)
            hit = &a;  // last one wins
    return hit;
}

enum CalcFuncId
{
    F_SUM,
    F_MUL,
    F_MIN,
    F_MAX,
    F_MEAN,
    F_MEDIAN,
    F_MODE,
    F_GEOMEAN,
    F_HARMEAN,
    F_SQRT,
    F_EXP,
    F_LOG10,
    F_ROUND,
    F_INV,
    F_DB,
    F_DB2AMP,
    F_DB2POW,
    F_POW,
    F_INTERP_LIN,
    F_INTERP_EXP,
    F_SCALE,
    F_NORMDIFF,
    F_DIFF,
    F_DIV,
    F_CMUL,
    F_COMPLEX,
    F_POLAR,
    F_REAL,
    F_IMAG,
    F_MOD,
    F_PHASE,
    F_CONJ,
    F_INTENSITY,
    F_REPLACE_ND,
    F_RECLASSIFY,
    F_EXPRESSION
};

// source-count classes
enum CalcSrcClass
{
    SC_ANY,         // aggregates: 1..N
    SC_ONE,         // exactly one source, silent failure otherwise
    SC_TWO,         // exactly two sources, silent failure otherwise
    SC_MUL,         // >=2 or constant k
    SC_INTERP       // >=2 with its own message
};

// default nodata behaviour
enum CalcNdMode
{
    ND_SKIP,  // aggregates skip nodata inputs
    ND_PROP,  // any nodata input propagates
    ND_RAW    // computes on raw values
};

struct CalcFuncInfo
{
    const char *name;
    CalcFuncId id;
    CalcSrcClass srcClass;
    CalcNdMode ndMode;
    bool preserveType;  // keeps common source type instead of Float64
    const char *allowed;  // pre-rendered arg list for the E5 message
};

const CalcFuncInfo kCalcFuncs[] = {
    {"sum", F_SUM, SC_ANY, ND_SKIP, false,
     "Only are 'k', 'propagateNoData' supported"},
    {"mul", F_MUL, SC_MUL, ND_SKIP, false,
     "Only are 'k', 'propagateNoData' supported"},
    {"min", F_MIN, SC_ANY, ND_SKIP, true,
     "Only are 'k', 'propagateNoData' supported"},
    {"max", F_MAX, SC_ANY, ND_SKIP, true,
     "Only are 'k', 'propagateNoData' supported"},
    {"mean", F_MEAN, SC_ANY, ND_SKIP, false,
     "Only is 'propagateNoData' supported"},
    {"median", F_MEDIAN, SC_ANY, ND_SKIP, false,
     "Only is 'propagateNoData' supported"},
    {"mode", F_MODE, SC_ANY, ND_SKIP, true,
     "Only is 'propagateNoData' supported"},
    {"geometric_mean", F_GEOMEAN, SC_ANY, ND_SKIP, false,
     "Only is 'propagateNoData' supported"},
    {"harmonic_mean", F_HARMEAN, SC_ANY, ND_SKIP, false,
     "Only is 'propagateNoData' supported"},
    {"sqrt", F_SQRT, SC_ONE, ND_PROP, false,
     "It does not accept any argument"},
    {"exp", F_EXP, SC_ONE, ND_PROP, false,
     "Only are 'base', 'fact' supported"},
    {"log10", F_LOG10, SC_ONE, ND_PROP, false,
     "It does not accept any argument"},
    {"round", F_ROUND, SC_ONE, ND_PROP, false,
     "Only is 'digits' supported"},
    {"inv", F_INV, SC_ONE, ND_PROP, false, "Only is 'k' supported"},
    {"dB", F_DB, SC_ONE, ND_PROP, false, "Only is 'fact' supported"},
    {"dB2amp", F_DB2AMP, SC_ONE, ND_PROP, false,
     "It does not accept any argument"},
    {"dB2pow", F_DB2POW, SC_ONE, ND_PROP, false,
     "It does not accept any argument"},
    {"pow", F_POW, SC_ONE, ND_PROP, false, "Only is 'power' supported"},
    {"interpolate_linear", F_INTERP_LIN, SC_INTERP, ND_PROP, false,
     "Only are 't0', 'dt', 't' supported"},
    {"interpolate_exp", F_INTERP_EXP, SC_INTERP, ND_PROP, false,
     "Only are 't0', 'dt', 't' supported"},
    {"scale", F_SCALE, SC_ONE, ND_PROP, false,
     "It does not accept any argument"},
    {"norm_diff", F_NORMDIFF, SC_TWO, ND_PROP, false,
     "It does not accept any argument"},
    {"diff", F_DIFF, SC_TWO, ND_PROP, false,
     "It does not accept any argument"},
    {"div", F_DIV, SC_TWO, ND_PROP, false,
     "It does not accept any argument"},
    {"cmul", F_CMUL, SC_TWO, ND_RAW, false,
     "It does not accept any argument"},
    {"complex", F_COMPLEX, SC_TWO, ND_PROP, true,
     "It does not accept any argument"},
    {"polar", F_POLAR, SC_TWO, ND_RAW, false,
     "Only is 'amplitude_type' supported"},
    {"real", F_REAL, SC_ONE, ND_PROP, true,
     "It does not accept any argument"},
    {"imag", F_IMAG, SC_ONE, ND_PROP, true,
     "It does not accept any argument"},
    {"mod", F_MOD, SC_ONE, ND_PROP, true,
     "It does not accept any argument"},
    {"phase", F_PHASE, SC_ONE, ND_PROP, true,
     "It does not accept any argument"},
    {"conj", F_CONJ, SC_ONE, ND_PROP, true,
     "It does not accept any argument"},
    {"intensity", F_INTENSITY, SC_ONE, ND_PROP, false,
     "It does not accept any argument"},
    {"replace_nodata", F_REPLACE_ND, SC_ONE, ND_RAW, false,
     "Only is 'to' supported"},
    {"reclassify", F_RECLASSIFY, SC_ONE, ND_RAW, false,
     "Only is 'mapping' supported"},
    {"expression", F_EXPRESSION, SC_ANY, ND_RAW, false,
     "Only are 'propagateNoData', 'expression', 'dialect' supported"},
};

const CalcFuncInfo *calcFuncLookup(const std::string &name)
{
    for (const CalcFuncInfo &f : kCalcFuncs)
        if (name == f.name)
            return &f;
    return nullptr;
}

bool calcArgAllowed(const CalcFuncInfo &f, const std::string &key)
{
    switch (f.id)
    {
        case F_SUM:
        case F_MUL:
        case F_MIN:
        case F_MAX:
            return key == "k" || key == "propagateNoData";
        case F_MEAN:
        case F_MEDIAN:
        case F_MODE:
        case F_GEOMEAN:
        case F_HARMEAN:
            return key == "propagateNoData";
        case F_EXP:
            return key == "base" || key == "fact";
        case F_ROUND:
            return key == "digits";
        case F_INV:
            return key == "k";
        case F_DB:
            return key == "fact";
        case F_POW:
            return key == "power";
        case F_INTERP_LIN:
        case F_INTERP_EXP:
            return key == "t0" || key == "dt" || key == "t";
        case F_POLAR:
            return key == "amplitude_type";
        case F_REPLACE_ND:
            return key == "to";
        case F_RECLASSIFY:
            return key == "mapping";
        case F_EXPRESSION:
            return key == "propagateNoData" || key == "expression" ||
                   key == "dialect";
        default:
            return false;
    }
}

// ------------------------------------------------------------------
// calc evaluation
// ------------------------------------------------------------------

struct CalcFail
{
    bool failed = false;
    bool silent = false;
    int errNum = CPLE_AppDefined;
    std::string msg;
    int failBand = 1;  // 1-based output band the failure surfaced on

    void set(int num, const std::string &m)
    {
        failed = true;
        silent = false;
        errNum = num;
        msg = m;
    }
    void setSilent()
    {
        failed = true;
        silent = true;
    }
};

struct CalcSourceRef
{
    RasterDatasetBase *ds = nullptr;
    size_t inputIdx = 0;
    int band = 1;
};

class CalcDataset final : public RasterDatasetBase
{
  public:
    std::vector<std::unique_ptr<RasterDatasetBase>> inputs;  // sorted
    std::vector<std::string> inputNames;   // sorted, parallel
    std::vector<std::string> inputPaths;   // as typed, parallel
    std::vector<CalcExpr> exprs;
    bool flatten = false;
    bool cliPropagate = false;
    int bandsPerExpr = 1;
    bool outNdSet = false;
    double outNd = 0;
    bool singleInput = false;

    std::vector<std::vector<double>> data;  // per out band
    bool evaluated = false;

    bool readBand(int band, std::vector<double> &out) override
    {
        if (!evaluated)
            return false;
        out = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        for (double &v : out)
            v = rasterFinishReal(v, t);
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        if (!evaluated)
            return false;
        const std::vector<double> &src = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        size_t sz = (size_t)dtypeSizeBytes(t);
        out.assign(src.size() * sz, 0);
        for (size_t i = 0; i < src.size(); ++i)
            rasterEncodeReal(t, out.data() + i * sz,
                             rasterFinishReal(src[i], t), 0);
        return true;
    }

    // sources feeding a given output band (order = alphabetical inputs)
    std::vector<CalcSourceRef> bandSources(int outBand) const
    {
        std::vector<CalcSourceRef> refs;
        int bi = flatten ? 0 : (outBand - 1) % bandsPerExpr;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            RasterDatasetBase *ds = inputs[i].get();
            if (flatten)
            {
                for (int b = 1; b <= (int)ds->bands.size(); ++b)
                    refs.push_back({ds, i, b});
            }
            else
            {
                int b = (int)ds->bands.size() > 1 ? bi + 1 : 1;
                refs.push_back({ds, i, b});
            }
        }
        return refs;
    }

    const CalcExpr &bandExpr(int outBand) const
    {
        return exprs[(size_t)(outBand - 1) / (size_t)bandsPerExpr];
    }

    bool evalAll(CalcFail &fail, bool configOnly = false);

    std::string customVrtXml(const std::string &input,
                             const std::string &output) override;

  private:
    // per-(input,band) source pixel cache resampled to the output grid
    std::map<std::pair<size_t, int>, std::vector<double>> srcCache;
    const std::vector<double> &srcPixels(const CalcSourceRef &ref);
    bool evalBand(int outBand, CalcFail &fail, bool configOnly);
};

const std::vector<double> &CalcDataset::srcPixels(const CalcSourceRef &ref)
{
    auto key = std::make_pair(ref.inputIdx, ref.band);
    auto it = srcCache.find(key);
    if (it != srcCache.end())
        return it->second;
    std::vector<double> native;
    ref.ds->readBand(ref.band, native);
    std::vector<double> out;
    int sw = ref.ds->width, sh = ref.ds->height;
    if (sw == width && sh == height)
        out = std::move(native);
    else
    {
        out.resize((size_t)width * (size_t)height);
        for (int y = 0; y < height; ++y)
        {
            int sy = (int)(((double)y + 0.5) * sh / height);
            if (sy >= sh)
                sy = sh - 1;
            for (int x = 0; x < width; ++x)
            {
                int sx = (int)(((double)x + 0.5) * sw / width);
                if (sx >= sw)
                    sx = sw - 1;
                out[(size_t)y * width + x] =
                    native[(size_t)sy * sw + sx];
            }
        }
    }
    return srcCache.emplace(key, std::move(out)).first->second;
}

bool CalcDataset::evalAll(CalcFail &fail, bool configOnly)
{
    for (int b = 1; b <= (int)bands.size(); ++b)
    {
        if (!evalBand(b, fail, configOnly))
        {
            fail.failBand = b;
            return false;
        }
    }
    if (!configOnly)
        evaluated = true;
    return true;
}

bool CalcDataset::evalBand(int outBand, CalcFail &fail, bool configOnly)
{
    const CalcExpr &e = bandExpr(outBand);
    const CalcFuncInfo *f = calcFuncLookup(e.func);
    std::vector<CalcSourceRef> refs = bandSources(outBand);
    size_t n = refs.size();

    // ---- per-function configuration checks ----
    double kVal = 0;
    bool kSet = false;
    ReclassMap rmap;
    int polarMode = 0;  // 0 amplitude, 1 intensity, 2 dB

    auto parseNamed = [&](const char *key, double &out, bool &set,
                          bool required) -> bool
    {
        const CalcArg *a = calcFindArg(e, key);
        if (!a)
        {
            if (required)
            {
                fail.set(CPLE_AppDefined,
                         std::string("Missing pixel function argument: ") +
                             key);
                return false;
            }
            return true;
        }
        if (!parseFullDouble(a->val, out))
        {
            fail.set(CPLE_AppDefined,
                     std::string(
                         "Failed to parse pixel function argument: ") +
                         key);
            return false;
        }
        set = true;
        return true;
    };

    switch (f->srcClass)
    {
        case SC_ONE:
            if (n != 1)
            {
                fail.setSilent();
                return false;
            }
            break;
        case SC_TWO:
            if (n != 2)
            {
                fail.setSilent();
                return false;
            }
            break;
        default:
            break;
    }

    bool argPropagate = false;
    if (const CalcArg *pa = calcFindArg(e, "propagateNoData"))
        // CPLTestBool: everything is true except NO/FALSE/OFF/0
        argPropagate = !(strEqualNoCase(pa->val, "no") ||
                         strEqualNoCase(pa->val, "false") ||
                         strEqualNoCase(pa->val, "off") ||
                         pa->val == "0");
    bool propagate = cliPropagate || argPropagate;

    double base = M_E, factExp = 1, digits = 0, invK = 1, dbFact = 20,
           powVal = 0, t0 = 0, dt = 0, tVal = 0, toVal = std::nan("");
    bool powSet = false, t0Set = false, dtSet = false, tSet = false,
         toSet = false;

    switch (f->id)
    {
        case F_SUM:
        case F_MIN:
        case F_MAX:
            if (!parseNamed("k", kVal, kSet, false))
                return false;
            break;
        case F_MUL:
        {
            if (!parseNamed("k", kVal, kSet, false))
                return false;
            if (n < 2 && !kSet)
            {
                fail.set(CPLE_AppDefined,
                         "mul requires at least two sources or a "
                         "specified constant k");
                return false;
            }
            break;
        }
        case F_EXP:
        {
            bool baseSet = false, factSet = false;
            if (!parseNamed("base", base, baseSet, false))
                return false;
            if (!parseNamed("fact", factExp, factSet, false))
                return false;
            break;
        }
        case F_ROUND:
        {
            bool ds = false;
            if (!parseNamed("digits", digits, ds, false))
                return false;
            break;
        }
        case F_INV:
        {
            bool ks = false;
            if (!parseNamed("k", invK, ks, false))
                return false;
            break;
        }
        case F_DB:
        {
            bool fs = false;
            if (!parseNamed("fact", dbFact, fs, false))
                return false;
            break;
        }
        case F_POW:
            if (!parseNamed("power", powVal, powSet, true))
                return false;
            break;
        case F_INTERP_LIN:
        case F_INTERP_EXP:
        {
            if (!parseNamed("t0", t0, t0Set, true))
                return false;
            if (!parseNamed("t", tVal, tSet, true))
                return false;
            if (!parseNamed("dt", dt, dtSet, true))
                return false;
            // the reference validates t here but names dt in the message
            if (tVal == 0 || !std::isfinite(tVal))
            {
                fail.set(CPLE_AppDefined,
                         "dt must be finite and non-zero");
                return false;
            }
            if (n < 2)
            {
                fail.set(CPLE_AppDefined,
                         "At least two sources required for "
                         "interpolation.");
                return false;
            }
            break;
        }
        case F_POLAR:
        {
            const CalcArg *pa = calcFindArg(e, "amplitude_type");
            if (pa)
            {
                if (pa->val == "AMPLITUDE")
                    polarMode = 0;
                else if (pa->val == "INTENSITY")
                    polarMode = 1;
                else if (pa->val == "dB")
                    polarMode = 2;
                else
                {
                    fail.set(CPLE_AppDefined,
                             "Invalid value for pixel function argument "
                             "'amplitude_type': " +
                                 pa->val);
                    return false;
                }
            }
            break;
        }
        case F_REPLACE_ND:
        {
            const Band &sb = refs[0].ds->bands[(size_t)refs[0].band - 1];
            if (!sb.hasNodata)
            {
                fail.set(CPLE_AppDefined, "Raster has no NoData");
                return false;
            }
            if (!parseNamed("to", toVal, toSet, false))
                return false;
            DType ot = bands[(size_t)outBand - 1].type;
            if (std::isnan(toVal) && !dtypeFloating(ot))
            {
                fail.set(CPLE_AppDefined,
                         "Using nan requires a floating point type "
                         "output buffer");
                return false;
            }
            break;
        }
        case F_RECLASSIFY:
        {
            const CalcArg *ma = calcFindArg(e, "mapping");
            if (!ma)
            {
                fail.set(CPLE_AppDefined,
                         "reclassify must be called with 'mapping' "
                         "argument");
                return false;
            }
            const Band &sb = refs[0].ds->bands[(size_t)refs[0].band - 1];
            std::string perr = reclassParse(ma->val, sb.hasNodata,
                                            sb.nodata, rmap);
            if (perr.empty())
                perr = reclassValidateDst(
                    rmap, bands[(size_t)outBand - 1].type, sb.hasNodata,
                    sb.nodata);
            if (!perr.empty())
            {
                fail.set(CPLE_AppDefined, perr);
                return false;
            }
            break;
        }
        case F_EXPRESSION:
        {
            const CalcArg *xa = calcFindArg(e, "expression");
            if (!xa)
            {
                fail.set(CPLE_AppDefined,
                         "Missing 'expression' pixel function argument");
                return false;
            }
            const CalcArg *da = calcFindArg(e, "dialect");
            std::string dia = da ? da->val : "muparser";
            if (dia == "muparser")
                fail.set(CPLE_IllegalArg,
                         "Dialect 'muparser' is not supported by this "
                         "GDAL build. A GDAL build with muparser is "
                         "needed.");
            else
                fail.set(CPLE_IllegalArg,
                         "Unknown expression dialect: " + dia);
            return false;
        }
        default:
            break;
    }

    if (configOnly)
        return true;

    // ---- pixel loop ----
    size_t pixels = (size_t)width * (size_t)height;
    std::vector<const std::vector<double> *> src(n);
    std::vector<bool> srcHasNd(n);
    std::vector<double> srcNd(n);
    for (size_t i = 0; i < n; ++i)
    {
        src[i] = &srcPixels(refs[i]);
        const Band &sb = refs[i].ds->bands[(size_t)refs[i].band - 1];
        srcHasNd[i] = sb.hasNodata;
        srcNd[i] = sb.nodata;
    }
    std::vector<double> &out = data[(size_t)outBand - 1];
    out.resize(pixels);

    double scScale = 1, scOffset = 0;
    if (f->id == F_SCALE)
    {
        const Band &sb = refs[0].ds->bands[(size_t)refs[0].band - 1];
        if (sb.hasScale)
            scScale = sb.scale;
        if (sb.hasOffset)
            scOffset = sb.offset;
    }

    std::vector<double> vals;
    vals.reserve(n);
    for (size_t px = 0; px < pixels; ++px)
    {
        bool anyNd = false;
        for (size_t i = 0; i < n && !anyNd; ++i)
            if (valueIsNodata((*src[i])[px], srcHasNd[i], srcNd[i]))
                anyNd = true;
        if (anyNd && (propagate || f->ndMode == ND_PROP))
        {
            out[px] = outNd;
            continue;
        }

        double r = 0;
        if (f->ndMode == ND_SKIP)
        {
            vals.clear();
            for (size_t i = 0; i < n; ++i)
            {
                double v = (*src[i])[px];
                if (!valueIsNodata(v, srcHasNd[i], srcNd[i]))
                    vals.push_back(v);
            }
            size_t m = vals.size();
            switch (f->id)
            {
                case F_SUM:
                {
                    double acc = kSet ? kVal : 0;
                    for (double v : vals)
                        acc += v;
                    r = acc;
                    break;
                }
                case F_MUL:
                {
                    // an all-nodata pixel keeps the empty product
                    double acc = kSet ? kVal : 1;
                    for (double v : vals)
                        acc *= v;
                    r = acc;
                    break;
                }
                case F_MIN:
                case F_MAX:
                {
                    bool isMin = f->id == F_MIN;
                    bool have = false;
                    double m0 = 0;
                    for (double v : vals)
                    {
                        if (!have || (isMin ? v < m0 : v > m0))
                            m0 = v;
                        have = true;
                    }
                    if (kSet && (!have || (isMin ? kVal < m0 : kVal > m0)))
                    {
                        m0 = kVal;
                        have = true;
                    }
                    r = have ? m0 : outNd;
                    break;
                }
                case F_MEAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    double s = 0;
                    for (double v : vals)
                        s += v;
                    r = s * (1.0 / (double)m);
                    break;
                }
                case F_MEDIAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    std::sort(vals.begin(), vals.end());
                    if (m % 2)
                        r = vals[m / 2];
                    else
                        r = 0.5 * (vals[m / 2 - 1] + vals[m / 2]);
                    break;
                }
                case F_MODE:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    size_t bestCount = 0;
                    double best = 0;
                    for (size_t i = 0; i < m; ++i)
                    {
                        size_t c = 0;
                        for (size_t j = 0; j < m; ++j)
                            if (vals[j] == vals[i] ||
                                (std::isnan(vals[j]) &&
                                 std::isnan(vals[i])))
                                c++;
                        if (c > bestCount)
                        {
                            bestCount = c;
                            best = vals[i];
                        }
                    }
                    r = best;
                    break;
                }
                case F_GEOMEAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    double prod = 1;
                    for (double v : vals)
                        prod *= v;
                    r = std::pow(prod, 1.0 / (double)m);
                    break;
                }
                case F_HARMEAN:
                {
                    if (m == 0)
                    {
                        r = outNd;
                        break;
                    }
                    double s = 0;
                    for (double v : vals)
                        s += 1.0 / v;
                    // reference yields 0 unless the reciprocal sum is
                    // strictly positive (covers negatives and NaN)
                    r = s > 0 ? (double)m / s : 0.0;
                    break;
                }
                default:
                    r = 0;
                    break;
            }
        }
        else
        {
            double a = (*src[0])[px];
            double b = n > 1 ? (*src[1])[px] : 0;
            switch (f->id)
            {
                case F_SQRT:
                    r = std::sqrt(a);
                    break;
                case F_EXP:
                    r = std::pow(base, factExp * a);
                    break;
                case F_LOG10:
                    r = std::log10(std::fabs(a));
                    break;
                case F_ROUND:
                    r = std::round(a * std::pow(10.0, digits)) *
                        std::pow(10.0, -digits);
                    break;
                case F_INV:
                    r = invK / a;
                    break;
                case F_DB:
                    r = dbFact * std::log10(std::fabs(a));
                    break;
                case F_DB2AMP:
                    r = std::pow(10.0, a * 0.05);
                    break;
                case F_DB2POW:
                    r = std::pow(10.0, a * 0.1);
                    break;
                case F_POW:
                    r = std::pow(a, powVal);
                    break;
                case F_SCALE:
                    r = a * scScale + scOffset;
                    break;
                case F_NORMDIFF:
                    r = (a - b) / (a + b);
                    break;
                case F_DIFF:
                    r = a - b;
                    break;
                case F_DIV:
                    r = a / b;
                    break;
                case F_CMUL:
                    r = a * b;
                    break;
                case F_COMPLEX:
                    r = a;
                    break;
                case F_POLAR:
                {
                    double amp = a;
                    if (polarMode == 1)
                        amp = std::sqrt(a);
                    else if (polarMode == 2)
                        amp = std::pow(10.0, a / 20.0);
                    r = amp * std::cos(b);
                    break;
                }
                case F_REAL:
                case F_CONJ:
                    r = a;
                    break;
                case F_IMAG:
                    r = 0;
                    break;
                case F_MOD:
                    r = std::fabs(a);
                    break;
                case F_PHASE:
                    r = a < 0 ? M_PI : 0;
                    break;
                case F_INTENSITY:
                    r = a * a;
                    break;
                case F_REPLACE_ND:
                    r = valueIsNodata(a, srcHasNd[0], srcNd[0]) ? toVal
                                                                : a;
                    break;
                case F_RECLASSIFY:
                {
                    std::string uerr;
                    if (reclassApply(rmap, a, srcHasNd[0], srcNd[0], r,
                                     uerr))
                    {
                        fail.set(CPLE_AppDefined, uerr);
                        return false;
                    }
                    break;
                }
                case F_INTERP_LIN:
                case F_INTERP_EXP:
                {
                    double q = (tVal - t0) / dt;
                    long i = 0;
                    if (std::isnan(q) || q < 0)
                        i = 0;
                    else if (q >= (double)(n - 1))
                        i = (long)n - 2;
                    else
                        i = (long)std::floor(q);
                    double xi = t0 + (double)i * dt;
                    double va = (*src[(size_t)i])[px];
                    double vb = (*src[(size_t)i + 1])[px];
                    if (tVal == xi)
                        r = va;
                    else if (f->id == F_INTERP_LIN)
                        r = va + (tVal - xi) * ((vb - va) / dt);
                    else
                        r = va * std::exp(std::log(vb / va) / dt *
                                          (tVal - xi));
                    break;
                }
                default:
                    r = 0;
                    break;
            }
        }
        out[px] = r;
    }
    return true;
}

// ------------------------------------------------------------------
// derived-band VRT serialization (calc)
// ------------------------------------------------------------------

void emitVrtHeader(std::string &x, const RasterDatasetBase &ds)
{
    x += strPrintf("<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\">\n",
                   ds.width, ds.height);
    if (ds.hasSrs && ds.srs.valid())
    {
        std::vector<int> mapv = ds.srs.dataAxisToSRSAxisMapping();
        std::string mapping;
        for (int m : mapv)
        {
            if (!mapping.empty())
                mapping += ",";
            mapping += strPrintf("%d", m);
        }
        std::string wkt = ds.srs.wkt1Gdal();
        if (wkt.empty())
            wkt = ds.srs.wkt2SingleLine();
        x += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             xmlEscT(wkt) + "</SRS>\n";
    }
    if (ds.hasGT)
    {
        x += "  <GeoTransform>";
        for (int i = 0; i < 6; i++)
        {
            if (i)
                x += ",";
            x += strPrintf("%24.16e", ds.gt[i]);
        }
        x += "</GeoTransform>\n";
    }
}

void emitDerivedSource(std::string &x, RasterDatasetBase *sds,
                       const std::string &srcPath, int srcBand,
                       bool withProps, int outW, int outH,
                       const std::string &output)
{
    const Band &sb = sds->bands[(size_t)srcBand - 1];
    bool complexTag = sb.hasNodata;
    const char *tag = complexTag ? "ComplexSource" : "SimpleSource";
    int relative = 0;
    std::string rel = relToOutput(srcPath, output, relative);
    x += strPrintf("    <%s>\n", tag);
    x += strPrintf("      <SourceFilename relativeToVRT=\"%d\">%s"
                   "</SourceFilename>\n",
                   relative, xmlEscA(rel).c_str());
    x += strPrintf("      <SourceBand>%d</SourceBand>\n", srcBand);
    if (withProps)
    {
        int bw = 0, bh = 0;
        sds->realBlockDims(bw, bh);
        x += strPrintf("      <SourceProperties RasterXSize=\"%d\" "
                       "RasterYSize=\"%d\" DataType=\"%s\" "
                       "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                       sds->width, sds->height, dtypeName(sb.type), bw,
                       bh);
    }
    x += strPrintf("      <SrcRect xOff=\"0\" yOff=\"0\" xSize=\"%d\" "
                   "ySize=\"%d\" />\n",
                   sds->width, sds->height);
    x += strPrintf("      <DstRect xOff=\"0\" yOff=\"0\" xSize=\"%d\" "
                   "ySize=\"%d\" />\n",
                   outW, outH);
    if (complexTag)
        x += "      <NODATA>" + fmt18(sb.nodata) + "</NODATA>\n";
    x += strPrintf("    </%s>\n", tag);
}

std::string CalcDataset::customVrtXml(const std::string &input,
                                      const std::string &output)
{
    (void)input;
    std::string x;
    emitVrtHeader(x, *this);
    for (int ob = 1; ob <= (int)bands.size(); ++ob)
    {
        const Band &b = bands[(size_t)ob - 1];
        const CalcExpr &e = bandExpr(ob);
        std::vector<CalcSourceRef> refs = bandSources(ob);
        x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\" "
                       "subClass=\"VRTDerivedRasterBand\">\n",
                       dtypeName(b.type), ob);
        if (b.hasNodata)
            x += "    <NoDataValue>" + fmt18(b.nodata) +
                 "</NoDataValue>\n";
        bool withProps = ob == 1 && singleInput && refs.size() == 1;
        for (const CalcSourceRef &ref : refs)
            emitDerivedSource(x, ref.ds, inputPaths[ref.inputIdx],
                              ref.band, withProps, width, height, output);
        x += "    <PixelFunctionType>" + xmlEscA(e.func) +
             "</PixelFunctionType>\n";
        std::string args;
        for (const CalcArg &a : e.args)
            args += " " + a.key + "=\"" + xmlEscA(a.val) + "\"";
        if (cliPropagate)
            args += " propagateNoData=\"1\"";
        if (!args.empty())
            x += "    <PixelFunctionArguments" + args + " />\n";
        x += "  </VRTRasterBand>\n";
    }
    x += "</VRTDataset>\n";
    return x;
}

// ------------------------------------------------------------------
// reclassify verb dataset
// ------------------------------------------------------------------

class ReclassifyDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    std::string srcPathTyped;
    std::string mappingEff;
    bool hasOt = false;

    std::vector<std::vector<double>> data;
    bool evaluated = false;

    ReclassifyDataset(std::unique_ptr<RasterDatasetBase> s,
                      const std::string &pathTyped,
                      const std::string &mapping, bool otSet, DType ot)
        : src(std::move(s)), srcPathTyped(pathTyped), mappingEff(mapping),
          hasOt(otSet)
    {
        path = src->path;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = src->width;
        height = src->height;
        hasGT = src->hasGT;
        memcpy(gt, src->gt, sizeof gt);
        srs = src->srs.clone();
        hasSrs = src->hasSrs;
        srsSynthetic = src->srsSynthetic;
        metadata = src->metadata;
        domainOrder = src->domainOrder;
        sortedDomains = src->sortedDomains;
        xmlDomains = src->xmlDomains;
        files = src->files;
        gcps = src->gcps;
        gcpSrs = src->gcpSrs.clone();
        hasGcpSrs = src->hasGcpSrs;
        gcpMapping = src->gcpMapping;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();
        pamPath = src->pamPath;
        pamExists = src->pamExists;
        pamSrsRaw = src->pamSrsRaw;
        pamSrsMapping = src->pamSrsMapping;
        pamGtRaw = src->pamGtRaw;
        pamMdi = src->pamMdi;
        pamXmlDomains = src->pamXmlDomains;
        pamBands = src->pamBands;
        pamSuppressItems = true;
        for (size_t i = 0; i < src->bands.size(); ++i)
        {
            Band b = src->bands[i];
            if (otSet)
                b.type = ot;
            b.index = (int)i + 1;
            bands.push_back(std::move(b));
        }
        data.resize(bands.size());
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        if (!evaluated)
            return false;
        out = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        for (double &v : out)
            v = rasterFinishReal(v, t);
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        if (!evaluated)
            return false;
        const std::vector<double> &vals = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        size_t sz = (size_t)dtypeSizeBytes(t);
        out.assign(vals.size() * sz, 0);
        for (size_t i = 0; i < vals.size(); ++i)
            rasterEncodeReal(t, out.data() + i * sz,
                             rasterFinishReal(vals[i], t), 0);
        return true;
    }

    void realBlockDims(int &bw, int &bh) const override
    {
        src->realBlockDims(bw, bh);
    }

    bool geoDoubleOrphanHint() override
    {
        return src->geoDoubleOrphanHint();
    }

    // parse + dest-validate + apply; failBand out for the progress model
    bool evalAll(CalcFail &fail)
    {
        for (size_t bi = 0; bi < bands.size(); ++bi)
        {
            const Band &sb = src->bands[bi];
            ReclassMap m;
            std::string err = reclassParse(mappingEff, sb.hasNodata,
                                           sb.nodata, m);
            if (err.empty() && hasOt)
                err = reclassValidateDst(m, bands[bi].type, sb.hasNodata,
                                         sb.nodata);
            if (!err.empty())
            {
                fail.set(CPLE_AppDefined, err);
                fail.failBand = (int)bi + 1;
                return false;
            }
            std::vector<double> vals;
            if (!src->readBand((int)bi + 1, vals))
            {
                fail.setSilent();
                fail.failBand = (int)bi + 1;
                return false;
            }
            std::vector<double> &out = data[bi];
            out.resize(vals.size());
            for (size_t px = 0; px < vals.size(); ++px)
            {
                std::string uerr;
                if (reclassApply(m, vals[px], sb.hasNodata, sb.nodata,
                                 out[px], uerr))
                {
                    fail.set(CPLE_AppDefined, uerr);
                    fail.failBand = (int)bi + 1;
                    return false;
                }
            }
            if (!hasOt)
            {
                std::string verr = reclassValidateDst(
                    m, bands[bi].type, sb.hasNodata, sb.nodata);
                if (!verr.empty())
                {
                    fail.set(CPLE_AppDefined, verr);
                    fail.failBand = (int)bi + 1;
                    return false;
                }
            }
        }
        evaluated = true;
        return true;
    }

    std::string customVrtXml(const std::string &input,
                             const std::string &output) override
    {
        (void)input;
        std::string x;
        emitVrtHeader(x, *this);
        for (int ob = 1; ob <= (int)bands.size(); ++ob)
        {
            const Band &b = bands[(size_t)ob - 1];
            x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\" "
                           "subClass=\"VRTDerivedRasterBand\">\n",
                           dtypeName(b.type), ob);
            if (b.hasNodata)
                x += "    <NoDataValue>" + fmt18(b.nodata) +
                     "</NoDataValue>\n";
            int relative = 0;
            std::string rel = relToOutput(srcPathTyped, output, relative);
            int bw = 0, bh = 0;
            src->realBlockDims(bw, bh);
            const Band &sb = src->bands[(size_t)ob - 1];
            x += "    <SimpleSource>\n";
            x += strPrintf("      <SourceFilename relativeToVRT=\"%d\">"
                           "%s</SourceFilename>\n",
                           relative, xmlEscA(rel).c_str());
            x += strPrintf("      <SourceBand>%d</SourceBand>\n", ob);
            x += strPrintf("      <SourceProperties RasterXSize=\"%d\" "
                           "RasterYSize=\"%d\" DataType=\"%s\" "
                           "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                           src->width, src->height, dtypeName(sb.type),
                           bw, bh);
            x += strPrintf("      <SrcRect xOff=\"0\" yOff=\"0\" "
                           "xSize=\"%d\" ySize=\"%d\" />\n",
                           src->width, src->height);
            x += strPrintf("      <DstRect xOff=\"0\" yOff=\"0\" "
                           "xSize=\"%d\" ySize=\"%d\" />\n",
                           width, height);
            x += "    </SimpleSource>\n";
            x += "    <PixelFunctionType>reclassify"
                 "</PixelFunctionType>\n";
            x += "    <PixelFunctionArguments mapping=\"" +
                 xmlEscA(mappingEff) + "\" />\n";
            x += std::string("    <SourceTransferType>") +
                 dtypeName(sb.type) + "</SourceTransferType>\n";
            x += "  </VRTRasterBand>\n";
        }
        x += "</VRTDataset>\n";
        return x;
    }
};

// ------------------------------------------------------------------
// handlers
// ------------------------------------------------------------------

int calcFormatPreValidator(const CmdSpec &cmd, ParseResult &r)
{
    std::string format = r.str("output-format");
    std::string drv;
    std::string issue = rasterOutFormatIssue(format, drv);
    if (!issue.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, cmd.name + ": " + issue);
        handlerPrintUsage();
        return 1;
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(false, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

std::string calcCanonOt(const CmdSpec &cmd, const std::string &val)
{
    for (const auto &a : cmd.args)
        if (a.name == "output-data-type")
            for (const auto &c : a.choices)
                if (strEqualNoCase(c, val))
                    return c;
    if (strEqualNoCase(val, "byte"))
        return "Byte";
    return val;
}

DType calcOtType(const std::string &canon)
{
    if (canon == "UInt8" || canon == "Byte")
        return DType::Byte;
    return dtypeFromName(canon);
}

bool exprRefsName(const std::string &expr, const std::string &name)
{
    if (name.empty())
        return false;
    auto ident = [](char c)
    { return isalnum((unsigned char)c) || c == '_'; };
    size_t pos = 0;
    while ((pos = expr.find(name, pos)) != std::string::npos)
    {
        size_t end = pos + name.size();
        if ((pos == 0 || !ident(expr[pos - 1])) &&
            (end >= expr.size() || !ident(expr[end])))
            return true;
        ++pos;
    }
    return false;
}

// failure-time progress: the bar advances through the failing band's
// read slot in the [0, 0.5] copy half
void calcFailBar(bool quiet, int failBand, int totalBands)
{
    if (quiet)
        return;
    TermProgress tp;
    tp.update(0.0);
    tp.update((double)failBand / (2.0 * (double)totalBands));
}

int rasterCalcHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("calc");
    std::string output = r.str("output");
    std::string of = r.str("output-format");
    std::string drv;
    rasterOutFormatIssue(of, drv);
    std::string lowOut = strToLower(output);
    bool gdalg = drv == "GDALG" ||
                 (of.empty() && strEndsWith(lowOut, ".gdalg.json"));
    bool vrtOut = drv == "VRT" || (of.empty() && !gdalg &&
                                   strEndsWith(lowOut, ".vrt"));
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    std::string dialect = r.str("dialect", "muparser");
    if (dialect.empty())
        dialect = "muparser";
    bool builtin = strEqualNoCase(dialect, "builtin");
    std::vector<std::string> inputs = r.list("input");
    std::vector<std::string> calcs = r.list("calc");

    auto failUsage = [&](void) -> int
    {
        if (gdalg)
            handlerPrintUsage();
        return 1;
    };

    std::vector<CalcExpr> exprs;
    if (builtin)
    {
        for (const std::string &c : calcs)
        {
            CalcExpr e = calcParseExpr(c);
            const CalcFuncInfo *f = calcFuncLookup(e.func);
            if (!f)
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "calc: '" + e.func +
                                "' is a unknown builtin function");
                return failUsage();
            }
            for (const CalcArg &a : e.args)
                if (!calcArgAllowed(*f, a.key))
                {
                    cplErrorStr(CE_Failure, CPLE_IllegalArg,
                                "calc: '" + a.key +
                                    "' is a unrecognized argument for "
                                    "builtin function '" +
                                    e.func + "'. " + f->allowed);
                    return failUsage();
                }
            exprs.push_back(e);
        }
    }

    if (!builtin && inputs.size() > 1)
        for (const auto &tok : inputs)
            if (tok.find('=') == std::string::npos)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Inputs must be named when more than one "
                            "input is provided.");
                return failUsage();
            }

    std::vector<std::string> seenNames;
    for (const auto &tok : inputs)
    {
        size_t eq = tok.find('=');
        if (eq == std::string::npos)
            continue;
        std::string name = tok.substr(0, eq);
        for (const auto &nm : seenNames)
            if (nm == name)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "An input with name '" + name +
                                "' has already been provided");
                return failUsage();
            }
        seenNames.push_back(name);
    }

    struct NamedInput
    {
        std::string name, pathTyped;
        std::unique_ptr<RasterDatasetBase> ds;
    };
    std::vector<NamedInput> named;
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        const std::string &tok = inputs[i];
        NamedInput ni;
        size_t eq = tok.find('=');
        if (eq == std::string::npos)
        {
            ni.name = inputs.size() == 1 ? "X"
                                         : strPrintf("X%d", (int)i);
            ni.pathTyped = tok;
        }
        else
        {
            ni.name = tok.substr(0, eq);
            ni.pathTyped = tok.substr(eq + 1);
        }
        std::string err;
        cplPushQuietHandler();
        ni.ds = openRaster(ni.pathTyped, err);
        cplPopHandler();
        if (!ni.ds)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to open " + ni.pathTyped);
            return failUsage();
        }
        named.push_back(std::move(ni));
    }

    // muparser exprs stay opaque: only the source-name scan looks at them
    bool muRefAny = false;
    if (!builtin)
    {
        for (const std::string &c : calcs)
        {
            CalcExpr e;
            e.func = c;
            e.raw = c;
            exprs.push_back(e);
        }
        for (const NamedInput &ni : named)
            if (exprRefsName(calcs.empty() ? "" : calcs[0], ni.name))
            {
                muRefAny = true;
                break;
            }
    }

    std::sort(named.begin(), named.end(),
              [](const NamedInput &a, const NamedInput &b)
              { return a.name < b.name; });

    bool checkCrs = !r.flag("no-check-crs");
    bool checkExtent = !r.flag("no-check-extent");
    bool flatten = r.flag("flatten");

    if (checkCrs)
    {
        const NamedInput *ref = nullptr;
        for (const NamedInput &ni : named)
        {
            if (!ni.ds->hasSrs)
                continue;
            if (!ref)
            {
                ref = &ni;
                continue;
            }
            if (ref->ds->srs.wkt2SingleLine() !=
                ni.ds->srs.wkt2SingleLine())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Input spatial reference systems are "
                            "inconsistent.");
                return failUsage();
            }
        }
    }

    bool anyGT = false, allGT = true;
    for (const NamedInput &ni : named)
    {
        if (ni.ds->hasGT)
            anyGT = true;
        else
            allGT = false;
    }
    if (checkExtent)
    {
        if (anyGT && !allGT)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Input extents are inconsistent.");
            return failUsage();
        }
        if (allGT && named.size() > 1)
        {
            const RasterDatasetBase *a = named[0].ds.get();
            double ax0 = a->gt[0], ax1 = a->gt[0] + a->gt[1] * a->width;
            double ay0 = a->gt[3], ay1 = a->gt[3] + a->gt[5] * a->height;
            for (size_t i = 1; i < named.size(); ++i)
            {
                const RasterDatasetBase *b = named[i].ds.get();
                double bx0 = b->gt[0],
                       bx1 = b->gt[0] + b->gt[1] * b->width;
                double by0 = b->gt[3],
                       by1 = b->gt[3] + b->gt[5] * b->height;
                if (bx0 != ax0 || bx1 != ax1 || by0 != ay0 || by1 != ay1)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Input extents are inconsistent.");
                    return failUsage();
                }
            }
        }
        // non-georeferenced inputs with differing dimensions are
        // stretched to the first-sorted grid, no gate
    }
    else
    {
        for (size_t i = 1; i < named.size(); ++i)
            if (named[i].ds->width != named[0].ds->width ||
                named[i].ds->height != named[0].ds->height)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Inputs do not have the same dimensions.");
                return failUsage();
            }
    }

    int commonBands = 1;
    if (!flatten)
        for (const NamedInput &ni : named)
        {
            int nb = (int)ni.ds->bands.size();
            if (nb <= 1)
                continue;
            if (commonBands == 1)
                commonBands = nb;
            else if (nb != commonBands)
            {
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("Expression cannot operate on all bands of "
                              "rasters with incompatible numbers of "
                              "bands (source %s has %d bands but "
                              "expected to have 1 or %d bands).",
                              ni.name.c_str(), nb, commonBands));
                return failUsage();
            }
        }

    // grid: highest-resolution georeferenced input, first-sorted wins ties
    size_t gridIdx = 0;
    if (allGT && named.size() > 1)
    {
        double best = 0;
        for (size_t i = 0; i < named.size(); ++i)
        {
            double res = std::fabs(named[i].ds->gt[1]);
            if (i == 0 || res < best)
            {
                best = res;
                gridIdx = i;
            }
        }
    }
    const RasterDatasetBase *grid = named[gridIdx].ds.get();

    // output nodata
    std::string ndTok = r.str("nodata");
    bool ndGiven = r.get("nodata") != nullptr;
    bool outNdSet = false;
    double outNd = 0;
    if (ndGiven)
    {
        if (ndTok == "none")
            outNdSet = false;
        else if (parseFullDouble(ndTok, outNd))
            outNdSet = true;
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid NoData value: " + ndTok);
            return 1;
        }
    }
    else
    {
        for (const NamedInput &ni : named)
            if (!ni.ds->bands.empty() && ni.ds->bands[0].hasNodata)
            {
                outNdSet = true;
                outNd = ni.ds->bands[0].nodata;
                break;
            }
    }

    // output type
    std::string otTyped = r.str("output-data-type");
    bool otSet = r.get("output-data-type") != nullptr;
    std::string otCanon = otSet ? calcCanonOt(cmd, otTyped) : "";
    DType outType = DType::Float64;
    if (otSet)
        outType = calcOtType(otCanon);
    else
    {
        bool preserveAll = builtin;
        if (builtin)
            for (const CalcExpr &e : exprs)
            {
                const CalcFuncInfo *f = calcFuncLookup(e.func);
                if (!f->preserveType)
                    preserveAll = false;
            }
        if (preserveAll)
        {
            DType common = DType::Unknown;
            bool same = true;
            for (const NamedInput &ni : named)
                for (const Band &b : ni.ds->bands)
                {
                    if (common == DType::Unknown)
                        common = b.type;
                    else if (b.type != common)
                        same = false;
                }
            if (same && common != DType::Unknown)
                outType = common;
        }
    }

    if (outNdSet && !valueExactAs(outNd, outType))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("Band output type %s cannot represent "
                              "NoData value %g",
                              dtypeName(outType), outNd));
        return failUsage();
    }

    auto ds = std::make_unique<CalcDataset>();
    CalcDataset *cd = ds.get();
    cd->driverShort = "VRT";
    cd->driverLong = "Virtual Raster";
    cd->width = grid->width;
    cd->height = grid->height;
    cd->hasGT = grid->hasGT;
    memcpy(cd->gt, grid->gt, sizeof cd->gt);
    if (grid->hasSrs)
    {
        cd->srs = grid->srs.clone();
        cd->hasSrs = true;
    }
    cd->flatten = flatten;
    cd->cliPropagate = r.flag("propagate-nodata");
    cd->bandsPerExpr = flatten ? 1 : commonBands;
    cd->exprs = exprs;
    cd->outNdSet = outNdSet;
    cd->outNd = outNd;
    cd->singleInput = named.size() == 1;
    for (NamedInput &ni : named)
    {
        cd->inputNames.push_back(ni.name);
        cd->inputPaths.push_back(ni.pathTyped);
        cd->inputs.push_back(std::move(ni.ds));
    }
    cd->path = cd->inputPaths.empty() ? "" : cd->inputPaths[0];
    int totalBands = (int)exprs.size() * cd->bandsPerExpr;
    for (int i = 0; i < totalBands; ++i)
    {
        Band b;
        b.index = i + 1;
        b.type = outType;
        b.hasNodata = outNdSet;
        b.nodata = outNd;
        b.blockX = std::min(cd->width, 128);
        b.blockY = std::min(cd->height, 128);
        cd->bands.push_back(std::move(b));
    }
    cd->data.resize((size_t)totalBands);

    if (gdalg && !builtin)
    {
        if (muRefAny)
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "Dialect 'muparser' is not supported by this "
                        "GDAL build. A GDAL build with muparser is "
                        "needed.");
        else
            cplErrorStr(
                CE_Failure, CPLE_AppDefined,
                "The source_names variable passed to ExprPixelFunc() "
                "has 1 values, whereas 0 were expected. An invalid "
                "variable name has likely been used");
        handlerPrintUsage();
        return 1;
    }

    if (gdalg)
    {
        // GDALG dry-runs the serialized command: named args are lost
        CalcDataset dry;
        dry.width = cd->width;
        dry.height = cd->height;
        dry.flatten = flatten;
        dry.cliPropagate = cd->cliPropagate;
        dry.bandsPerExpr = cd->bandsPerExpr;
        dry.outNdSet = outNdSet;
        dry.outNd = outNd;
        for (size_t i = 0; i < cd->inputs.size(); ++i)
        {
            dry.inputNames.push_back(cd->inputNames[i]);
            dry.inputPaths.push_back(cd->inputPaths[i]);
        }
        for (const CalcExpr &e : exprs)
        {
            CalcExpr s;
            s.func = e.func;
            s.raw = e.func;
            dry.exprs.push_back(s);
        }
        for (const Band &b : cd->bands)
            dry.bands.push_back(b);
        dry.data.resize(cd->data.size());
        // borrow the opened inputs for the dry-run
        for (auto &in : cd->inputs)
            dry.inputs.push_back(std::unique_ptr<RasterDatasetBase>(
                in.get()));
        CalcFail fail;
        bool ok = true;
        for (int b = 1; b <= (int)dry.bands.size() && ok; ++b)
        {
            const CalcExpr &e = dry.bandExpr(b);
            const CalcFuncInfo *f = calcFuncLookup(e.func);
            std::vector<CalcSourceRef> refs = dry.bandSources(b);
            size_t n = refs.size();
            switch (f->srcClass)
            {
                case SC_ONE:
                    if (n != 1)
                    {
                        fail.setSilent();
                        ok = false;
                    }
                    break;
                case SC_TWO:
                    if (n != 2)
                    {
                        fail.setSilent();
                        ok = false;
                    }
                    break;
                case SC_MUL:
                    if (n < 2)
                    {
                        fail.set(CPLE_AppDefined,
                                 "mul requires at least two sources or "
                                 "a specified constant k");
                        ok = false;
                    }
                    break;
                case SC_INTERP:
                    fail.set(CPLE_AppDefined,
                             "Missing pixel function argument: t0");
                    ok = false;
                    break;
                default:
                    break;
            }
            if (!ok)
                break;
            switch (f->id)
            {
                case F_POW:
                    fail.set(CPLE_AppDefined,
                             "Missing pixel function argument: power");
                    ok = false;
                    break;
                case F_RECLASSIFY:
                    fail.set(CPLE_AppDefined,
                             "reclassify must be called with 'mapping' "
                             "argument");
                    ok = false;
                    break;
                case F_EXPRESSION:
                    // args are lost, so the dialect falls back to
                    // muparser before the missing-expression check
                    fail.set(CPLE_IllegalArg,
                             "Dialect 'muparser' is not supported by "
                             "this GDAL build. A GDAL build with "
                             "muparser is needed.");
                    ok = false;
                    break;
                case F_REPLACE_ND:
                {
                    const Band &sb =
                        refs[0].ds->bands[(size_t)refs[0].band - 1];
                    // the dry-run buffer is never floating, so a
                    // nodata-bearing source always trips the nan check
                    if (!sb.hasNodata)
                        fail.set(CPLE_AppDefined, "Raster has no NoData");
                    else
                        fail.set(CPLE_AppDefined,
                                 "Using nan requires a floating point "
                                 "type output buffer");
                    ok = false;
                    break;
                }
                default:
                    break;
            }
        }
        for (auto &in : dry.inputs)
            in.release();
        if (!ok)
        {
            bool usage = fail.silent ||
                         fail.msg == "Raster has no NoData" ||
                         fail.errNum == CPLE_IllegalArg ||
                         strEndsWith(fail.msg, "output buffer");
            if (!fail.silent)
                cplErrorStr(CE_Failure, fail.errNum,
                            fail.msg);
            if (usage)
                handlerPrintUsage();
            return 1;
        }
    }

    std::string gdalgExtra;
    if (otSet)
        gdalgExtra += " --output-data-type " + otCanon;
    if (!checkCrs)
        gdalgExtra += " --no-check-crs";
    if (!checkExtent)
        gdalgExtra += " --no-check-extent";
    if (cd->cliPropagate)
        gdalgExtra += " --propagate-nodata";
    for (const CalcExpr &e : exprs)
        gdalgExtra += " --calc " + e.func;
    gdalgExtra += " --dialect builtin";
    if (flatten)
        gdalgExtra += " --flatten";
    if (ndGiven)
        gdalgExtra += " --nodata " + ndTok;

    std::string inputEcho;
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        if (i)
            inputEcho += " --input ";
        inputEcho += inputs[i];
    }

    bool streamOut = drv == "stream";
    auto mat =
        [cd, quiet, vrtOut, streamOut, builtin, muRefAny](
            std::unique_ptr<RasterDatasetBase> &dsr) -> int
    {
        (void)dsr;
        if (!builtin)
        {
            if (!vrtOut && !streamOut)
                calcFailBar(quiet, muRefAny ? 1 : 0,
                            (int)cd->bands.size());
            if (muRefAny)
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "Dialect 'muparser' is not supported by "
                            "this GDAL build. A GDAL build with "
                            "muparser is needed.");
            else
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    "The source_names variable passed to ExprPixelFunc() "
                    "has 1 values, whereas 0 were expected. An invalid "
                    "variable name has likely been used");
            return 1;
        }
        CalcFail fail;
        if (!cd->evalAll(fail, vrtOut))
        {
            if (!vrtOut && !streamOut)
                calcFailBar(quiet, fail.failBand, (int)cd->bands.size());
            if (!fail.silent)
                cplErrorStr(CE_Failure, fail.errNum, fail.msg);
            return 1;
        }
        return 0;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    return rasterConvertWriteOutput(base, r, inputEcho, output,
                                    quiet || vrtOut || streamOut,
                                    overwrite, false, drv, gdalgExtra, mat,
                                    nullptr);
}

int rasterReclassifyHandler(const CmdSpec &cmd, ParseResult &r)
{
    PrefixScope prefix("reclassify");
    std::string input = r.str("input");
    std::string output = r.str("output");
    std::string of = r.str("output-format");
    std::string drv;
    rasterOutFormatIssue(of, drv);
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");

    std::string err;
    auto src = openRaster(input, err);
    if (!src)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    bool gdalgOut =
        drv == "GDALG" ||
        (drv.empty() && strEndsWith(strToLower(output), ".gdalg.json"));

    std::string mappingRaw = r.str("mapping");
    std::string mappingEff = mappingRaw;
    if (!gdalgOut && !mappingRaw.empty() && mappingRaw[0] == '@')
    {
        std::string content;
        if (!readFileToString(mappingRaw.substr(1), content))
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "reclassify: Cannot open " + mappingRaw.substr(1));
            return 1;
        }
        mappingEff = reclassMappingFromFile(content);
    }

    std::string otTyped = r.str("output-data-type");
    bool otSet = r.get("output-data-type") != nullptr;
    std::string otCanon = otSet ? calcCanonOt(cmd, otTyped) : "";
    DType ot = otSet ? calcOtType(otCanon) : DType::Byte;

    // construction-stage parse (and, with an explicit type, validation);
    // a GDALG target serializes the raw mapping without ever parsing it
    for (size_t bi = 0; !gdalgOut && bi < src->bands.size(); ++bi)
    {
        const Band &sb = src->bands[bi];
        ReclassMap m;
        std::string perr =
            reclassParse(mappingEff, sb.hasNodata, sb.nodata, m);
        if (perr.empty() && otSet)
            perr = reclassValidateDst(m, ot, sb.hasNodata, sb.nodata);
        if (!perr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, perr);
            return 1;
        }
    }

    auto ds = std::make_unique<ReclassifyDataset>(
        std::move(src), input, mappingEff, otSet, ot);
    ReclassifyDataset *rd = ds.get();

    std::string gdalgExtra = " --mapping " + gdalgQuote(mappingRaw);
    if (otSet)
        gdalgExtra += " --output-data-type " + otCanon;

    bool evalTarget = drv == "GTiff" || drv == "MEM" || drv == "COG" ||
                      (drv.empty() && !strEndsWith(strToLower(output),
                                                   ".vrt") &&
                       !strEndsWith(strToLower(output), ".gdalg.json"));
    auto mat = [rd, quiet,
                evalTarget](std::unique_ptr<RasterDatasetBase> &dsr) -> int
    {
        (void)dsr;
        if (!evalTarget)
            return 0;
        CalcFail fail;
        if (!rd->evalAll(fail))
        {
            calcFailBar(quiet, fail.failBand, (int)rd->bands.size());
            if (!fail.silent)
                cplErrorStr(CE_Failure, fail.errNum,
                            fail.msg);
            return 1;
        }
        return 0;
    };

    std::unique_ptr<RasterDatasetBase> base = std::move(ds);
    return rasterConvertWriteOutput(base, r, input, output, quiet,
                                    overwrite, append, drv, gdalgExtra,
                                    mat, nullptr);
}

std::string calcNodataValueCheck(const std::string &argName,
                                 const std::string &value)
{
    if (argName != "nodata")
        return "";
    if (value == "none")
        return "";
    double v;
    if (parseFullDouble(value, v))
        return "";
    if (strEqualNoCase(value, "none"))
        return "";
    return "\x05Value of 'nodata' should be 'none', a numeric value, "
           "'nan', 'inf' or '-inf'";
}

}  // namespace

void registerRasterCalcHandlers()
{
    registerHandler("raster_calc", rasterCalcHandler);
    registerPreValidator("raster_calc", calcFormatPreValidator);
    registerArgValueCheck("raster_calc", calcNodataValueCheck);
    registerHandler("raster_reclassify", rasterReclassifyHandler);
    registerPreValidator("raster_reclassify", calcFormatPreValidator);
}
