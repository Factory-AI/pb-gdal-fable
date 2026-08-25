#include "cpl.h"
#include "jsonc.h"
#include "ogr.h"
#include "util.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>

namespace
{

std::vector<OgrLayer::Diag> *gDiags = nullptr;

void diag(int sev, const std::string &msg)
{
    if (gDiags)
        gDiags->push_back({sev, msg, -1, true});
    else
        cplErrorStr((CPLErrClass)sev, CPLE_AppDefined, msg);
}

// like diag() but not gated on feature iteration: surfaces in plain
// `vector info` too
void diagOpen(int sev, const std::string &msg)
{
    if (gDiags)
        gDiags->push_back({sev, msg, -1, false});
    else
        cplErrorStr((CPLErrClass)sev, CPLE_AppDefined, msg);
}

// surfaces at open in every mode and repeats on feature iteration
void diagReplay(int sev, const std::string &msg)
{
    if (gDiags)
        gDiags->push_back({sev, msg, -1, true, true});
    else
        cplErrorStr((CPLErrClass)sev, CPLE_AppDefined, msg);
}

bool gTooManyCoordWarned = false;

// collected once for the whole process; a once-diag that every pass may
// replay but the emission gate lets through a single time
void warnTooManyCoords(const JVal &arr)
{
    if (gTooManyCoordWarned)
        return;
    gTooManyCoordWarned = true;
    std::string msg =
        strPrintf("OGRGeoJSONReadRawPoint(): too many members in array "
                  "'%s': %d. At most 3 are handled. Ignoring extra "
                  "members. Further messages of this type will be "
                  "suppressed.",
                  ogrJsonSpacedSerialize(arr).c_str(),
                  (int)arr.arr.size());
    if (gDiags)
        gDiags->push_back({CE_Warning, msg, -1, true, true, true});
    else
        cplErrorStr(CE_Warning, CPLE_AppDefined, msg);
}

// the reference parses FeatureCollection files with a streaming parser
// whose errors read "At line L, character C: ..."; other GeoJSON content
// goes through a json-c full parse reporting
// "JSON parsing error: <desc> (at offset N)"
bool streamingEligible(const std::string &content)
{
    size_t pos = 0;
    while ((pos = content.find("\"features\"", pos)) != std::string::npos)
    {
        size_t i = pos + 10;
        while (i < content.size() &&
               (content[i] == ' ' || content[i] == '\t' ||
                content[i] == '\n' || content[i] == '\r'))
            ++i;
        if (i < content.size() && content[i] == ':')
        {
            ++i;
            while (i < content.size() &&
                   (content[i] == ' ' || content[i] == '\t' ||
                    content[i] == '\n' || content[i] == '\r'))
                ++i;
            if (i < content.size() && content[i] == '[')
                return true;
        }
        pos += 10;
    }
    return false;
}

// streaming-parser emulation with its position quirks: a completed
// string counts as a single character; a completed number counts as one
// character and swallows its terminating delimiter; everything else
// counts per character, newline resets the column
struct StreamScan
{
    const std::string &s;
    size_t i = 0;
    int line = 1, ch = 1;
    std::string msg;
    bool failed = false;

    explicit StreamScan(const std::string &t) : s(t) {}

    void adv(char c)
    {
        if (c == '\n')
        {
            ++line;
            ch = 1;
        }
        else
            ++ch;
    }
    bool eof() const { return i >= s.size(); }
    void fail(const std::string &m)
    {
        if (!failed)
        {
            failed = true;
            msg = strPrintf("At line %d, character %d: %s", line, ch,
                            m.c_str());
        }
    }
    static bool valueStart(char c)
    {
        return c == '"' || c == '{' || c == '[' || c == '-' ||
               (c >= '0' && c <= '9') || c == 't' || c == 'f' ||
               c == 'n' || c == 'N' || c == 'I' || c == 'i' || c == '.';
    }
    // returns false on failure; on success counts one character;
    // declen receives the decoded byte length when requested; a string
    // holding any escape sequence counts per byte (quotes included),
    // escape-free strings count one character
    bool parseString(long long *declen = nullptr)
    {
        if (declen)
            *declen = 0;
        bool hasEscape = false;
        size_t j = i + 1;
        while (true)
        {
            if (j >= s.size())
            {
                for (; i < j; ++i)
                    adv(s[i]);
                fail("Unterminated string");
                return false;
            }
            char c = s[j];
            if (c == '"')
            {
                if (hasEscape)
                    ch += (int)(j + 1 - i);
                else
                    ++ch;
                i = j + 1;
                return true;
            }
            if (c == '\\')
            {
                hasEscape = true;
                if (j + 1 >= s.size())
                {
                    ++j;
                    for (; i < j; ++i)
                        adv(s[i]);
                    fail("Unterminated string");
                    return false;
                }
                char e = s[j + 1];
                if (e == 'u')
                {
                    for (int k = 0; k < 4; ++k)
                    {
                        size_t p = j + 2 + k;
                        if (p >= s.size())
                        {
                            for (; i < p; ++i)
                                adv(s[i]);
                            fail("Unterminated string");
                            return false;
                        }
                        char h = s[p];
                        if (!((h >= '0' && h <= '9') ||
                              (h >= 'a' && h <= 'f') ||
                              (h >= 'A' && h <= 'F')))
                        {
                            for (; i < p; ++i)
                                adv(s[i]);
                            fail(strPrintf("Illegal character in unicode "
                                           "sequence (\\%c)",
                                           h));
                            return false;
                        }
                    }
                    if (declen)
                    {
                        unsigned int cp = (unsigned int)strtoul(
                            s.substr(j + 2, 4).c_str(), nullptr, 16);
                        *declen += cp < 0x80 ? 1 : (cp < 0x800 ? 2 : 3);
                    }
                    j += 6;
                    continue;
                }
                if (e != '"' && e != '\\' && e != '/' && e != 'b' &&
                    e != 'f' && e != 'n' && e != 'r' && e != 't')
                {
                    ++j;
                    for (; i < j; ++i)
                        adv(s[i]);
                    fail(strPrintf("Illegal escape sequence (\\%c)", e));
                    return false;
                }
                if (declen)
                    ++*declen;
                j += 2;
                continue;
            }
            if (declen)
                ++*declen;
            ++j;
        }
    }
};

// The reference's streaming pass also meters each feature's in-memory
// estimate against OGR_GEOJSON_MAX_OBJ_SIZE (megabytes, fractions
// allowed, truncated to whole bytes; non-positive or sub-byte values
// disable the limit). Costs calibrated black-box: nested object 744,
// array 80, member 32 (key text free), string len+56, number/literal
// 48; the feature's own object is free. The check runs at every parser
// callback entry BEFORE that callback's own cost is added, so the
// error surfaces at the event following the crossing token, at the
// scanner's position for that event: strings/literals report after
// their completion, numbers report their own start column (their
// delimiter is swallowed), brackets report pre-advance.
struct GeojsonSizeMeter
{
    bool on = false;
    long long limit = 0, est = 0;
    bool inFeature = false;
    size_t featureDepth = 0;
    size_t featuresArr = 0;
    bool fired = false;
    int line = 1, ch = 1;

    void event(int l, int c, long long bump)
    {
        if (!on || fired || !inFeature)
            return;
        if (est >= limit)
        {
            fired = true;
            line = l;
            ch = c;
            return;
        }
        est += bump;
    }
};

std::string streamingParseError(const std::string &content,
                                bool meterSize = false)
{
    StreamScan sc(content);
    GeojsonSizeMeter mt;
    if (meterSize)
    {
        double mb = 200;
        if (configIsSet("OGR_GEOJSON_MAX_OBJ_SIZE"))
            mb = strtod(configGet("OGR_GEOJSON_MAX_OBJ_SIZE").c_str(),
                        nullptr);
        if (mb > 0)
        {
            mt.limit = mb >= 8.0e12 ? LLONG_MAX
                                    : (long long)(mb * 1024 * 1024);
            mt.on = mt.limit > 0;
        }
    }
    bool lastKeyFeatures = false;
    // scope stack: 'O' object, 'A' array; states describe what comes next
    enum State
    {
        RootValue,
        RootDone,
        ObjKeyFresh,
        ObjKeyAfterComma,
        ObjColon,
        ObjValue,
        ObjAfterVal,
        ArrValOrClose,
        ArrValStrict,
        ArrAfterVal
    };
    std::vector<char> stack;
    State st = RootValue;
    auto popScope = [&]()
    {
        stack.pop_back();
        if (stack.empty())
            st = RootDone;
        else
            st = stack.back() == 'O' ? ObjAfterVal : ArrAfterVal;
    };
    // close-bracket event: check, then retire feature/features scopes
    auto endEvent = [&](int l, int c)
    {
        mt.event(l, c, 0);
        if (mt.fired)
            return;
        if (mt.inFeature && !stack.empty() &&
            stack.size() == mt.featureDepth && stack.back() == 'O')
        {
            mt.inFeature = false;
            mt.est = 0;
        }
        if (mt.featuresArr && stack.size() == mt.featuresArr &&
            stack.back() == 'A')
            mt.featuresArr = 0;
    };
    while (!sc.failed && !mt.fired)
    {
        if (sc.eof())
        {
            if (!stack.empty())
                sc.fail(stack.back() == 'O' ? "Unterminated object"
                                            : "Unterminated array");
            break;
        }
        char c = sc.s[sc.i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
            sc.adv(c);
            ++sc.i;
            continue;
        }
        bool wantValue = st == RootValue || st == ObjValue ||
                         st == ArrValOrClose || st == ArrValStrict;
        if (wantValue && StreamScan::valueStart(c))
        {
            State after = st == RootValue ? RootDone
                          : st == ObjValue
                              ? ObjAfterVal
                              : ArrAfterVal;
            if (c == '"')
            {
                long long declen = 0;
                if (!sc.parseString(&declen))
                    break;
                mt.event(sc.line, sc.ch, declen + 56);
                if (mt.fired)
                    break;
                st = after;
                continue;
            }
            if (c == '{')
            {
                bool isFeature = mt.on && !mt.inFeature &&
                                 mt.featuresArr != 0 &&
                                 stack.size() == mt.featuresArr &&
                                 stack.back() == 'A';
                if (isFeature)
                {
                    mt.inFeature = true;
                    mt.featureDepth = stack.size() + 1;
                    mt.est = 0;
                }
                mt.event(sc.line, sc.ch, isFeature ? 0 : 744);
                if (mt.fired)
                    break;
                sc.adv(c);
                ++sc.i;
                stack.push_back('O');
                st = ObjKeyFresh;
                continue;
            }
            if (c == '[')
            {
                if (mt.on && mt.featuresArr == 0 && st == ObjValue &&
                    stack.size() == 1 && lastKeyFeatures)
                    mt.featuresArr = stack.size() + 1;
                mt.event(sc.line, sc.ch, 80);
                if (mt.fired)
                    break;
                sc.adv(c);
                ++sc.i;
                stack.push_back('A');
                st = ArrValOrClose;
                continue;
            }
            if (c == 't' || c == 'f' ||
                (c == 'n' &&
                 !(sc.i + 1 < sc.s.size() &&
                   (sc.s[sc.i + 1] == 'a' || sc.s[sc.i + 1] == 'A'))))
            {
                const char *lit = c == 't'   ? "true"
                                  : c == 'f' ? "false"
                                             : "null";
                for (size_t k = 0; lit[k]; ++k)
                {
                    if (sc.eof())
                    {
                        if (!stack.empty())
                            sc.fail(stack.back() == 'O'
                                        ? "Unterminated object"
                                        : "Unterminated array");
                        else
                            sc.fail("Unexpected state");
                        break;
                    }
                    if (sc.s[sc.i] != lit[k])
                    {
                        sc.fail(strPrintf("Unexpected character (%c)",
                                          sc.s[sc.i]));
                        break;
                    }
                    sc.adv(sc.s[sc.i]);
                    ++sc.i;
                }
                if (sc.failed)
                    break;
                mt.event(sc.line, sc.ch, 48);
                if (mt.fired)
                    break;
                st = after;
                continue;
            }
            if (c == 'n')
            {
                // "nan" literal: counts per character, mismatch on a
                // delimiter reads as a number cut short
                const char *lit = "nan";
                for (size_t k = 0; lit[k]; ++k)
                {
                    if (sc.eof())
                    {
                        if (!stack.empty())
                            sc.fail(stack.back() == 'O'
                                        ? "Unterminated object"
                                        : "Unterminated array");
                        else
                            sc.fail("Unexpected state");
                        break;
                    }
                    char lc = sc.s[sc.i];
                    if (lc >= 'A' && lc <= 'Z')
                        lc = (char)(lc - 'A' + 'a');
                    if (lc != lit[k])
                    {
                        char d = sc.s[sc.i];
                        if (d == ',' || d == ':' || d == '}' || d == ']' ||
                            d == ' ' || d == '\t' || d == '\n' || d == '\r')
                            sc.fail("Invalid number");
                        else
                            sc.fail(strPrintf("Unexpected character (%c)",
                                              d));
                        break;
                    }
                    sc.adv(sc.s[sc.i]);
                    ++sc.i;
                }
                if (sc.failed)
                    break;
                mt.event(sc.line, sc.ch, 48);
                if (mt.fired)
                    break;
                st = after;
                continue;
            }
            // number: counts one character on completion and swallows
            // the delimiter; incomplete numbers count per character
            int startLine = sc.line, startCh = sc.ch;
            size_t j = sc.i;
            while (j < sc.s.size() && sc.s[j] != ',' && sc.s[j] != '}' &&
                   sc.s[j] != ']' && sc.s[j] != ':' && sc.s[j] != '"' &&
                   sc.s[j] != '{' && sc.s[j] != '[' && sc.s[j] != ' ' &&
                   sc.s[j] != '\t' && sc.s[j] != '\n' && sc.s[j] != '\r')
                ++j;
            std::string num = sc.s.substr(sc.i, j - sc.i);
            if (j >= sc.s.size())
            {
                for (; sc.i < j; ++sc.i)
                    sc.adv(sc.s[sc.i]);
                if (!stack.empty())
                    sc.fail(stack.back() == 'O' ? "Unterminated object"
                                                : "Unterminated array");
                else
                    sc.fail("Unexpected state");
                break;
            }
            bool numOk = false;
            {
                if (strEqualNoCase(num, "nan") ||
                    strEqualNoCase(num, "infinity") ||
                    strEqualNoCase(num, "-infinity"))
                    numOk = true;
                else
                {
                    size_t k = 0;
                    bool anyDigit = false, expOk = true;
                    if (k < num.size() && num[k] == '-')
                        ++k;
                    while (k < num.size() && num[k] >= '0' && num[k] <= '9')
                    {
                        ++k;
                        anyDigit = true;
                    }
                    if (k < num.size() && num[k] == '.')
                    {
                        ++k;
                        while (k < num.size() && num[k] >= '0' &&
                               num[k] <= '9')
                        {
                            ++k;
                            anyDigit = true;
                        }
                    }
                    if (anyDigit && k < num.size() &&
                        (num[k] == 'e' || num[k] == 'E'))
                    {
                        ++k;
                        if (k < num.size() &&
                            (num[k] == '+' || num[k] == '-'))
                            ++k;
                        expOk = false;
                        while (k < num.size() && num[k] >= '0' &&
                               num[k] <= '9')
                        {
                            ++k;
                            expOk = true;
                        }
                    }
                    numOk = anyDigit && expOk && k == num.size();
                }
            }
            if (!numOk)
            {
                sc.failed = true;
                sc.msg = strPrintf(
                    "At line %d, character %d: Unrecognized number: %s",
                    startLine, startCh, num.c_str());
                break;
            }
            mt.event(startLine, startCh, 48);
            if (mt.fired)
                break;
            ++sc.ch;
            sc.i = j;
            st = after;
            // swallow the delimiter without counting it, but let it act
            char d = sc.s[sc.i];
            if (d == ',' || d == ':')
            {
                ++sc.i;
                if (st == ObjAfterVal)
                    st = ObjKeyAfterComma;
                else if (st == ArrAfterVal)
                    st = ArrValStrict;
                else if (st == RootDone)
                    sc.fail(strPrintf("Unexpected character (%c)", d));
            }
            else if (d == '}' || d == ']')
            {
                ++sc.i;
                if ((d == '}' && st == ObjAfterVal) ||
                    (d == ']' && st == ArrAfterVal))
                {
                    endEvent(startLine, startCh);
                    if (mt.fired)
                        break;
                    popScope();
                }
                else
                    sc.fail(strPrintf("Unexpected character (%c)", d));
            }
            else if (d == ' ' || d == '\t' || d == '\r')
                ++sc.i;
            else if (d == '\n')
            {
                // a swallowed newline still advances the line counter
                ++sc.line;
                sc.ch = 1;
                ++sc.i;
            }
            else
                sc.fail(strPrintf("Unexpected character (%c)", d));
            continue;
        }
        switch (st)
        {
            case RootValue:
                sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case RootDone:
                sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ObjKeyFresh:
            case ObjKeyAfterComma:
                if (c == '"')
                {
                    size_t ks = sc.i;
                    if (sc.parseString())
                    {
                        if (mt.on && stack.size() == 1)
                            lastKeyFeatures =
                                sc.s.compare(ks + 1, sc.i - ks - 2,
                                             "features") == 0;
                        st = ObjColon;
                    }
                }
                else if (c == '}')
                {
                    endEvent(sc.line, sc.ch);
                    if (mt.fired)
                        break;
                    sc.adv(c);
                    ++sc.i;
                    popScope();
                }
                else if (StreamScan::valueStart(c))
                    sc.fail(strPrintf(
                        "Unexpected character (%c). Expecting '\"'", c));
                else
                    sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ObjColon:
                if (c == ':')
                {
                    mt.event(sc.line, sc.ch, 32);
                    if (mt.fired)
                        break;
                    sc.adv(c);
                    ++sc.i;
                    st = ObjValue;
                }
                else if (StreamScan::valueStart(c))
                    sc.fail("Unexpected state");
                else
                    sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ObjValue:
                sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ObjAfterVal:
                if (c == ',')
                {
                    sc.adv(c);
                    ++sc.i;
                    st = ObjKeyAfterComma;
                }
                else if (c == '}')
                {
                    endEvent(sc.line, sc.ch);
                    if (mt.fired)
                        break;
                    sc.adv(c);
                    ++sc.i;
                    popScope();
                }
                else if (StreamScan::valueStart(c))
                    sc.fail("Unexpected state");
                else
                    sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ArrValOrClose:
                if (c == ']')
                {
                    endEvent(sc.line, sc.ch);
                    if (mt.fired)
                        break;
                    sc.adv(c);
                    ++sc.i;
                    popScope();
                }
                else if (c == ',')
                    sc.fail("Unexpected character (,). Expecting ','");
                else
                    sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ArrValStrict:
                if (c == ']')
                    sc.fail("Missing value");
                else
                    sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
            case ArrAfterVal:
                if (c == ',' || c == ':')
                {
                    sc.adv(c);
                    ++sc.i;
                    st = ArrValStrict;
                }
                else if (c == ']')
                {
                    endEvent(sc.line, sc.ch);
                    if (mt.fired)
                        break;
                    sc.adv(c);
                    ++sc.i;
                    popScope();
                }
                else if (StreamScan::valueStart(c))
                    sc.fail("Unexpected state");
                else
                    sc.fail(strPrintf("Unexpected character (%c)", c));
                break;
        }
    }
    if (mt.fired)
        return strPrintf(
            "At line %d, character %d: GeoJSON object too "
            "complex/large. You may define the OGR_GEOJSON_MAX_OBJ_SIZE "
            "configuration option to a value in megabytes to allow for "
            "larger features, or 0 to remove any size limit.",
            mt.line, mt.ch);
    return sc.failed ? sc.msg : "";
}

// rewrites streaming-parser-tolerated laxness (trailing commas, ':' as
// array separator) into strict JSON for the tree parser
std::string sanitizeLenientJson(const std::string &s)
{
    std::string out = s;
    std::vector<char> st;
    bool inStr = false, esc = false;
    for (size_t i = 0; i < out.size(); ++i)
    {
        char c = out[i];
        if (inStr)
        {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
            inStr = true;
        else if (c == '{' || c == '[')
            st.push_back(c);
        else if (c == '}' || c == ']')
        {
            if (!st.empty())
                st.pop_back();
        }
        else if (c == ':' && !st.empty() && st.back() == '[')
            out[i] = ',';
        else if (c == ',')
        {
            size_t j = i + 1;
            while (j < out.size() &&
                   (out[j] == ' ' || out[j] == '\t' || out[j] == '\n' ||
                    out[j] == '\r'))
                ++j;
            if (j < out.size() && (out[j] == '}' || out[j] == ']'))
                out[i] = ' ';
        }
    }
    return out;
}

std::string jsonParseErrorMessage(const std::string &content)
{
    if (streamingEligible(content))
    {
        std::string m = streamingParseError(content);
        if (!m.empty())
            return m;
    }
    else
    {
        std::string desc;
        size_t off = 0;
        if (jsoncTokenerError(content, desc, off))
            return strPrintf("JSON parsing error: %s (at offset %zu)",
                             desc.c_str(), off);
    }
    return strPrintf("At line %d, character %d: Unexpected state", 1, 1);
}

int geomTypeFromName(const std::string &t)
{
    if (t == "Point") return 1;
    if (t == "LineString") return 2;
    if (t == "Polygon") return 3;
    if (t == "MultiPoint") return 4;
    if (t == "MultiLineString") return 5;
    if (t == "MultiPolygon") return 6;
    if (t == "GeometryCollection") return 7;
    return -1;
}

const char *jsonTypeName(const JVal &v)
{
    switch (v.type)
    {
        case JVal::INT:
            return "int";
        case JVal::DOUBLE:
            return "double";
        case JVal::STRING:
            return "string";
        case JVal::BOOL:
            return "boolean";
        case JVal::NUL:
            return "null";
        case JVal::ARRAY:
            return "array";
        default:
            return "object";
    }
}

std::string unexpectedArrayMsg(const char *fn, const char *what,
                               const JVal &v)
{
    return strPrintf("%s(): %s %s for '%s'. Expected array.", fn, what,
                     jsonTypeName(v),
                     ogrJsonSpacedSerialize(v).c_str());
}

bool readCoordTuple(const JVal &arr, double *xyz, bool &hasZ, bool &err)
{
    if (arr.type != JVal::ARRAY)
    {
        diagReplay(CE_Failure,
                   unexpectedArrayMsg("OGRGeoJSONReadRawPoint",
                                      "invalid Point. Unexpected type",
                                      arr));
        err = true;
        return false;
    }
    if (arr.arr.size() < 2)
    {
        diag(CE_Warning,
             "OGRGeoJSONReadRawPoint(): Invalid coord dimension for '" +
                 ogrJsonSpacedSerialize(arr) +
                 "'. At least 2 dimensions must be present.");
        err = true;
        return false;
    }
    const char *names[3] = {"x", "y", "z"};
    bool bad = false;
    for (int i = 0; i < 3 && i < (int)arr.arr.size(); i++)
    {
        const JVal &c = arr.arr[i];
        if (c.type == JVal::INT)
            xyz[i] = (double)c.i;
        else if (c.type == JVal::DOUBLE)
            xyz[i] = c.d;
        else
        {
            const char *tn = c.type == JVal::STRING   ? "string"
                             : c.type == JVal::BOOL   ? "boolean"
                             : c.type == JVal::NUL    ? "null"
                             : c.type == JVal::ARRAY  ? "array"
                                                      : "object";
            diagReplay(CE_Failure,
                       strPrintf("OGRGeoJSONGetCoordinate(): invalid '%s' "
                                 "coordinate. Unexpected type %s for '%s'. "
                                 "Expected double or integer.",
                                 names[i], tn,
                                 ogrJsonSpacedSerialize(c).c_str()));
            bad = true;
        }
    }
    if (bad)
    {
        err = true;
        return false;
    }
    if (arr.arr.size() > 3)
        warnTooManyCoords(arr);
    if (arr.arr.size() >= 3)
        hasZ = true;
    else
        xyz[2] = 0.0;
    return true;
}

bool readCoordList(const JVal &arr, OgrGeometry &g, bool &err)
{
    if (arr.type != JVal::ARRAY)
    {
        err = true;
        return false;
    }
    for (const JVal &c : arr.arr)
    {
        double xyz[3] = {0, 0, 0};
        if (!readCoordTuple(c, xyz, g.hasZ, err))
            return false;
        g.coords.push_back(xyz[0]);
        g.coords.push_back(xyz[1]);
        g.coords.push_back(xyz[2]);
    }
    return true;
}

void setZRecursive(OgrGeometry &g)
{
    g.hasZ = true;
    for (OgrGeometry &p : g.parts)
        setZRecursive(p);
}

bool readGeometry(const JVal &j, OgrGeometry &g, bool &err,
                  bool top = true);

// polygon ring semantics: a failed first ring nulls the whole polygon
// (parsing stops there); a failed later ring is dropped and parsing
// continues; empty rings are kept (visible in the JSON export, elided
// by the WKT writer); non-empty rings get a closure check (z
// participates once the ring is 3D)
bool readPolygonRings(const JVal &coords, OgrGeometry &g, bool &err)
{
    size_t idx = 0;
    for (const JVal &sub : coords.arr)
    {
        // a null ring is skipped silently but keeps its element index
        if (sub.type == JVal::NUL)
        {
            ++idx;
            continue;
        }
        if (sub.type != JVal::ARRAY)
        {
            diagReplay(CE_Failure,
                       unexpectedArrayMsg("OGRGeoJSONReadLinearRing",
                                          "invalid geometry. Unexpected "
                                          "type",
                                          sub));
            if (idx == 0)
            {
                err = true;
                return false;
            }
            ++idx;
            continue;
        }
        OgrGeometry ring;
        ring.type = 2;
        bool rerr = false;
        if (!readCoordList(sub, ring, rerr))
        {
            if (idx == 0)
            {
                err = rerr;
                return false;
            }
            ++idx;
            continue;
        }
        size_t np = ring.coords.size() / 3;
        if (np >= 1)
        {
            size_t last = (np - 1) * 3;
            bool closed = ring.coords[0] == ring.coords[last] &&
                          ring.coords[1] == ring.coords[last + 1] &&
                          (!ring.hasZ ||
                           ring.coords[2] == ring.coords[last + 2]);
            if (!closed)
                diag(CE_Warning,
                     "Non closed ring detected. To avoid accepting "
                     "it, set the OGR_GEOMETRY_ACCEPT_UNCLOSED_RING "
                     "configuration option to NO");
        }
        if (ring.hasZ)
            g.hasZ = true;
        g.parts.push_back(std::move(ring));
        ++idx;
    }
    return true;
}

bool readGeometryBody(const JVal &j, int type, OgrGeometry &g, bool &err)
{
    static const char *readerName[8] = {"",
                                        "Point",
                                        "LineString",
                                        "Polygon",
                                        "MultiPoint",
                                        "MultiLineString",
                                        "MultiPolygon",
                                        "GeometryCollection"};
    g.type = type;
    if (type == 7)
    {
        const JVal *geoms = j.get("geometries");
        if (!geoms || geoms->type == JVal::NUL)
        {
            diagReplay(CE_Failure,
                       "Invalid GeometryCollection object. Missing "
                       "'geometries' member.");
            err = true;
            return false;
        }
        if (geoms->type != JVal::ARRAY)
        {
            diagReplay(
                CE_Warning,
                unexpectedArrayMsg("OGRGeoJSONReadGeometryCollection",
                                   "unexpected type of JSON construct",
                                   *geoms));
            err = true;
            return false;
        }
        for (const JVal &sub : geoms->arr)
        {
            if (sub.type == JVal::NUL)
            {
                diagReplay(CE_Warning,
                           "OGRGeoJSONReadGeometryCollection(): "
                           "skipping null sub-geometry");
                continue;
            }
            OgrGeometry sg;
            bool serr = false;
            if (!readGeometry(sub, sg, serr, false))
                continue;
            if (sg.hasZ)
                g.hasZ = true;
            g.parts.push_back(std::move(sg));
        }
        if (g.hasZ)
            setZRecursive(g);
        return true;
    }
    const JVal *coords = j.get("coordinates");
    if (!coords || coords->type == JVal::NUL)
    {
        // only the Point reader announces itself in the missing-member
        // message
        diagReplay(CE_Failure,
                   type == 1
                       ? strPrintf("OGRGeoJSONRead%s(): invalid %s "
                                   "object. Missing 'coordinates' "
                                   "member.",
                                   readerName[type], readerName[type])
                       : strPrintf("Invalid %s object. Missing "
                                   "'coordinates' member.",
                                   readerName[type]));
        err = true;
        return false;
    }
    if (coords->type != JVal::ARRAY && type != 1)
    {
        switch (type)
        {
            case 2:
                diagReplay(CE_Failure,
                           unexpectedArrayMsg(
                               "OGRGeoJSONReadLineString",
                               "invalid geometry. Unexpected type", *coords));
                break;
            case 3:
                diagReplay(CE_Warning,
                           unexpectedArrayMsg(
                               "OGRGeoJSONReadPolygon",
                               "unexpected type of JSON construct",
                               *coords));
                break;
            case 4:
                diagReplay(CE_Failure,
                           unexpectedArrayMsg(
                               "OGRGeoJSONReadMultiPoint",
                               "invalid MultiPoint. Unexpected type",
                               *coords));
                break;
            case 5:
                diagReplay(CE_Failure,
                           unexpectedArrayMsg(
                               "OGRGeoJSONReadLineString",
                               "invalid LineString. Unexpected type",
                               *coords));
                break;
            case 6:
                diagReplay(CE_Warning,
                           unexpectedArrayMsg(
                               "OGRGeoJSONReadMultiPolygon",
                               "unexpected type of JSON construct",
                               *coords));
                break;
        }
        err = true;
        return false;
    }
    switch (type)
    {
        case 1:
        {
            double xyz[3] = {0, 0, 0};
            if (!readCoordTuple(*coords, xyz, g.hasZ, err))
                return false;
            // a NaN in x or y degrades the point to EMPTY (z may be NaN)
            if (!std::isnan(xyz[0]) && !std::isnan(xyz[1]))
                g.coords.assign(xyz, xyz + 3);
            return true;
        }
        case 2:
            return readCoordList(*coords, g, err);
        case 3:
            return readPolygonRings(*coords, g, err);
        case 5:
        {
            for (const JVal &sub : coords->arr)
            {
                if (sub.type == JVal::NUL)
                {
                    diagReplay(CE_Failure,
                               "OGRGeoJSONReadLineString(): invalid "
                               "LineString object. Got null.");
                    continue;
                }
                if (sub.type != JVal::ARRAY)
                {
                    diagReplay(CE_Failure,
                               unexpectedArrayMsg(
                                   "OGRGeoJSONReadLineString",
                                   "invalid geometry. Unexpected type",
                                   sub));
                    continue;
                }
                OgrGeometry line;
                line.type = 2;
                bool lerr = false;
                if (!readCoordList(sub, line, lerr))
                    continue;
                if (line.hasZ)
                    g.hasZ = true;
                g.parts.push_back(std::move(line));
            }
            return true;
        }
        case 4:
        {
            for (const JVal &sub : coords->arr)
            {
                OgrGeometry pt;
                pt.type = 1;
                double xyz[3] = {0, 0, 0};
                if (!readCoordTuple(sub, xyz, pt.hasZ, err))
                    return false;
                if (std::isnan(xyz[0]) || std::isnan(xyz[1]))
                {
                    // NaN x/y degrades the member to an empty point
                    pt.hasZ = false;
                    g.parts.push_back(std::move(pt));
                    continue;
                }
                pt.coords.assign(xyz, xyz + 3);
                if (pt.hasZ)
                    g.hasZ = true;
                g.parts.push_back(std::move(pt));
            }
            return true;
        }
        case 6:
        {
            for (const JVal &sub : coords->arr)
            {
                if (sub.type == JVal::NUL)
                {
                    OgrGeometry emptyPoly;
                    emptyPoly.type = 3;
                    g.parts.push_back(std::move(emptyPoly));
                    continue;
                }
                if (sub.type != JVal::ARRAY)
                {
                    diagReplay(CE_Warning,
                               unexpectedArrayMsg(
                                   "OGRGeoJSONReadPolygon",
                                   "unexpected type of JSON construct",
                                   sub));
                    continue;
                }
                OgrGeometry poly;
                poly.type = 3;
                bool perr = false;
                if (!readPolygonRings(sub, poly, perr))
                    continue;
                if (poly.hasZ)
                    g.hasZ = true;
                g.parts.push_back(std::move(poly));
            }
            return true;
        }
    }
    err = true;
    return false;
}

bool readGeometry(const JVal &j, OgrGeometry &g, bool &err, bool top)
{
    if (j.type != JVal::OBJECT)
    {
        diagReplay(CE_Warning,
                   "Unsupported geometry type detected. Feature gets "
                   "NULL geometry assigned.");
        err = true;
        return false;
    }
    std::string t = j.getString("type");
    int type = geomTypeFromName(t);
    if (type < 0)
    {
        // top-level unknown types surface at open too; nested ones only
        // on feature iteration
        if (top)
            diagReplay(CE_Warning,
                       "Unsupported geometry type detected. Feature gets "
                       "NULL geometry assigned.");
        else
            diag(CE_Warning,
                 "Unsupported geometry type detected. Feature gets "
                 "NULL geometry assigned.");
        err = false;
        return false;
    }
    if (!readGeometryBody(j, type, g, err))
        return false;
    // mixed-dimension containers promote every member to Z (fill 0),
    // GeometryCollection included
    if (g.hasZ)
    {
        std::function<void(OgrGeometry &)> pz = [&](OgrGeometry &p) {
            p.hasZ = true;
            for (OgrGeometry &s : p.parts)
                pz(s);
        };
        pz(g);
    }
    return true;
}

struct FieldAccum
{
    std::string name;
    bool typeSet = false;
    int type = OFTString;
    int subType = OFSTNone;
    int tzAggr = -1;
};

// mirror of OGRGeoJSONReaderAddOrUpdateField type promotion
void updateFieldType(FieldAccum &f, const JVal &v)
{
    if (v.type == JVal::NUL)
    {
        return;
    }

    int nt;
    int nst = OFSTNone;
    switch (v.type)
    {
        case JVal::BOOL:
            nt = OFTInteger;
            nst = OFSTBoolean;
            break;
        case JVal::INT:
            nt = (v.i >= INT32_MIN && v.i <= INT32_MAX) ? OFTInteger
                                                        : OFTInteger64;
            break;
        case JVal::DOUBLE:
            nt = OFTReal;
            break;
        case JVal::STRING:
        {
            OgrDateTime dt;
            if (ogrParseDate(v.s, dt))
            {
                if (dt.hasDate && dt.hasTime)
                    nt = OFTDateTime;
                else if (dt.hasDate)
                    nt = OFTDate;
                else
                    nt = OFTTime;
            }
            else
                nt = OFTString;
            break;
        }
        case JVal::ARRAY:
        {
            // an empty array types a fresh field as String(JSON), is
            // absorbed silently by list-typed and String-typed fields,
            // and degrades scalar-typed fields to String(JSON)
            if (v.arr.empty())
            {
                if (f.typeSet &&
                    (f.type == OFTIntegerList ||
                     f.type == OFTInteger64List ||
                     f.type == OFTRealList || f.type == OFTStringList ||
                     f.type == OFTString))
                    return;
                f.typeSet = true;
                f.type = OFTString;
                f.subType = OFSTJSON;
                return;
            }
            bool anyStr = false, anyReal = false, any64 = false,
                 anyBool = false, allBool = true, anyOther = false;
            for (const JVal &e : v.arr)
            {
                if (e.type == JVal::BOOL)
                {
                    anyBool = true;
                    continue;
                }
                allBool = false;
                if (e.type == JVal::INT)
                {
                    if (e.i < INT32_MIN || e.i > INT32_MAX)
                        any64 = true;
                }
                else if (e.type == JVal::DOUBLE)
                    anyReal = true;
                else if (e.type == JVal::STRING)
                    anyStr = true;
                else
                    anyOther = true;
            }
            bool anyNonStr = false;
            for (const JVal &e : v.arr)
                if (e.type != JVal::STRING)
                    anyNonStr = true;
            if (anyOther || (anyStr && anyNonStr) ||
                (anyBool && !allBool))
            {
                nt = OFTString;
                nst = OFSTJSON;
            }
            else if (anyStr)
                nt = OFTStringList;
            else if (anyReal)
                nt = OFTRealList;
            else if (any64)
                nt = OFTInteger64List;
            else
            {
                nt = OFTIntegerList;
                if (allBool)
                    nst = OFSTBoolean;
                else
                    nst = OFSTNone;
            }
            break;
        }
        case JVal::OBJECT:
            nt = OFTString;
            nst = OFSTJSON;
            break;
        default:
            return;
    }

    // datetime timezone aggregation
    if (nt == OFTDateTime || nt == OFTDate || nt == OFTTime)
    {
        OgrDateTime dt;
        ogrParseDate(v.s, dt);
        int tz = dt.tzFlag;
        if (tz == 0)
            f.tzAggr = 0;  // a naive value locks the field to unknown
        else if (f.tzAggr == -1)
            f.tzAggr = tz;
        else if (f.tzAggr != 0 && f.tzAggr != tz)
            f.tzAggr = -2;
    }

    if (!f.typeSet)
    {
        f.typeSet = true;
        f.type = nt;
        f.subType = nst;
        return;
    }

    int ct = f.type, cst = f.subType;
    if (ct == nt)
    {
        if (cst != nst)
        {
            // Boolean + non-boolean of same type drops subtype
            if (cst == OFSTBoolean || nst == OFSTBoolean)
                f.subType = OFSTNone;
            else if (cst == OFSTJSON || nst == OFSTJSON)
                f.subType = OFSTJSON;
        }
        return;
    }

    auto isNum = [](int t) {
        return t == OFTInteger || t == OFTInteger64 || t == OFTReal;
    };
    auto isNumList = [](int t) {
        return t == OFTIntegerList || t == OFTInteger64List ||
               t == OFTRealList;
    };
    auto isDT = [](int t) {
        return t == OFTDate || t == OFTTime || t == OFTDateTime;
    };

    if (isNum(ct) && isNum(nt))
    {
        if (ct == OFTReal || nt == OFTReal)
            f.type = OFTReal;
        else
            f.type = OFTInteger64;
        f.subType = OFSTNone;
        return;
    }
    if (isDT(ct) && isDT(nt))
    {
        if ((ct == OFTDate && nt == OFTDateTime) ||
            (ct == OFTDateTime && nt == OFTDate))
        {
            f.type = OFTDateTime;
            f.subType = OFSTNone;
            return;
        }
        f.type = OFTString;
        f.subType = OFSTJSON;
        return;
    }
    if ((isDT(ct) && nt == OFTString) || (ct == OFTString && isDT(nt)))
    {
        if (cst == OFSTJSON || nst == OFSTJSON)
        {
            f.type = OFTString;
            f.subType = OFSTJSON;
            return;
        }
        f.type = OFTString;
        f.subType = OFSTNone;
        return;
    }
    if ((isNum(ct) || isNumList(ct)) && (isNum(nt) || isNumList(nt)))
    {
        // scalar/list promotion
        bool anyReal = ct == OFTReal || nt == OFTReal ||
                       ct == OFTRealList || nt == OFTRealList;
        bool any64 = ct == OFTInteger64 || nt == OFTInteger64 ||
                     ct == OFTInteger64List || nt == OFTInteger64List;
        if (anyReal)
            f.type = OFTRealList;
        else if (any64)
            f.type = OFTInteger64List;
        else
            f.type = OFTIntegerList;
        if (!(f.subType == OFSTBoolean && nst == OFSTBoolean))
            f.subType = OFSTNone;
        return;
    }
    if ((ct == OFTStringList || nt == OFTStringList) &&
        (ct == OFTStringList || (ct == OFTString && cst != OFSTJSON) ||
         isDT(ct)) &&
        (nt == OFTStringList || nt == OFTString || isDT(nt)))
    {
        f.type = OFTStringList;
        f.subType = OFSTNone;
        return;
    }
    if (isNumList(ct) && (nt == OFTStringList))
    {
        f.type = OFTStringList;
        f.subType = OFSTNone;
        return;
    }
    if (ct == OFTStringList && isNumList(nt))
    {
        f.type = OFTStringList;
        f.subType = OFSTNone;
        return;
    }
    // everything else degrades to String(JSON)
    f.type = OFTString;
    f.subType = OFSTJSON;
}

struct DagNode
{
    std::string name;
    std::vector<int> after;  // edges: this -> after (this precedes)
};

// structural dimension: only the FIRST position of the array decides;
// arrays with more than 3 members never count as 3D even though
// translation keeps z
bool structZOf(const JVal &a)
{
    if (a.type != JVal::ARRAY || a.arr.empty())
        return false;
    if (a.arr[0].type == JVal::ARRAY)
        return structZOf(a.arr[0]);
    return a.arr.size() == 3;
}

bool geomStructZ(const JVal &gj)
{
    int ty = geomTypeFromName(gj.getString("type"));
    if (ty < 0 || ty == 7)
        return false;
    const JVal *c = gj.get("coordinates");
    return c && structZOf(*c);
}

enum
{
    kGeoJsonFile = 0,
    kGeoJsonStdin = 1,
    kGeoJsonSeq = 2,
};

void buildGeoJsonLayerCore(OgrLayer &lyr,
                           const std::vector<const JVal *> &featureObjs,
                           bool bareFeatureRoot, const JVal *bareGeomJson,
                           const std::vector<bool> *aggMask = nullptr,
                           int srcMode = kGeoJsonFile)
{
    // ---- schema pass: field types + DAG field ordering
    std::map<std::string, int> nodeIdx;
    std::vector<DagNode> nodes;
    std::vector<FieldAccum> accums;
    std::vector<std::set<std::pair<int, int>>::value_type> edgeList;
    std::set<std::pair<int, int>> edges;

    bool anyId = false;
    bool allIdInt = true, allIdString = true;
    bool anyIdNeg = false, anyId64 = false;
    bool firstIdString = false, anyIdOther = false;
    bool anyIdNonInt = false;

    for (const JVal *fj : featureObjs)
    {
        const JVal *idM = fj->get("id");
        if (idM)
        {
            bool first = !anyId;
            anyId = true;
            if (idM->type != JVal::INT)
            {
                allIdInt = false;
                anyIdNonInt = true;
            }
            else
            {
                if (idM->i < 0)
                    anyIdNeg = true;
                if (idM->i > INT32_MAX)
                    anyId64 = true;
            }
            if (idM->type == JVal::STRING)
            {
                if (first)
                    firstIdString = true;
            }
            else
                allIdString = false;
            if (first && (idM->type == JVal::DOUBLE ||
                          idM->type == JVal::BOOL))
                anyIdOther = true;
        }
        else
        {
            allIdString = false;
            allIdInt = false;
        }
    }
    // features without a "properties" member expose their top-level
    // members (except type/geometry/bbox) as attributes, id included;
    // a properties key named "id" absorbs feature-level ids and keeps
    // them out of the FID
    bool anyFlatId = false;
    bool propsIdKey = false;
    size_t firstTopIdIdx = SIZE_MAX, firstPropsIdIdx = SIZE_MAX;
    bool firstTopIdIsInt = false;
    for (size_t fi = 0; fi < featureObjs.size(); ++fi)
    {
        const JVal *fj = featureObjs[fi];
        const JVal *props = fj->get("properties");
        if (!props && fj->get("id"))
            anyFlatId = true;
        if (fj->get("id") && firstTopIdIdx == SIZE_MAX)
        {
            firstTopIdIdx = fi;
            firstTopIdIsInt = fj->get("id")->type == JVal::INT;
        }
        if (props && props->type == JVal::OBJECT && props->get("id"))
        {
            propsIdKey = true;
            if (firstPropsIdIdx == SIZE_MAX)
                firstPropsIdIdx = fi;
        }
    }
    bool idNegField = anyId && allIdInt && anyIdNeg && !propsIdKey;
    bool idStrField = !idNegField && (firstIdString || anyIdOther);
    bool idAsField = idStrField || idNegField || anyFlatId;
    if (idNegField)
        lyr.fidColumn = "id";

    // the "id" pseudo-field enters the ordering DAG when the first
    // id-bearing feature is processed, ahead of that feature's own
    // property keys
    int idNode = -1;
    auto ensureIdNode = [&]() {
        if (idNode >= 0)
            return;
        auto it = nodeIdx.find("id");
        if (it != nodeIdx.end())
        {
            idNode = it->second;
            return;
        }
        idNode = (int)nodes.size();
        nodeIdx["id"] = idNode;
        nodes.push_back({"\x01id", {}});
        accums.emplace_back();
        accums.back().name = "\x01id";
    };
    // feature-level ids weigh on the field type absorbingly: ints keep
    // or widen integer fields, everything else lands as plain String,
    // and no JSON subtype promotion happens
    auto feedIdSample = [&](const JVal &v) {
        FieldAccum &ia = accums[idNode];
        bool isInt = v.type == JVal::INT;
        bool wide = isInt && (v.i > INT32_MAX || v.i < INT32_MIN);
        if (!ia.typeSet)
        {
            ia.typeSet = true;
            ia.type = isInt ? (wide ? OFTInteger64 : OFTInteger)
                            : OFTString;
            ia.subType = OFSTNone;
        }
        else if (isInt && ia.type == OFTInteger && wide)
            ia.type = OFTInteger64;
    };
    for (const JVal *fj : featureObjs)
    {
        const JVal *props = fj->get("properties");
        int prev = -1;
        if (props && idAsField && !propsIdKey && fj->get("id"))
        {
            ensureIdNode();
            feedIdSample(*fj->get("id"));
            prev = idNode;
        }
        // an edge that would close a cycle is refused, like
        // DirectedAcyclicGraph::addEdge
        auto pathExists = [&](int from, int to) {
            std::vector<int> stack{from};
            std::set<int> seen;
            while (!stack.empty())
            {
                int cur = stack.back();
                stack.pop_back();
                if (cur == to)
                    return true;
                if (!seen.insert(cur).second)
                    continue;
                for (int t : nodes[cur].after)
                    stack.push_back(t);
            }
            return false;
        };
        auto chainField = [&](const std::string &key, const JVal &val) {
            int idx;
            if (key == "id" && !props)
            {
                ensureIdNode();
                idx = idNode;
                feedIdSample(val);
            }
            else
            {
                auto it = nodeIdx.find(key);
                if (it == nodeIdx.end())
                {
                    idx = (int)nodes.size();
                    nodeIdx[key] = idx;
                    nodes.push_back({key, {}});
                    accums.emplace_back();
                    accums.back().name = key;
                }
                else
                    idx = it->second;
                updateFieldType(accums[idx], val);
            }
            if (prev >= 0 && prev != idx && !edges.count({prev, idx}) &&
                !pathExists(idx, prev) &&
                edges.insert({prev, idx}).second)
                nodes[prev].after.push_back(idx);
            prev = idx;
        };
        if (props && props->type == JVal::OBJECT)
        {
            for (const auto &kv : props->obj)
                chainField(kv.first, kv.second);
        }
        else if (!props)
        {
            for (const auto &kv : fj->obj)
            {
                if (kv.first == "type" || kv.first == "geometry" ||
                    kv.first == "bbox")
                    continue;
                chainField(kv.first, kv.second);
            }
        }
    }

    // topological sort (Kahn); ties broken by field name, matching
    // GDAL's DirectedAcyclicGraph ordering
    auto sortName = [&](int i) -> const std::string & {
        static const std::string idName = "id";
        return nodes[i].name == "\x01id" ? idName : nodes[i].name;
    };
    std::vector<int> indeg(nodes.size(), 0);
    for (const auto &n : nodes)
        for (int t : n.after)
            indeg[t]++;
    std::vector<int> order;
    std::vector<bool> done(nodes.size(), false);
    for (size_t iter = 0; iter < nodes.size(); iter++)
    {
        int pick = -1;
        for (size_t i = 0; i < nodes.size(); i++)
            if (!done[i] && indeg[i] == 0 &&
                (pick < 0 || sortName((int)i) < sortName(pick)))
                pick = (int)i;
        if (pick < 0)
        {
            for (size_t i = 0; i < nodes.size(); i++)
                if (!done[i] &&
                    (pick < 0 || sortName((int)i) < sortName(pick)))
                    pick = (int)i;
        }
        done[pick] = true;
        order.push_back(pick);
        for (int t : nodes[pick].after)
            indeg[t]--;
    }

    std::vector<int> fieldPos(nodes.size(), -1);
    int idFieldPos = -1;
    (void)allIdString;
    for (size_t i = 0; i < order.size(); i++)
    {
        if (order[i] == idNode)
        {
            OgrFieldDefn fd;
            fd.name = "id";
            const FieldAccum &ia = accums[idNode];
            fd.type = idNegField ? (anyId64 ? OFTInteger64 : OFTInteger)
                      : ia.typeSet ? ia.type
                                   : OFTString;
            if (!idNegField && ia.typeSet)
                fd.subType = ia.subType;
            idFieldPos = (int)lyr.fields.size();
            fieldPos[order[i]] = idFieldPos;
            lyr.fields.push_back(fd);
            continue;
        }
        const FieldAccum &a = accums[order[i]];
        OgrFieldDefn fd;
        fd.name = a.name;
        fd.type = a.typeSet ? a.type : OFTString;
        fd.subType = a.typeSet ? a.subType : OFSTNone;
        fd.tzAggr = a.tzAggr;
        if (fd.subType == OFSTBoolean)
            fd.width = 1;
        fieldPos[order[i]] = (int)lyr.fields.size();
        lyr.fields.push_back(fd);
    }

    // an integer-typed "id" property becomes the FID column when no
    // feature carries a usable top-level id; explicit values are taken
    // as-is (duplicates allowed), absent/null ones fall back to the
    // first-free-at-or-after-index rule
    int propIdPos = -1;
    if (!(anyId && !anyIdNonInt && !anyIdNeg) && propsIdKey && idNode < 0)
        for (size_t i = 0; i < lyr.fields.size(); ++i)
            if (lyr.fields[i].name == "id" &&
                (lyr.fields[i].type == OFTInteger ||
                 lyr.fields[i].type == OFTInteger64))
            {
                propIdPos = (int)i;
                lyr.fidColumn = "id";
                break;
            }
    // a String "id" field created by properties absorbs feature-level
    // ids of non-flat features as attributes and keeps them out of the
    // FID
    int propsIdStrPos = -1;
    if (propsIdKey)
        for (size_t i = 0; i < lyr.fields.size(); ++i)
            if (lyr.fields[i].name == "id" &&
                lyr.fields[i].type == OFTString)
            {
                propsIdStrPos = (int)i;
                break;
            }
    // int feature-level ids lock in FID duty when the first id-bearing
    // feature does not come after the first properties-id feature;
    // otherwise a props-created String id field absorbs them
    bool fidLock = firstTopIdIdx <= firstPropsIdIdx && firstTopIdIsInt;
    bool absorbMode = propsIdStrPos >= 0 && !fidLock;

    // ---- feature pass
    long long autoFid = 0;
    std::set<long long> seenFids;
    bool fidWarned = false;
    bool any64Fid = false;
    bool haveGeomType = false;
    int layerGeomType = 0;
    bool layerHasZ = false;
    bool anyGeom = false;

    for (const JVal *fj : featureObjs)
    {
        OgrFeature feat;
        if (!aggMask && !bareFeatureRoot && !bareGeomJson)
            feat.gjNative =
                std::shared_ptr<const JVal>(std::shared_ptr<void>(), fj);
        feat.values.resize(lyr.fields.size());
        const JVal *props = fj->get("properties");
        bool flatFeat = props == nullptr;
        if (props && props->type == JVal::OBJECT)
        {
            for (const auto &kv : props->obj)
            {
                int idx = fieldPos[nodeIdx[kv.first]];
                feat.values[idx].set = true;
                feat.values[idx].v = kv.second;
            }
        }
        else if (flatFeat)
        {
            for (const auto &kv : fj->obj)
            {
                if (kv.first == "type" || kv.first == "geometry" ||
                    kv.first == "bbox" || kv.first == "id")
                    continue;
                int idx = fieldPos[nodeIdx[kv.first]];
                feat.values[idx].set = true;
                feat.values[idx].v = kv.second;
            }
        }
        // fid-from-props only counts values the properties object itself
        // provided; a flat "id" landing in the same field stays an
        // attribute
        bool propIdFromProps =
            propIdPos >= 0 && feat.values[propIdPos].set;
        const JVal *idM = fj->get("id");
        bool idMFilled = false;
        if (idM && idFieldPos >= 0 &&
            (flatFeat || ((idStrField || idNegField) &&
                          !feat.values[idFieldPos].set)))
        {
            idMFilled = true;
            bool fieldIsInt =
                lyr.fields[idFieldPos].type == OFTInteger ||
                lyr.fields[idFieldPos].type == OFTInteger64;
            OgrFieldValue &iv = feat.values[idFieldPos];
            iv.set = true;
            if (fieldIsInt && idM->type == JVal::STRING)
            {
                char *endp = nullptr;
                long long v = strtoll(idM->s.c_str(), &endp, 10);
                if (!endp || *endp != '\0')
                    lyr.matEvents.push_back(
                        {(long long)lyr.features.size(), (int)CE_Warning,
                         strPrintf("Value '%s' of field %s.id parsed "
                                   "incompletely to integer %lld.",
                                   idM->s.c_str(), lyr.name.c_str(), v),
                         false});
                JVal nv;
                nv.type = JVal::INT;
                nv.i = v;
                iv.v = std::move(nv);
            }
            else if (idNegField || idM->type == JVal::STRING ||
                     (flatFeat && idM->type == JVal::INT))
                iv.v = *idM;
            else
            {
                JVal sv;
                sv.type = JVal::STRING;
                sv.s = ogrJsonSpacedSerialize(*idM);
                iv.v = std::move(sv);
            }
        }
        if (!idMFilled && idM && !flatFeat && absorbMode &&
            !feat.values[propsIdStrPos].set)
        {
            idMFilled = true;
            OgrFieldValue &iv = feat.values[propsIdStrPos];
            iv.set = true;
            if (idM->type == JVal::STRING)
                iv.v = *idM;
            else
            {
                JVal sv;
                sv.type = JVal::STRING;
                sv.s = ogrJsonSpacedSerialize(*idM);
                iv.v = std::move(sv);
            }
        }

        const JVal *geomM = fj->get("geometry");
        std::vector<OgrLayer::Diag> featDiags;
        if (geomM && geomM->type == JVal::OBJECT)
        {
            bool gerr = false;
            OgrGeometry g;
            if (!bareFeatureRoot)
                gDiags = &featDiags;
            if (readGeometry(*geomM, g, gerr))
            {
                feat.hasGeom = true;
                feat.geom = std::move(g);
            }
            gDiags = nullptr;
        }

        // fid: explicit ids come from usable "id" members; auto features
        // take the first free fid at or after their index; duplicated
        // explicit ids are altered the same way with a one-shot warning
        bool explicitFid = false;
        if (propIdPos >= 0)
        {
            const OgrFieldValue &iv = feat.values[propIdPos];
            if (propIdFromProps && iv.set && iv.v.type == JVal::INT)
            {
                feat.fid = iv.v.i;
                explicitFid = true;
            }
            else if (propIdFromProps && iv.set &&
                     iv.v.type == JVal::BOOL)
            {
                feat.fid = iv.v.b ? 1 : 0;
                explicitFid = true;
            }
        }
        else if (!idNegField && !idStrField &&
                 !(flatFeat && propsIdKey) &&
                 !(!flatFeat && absorbMode) && idM &&
                 idM->type == JVal::INT && idM->i >= 0)
        {
            feat.fid = idM->i;
            explicitFid = true;
        }
        else if (!idNegField && !idStrField && !propsIdKey && idM &&
                 idM->type == JVal::STRING)
        {
            feat.fid = strtoll(idM->s.c_str(), nullptr, 10);
            explicitFid = true;
        }
        bool needAlter = !explicitFid;
        if (explicitFid && seenFids.count(feat.fid))
        {
            if (!fidWarned)
            {
                featDiags.push_back(
                    {(int)CE_Warning,
                     strPrintf("Several features with id = %lld have been "
                               "found. Altering it to be unique. This "
                               "warning will not be emitted anymore for "
                               "this layer",
                               feat.fid),
                     -1, false});
                fidWarned = true;
            }
            needAlter = true;
        }
        if (needAlter)
        {
            long long cand = autoFid;
            while (seenFids.count(cand))
                cand++;
            feat.fid = cand;
        }
        seenFids.insert(feat.fid);
        if (feat.fid > INT32_MAX)
            any64Fid = true;
        autoFid++;
        for (OgrLayer::Diag &d : featDiags)
        {
            d.fid = feat.fid;
            lyr.pendingDiags.push_back(std::move(d));
        }
        lyr.features.push_back(std::move(feat));
    }
    if (any64Fid || anyId64)
        lyr.metadata.emplace_back("OLMD_FID64", "YES");

    // ---- layer geometry type: structural, from the declared JSON type
    // names and raw coordinate nesting (works even when coordinate
    // parsing fails at feature-iteration time)
    // only the FIRST position of each geometry decides its dimension:
    // probed with a MultiLineString whose second line is 3D (layer
    // stays 2D) vs mixed Point features (any 3D feature promotes);
    // arrays with more than 3 members never count as 3D even though
    // translation keeps z
    bool anyStructZ = false;
    auto noteGeom = [&](const JVal &gj) {
        int ty = geomTypeFromName(gj.getString("type"));
        if (ty < 0)
        {
            // an unrecognized type name still contributes: it can never
            // match anything so the layer type goes Unknown
            haveGeomType = true;
            layerGeomType = 0;
            return;
        }
        anyGeom = true;
        if (geomStructZ(gj))
            anyStructZ = true;
        if (!haveGeomType)
        {
            haveGeomType = true;
            layerGeomType = ty;
        }
        else if (layerGeomType != ty)
            layerGeomType = 0;
    };
    if (bareGeomJson)
        noteGeom(*bareGeomJson);
    for (size_t fi = 0; fi < featureObjs.size(); ++fi)
    {
        if (aggMask && !(*aggMask)[fi])
            continue;
        const JVal *geomM = featureObjs[fi]->get("geometry");
        if (geomM && geomM->type == JVal::OBJECT)
            noteGeom(*geomM);
    }
    layerHasZ = layerGeomType != 0 && anyStructZ;
    lyr.geomType = layerGeomType;
    lyr.geomHasZ = layerGeomType != 0 && layerHasZ;

    // extent: derived from a structural scan of the raw coordinate
    // arrays; a DIRECT Point position with more than 3 members is
    // skipped (its translated geometry still exists), nested vertices
    // always count x/y; only when the scan collects nothing does the
    // extent fall back to the translated geometry envelopes
    auto extPoint = [&](double x, double y) {
        if (std::isnan(x) || std::isnan(y))
            return;
        if (!lyr.hasExtent)
        {
            lyr.extent[0] = lyr.extent[2] = x;
            lyr.extent[1] = lyr.extent[3] = y;
            lyr.hasExtent = true;
        }
        else
        {
            lyr.extent[0] = std::min(lyr.extent[0], x);
            lyr.extent[1] = std::min(lyr.extent[1], y);
            lyr.extent[2] = std::max(lyr.extent[2], x);
            lyr.extent[3] = std::max(lyr.extent[3], y);
        }
    };
    auto numOf = [](const JVal &v, double &out) {
        if (v.type == JVal::INT)
            out = (double)v.i;
        else if (v.type == JVal::DOUBLE)
            out = v.d;
        else
            return false;
        return true;
    };
    auto tupleJunk = [&](const JVal &t) {
        if (t.type != JVal::ARRAY)
            return true;
        double d;
        for (size_t i = 0; i < 3 && i < t.arr.size(); ++i)
            if (!numOf(t.arr[i], d))
                return true;
        return false;
    };
    // legacy structure-probed scan, kept for nested unrecognized types
    std::function<bool(const JVal &, bool)> scanC =
        [&](const JVal &c, bool direct) -> bool {
        if (c.type != JVal::ARRAY || c.arr.empty())
            return true;
        if (c.arr[0].type == JVal::ARRAY)
        {
            const JVal *probe = nullptr;
            for (const JVal &s : c.arr)
                if (s.type == JVal::ARRAY && !s.arr.empty())
                {
                    probe = &s;
                    break;
                }
            if (!probe)
                return true;
            if (probe->arr[0].type == JVal::ARRAY)
            {
                bool clean = true;
                for (const JVal &s : c.arr)
                    clean = scanC(s, false) && clean;
                return clean;
            }
            for (const JVal &t : c.arr)
                if (tupleJunk(t))
                    return false;
            bool present = true;
            for (const JVal &t : c.arr)
            {
                if (t.arr.size() > 3)
                {
                    present = false;
                    continue;
                }
                if (t.arr.size() < 2)
                    continue;
                double x, y;
                if (numOf(t.arr[0], x) && numOf(t.arr[1], y))
                    extPoint(x, y);
            }
            return present;
        }
        if (tupleJunk(c))
            return false;
        if (direct && c.arr.size() > 3)
            return false;
        if (c.arr.size() < 2)
            return true;
        double x, y;
        if (numOf(c.arr[0], x) && numOf(c.arr[1], y))
            extPoint(x, y);
        return true;
    };
    // typed sequential scan: a non-numeric junk element or tuple member
    // stops the whole feature scan (its prefix keeps its extent
    // contribution), a numeric non-array element is skipped harmlessly,
    // a >3-member tuple kills presence and contributes nothing, short
    // and NaN tuples are skipped
    enum
    {
        kScanOk = 0,
        kScanStop = 1
    };
    auto scanTuple = [&](const JVal &t, bool &present) -> int {
        double d;
        for (size_t i = 0; i < 3 && i < t.arr.size(); ++i)
            if (!numOf(t.arr[i], d))
            {
                present = false;
                return kScanStop;
            }
        if (t.arr.size() > 3)
        {
            present = false;
            return kScanOk;
        }
        if (t.arr.size() < 2)
            return kScanOk;
        double x, y;
        numOf(t.arr[0], x);
        numOf(t.arr[1], y);
        extPoint(x, y);
        return kScanOk;
    };
    std::function<int(const JVal &, int, bool &)> scanList =
        [&](const JVal &c, int level, bool &present) -> int {
        for (const JVal &el : c.arr)
        {
            if (el.type == JVal::ARRAY)
            {
                int r = level <= 1
                            ? scanTuple(el, present)
                            : scanList(el, level - 1, present);
                if (r == kScanStop)
                    return kScanStop;
            }
            else if (el.type == JVal::INT || el.type == JVal::DOUBLE)
                continue;
            else
            {
                present = false;
                return kScanStop;
            }
        }
        return kScanOk;
    };
    // returns the feature's "presence": whether the geometry is a
    // recognized structure free of invalid tuples; only present
    // features arm the all-infinity extent fallback
    std::function<bool(const JVal &, bool)> scanG =
        [&](const JVal &gj, bool top) -> bool {
        int ty = geomTypeFromName(gj.getString("type"));
        if (ty < 0)
        {
            if (top)
                return false;
            // nested unrecognized types still get their raw
            // coordinates scanned generically
            const JVal *c = gj.get("coordinates");
            return !c || scanC(*c, false);
        }
        if (ty == 7)
        {
            const JVal *gs = gj.get("geometries");
            if (!gs || gs->type != JVal::ARRAY)
                return false;
            // a failed sub-geometry stops the collection scan
            for (const JVal &s : gs->arr)
                if (s.type != JVal::OBJECT || !scanG(s, false))
                    return false;
            return true;
        }
        const JVal *c = gj.get("coordinates");
        if (!c || c->type != JVal::ARRAY)
            return false;
        bool present = true;
        switch (ty)
        {
            case 1:
                scanTuple(*c, present);
                break;
            case 2:
            case 4:
                scanList(*c, 1, present);
                break;
            case 3:
            case 5:
                scanList(*c, 2, present);
                break;
            case 6:
                scanList(*c, 3, present);
                break;
        }
        return present;
    };
    bool anyPresence = false;
    if (bareGeomJson)
        anyPresence = scanG(*bareGeomJson, true) || anyPresence;
    for (size_t fi = 0; fi < featureObjs.size(); ++fi)
    {
        if (aggMask && !(*aggMask)[fi])
            continue;
        const JVal *geomM = featureObjs[fi]->get("geometry");
        if (geomM && geomM->type == JVal::OBJECT)
            anyPresence = scanG(*geomM, true) || anyPresence;
    }
    bool bareRoot = bareFeatureRoot || bareGeomJson;
    if (anyPresence && !lyr.hasExtent && srcMode == kGeoJsonFile &&
        !bareRoot)
    {
        lyr.extent[0] = lyr.extent[1] = INFINITY;
        lyr.extent[2] = lyr.extent[3] = -INFINITY;
        lyr.hasExtent = true;
    }
    lyr.geomDiagBase = !anyPresence;

    // a streamed source, a bare-root file, and a file none of whose
    // features is present derive the extent from the translated
    // geometry envelopes instead of the raw coordinate scan: curve
    // envelopes seed from the first vertex so a leading NaN sticks to
    // that axis while a later NaN fails every comparison and is
    // dropped, and a feature whose final envelope carries any NaN
    // contributes nothing at all
    if (srcMode != kGeoJsonFile || !anyPresence || bareRoot)
    {
        struct Env
        {
            double minx = HUGE_VAL, maxx = -HUGE_VAL;
            double miny = HUGE_VAL, maxy = -HUGE_VAL;
        };
        auto mn = [](double a, double b) { return a < b ? a : b; };
        auto mx = [](double a, double b) { return a > b ? a : b; };
        auto mergeEnv = [&](Env &e, const Env &o) {
            e.minx = mn(e.minx, o.minx);
            e.maxx = mx(e.maxx, o.maxx);
            e.miny = mn(e.miny, o.miny);
            e.maxy = mx(e.maxy, o.maxy);
        };
        std::function<void(const OgrGeometry &, Env &)> envOf =
            [&](const OgrGeometry &g, Env &e) {
                if (g.empty)
                    return;
                if (g.type == 1 && g.coords.size() >= 2)
                {
                    Env p;
                    p.minx = p.maxx = g.coords[0];
                    p.miny = p.maxy = g.coords[1];
                    mergeEnv(e, p);
                }
                else if (g.coords.size() >= 2)
                {
                    Env c;
                    c.minx = c.maxx = g.coords[0];
                    c.miny = c.maxy = g.coords[1];
                    for (size_t i = 3; i + 1 < g.coords.size(); i += 3)
                    {
                        double x = g.coords[i], y = g.coords[i + 1];
                        if (x < c.minx)
                            c.minx = x;
                        if (x > c.maxx)
                            c.maxx = x;
                        if (y < c.miny)
                            c.miny = y;
                        if (y > c.maxy)
                            c.maxy = y;
                    }
                    mergeEnv(e, c);
                }
                for (const OgrGeometry &p : g.parts)
                {
                    Env s;
                    envOf(p, s);
                    mergeEnv(e, s);
                }
            };
        lyr.hasExtent = false;
        for (const OgrFeature &feat : lyr.features)
        {
            if (!feat.hasGeom || feat.geom.empty)
                continue;
            Env fe;
            envOf(feat.geom, fe);
            if (fe.minx == HUGE_VAL && fe.maxx == -HUGE_VAL)
                continue;
            if (std::isnan(fe.minx) || std::isnan(fe.maxx) ||
                std::isnan(fe.miny) || std::isnan(fe.maxy))
                continue;
            if (!lyr.hasExtent)
            {
                lyr.extent[0] = fe.minx;
                lyr.extent[1] = fe.miny;
                lyr.extent[2] = fe.maxx;
                lyr.extent[3] = fe.maxy;
                lyr.hasExtent = true;
            }
            else
            {
                lyr.extent[0] = mn(lyr.extent[0], fe.minx);
                lyr.extent[1] = mn(lyr.extent[1], fe.miny);
                lyr.extent[2] = mx(lyr.extent[2], fe.maxx);
                lyr.extent[3] = mx(lyr.extent[3], fe.maxy);
            }
        }

        // when the structural detection lands on Unknown a streamed
        // layer falls back to the translated geometries (dropped ones
        // contribute nothing, no multi promotion, translated z counts);
        // files keep the structural Unknown
        if (srcMode == kGeoJsonStdin && lyr.geomType == 0)
        {
            bool any = false, mixed = false, tz = false;
            int tt = 0;
            for (const OgrFeature &feat : lyr.features)
            {
                if (!feat.hasGeom)
                    continue;
                if (!any)
                    tt = feat.geom.type;
                else if (tt != feat.geom.type)
                    mixed = true;
                any = true;
                tz = tz || feat.geom.hasZ;
            }
            if (any)
            {
                lyr.geomType = mixed ? 0 : tt;
                lyr.geomHasZ = !mixed && tz;
            }
        }
    }
}

void applyDefaultWgs84(OgrLayer &lyr)
{
    static const char *kWgs84Wkt =
        "GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\","
        "ELLIPSOID[\"WGS 84\",6378137,298.257223563,"
        "LENGTHUNIT[\"metre\",1]]],PRIMEM[\"Greenwich\",0,"
        "ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "CS[ellipsoidal,2],AXIS[\"geodetic latitude (Lat)\",north,"
        "ORDER[1],ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "AXIS[\"geodetic longitude (Lon)\",east,ORDER[2],"
        "ANGLEUNIT[\"degree\",0.0174532925199433]],"
        "ID[\"EPSG\",4326]]";
    bool sok = false;
    Srs s = Srs::fromUserInput(kWgs84Wkt, sok);
    if (sok)
    {
        lyr.srs = std::move(s);
        lyr.hasSrs = true;
    }
}

std::string layerNameFromPath(const std::string &path)
{
    std::string base = path;
    size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        base = base.substr(0, dot);
    if (base.empty())
        base = "OGRGeoJSON";
    return base;
}

// whitespace stripped outside string literals, escape-aware
std::string compactJson(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool inStr = false, esc = false;
    for (char c : s)
    {
        if (inStr)
        {
            out += c;
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
        {
            inStr = true;
            out += c;
            continue;
        }
        if (!isspace((unsigned char)c))
            out += c;
    }
    return out;
}

bool hasTypePair(const std::string &compact, const char *value)
{
    std::string pat = std::string("\"type\":\"") + value + "\"";
    return compact.find(pat) != std::string::npos;
}

// identification runs on the prefix the json-c tokenizer accepts before
// a hard error; a file that parses fully (or only fails at EOF) is
// scanned whole
std::string identifyHead(const std::string &content)
{
    std::string tdesc;
    size_t toff = 0;
    if (jsoncTokenerError(content, tdesc, toff) && toff < content.size())
        return content.substr(0, toff);
    return content;
}

bool firstCharIsBrace(const std::string &content)
{
    for (char c : content)
    {
        if (isspace((unsigned char)c))
            continue;
        return c == '{';
    }
    return false;
}

}  // namespace

// standalone geometry parse for callers outside the reader (raster clip
// --geometry); reader diagnostics are swallowed
bool ogrGeometryFromJsonValue(const JVal &j, OgrGeometry &g)
{
    std::vector<OgrLayer::Diag> sink;
    std::vector<OgrLayer::Diag> *saved = gDiags;
    gDiags = &sink;
    bool err = false;
    bool ok = readGeometry(j, g, err);
    gDiags = saved;
    return ok && !err;
}

bool jsonVectorIdentify(const std::string &head)
{
    if (head.find("\"type\"") != std::string::npos)
        return true;
    std::string chead = compactJson(head);
    if (chead.find("\"features\":[") != std::string::npos)
        return true;
    if (chead.rfind("{\"coordinates\":[", 0) == 0)
        return true;
    if (chead.find("\"fieldAliases\"") != std::string::npos ||
        chead.rfind("{\"spatialReference\":{\"wkid\"", 0) == 0 ||
        chead.find("\"esriFieldType") != std::string::npos ||
        chead.find("\"geometryType\":\"esriGeometry") !=
            std::string::npos)
        return true;
    return false;
}

std::unique_ptr<OgrDataset> openGeoJson(const std::string &path,
                                        std::string &err, bool weakPass)
{
    std::string content;
    if (!readFileToString(path, content) || !firstCharIsBrace(content))
    {
        err = "";
        return nullptr;
    }

    // identification: needs a "type":"<GeoJSON type>" pair found within
    // the prefix the tokenizer accepts before a hard error; files whose
    // prefix carries a "type":"Topology" pair are left to TopoJSON.
    // The weak pass (chained after ESRIJSON/TopoJSON) instead engages on
    // a features array or a leading coordinates member.
    std::string chead = compactJson(identifyHead(content));
    bool looksGeoJson = false;
    if (!weakPass)
    {
        for (const char *k :
             {"FeatureCollection", "Feature", "Point", "LineString",
              "Polygon", "MultiPoint", "MultiLineString", "MultiPolygon",
              "GeometryCollection"})
            if (hasTypePair(chead, k))
                looksGeoJson = true;
        if (hasTypePair(chead, "Topology"))
            looksGeoJson = false;
    }
    else
    {
        looksGeoJson =
            chead.find("\"features\":[") != std::string::npos ||
            chead.rfind("{\"coordinates\":[", 0) == 0;
    }
    if (!looksGeoJson)
    {
        err = "";
        return nullptr;
    }

    if (streamingEligible(content))
    {
        std::string m = streamingParseError(content, true);
        if (!m.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, m);
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to read GeoJSON data");
            err = "reported";
            return nullptr;
        }
    }
    else
    {
        std::string desc;
        size_t off = 0;
        if (jsoncTokenerError(content, desc, off))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JSON parsing error: %s (at offset "
                                  "%zu)",
                                  desc.c_str(), off));
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to read GeoJSON data");
            err = "reported";
            return nullptr;
        }
    }
    bool ok = false;
    JVal root = JVal::parse(content, &ok);
    if ((!ok || root.type != JVal::OBJECT) && streamingEligible(content))
        root = JVal::parse(sanitizeLenientJson(content), &ok);
    if (!ok || root.type != JVal::OBJECT)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    jsonParseErrorMessage(content));
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "Failed to read GeoJSON data");
        err = "reported";
        return nullptr;
    }

    std::string rootType = root.getString("type");

    auto ds = std::make_unique<OgrDataset>();
    ds->path = path;
    ds->driverShort = "GeoJSON";
    ds->driverLong = "GeoJSON";
    ds->files.push_back(path);
    ds->layers.emplace_back();
    OgrLayer &lyr = ds->layers.back();

    // layer name: "name" member, else basename without extension
    lyr.name = layerNameFromPath(path);

    std::vector<const JVal *> featureObjs;
    const JVal *bareGeomJson = nullptr;
    bool bareFeatureRoot = false;
    if (rootType == "FeatureCollection")
    {
        const JVal *nameM = root.get("name");
        if (nameM && nameM->type == JVal::STRING)
            lyr.name = nameM->s;
        const JVal *descM = root.get("description");
        if (descM && descM->type == JVal::STRING)
            lyr.metadata.emplace_back("DESCRIPTION", descM->s);
        const JVal *feats = root.get("features");
        if (!feats || feats->type != JVal::ARRAY)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid FeatureCollection object. Missing "
                        "'features' member.");
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to read GeoJSON data");
            err = "reported";
            return nullptr;
        }
        for (const JVal &f : feats->arr)
            if (f.type == JVal::OBJECT)
                featureObjs.push_back(&f);
    }
    else if (rootType == "Feature")
    {
        featureObjs.push_back(&root);
        bareFeatureRoot = true;
    }
    else if (geomTypeFromName(rootType) >= 0)
    {
        // bare geometry: single feature, no fields
        featureObjs.clear();
        bareGeomJson = &root;
        OgrFeature feat;
        feat.fid = 0;
        bool gerr = false;
        OgrGeometry g;
        if (readGeometry(root, g, gerr))
        {
            feat.hasGeom = true;
            feat.geom = std::move(g);
        }
        else
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to read GeoJSON data");
            err = "reported";
            return nullptr;
        }
        lyr.features.push_back(std::move(feat));
    }
    else
    {
        if (weakPass)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "Failed to read GeoJSON data");
            err = "reported";
        }
        else
            err = "";
        return nullptr;
    }

    buildGeoJsonLayerCore(lyr, featureObjs, bareFeatureRoot, bareGeomJson,
                          nullptr,
                          path == "/vsistdin/" ? kGeoJsonStdin
                                               : kGeoJsonFile);

    // a streamed source surfaces every reader diagnostic exactly once,
    // at open, in every mode
    if (path == "/vsistdin/")
        for (auto &d : lyr.pendingDiags)
        {
            d.geom = false;
            d.openAlso = false;
        }

    // ---- CRS
    const JVal *crsM = root.get("crs");
    bool srsSet = false;
    if (crsM && crsM->type == JVal::OBJECT)
    {
        const JVal *p = crsM->get("properties");
        std::string crsName =
            p && p->type == JVal::OBJECT ? p->getString("name") : "";
        if (!crsName.empty())
        {
            bool sok = false;
            Srs s = Srs::fromUserInput(crsName, sok);
            if (sok)
            {
                bool isDefault =
                    s.epsgCode() == 4326 ||
                    (s.authName() == "OGC" && s.code() == "CRS84");
                if (!isDefault)
                {
                    lyr.srs = std::move(s);
                    lyr.hasSrs = true;
                    srsSet = true;
                }
            }
        }
    }
    if (!srsSet)
    {
        if (lyr.geomHasZ)
        {
            bool sok = false;
            Srs s = Srs::fromEpsg(4979, sok);
            if (sok)
            {
                lyr.srs = std::move(s);
                lyr.hasSrs = true;
            }
        }
        else
            applyDefaultWgs84(lyr);
    }

    if (rootType == "FeatureCollection")
        lyr.gjRoot = std::make_shared<const JVal>(std::move(root));
    else
        for (OgrFeature &f : lyr.features)
            f.gjNative.reset();

    if (rootType == "FeatureCollection")
        cplDebug("GeoJSON", "First pass: 100.00 %");
    err = "";
    return ds;
}

std::unique_ptr<OgrDataset> openGeoJsonSeq(const std::string &path,
                                           std::string &err)
{
    err = "";
    std::string content;
    if (!readFileToString(path, content) || content.empty())
        return nullptr;

    auto stripTrailWs = [](std::string &s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                              s.back() == '\r' || s.back() == '\n'))
            s.pop_back();
    };
    auto firstNonWs = [](const std::string &s) -> char {
        size_t i = s.find_first_not_of(" \t\r\n");
        return i == std::string::npos ? '\0' : s[i];
    };

    // texts are delimited by RS (0x1e) when the file starts with one,
    // otherwise one text per line
    std::vector<std::string> texts;
    bool rsMode = content[0] == '\x1e';
    if (rsMode)
    {
        size_t pos = 1;
        while (pos <= content.size())
        {
            size_t nxt = content.find('\x1e', pos);
            std::string seg =
                nxt == std::string::npos
                    ? content.substr(pos)
                    : content.substr(pos, nxt - pos);
            stripTrailWs(seg);
            if (firstNonWs(seg))
                texts.push_back(std::move(seg));
            if (nxt == std::string::npos)
                break;
            pos = nxt + 1;
        }
    }
    else
    {
        size_t pos = 0;
        while (pos < content.size())
        {
            size_t nl = content.find('\n', pos);
            std::string seg =
                nl == std::string::npos ? content.substr(pos)
                                        : content.substr(pos, nl - pos);
            stripTrailWs(seg);
            if (firstNonWs(seg))
                texts.push_back(std::move(seg));
            if (nl == std::string::npos)
                break;
            pos = nl + 1;
        }
    }
    if (texts.empty())
        return nullptr;

    // identification: the first text must be a Feature or bare geometry
    // (line mode also needs a second '{'-led text that is not a
    // FeatureCollection; RS mode also accepts a FeatureCollection and a
    // single text)
    {
        bool ok = false;
        JVal first = JVal::parse(texts[0], &ok);
        std::string t0 = ok && first.type == JVal::OBJECT
                             ? first.getString("type")
                             : "";
        bool featureish = t0 == "Feature" || geomTypeFromName(t0) >= 0;
        if (rsMode)
        {
            if (!featureish && t0 != "FeatureCollection")
                return nullptr;
        }
        else
        {
            if (!featureish || texts.size() < 2)
                return nullptr;
            if (firstNonWs(texts[1]) != '{')
                return nullptr;
            bool ok2 = false;
            JVal second = JVal::parse(texts[1], &ok2);
            if (ok2 && second.type == JVal::OBJECT &&
                second.getString("type") == "FeatureCollection")
                return nullptr;
        }
    }

    auto ds = std::make_unique<OgrDataset>();
    ds->path = path;
    ds->driverShort = "GeoJSONSeq";
    ds->driverLong = "GeoJSON Sequence";
    ds->files.push_back(path);
    ds->layers.emplace_back();
    OgrLayer &lyr = ds->layers.back();
    lyr.name = layerNameFromPath(path);

    std::deque<JVal> owned;
    std::vector<const JVal *> featureObjs;
    std::vector<bool> aggMask;
    std::vector<OgrLayer::SeqEvent> parseEvents;
    long long parsed = 0;
    for (const auto &text : texts)
    {
        std::string desc;
        size_t off = 0;
        if (jsoncTokenerError(text, desc, off))
        {
            std::string msg =
                strPrintf("JSON parsing error: %s (at offset %zu)",
                          desc.c_str(), off);
            parseEvents.push_back({(long long)featureObjs.size(),
                                   (int)CE_Failure, msg, false});
            continue;
        }
        ++parsed;
        bool ok = false;
        JVal v = JVal::parse(text, &ok);
        if (!ok)
            v = JVal::parse(sanitizeLenientJson(text), &ok);
        if (!ok || v.type != JVal::OBJECT)
            continue;
        std::string vt = v.getString("type");
        if (vt == "Feature")
        {
            owned.push_back(std::move(v));
            featureObjs.push_back(&owned.back());
            aggMask.push_back(true);
        }
        else if (geomTypeFromName(vt) >= 0)
        {
            // bare geometry texts become schema-less features and do
            // not weigh in on the declared layer geometry type
            JVal f;
            f.type = JVal::OBJECT;
            JVal t;
            t.type = JVal::STRING;
            t.s = "Feature";
            f.obj.emplace_back("type", std::move(t));
            f.obj.emplace_back("geometry", std::move(v));
            owned.push_back(std::move(f));
            featureObjs.push_back(&owned.back());
            aggMask.push_back(false);
        }
    }

    buildGeoJsonLayerCore(lyr, featureObjs, false, nullptr, &aggMask,
                          path == "/vsistdin/" ? kGeoJsonStdin
                                               : kGeoJsonSeq);
    lyr.countOverride = parsed;
    applyDefaultWgs84(lyr);

    // fold per-feature diagnostics into the parse-error stream in file
    // order: an error recorded after k features precedes feature k
    lyr.seqRescan = true;
    size_t pe = 0;
    for (size_t k = 0; k < lyr.features.size(); ++k)
    {
        while (pe < parseEvents.size() &&
               parseEvents[pe].featsBefore <= (long long)k)
            lyr.seqEvents.push_back(std::move(parseEvents[pe++]));
        for (const auto &d : lyr.pendingDiags)
            if (d.fid == lyr.features[k].fid)
                lyr.seqEvents.push_back(
                    {(long long)k, d.sev, d.msg, true, d.once});
    }
    while (pe < parseEvents.size())
        lyr.seqEvents.push_back(std::move(parseEvents[pe++]));
    lyr.pendingDiags.clear();
    cplDebug("GeoJSONSeq", "First pass: 100.00 %");
    return ds;
}

namespace
{

// epoch-millisecond date values render through the same pipeline as
// GeoJSON ISO strings, so they are stored pre-formatted
std::string esriMsToIso(long long ms)
{
    long long days = ms / 86400000;
    long long rem = ms % 86400000;
    if (rem < 0)
    {
        rem += 86400000;
        days -= 1;
    }
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    if (mm <= 2)
        y++;
    int hh = (int)(rem / 3600000);
    rem %= 3600000;
    int mi = (int)(rem / 60000);
    rem %= 60000;
    int ss = (int)(rem / 1000);
    int msec = (int)(rem % 1000);
    std::string t = strPrintf("%04lld-%02u-%02uT%02d:%02d:%02d", y, mm,
                              dd, hh, mi, ss);
    if (msec)
        t += strPrintf(".%03d", msec);
    t += "Z";
    return t;
}

// json-c value coercions: get_int64 parses string prefixes, get_double
// requires full consumption or yields 0
long long jcInt64(const JVal &v)
{
    switch (v.type)
    {
        case JVal::BOOL:
            return v.b ? 1 : 0;
        case JVal::INT:
            return v.i;
        case JVal::DOUBLE:
            if (v.d >= (double)INT64_MAX)
                return INT64_MAX;
            if (v.d <= (double)INT64_MIN)
                return INT64_MIN;
            return (long long)v.d;
        case JVal::STRING:
            return strtoll(v.s.c_str(), nullptr, 10);
        default:
            return 0;
    }
}

int jcInt32(const JVal &v)
{
    long long r = jcInt64(v);
    if (r > INT32_MAX)
        return INT32_MAX;
    if (r < INT32_MIN)
        return INT32_MIN;
    return (int)r;
}

double jcDouble(const JVal &v)
{
    switch (v.type)
    {
        case JVal::BOOL:
            return v.b ? 1 : 0;
        case JVal::INT:
            return (double)v.i;
        case JVal::DOUBLE:
            return v.d;
        case JVal::STRING:
        {
            char *end = nullptr;
            double r = strtod(v.s.c_str(), &end);
            if (!v.s.empty() && end == v.s.c_str() + v.s.size())
                return r;
            return 0.0;
        }
        default:
            return 0.0;
    }
}

struct EsriCoordCtx
{
    bool fail = false;
};

bool esriCoordNum(const JVal &v, const char *name, double &out,
                  EsriCoordCtx &cc)
{
    if (v.type == JVal::INT)
    {
        out = (double)v.i;
        return true;
    }
    if (v.type == JVal::DOUBLE)
    {
        out = v.d;
        return true;
    }
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                strPrintf("Invalid '%s' coordinate. Type is not double "
                          "or integer for '%s'.",
                          name, ogrJsonSpacedSerialize(v).c_str()));
    cc.fail = true;
    return false;
}

// 2/3/4-element tuples decide dimensionality by arity alone; the root
// hasZ/hasM flags do not
bool esriTuple(const JVal &t, double *xyz, double &m, bool &hasZ,
               bool &hasM, EsriCoordCtx &cc)
{
    if (t.type != JVal::ARRAY || t.arr.size() < 2)
    {
        cc.fail = true;
        return false;
    }
    static const char *names[] = {"x", "y", "z", "m"};
    size_t n = t.arr.size() < 4 ? t.arr.size() : 4;
    double vals[4] = {0, 0, 0, 0};
    bool ok = true;
    for (size_t i = 0; i < n; i++)
        if (!esriCoordNum(t.arr[i], names[i], vals[i], cc))
            ok = false;
    if (!ok)
        return false;
    xyz[0] = vals[0];
    xyz[1] = vals[1];
    xyz[2] = n >= 3 ? vals[2] : 0.0;
    m = n >= 4 ? vals[3] : 0.0;
    if (n >= 3)
        hasZ = true;
    if (n >= 4)
        hasM = true;
    return true;
}

bool esriTupleList(const JVal &arr, OgrGeometry &g, EsriCoordCtx &cc)
{
    if (arr.type != JVal::ARRAY)
    {
        cc.fail = true;
        return false;
    }
    std::vector<double> ms;
    for (const JVal &t : arr.arr)
    {
        double xyz[3], m = 0.0;
        if (!esriTuple(t, xyz, m, g.hasZ, g.hasM, cc))
            return false;
        g.coords.insert(g.coords.end(), xyz, xyz + 3);
        ms.push_back(m);
    }
    if (g.hasM)
        g.m = std::move(ms);
    return true;
}

bool pointInRing2D(const OgrGeometry &ring, double x, double y)
{
    bool in = false;
    size_t n = ring.coords.size() / 3;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        double xi = ring.coords[i * 3], yi = ring.coords[i * 3 + 1];
        double xj = ring.coords[j * 3], yj = ring.coords[j * 3 + 1];
        if ((yi > y) != (yj > y) &&
            x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            in = !in;
    }
    return in;
}

// even-odd nesting: rings at even containment depth are shells, odd
// rings become holes of their innermost containing shell
OgrGeometry esriOrganizeRings(std::vector<OgrGeometry> &rings)
{
    size_t n = rings.size();
    std::vector<int> depth(n, 0);
    for (size_t i = 0; i < n; i++)
    {
        if (rings[i].coords.empty())
            continue;
        double x = rings[i].coords[0], y = rings[i].coords[1];
        for (size_t j = 0; j < n; j++)
            if (j != i && pointInRing2D(rings[j], x, y))
                depth[i]++;
    }
    std::vector<int> shellOf(n, -1);
    std::vector<size_t> shells;
    for (size_t i = 0; i < n; i++)
        if (depth[i] % 2 == 0)
            shells.push_back(i);
    for (size_t i = 0; i < n; i++)
    {
        if (depth[i] % 2 == 0 || rings[i].coords.empty())
            continue;
        int best = -1, bestDepth = -1;
        double x = rings[i].coords[0], y = rings[i].coords[1];
        for (size_t s : shells)
            if (depth[s] < depth[i] && depth[s] > bestDepth &&
                pointInRing2D(rings[s], x, y))
            {
                best = (int)s;
                bestDepth = depth[s];
            }
        shellOf[i] = best;
    }
    auto buildPoly = [&](size_t shellIdx) {
        OgrGeometry poly;
        poly.type = 3;
        poly.hasZ = rings[shellIdx].hasZ;
        poly.hasM = rings[shellIdx].hasM;
        poly.parts.push_back(rings[shellIdx]);
        for (size_t i = 0; i < n; i++)
            if (shellOf[i] == (int)shellIdx)
            {
                if (rings[i].hasZ)
                    poly.hasZ = true;
                if (rings[i].hasM)
                    poly.hasM = true;
                poly.parts.push_back(rings[i]);
            }
        return poly;
    };
    if (shells.size() <= 1)
    {
        if (shells.empty())
        {
            OgrGeometry poly;
            poly.type = 3;
            return poly;
        }
        return buildPoly(shells[0]);
    }
    OgrGeometry multi;
    multi.type = 6;
    for (size_t s : shells)
    {
        OgrGeometry poly = buildPoly(s);
        if (poly.hasZ)
            multi.hasZ = true;
        if (poly.hasM)
            multi.hasM = true;
        multi.parts.push_back(std::move(poly));
    }
    return multi;
}

// dispatch keyed on the members actually present, independent of the
// declared layer geometry type
bool esriReadGeometry(const JVal &gm, OgrGeometry &g)
{
    EsriCoordCtx cc;
    if (gm.get("x"))
    {
        double x = 0, y = 0, z = 0;
        const JVal *xm = gm.get("x");
        const JVal *ym = gm.get("y");
        bool ok = esriCoordNum(*xm, "x", x, cc);
        if (!ym)
            return false;
        if (!esriCoordNum(*ym, "y", y, cc))
            ok = false;
        const JVal *zm = gm.get("z");
        bool hasZ = false;
        if (zm)
        {
            if (!esriCoordNum(*zm, "z", z, cc))
                ok = false;
            else
                hasZ = true;
        }
        if (!ok)
            return false;
        g.type = 1;
        g.hasZ = hasZ;
        g.coords = {x, y, z};
        return true;
    }
    const JVal *pts = gm.get("points");
    if (pts)
    {
        g.type = 4;
        if (pts->type != JVal::ARRAY)
            return false;
        for (const JVal &t : pts->arr)
        {
            OgrGeometry p;
            p.type = 1;
            double xyz[3], m = 0.0;
            bool hz = false, hm = false;
            if (!esriTuple(t, xyz, m, hz, hm, cc))
                return false;
            p.hasZ = hz;
            p.hasM = hm;
            p.coords = {xyz[0], xyz[1], xyz[2]};
            if (hm)
                p.m = {m};
            if (hz)
                g.hasZ = true;
            if (hm)
                g.hasM = true;
            g.parts.push_back(std::move(p));
        }
        return true;
    }
    const JVal *paths = gm.get("paths");
    if (paths)
    {
        if (paths->type != JVal::ARRAY)
            return false;
        std::vector<OgrGeometry> lines;
        for (const JVal &pa : paths->arr)
        {
            OgrGeometry line;
            line.type = 2;
            if (!esriTupleList(pa, line, cc))
                return false;
            lines.push_back(std::move(line));
        }
        if (lines.size() == 1)
        {
            g = std::move(lines[0]);
            return true;
        }
        g.type = 5;
        for (auto &l : lines)
        {
            if (l.hasZ)
                g.hasZ = true;
            if (l.hasM)
                g.hasM = true;
            g.parts.push_back(std::move(l));
        }
        return true;
    }
    const JVal *rings = gm.get("rings");
    if (rings)
    {
        if (rings->type != JVal::ARRAY)
            return false;
        std::vector<OgrGeometry> ringGeoms;
        for (const JVal &r : rings->arr)
        {
            OgrGeometry ring;
            ring.type = 2;
            if (!esriTupleList(r, ring, cc))
                return false;
            size_t np = ring.coords.size() / 3;
            if (np >= 1 &&
                (ring.coords[0] != ring.coords[(np - 1) * 3] ||
                 ring.coords[1] != ring.coords[(np - 1) * 3 + 1]))
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    "Non closed ring detected. To avoid accepting it, "
                    "set the OGR_GEOMETRY_ACCEPT_UNCLOSED_RING "
                    "configuration option to NO");
            ringGeoms.push_back(std::move(ring));
        }
        g = esriOrganizeRings(ringGeoms);
        return true;
    }
    return false;
}

}  // namespace

std::unique_ptr<OgrDataset> openEsriJson(const std::string &path,
                                         std::string &err)
{
    std::string content;
    if (!readFileToString(path, content) || !firstCharIsBrace(content))
    {
        err = "";
        return nullptr;
    }
    std::string chead = compactJson(identifyHead(content));
    // spatialReference only engages as a leading member whose first key
    // is wkid; the other markers match anywhere in the prefix
    bool looksEsri =
        chead.find("\"fieldAliases\"") != std::string::npos ||
        chead.rfind("{\"spatialReference\":{\"wkid\"", 0) == 0 ||
        chead.find("\"geometryType\":\"esriGeometry") !=
            std::string::npos ||
        chead.find("\"features\":[{\"attributes\":") !=
            std::string::npos ||
        chead.find("\"esriFieldType") != std::string::npos;
    if (!looksEsri)
    {
        err = "";
        return nullptr;
    }

    auto fatal = [&]() {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "Failed to read ESRIJSON data");
        err = "reported";
    };

    bool ok = false;
    JVal root = JVal::parse(content, &ok);
    if (!ok || root.type != JVal::OBJECT)
    {
        // ESRIJSON never goes through the streaming parser, so parse
        // failures always carry the json-c tokener wording
        std::string desc;
        size_t off = 0;
        std::string msg =
            jsoncTokenerError(content, desc, off)
                ? strPrintf("JSON parsing error: %s (at offset %zu)",
                            desc.c_str(), off)
                : strPrintf("At line %d, character %d: Unexpected state",
                            1, 1);
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        fatal();
        return nullptr;
    }

    const JVal *feats = root.get("features");
    if (!feats)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Invalid FeatureCollection object. Missing "
                    "'features' member.");
        fatal();
        return nullptr;
    }

    auto ds = std::make_unique<OgrDataset>();
    ds->path = path;
    ds->driverShort = "ESRIJSON";
    ds->driverLong = "ESRIJSON";
    ds->files.push_back(path);
    ds->layers.emplace_back();
    OgrLayer &lyr = ds->layers.back();
    lyr.name = layerNameFromPath(path);

    std::vector<const JVal *> featureObjs;
    if (feats->type == JVal::ARRAY)
        for (const JVal &f : feats->arr)
            if (f.type == JVal::OBJECT)
                featureObjs.push_back(&f);

    // ---- schema
    int oidIdx = -1;
    // fields born as OFTString from an esri type coerce their values to
    // strings at read time (json-c get_string), unlike the attribute
    // schema whose string fields keep raw JSON values
    std::vector<bool> strCoerce;
    const JVal *fieldsM = root.get("fields");
    const JVal *aliasesM = root.get("fieldAliases");
    bool typedSchema = (fieldsM && fieldsM->type == JVal::ARRAY) ||
                       (!fieldsM && aliasesM &&
                        aliasesM->type == JVal::OBJECT);
    if (fieldsM && fieldsM->type == JVal::ARRAY)
    {
        for (const JVal &fj : fieldsM->arr)
        {
            if (fj.type != JVal::OBJECT)
                continue;
            const JVal *nameM = fj.get("name");
            const JVal *typeM = fj.get("type");
            if (!nameM || nameM->type != JVal::STRING ||
                nameM->s.empty() || !typeM)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Layer schema generation failed.");
                fatal();
                return nullptr;
            }
            std::string ty =
                typeM->type == JVal::STRING ? typeM->s : "";
            OgrFieldDefn fd;
            fd.name = nameM->s;
            bool lengthAsWidth = false;
            if (ty == "esriFieldTypeOID")
            {
                fd.type = OFTInteger;
                if (oidIdx < 0)
                {
                    oidIdx = (int)lyr.fields.size();
                    lyr.fidColumn = fd.name;
                }
            }
            else if (ty == "esriFieldTypeInteger")
                fd.type = OFTInteger;
            else if (ty == "esriFieldTypeSmallInteger")
            {
                fd.type = OFTInteger;
                fd.subType = OFSTInt16;
            }
            else if (ty == "esriFieldTypeBigInteger")
                fd.type = OFTInteger64;
            else if (ty == "esriFieldTypeDouble")
                fd.type = OFTReal;
            else if (ty == "esriFieldTypeSingle")
            {
                fd.type = OFTReal;
                fd.subType = OFSTFloat32;
            }
            else if (ty == "esriFieldTypeDate")
                fd.type = OFTDateTime;
            else if (ty == "esriFieldTypeGlobalID" ||
                     ty == "esriFieldTypeGUID")
            {
                fd.type = OFTString;
                fd.subType = OFSTUUID;
                lengthAsWidth = true;
            }
            else
            {
                fd.type = OFTString;
                lengthAsWidth = ty == "esriFieldTypeString";
            }
            if (lengthAsWidth)
            {
                const JVal *lenM = fj.get("length");
                if (lenM && lenM->type == JVal::INT && lenM->i > 0)
                    fd.width = (int)lenM->i;
            }
            const JVal *aliasM = fj.get("alias");
            if (aliasM && aliasM->type == JVal::STRING &&
                !aliasM->s.empty() && aliasM->s != fd.name)
                fd.altName = aliasM->s;
            strCoerce.push_back(fd.type == OFTString);
            lyr.fields.push_back(std::move(fd));
        }
    }
    else if (aliasesM && aliasesM->type == JVal::OBJECT)
    {
        for (const auto &kv : aliasesM->obj)
        {
            OgrFieldDefn fd;
            fd.name = kv.first;
            fd.type = OFTString;
            strCoerce.push_back(true);
            lyr.fields.push_back(std::move(fd));
        }
    }
    else
    {
        // schema from attributes with GeoJSON-style accumulation and
        // name-tiebreak DAG ordering
        std::map<std::string, int> nodeIdx;
        std::vector<DagNode> nodes;
        std::vector<FieldAccum> accums;
        std::set<std::pair<int, int>> edges;
        for (const JVal *fj : featureObjs)
        {
            const JVal *attrs = fj->get("attributes");
            if (!attrs || attrs->type != JVal::OBJECT)
                continue;
            int prev = -1;
            for (const auto &kv : attrs->obj)
            {
                int idx;
                auto it = nodeIdx.find(kv.first);
                if (it == nodeIdx.end())
                {
                    idx = (int)nodes.size();
                    nodeIdx[kv.first] = idx;
                    nodes.push_back({kv.first, {}});
                    accums.emplace_back();
                    accums.back().name = kv.first;
                }
                else
                    idx = it->second;
                updateFieldType(accums[idx], kv.second);
                if (prev >= 0 && prev != idx &&
                    edges.insert({prev, idx}).second)
                    nodes[prev].after.push_back(idx);
                prev = idx;
            }
        }
        std::vector<int> indeg(nodes.size(), 0);
        for (const auto &nd : nodes)
            for (int t : nd.after)
                indeg[t]++;
        std::vector<bool> done(nodes.size(), false);
        for (size_t iter = 0; iter < nodes.size(); iter++)
        {
            int pick = -1;
            for (size_t i = 0; i < nodes.size(); i++)
                if (!done[i] && indeg[i] == 0 &&
                    (pick < 0 || nodes[i].name < nodes[pick].name))
                    pick = (int)i;
            if (pick < 0)
                for (size_t i = 0; i < nodes.size(); i++)
                    if (!done[i] &&
                        (pick < 0 || nodes[i].name < nodes[pick].name))
                        pick = (int)i;
            done[pick] = true;
            for (int t : nodes[pick].after)
                indeg[t]--;
            const FieldAccum &a = accums[pick];
            OgrFieldDefn fd;
            fd.name = a.name;
            fd.type = a.typeSet ? a.type : OFTString;
            fd.subType = a.typeSet ? a.subType : OFSTNone;
            fd.tzAggr = a.tzAggr;
            if (fd.type == OFTInteger && fd.subType == OFSTBoolean)
                fd.width = 1;
            lyr.fields.push_back(std::move(fd));
        }
    }

    std::map<std::string, int> fieldIdxExact;
    for (size_t i = 0; i < lyr.fields.size(); i++)
        fieldIdxExact.emplace(lyr.fields[i].name, (int)i);
    auto findField = [&](const std::string &key) -> int {
        auto it = fieldIdxExact.find(key);
        if (it != fieldIdxExact.end())
            return it->second;
        for (size_t i = 0; i < lyr.fields.size(); i++)
            if (strEqualNoCase(lyr.fields[i].name, key))
                return (int)i;
        return -1;
    };

    // ---- SRS: latestWkid beats wkid, ESRI fallback carries a warning,
    // a code unknown to both authorities surfaces one PROJ error
    bool srsOk = false;
    const JVal *srM = root.get("spatialReference");
    if (srM && srM->type == JVal::OBJECT)
    {
        auto tryWkid = [&](long long code) -> bool {
            bool sok = false;
            Srs s = Srs::fromEpsg((int)code, sok);
            if (sok)
            {
                lyr.srs = std::move(s);
                lyr.hasSrs = true;
                return true;
            }
            s = Srs::fromAuthority("ESRI", (int)code, sok);
            if (sok)
            {
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("EPSG:%lld is not a valid CRS code, but "
                              "ESRI:%lld is. Assuming ESRI:%lld was "
                              "meant",
                              code, code, code));
                lyr.srs = std::move(s);
                lyr.hasSrs = true;
                return true;
            }
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "PROJ: proj_create_from_database: crs not "
                        "found");
            return false;
        };
        const JVal *lw = srM->get("latestWkid");
        const JVal *w = srM->get("wkid");
        if (lw && (lw->type == JVal::INT || lw->type == JVal::DOUBLE))
            srsOk = tryWkid(jcInt64(*lw));
        if (!srsOk && w &&
            (w->type == JVal::INT || w->type == JVal::DOUBLE))
            srsOk = tryWkid(jcInt64(*w));
        if (!srsOk)
        {
            const JVal *wkt = srM->get("wkt");
            if (wkt && wkt->type == JVal::STRING && !wkt->s.empty())
            {
                bool sok = false;
                Srs s = Srs::fromEsriPrj(wkt->s, sok);
                if (sok)
                {
                    lyr.srs = std::move(s);
                    lyr.hasSrs = true;
                    srsOk = true;
                }
            }
        }
    }

    // ---- layer geometry type
    std::string gtName = root.getString("geometryType");
    int layerGeomType = -1;
    if (gtName == "esriGeometryPoint")
        layerGeomType = 1;
    else if (gtName == "esriGeometryMultipoint")
        layerGeomType = 4;
    else if (gtName == "esriGeometryPolyline")
        layerGeomType = 2;
    else if (gtName == "esriGeometryPolygon")
        layerGeomType = 3;
    else if (gtName == "esriGeometryEnvelope")
        layerGeomType = 0;
    if (layerGeomType < 0)
    {
        for (const JVal *fj : featureObjs)
        {
            const JVal *gm = fj->get("geometry");
            if (!gm || gm->type != JVal::OBJECT)
                continue;
            if (gm->get("x"))
                layerGeomType = 1;
            else if (gm->get("points"))
                layerGeomType = 4;
            else if (gm->get("paths"))
                layerGeomType = 2;
            else if (gm->get("rings"))
                layerGeomType = 3;
            if (layerGeomType >= 0)
                break;
        }
    }
    if (layerGeomType < 0)
        layerGeomType = srsOk ? 0 : 101;
    lyr.geomType = layerGeomType;
    lyr.geomHasZ = false;
    lyr.hasGeomField = layerGeomType != 101;

    // ---- features
    std::set<long long> seenFids;
    long long autoFid = 0;
    bool fidWarned = false;
    for (const JVal *fj : featureObjs)
    {
        OgrFeature feat;
        feat.values.resize(lyr.fields.size());
        const JVal *attrs = fj->get("attributes");
        if (attrs && attrs->type == JVal::OBJECT)
        {
            for (const auto &kv : attrs->obj)
            {
                int fi = findField(kv.first);
                if (fi < 0 || kv.second.type == JVal::NUL)
                    continue;
                const OgrFieldDefn &fd = lyr.fields[fi];
                OgrFieldValue &fv = feat.values[fi];
                fv.set = true;
                // attribute-derived schemas keep raw JSON values with
                // GeoJSON display semantics; only DateTime goes through
                // the epoch-millisecond reading
                if (!typedSchema)
                {
                    if (fd.type == OFTDateTime)
                    {
                        fv.v.type = JVal::STRING;
                        fv.v.s = esriMsToIso(jcInt64(kv.second));
                    }
                    else
                        fv.v = kv.second;
                    continue;
                }
                switch (fd.type)
                {
                    case OFTInteger:
                    {
                        int v = jcInt32(kv.second);
                        if (fd.subType == OFSTInt16 &&
                            (v > 32767 || v < -32768))
                        {
                            int clamped = v > 32767 ? 32767 : -32768;
                            cplErrorStr(
                                CE_Warning, CPLE_AppDefined,
                                strPrintf(
                                    "Field %s.%s: Out-of-range value "
                                    "for a OFSTInt16 subtype. "
                                    "Considering value %d as %d.",
                                    lyr.name.c_str(), fd.name.c_str(),
                                    v, clamped));
                            v = clamped;
                        }
                        fv.v.type = JVal::INT;
                        fv.v.i = v;
                        break;
                    }
                    case OFTInteger64:
                        fv.v.type = JVal::INT;
                        fv.v.i = jcInt64(kv.second);
                        break;
                    case OFTReal:
                        if (kv.second.type == JVal::INT ||
                            kv.second.type == JVal::DOUBLE)
                            fv.v = kv.second;
                        else if (kv.second.type == JVal::BOOL)
                        {
                            fv.v.type = JVal::INT;
                            fv.v.i = kv.second.b ? 1 : 0;
                        }
                        else
                        {
                            fv.v.type = JVal::DOUBLE;
                            fv.v.d = jcDouble(kv.second);
                        }
                        break;
                    case OFTDateTime:
                        fv.v.type = JVal::STRING;
                        fv.v.s = esriMsToIso(jcInt64(kv.second));
                        break;
                    default:
                        if ((size_t)fi < strCoerce.size() &&
                            strCoerce[fi] &&
                            kv.second.type != JVal::STRING)
                        {
                            fv.v.type = JVal::STRING;
                            fv.v.s = ogrJsonSpacedSerialize(kv.second);
                        }
                        else
                            fv.v = kv.second;
                        break;
                }
            }
        }

        const JVal *gm = fj->get("geometry");
        if (gm && gm->type == JVal::OBJECT)
        {
            OgrGeometry g;
            if (esriReadGeometry(*gm, g))
            {
                feat.hasGeom = true;
                feat.geom = std::move(g);
            }
        }

        bool explicitFid = false;
        if (oidIdx >= 0 && attrs && attrs->type == JVal::OBJECT)
        {
            const JVal *ov = attrs->get(lyr.fidColumn);
            if (ov && ov->type != JVal::NUL)
            {
                feat.fid = jcInt32(*ov);
                explicitFid = true;
            }
        }
        bool needAlter = !explicitFid;
        if (explicitFid && seenFids.count(feat.fid))
        {
            if (!fidWarned)
            {
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("Several features with id = %lld have "
                              "been found. Altering it to be unique. "
                              "This warning will not be emitted anymore "
                              "for this layer",
                              feat.fid));
                fidWarned = true;
            }
            needAlter = true;
        }
        if (needAlter)
        {
            long long cand = autoFid;
            while (seenFids.count(cand))
                cand++;
            feat.fid = cand;
        }
        seenFids.insert(feat.fid);
        autoFid++;
        lyr.features.push_back(std::move(feat));
    }

    // mem-layer semantics: iteration follows ascending FID
    std::stable_sort(lyr.features.begin(), lyr.features.end(),
                     [](const OgrFeature &a, const OgrFeature &b) {
                         return a.fid < b.fid;
                     });

    // ---- extent
    std::function<void(const OgrGeometry &)> acc =
        [&](const OgrGeometry &g) {
            for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
            {
                double x = g.coords[i], y = g.coords[i + 1];
                if (!lyr.hasExtent)
                {
                    lyr.extent[0] = lyr.extent[2] = x;
                    lyr.extent[1] = lyr.extent[3] = y;
                    lyr.hasExtent = true;
                }
                else
                {
                    lyr.extent[0] = std::min(lyr.extent[0], x);
                    lyr.extent[1] = std::min(lyr.extent[1], y);
                    lyr.extent[2] = std::max(lyr.extent[2], x);
                    lyr.extent[3] = std::max(lyr.extent[3], y);
                }
            }
            for (const OgrGeometry &p : g.parts)
                acc(p);
        };
    bool anyParsedGeom = false;
    for (const OgrFeature &feat : lyr.features)
        if (feat.hasGeom)
        {
            anyParsedGeom = true;
            acc(feat.geom);
        }
    if (anyParsedGeom && !lyr.hasExtent)
    {
        lyr.extent[0] = lyr.extent[1] = INFINITY;
        lyr.extent[2] = lyr.extent[3] = -INFINITY;
        lyr.hasExtent = true;
    }

    err = "";
    return ds;
}

std::unique_ptr<OgrDataset> openTopoJson(const std::string &path,
                                         std::string &err)
{
    std::string content;
    if (!readFileToString(path, content) || !firstCharIsBrace(content))
    {
        err = "";
        return nullptr;
    }
    std::string chead = compactJson(identifyHead(content));
    if (chead.find("\"type\":\"Topology\"") == std::string::npos)
    {
        err = "";
        return nullptr;
    }

    auto fatal = [&]() {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "Failed to read TopoJSON data");
        err = "reported";
    };

    bool ok = false;
    JVal root = JVal::parse(content, &ok);
    if (!ok || root.type != JVal::OBJECT)
    {
        std::string desc;
        size_t off = 0;
        std::string msg =
            jsoncTokenerError(content, desc, off)
                ? strPrintf("JSON parsing error: %s (at offset %zu)",
                            desc.c_str(), off)
                : strPrintf("At line %d, character %d: Unexpected state",
                            1, 1);
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        fatal();
        return nullptr;
    }

    const JVal *objsM = root.get("objects");
    const JVal *arcsM = root.get("arcs");
    if (!objsM || objsM->type != JVal::OBJECT || !arcsM ||
        arcsM->type != JVal::ARRAY)
    {
        fatal();
        return nullptr;
    }

    // a usable transform requires scale or translate to be a 2-element
    // array; its validity also gates the delta-decoding of arc points
    bool tValid = false;
    double sc[2] = {1, 1}, tr[2] = {0, 0};
    const JVal *tfM = root.get("transform");
    if (tfM && tfM->type == JVal::OBJECT)
    {
        auto readPair = [&](const char *name, double *dst) -> bool {
            const JVal *a = tfM->get(name);
            if (!a || a->type != JVal::ARRAY || a->arr.size() != 2)
                return false;
            for (int i = 0; i < 2; i++)
            {
                const JVal &e = a->arr[i];
                if (e.type == JVal::INT)
                    dst[i] = (double)e.i;
                else if (e.type == JVal::DOUBLE)
                    dst[i] = e.d;
            }
            return true;
        };
        bool sOk = readPair("scale", sc);
        bool trOk = readPair("translate", tr);
        tValid = sOk || trOk;
    }

    std::vector<std::vector<std::pair<double, double>>> arcs;
    for (const JVal &aj : arcsM->arr)
    {
        arcs.emplace_back();
        if (aj.type != JVal::ARRAY)
            continue;
        auto &pts = arcs.back();
        double ax = 0, ay = 0;
        for (const JVal &pj : aj.arr)
        {
            if (pj.type != JVal::ARRAY || pj.arr.size() < 2)
                continue;
            double x = jcDouble(pj.arr[0]);
            double y = jcDouble(pj.arr[1]);
            if (tValid)
            {
                ax += x;
                ay += y;
                pts.emplace_back(ax * sc[0] + tr[0],
                                 ay * sc[1] + tr[1]);
            }
            else
                pts.emplace_back(x, y);
        }
    }

    auto numOk = [](const JVal &e) {
        return e.type == JVal::INT || e.type == JVal::DOUBLE;
    };
    auto numVal = [](const JVal &e) {
        return e.type == JVal::INT ? (double)e.i : e.d;
    };
    auto xfPoint = [&](double &x, double &y) {
        if (tValid)
        {
            x = x * sc[0] + tr[0];
            y = y * sc[1] + tr[1];
        }
    };
    // arcs are concatenated dropping each subsequent arc's first vertex;
    // out-of-range references are skipped, not fatal
    auto buildLine =
        [&](const JVal &idxArr) -> std::vector<std::pair<double, double>> {
        std::vector<std::pair<double, double>> out;
        for (const JVal &e : idxArr.arr)
        {
            long long v = jcInt64(e);
            long long idx = v < 0 ? -v - 1 : v;
            if (idx < 0 || (size_t)idx >= arcs.size())
                continue;
            std::vector<std::pair<double, double>> pts = arcs[(size_t)idx];
            if (v < 0)
                std::reverse(pts.begin(), pts.end());
            size_t start = out.empty() ? 0 : 1;
            for (size_t i = start; i < pts.size(); i++)
                out.push_back(pts[i]);
        }
        return out;
    };

    auto lineFromIdx = [&](const JVal &arr, OgrGeometry &ls) {
        ls.type = 2;
        for (const auto &p : buildLine(arr))
        {
            ls.coords.push_back(p.first);
            ls.coords.push_back(p.second);
            ls.coords.push_back(0);
        }
        ls.empty = ls.coords.empty();
    };
    auto ringsFromArr = [&](const JVal &arr, OgrGeometry &poly) {
        poly.type = 3;
        for (const JVal &rj : arr.arr)
        {
            if (rj.type != JVal::ARRAY)
                continue;
            auto pts = buildLine(rj);
            if (!pts.empty() && pts.front() != pts.back())
                pts.push_back(pts.front());
            // rings shorter than 4 points after closing are dropped
            if (pts.size() < 4)
                continue;
            OgrGeometry ring;
            ring.type = 2;
            for (const auto &p : pts)
            {
                ring.coords.push_back(p.first);
                ring.coords.push_back(p.second);
                ring.coords.push_back(0);
            }
            poly.parts.push_back(std::move(ring));
        }
        poly.empty = poly.parts.empty();
    };

    // false = the object yields no feature at all (unusable type or
    // missing/non-array geometry member); parse failures inside a
    // present member still yield an EMPTY-geometry feature
    auto parseGeom = [&](const JVal &gj, OgrGeometry &g) -> bool {
        std::string ty = gj.getString("type");
        if (ty == "Point")
        {
            const JVal *c = gj.get("coordinates");
            if (!c || c->type != JVal::ARRAY)
                return false;
            g.type = 1;
            if (c->arr.size() == 2 && numOk(c->arr[0]) &&
                numOk(c->arr[1]))
            {
                double x = numVal(c->arr[0]), y = numVal(c->arr[1]);
                xfPoint(x, y);
                g.coords = {x, y, 0};
            }
            else
                g.empty = true;
            return true;
        }
        if (ty == "MultiPoint")
        {
            const JVal *c = gj.get("coordinates");
            if (!c || c->type != JVal::ARRAY)
                return false;
            g.type = 4;
            for (const JVal &e : c->arr)
            {
                if (e.type != JVal::ARRAY || e.arr.size() != 2 ||
                    !numOk(e.arr[0]) || !numOk(e.arr[1]))
                    continue;
                double x = numVal(e.arr[0]), y = numVal(e.arr[1]);
                xfPoint(x, y);
                OgrGeometry pt;
                pt.type = 1;
                pt.coords = {x, y, 0};
                g.parts.push_back(std::move(pt));
            }
            g.empty = g.parts.empty();
            return true;
        }
        if (ty == "LineString")
        {
            const JVal *a = gj.get("arcs");
            if (!a || a->type != JVal::ARRAY)
                return false;
            lineFromIdx(*a, g);
            return true;
        }
        if (ty == "MultiLineString")
        {
            const JVal *a = gj.get("arcs");
            if (!a || a->type != JVal::ARRAY)
                return false;
            g.type = 5;
            bool anyPts = false;
            // member lines are kept even when they decode to nothing;
            // only the recursive-empty aggregate reports EMPTY
            for (const JVal &e : a->arr)
            {
                if (e.type != JVal::ARRAY)
                    continue;
                OgrGeometry ls;
                lineFromIdx(e, ls);
                anyPts = anyPts || !ls.coords.empty();
                g.parts.push_back(std::move(ls));
            }
            g.empty = !anyPts;
            return true;
        }
        if (ty == "Polygon")
        {
            const JVal *a = gj.get("arcs");
            if (!a || a->type != JVal::ARRAY)
                return false;
            ringsFromArr(*a, g);
            return true;
        }
        if (ty == "MultiPolygon")
        {
            const JVal *a = gj.get("arcs");
            if (!a || a->type != JVal::ARRAY)
                return false;
            g.type = 6;
            for (const JVal &e : a->arr)
            {
                if (e.type != JVal::ARRAY)
                    continue;
                OgrGeometry poly;
                ringsFromArr(e, poly);
                if (!poly.parts.empty())
                {
                    poly.empty = false;
                    g.parts.push_back(std::move(poly));
                }
            }
            g.empty = g.parts.empty();
            return true;
        }
        return false;
    };

    struct TopoFeat
    {
        bool hasId = false;
        JVal idVal;  // raw member value (string or int) or the object key
        std::vector<std::pair<std::string, JVal>> props;
        OgrGeometry geom;
    };
    struct LayerBuild
    {
        OgrLayer lyr;
        std::map<std::string, int> nodeIdx;
        std::vector<DagNode> nodes;
        std::vector<FieldAccum> accums;
        std::set<std::pair<int, int>> edges;
        // the id column exists on every layer, born String; property
        // "id" values merge into it without entering the ordering DAG
        FieldAccum idAccum;
        std::vector<TopoFeat> feats;
        int unifyType = -2;

        LayerBuild()
        {
            idAccum.name = "id";
            idAccum.typeSet = true;
            idAccum.type = OFTString;
        }
    };

    auto addGeomObj = [&](LayerBuild &L, const JVal &gj,
                          const std::string *keyAsId) {
        if (gj.type != JVal::OBJECT)
            return;
        const JVal *tyM = gj.get("type");
        if (!tyM || tyM->type != JVal::STRING)
            return;
        OgrGeometry g;
        if (!parseGeom(gj, g))
            return;
        TopoFeat ft;
        ft.geom = std::move(g);
        if (keyAsId)
        {
            ft.hasId = true;
            ft.idVal.type = JVal::STRING;
            ft.idVal.s = *keyAsId;
        }
        else
        {
            const JVal *idM = gj.get("id");
            if (idM &&
                (idM->type == JVal::STRING || idM->type == JVal::INT))
            {
                ft.hasId = true;
                ft.idVal = *idM;
            }
        }
        const JVal *props = gj.get("properties");
        if (props && props->type == JVal::OBJECT)
        {
            int prev = -1;
            for (const auto &kv : props->obj)
            {
                ft.props.emplace_back(kv.first, kv.second);
                if (kv.first == "id")
                {
                    updateFieldType(L.idAccum, kv.second);
                    continue;
                }
                int idx;
                auto it = L.nodeIdx.find(kv.first);
                if (it == L.nodeIdx.end())
                {
                    idx = (int)L.nodes.size();
                    L.nodeIdx[kv.first] = idx;
                    L.nodes.push_back({kv.first, {}});
                    L.accums.emplace_back();
                    L.accums.back().name = kv.first;
                }
                else
                    idx = it->second;
                updateFieldType(L.accums[idx], kv.second);
                if (prev >= 0 && prev != idx &&
                    L.edges.insert({prev, idx}).second)
                    L.nodes[prev].after.push_back(idx);
                prev = idx;
            }
        }
        int bt = ft.geom.type;
        if (L.unifyType == -2)
            L.unifyType = bt;
        else if (L.unifyType != bt)
            L.unifyType = -1;
        L.feats.push_back(std::move(ft));
    };

    auto finishLayer = [&](LayerBuild &L) {
        OgrLayer &lyr = L.lyr;
        OgrFieldDefn idf;
        idf.name = "id";
        idf.type = L.idAccum.typeSet ? L.idAccum.type : OFTString;
        idf.subType = L.idAccum.typeSet ? L.idAccum.subType : OFSTNone;
        idf.tzAggr = L.idAccum.tzAggr;
        if (idf.type == OFTInteger && idf.subType == OFSTBoolean)
            idf.width = 1;
        lyr.fields.push_back(std::move(idf));
        std::vector<int> indeg(L.nodes.size(), 0);
        for (const auto &nd : L.nodes)
            for (int t : nd.after)
                indeg[t]++;
        std::vector<bool> done(L.nodes.size(), false);
        for (size_t iter = 0; iter < L.nodes.size(); iter++)
        {
            int pick = -1;
            for (size_t i = 0; i < L.nodes.size(); i++)
                if (!done[i] && indeg[i] == 0 &&
                    (pick < 0 || L.nodes[i].name < L.nodes[pick].name))
                    pick = (int)i;
            if (pick < 0)
                for (size_t i = 0; i < L.nodes.size(); i++)
                    if (!done[i] &&
                        (pick < 0 ||
                         L.nodes[i].name < L.nodes[pick].name))
                        pick = (int)i;
            done[pick] = true;
            for (int t : L.nodes[pick].after)
                indeg[t]--;
            const FieldAccum &a = L.accums[pick];
            OgrFieldDefn fd;
            fd.name = a.name;
            fd.type = a.typeSet ? a.type : OFTString;
            fd.subType = a.typeSet ? a.subType : OFSTNone;
            fd.tzAggr = a.tzAggr;
            if (fd.type == OFTInteger && fd.subType == OFSTBoolean)
                fd.width = 1;
            lyr.fields.push_back(std::move(fd));
        }
        std::map<std::string, int> fidx;
        for (size_t i = 0; i < lyr.fields.size(); i++)
            fidx.emplace(lyr.fields[i].name, (int)i);

        double ext[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
        bool anyPt = false;
        std::function<void(const OgrGeometry &)> acc =
            [&](const OgrGeometry &g) {
                for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
                {
                    anyPt = true;
                    ext[0] = std::min(ext[0], g.coords[i]);
                    ext[1] = std::min(ext[1], g.coords[i + 1]);
                    ext[2] = std::max(ext[2], g.coords[i]);
                    ext[3] = std::max(ext[3], g.coords[i + 1]);
                }
                for (const auto &p : g.parts)
                    acc(p);
            };

        long long seq = 0;
        for (TopoFeat &ft : L.feats)
        {
            OgrFeature feat;
            feat.fid = seq++;
            feat.values.resize(lyr.fields.size());
            if (ft.hasId)
            {
                feat.values[0].set = true;
                feat.values[0].v = std::move(ft.idVal);
            }
            for (auto &kv : ft.props)
            {
                auto it = fidx.find(kv.first);
                if (it == fidx.end())
                    continue;
                OgrFieldValue &fv = feat.values[it->second];
                fv.set = true;
                fv.v = std::move(kv.second);
            }
            feat.hasGeom = true;
            acc(ft.geom);
            feat.geom = std::move(ft.geom);
            lyr.features.push_back(std::move(feat));
        }
        lyr.geomType = L.unifyType > 0 ? L.unifyType : 0;
        if (anyPt)
        {
            lyr.hasExtent = true;
            for (int i = 0; i < 4; i++)
                lyr.extent[i] = ext[i];
        }
    };

    std::vector<LayerBuild> gcLayers;
    LayerBuild grouped;
    bool groupedEngaged = false;
    for (const auto &kv : objsM->obj)
    {
        const JVal &ov = kv.second;
        if (ov.type != JVal::OBJECT)
            continue;
        const JVal *tyM = ov.get("type");
        if (!tyM || tyM->type != JVal::STRING)
            continue;
        if (tyM->s == "GeometryCollection")
        {
            const JVal *geoms = ov.get("geometries");
            if (!geoms || geoms->type != JVal::ARRAY)
                continue;
            gcLayers.emplace_back();
            LayerBuild &L = gcLayers.back();
            L.lyr.name = kv.first;
            for (const JVal &gj : geoms->arr)
                addGeomObj(L, gj, nullptr);
        }
        else
        {
            // every non-GeometryCollection object lands in one shared
            // trailing layer, its object key as the id value
            groupedEngaged = true;
            addGeomObj(grouped, ov, &kv.first);
        }
    }
    if (gcLayers.empty() && !groupedEngaged)
    {
        fatal();
        return nullptr;
    }

    auto ds = std::make_unique<OgrDataset>();
    ds->path = path;
    ds->driverShort = "TopoJSON";
    ds->driverLong = "TopoJSON";
    ds->files.push_back(path);
    for (auto &L : gcLayers)
    {
        finishLayer(L);
        ds->layers.push_back(std::move(L.lyr));
    }
    if (groupedEngaged)
    {
        grouped.lyr.name = "TopoJSON";
        finishLayer(grouped);
        ds->layers.push_back(std::move(grouped.lyr));
    }

    // root crs member read like GeoJSON, but with no WGS84 default
    const JVal *crsM = root.get("crs");
    if (crsM && crsM->type == JVal::OBJECT)
    {
        const JVal *p = crsM->get("properties");
        std::string crsName =
            p && p->type == JVal::OBJECT ? p->getString("name") : "";
        if (!crsName.empty())
        {
            bool sok = false;
            Srs s = Srs::fromUserInput(crsName, sok);
            if (sok)
            {
                bool isDefault =
                    s.epsgCode() == 4326 ||
                    (s.authName() == "OGC" && s.code() == "CRS84");
                if (!isDefault)
                    for (auto &lyr : ds->layers)
                    {
                        bool lok = false;
                        lyr.srs = Srs::fromUserInput(crsName, lok);
                        lyr.hasSrs = lok;
                    }
            }
        }
    }

    err = "";
    return ds;
}
