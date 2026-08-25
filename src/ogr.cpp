#include "ogr.h"
#include "util.h"
#include <cmath>
#include <cstdio>
#include <cstring>

std::string ogrFieldTypeName(int t)
{
    switch (t)
    {
        case OFTInteger: return "Integer";
        case OFTIntegerList: return "IntegerList";
        case OFTReal: return "Real";
        case OFTRealList: return "RealList";
        case OFTString: return "String";
        case OFTStringList: return "StringList";
        case OFTBinary: return "Binary";
        case OFTDate: return "Date";
        case OFTTime: return "Time";
        case OFTDateTime: return "DateTime";
        case OFTInteger64: return "Integer64";
        case OFTInteger64List: return "Integer64List";
    }
    return "String";
}

std::string ogrFieldSubTypeName(int st)
{
    switch (st)
    {
        case OFSTBoolean: return "Boolean";
        case OFSTInt16: return "Int16";
        case OFSTFloat32: return "Float32";
        case OFSTJSON: return "JSON";
        case OFSTUUID: return "UUID";
    }
    return "None";
}

std::string ogrGeomTypeName(int t, bool hasZ, bool hasM)
{
    std::string base;
    switch (t)
    {
        case 0: base = "Unknown (any)"; break;
        case 1: base = "Point"; break;
        case 2: base = "Line String"; break;
        case 3: base = "Polygon"; break;
        case 4: base = "Multi Point"; break;
        case 5: base = "Multi Line String"; break;
        case 6: base = "Multi Polygon"; break;
        case 7: base = "Geometry Collection"; break;
        case 8: base = "Circular String"; break;
        case 9: base = "Compound Curve"; break;
        case 10: base = "Curve Polygon"; break;
        case 11: base = "Multi Curve"; break;
        case 12: base = "Multi Surface"; break;
        case 13: base = "Curve"; break;
        case 14: base = "Surface"; break;
        case 15: base = "PolyhedralSurface"; break;
        case 16: base = "TIN"; break;
        case 17: base = "Triangle"; break;
        case 101: return "None";
        default: return "Unknown (any)";
    }
    if (hasM)
        base = "Measured " + base;
    return hasZ ? "3D " + base : base;
}

// OGRFormatDouble semantics: %.<precision>g, then if the fraction part
// contains a run of six 0s or 9s that starts after at least one
// different fraction digit, reformat with only the significant digits
// that precede the run (".xxxx000000y" patterns; runs starting right at
// the decimal point are kept as-is)
std::string ogrFormatDouble(double v, int precision)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "%.*g", precision, v);
    std::string s = buf;
    size_t dot = s.find('.');
    if (dot == std::string::npos)
        return s;
    size_t end = s.find('e');
    if (end == std::string::npos)
        end = s.size();
    for (size_t p = dot + 2; p + 6 <= end; p++)
    {
        char c = s[p];
        if (c != '0' && c != '9')
            continue;
        if (s[p - 1] == c)
            continue;
        bool run = true;
        for (size_t k = p + 1; k < p + 6; k++)
            if (s[k] != c)
            {
                run = false;
                break;
            }
        if (!run)
            continue;
        int sig = 0;
        bool leading = true;
        for (size_t k = 0; k < p; k++)
        {
            if (!isdigit((unsigned char)s[k]))
                continue;
            if (leading && s[k] == '0')
                continue;
            leading = false;
            sig++;
        }
        if (sig == 0)
            sig = 1;
        snprintf(buf, sizeof(buf), "%.*g", sig, v);
        return buf;
    }
    return s;
}

// double serialization used by the vector info json writer: 17
// significant digits with the pattern-trim above, then ".0" when the
// result has neither '.' nor exponent
std::string ogrJsonDouble(double v)
{
    if (std::isnan(v))
        return "NaN";
    if (std::isinf(v))
        return v > 0 ? "Infinity" : "-Infinity";
    std::string s = ogrFormatDouble(v, 17);
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos)
        s += ".0";
    return s;
}

// json-c "spaced" plain serialization: "{ \"a\": 1, \"b\": [ 1, 2 ] }"
std::string ogrJsonSpacedSerialize(const JVal &v)
{
    switch (v.type)
    {
        case JVal::NUL: return "null";
        case JVal::BOOL: return v.b ? "true" : "false";
        case JVal::INT: return strPrintf("%lld", v.i);
        case JVal::DOUBLE:
            return v.s.empty() ? ogrJsonDouble(v.d) : v.s;
        case JVal::STRING:
        {
            std::string out = "\"";
            for (char c : v.s)
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += c;
                }
                else
                    out += c;
            }
            out += '"';
            return out;
        }
        case JVal::ARRAY:
        {
            if (v.arr.empty())
                return "[ ]";
            std::string out = "[ ";
            for (size_t i = 0; i < v.arr.size(); i++)
            {
                if (i)
                    out += ", ";
                out += ogrJsonSpacedSerialize(v.arr[i]);
            }
            out += " ]";
            return out;
        }
        case JVal::OBJECT:
        {
            if (v.obj.empty())
                return "{ }";
            std::string out = "{ ";
            for (size_t i = 0; i < v.obj.size(); i++)
            {
                if (i)
                    out += ", ";
                out += "\"" + v.obj[i].first +
                       "\": " + ogrJsonSpacedSerialize(v.obj[i].second);
            }
            out += " }";
            return out;
        }
    }
    return "";
}

static bool isIntegral(double v)
{
    return v == std::floor(v) && std::fabs(v) < 1e15;
}

static std::string wktNum(double v, bool forceDecimal)
{
    std::string s = ogrFormatDouble(v, 15);
    size_t ep = s.find('e');
    if (ep != std::string::npos && ep + 1 < s.size())
    {
        if (s[ep + 1] == '-')
        {
            // negative exponents render as fixed %.15f, trimmed of
            // trailing zeros down to one fraction digit
            char buf[512];
            snprintf(buf, sizeof(buf), "%.15f", v);
            s = buf;
            size_t dot = s.find('.');
            size_t last = s.size() - 1;
            while (last > dot + 1 && s[last] == '0')
                last--;
            s.resize(last + 1);
        }
        else
        {
            // uppercase exponent; the naive trailing-zero trim GDAL
            // applies to any dotted string also eats exponent zeros
            // (1.5E+200 -> 1.5E+2)
            s[ep] = 'E';
            if (s.find('.') != std::string::npos)
                while (!s.empty() && s.back() == '0')
                    s.pop_back();
        }
    }
    if (forceDecimal && s.find('.') == std::string::npos &&
        s.find('E') == std::string::npos &&
        s.find("inf") == std::string::npos &&
        s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

static void wktCoord(std::string &out, const double *c, bool hasZ,
                     const double *m = nullptr)
{
    bool bothInt = isIntegral(c[0]) && isIntegral(c[1]);
    if (bothInt)
    {
        out += strPrintf("%d %d", (int)c[0], (int)c[1]);
    }
    else
    {
        out += wktNum(c[0], true);
        out += ' ';
        out += wktNum(c[1], true);
    }
    if (hasZ)
    {
        out += ' ';
        out += wktNum(c[2], false);
    }
    if (m)
    {
        out += ' ';
        out += wktNum(*m, false);
    }
}

static void wktCoordList(std::string &out, const OgrGeometry &g, bool hasZ)
{
    if (g.coords.empty())
    {
        out += "EMPTY";
        return;
    }
    out += '(';
    for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
    {
        if (i)
            out += ',';
        wktCoord(out, &g.coords[i], hasZ,
                 g.hasM && i / 3 < g.m.size() ? &g.m[i / 3] : nullptr);
    }
    out += ')';
}

std::string ogrWktLegacy(const OgrGeometry &g);

static void wktBody(std::string &out, const OgrGeometry &g, bool hasZ,
                    bool legacy = false)
{
    // recursive IsEmpty short-circuit: a multi whose members are all
    // empty still prints a bare EMPTY
    if (g.empty)
    {
        out += "EMPTY";
        return;
    }
    switch (g.type)
    {
        case 1:
            if (g.coords.empty())
                out += "EMPTY";
            else
            {
                out += '(';
                wktCoord(out, &g.coords[0], hasZ,
                         g.hasM && !g.m.empty() ? &g.m[0] : nullptr);
                out += ')';
            }
            break;
        case 2:
            wktCoordList(out, g, hasZ);
            break;
        case 3:
        case 5:
        {
            // a polygon whose exterior ring is empty prints a bare
            // EMPTY even when interior rings hold vertices
            if (g.parts.empty() ||
                (g.type == 3 && g.parts[0].coords.empty()))
            {
                out += "EMPTY";
                break;
            }
            std::string inner;
            bool first = true;
            for (size_t i = 0; i < g.parts.size(); i++)
            {
                // empty member rings/lines are omitted (they stay
                // visible in the JSON export only)
                if (g.parts[i].coords.empty())
                    continue;
                if (!first)
                    inner += ',';
                first = false;
                wktCoordList(inner, g.parts[i], hasZ);
            }
            if (first)
                out += "EMPTY";
            else
                out += '(' + inner + ')';
            break;
        }
        case 4:
        {
            // empty member points are omitted; a multipoint left with
            // nothing prints a bare EMPTY
            bool anyPart = false;
            for (const OgrGeometry &p : g.parts)
                if (!p.coords.empty())
                    anyPart = true;
            if (!anyPart)
            {
                out += "EMPTY";
                break;
            }
            out += '(';
            bool first = true;
            for (size_t i = 0; i < g.parts.size(); i++)
            {
                if (g.parts[i].coords.empty())
                    continue;
                if (!first)
                    out += ',';
                first = false;
                if (!legacy)
                    out += '(';
                wktCoord(out, &g.parts[i].coords[0], hasZ,
                         g.parts[i].hasM && !g.parts[i].m.empty()
                             ? &g.parts[i].m[0]
                             : nullptr);
                if (!legacy)
                    out += ')';
            }
            out += ')';
            break;
        }
        case 6:
        {
            // member polygons with no rings or an empty exterior ring
            // are omitted; nothing left prints a bare EMPTY
            std::string inner;
            bool first = true;
            for (size_t i = 0; i < g.parts.size(); i++)
            {
                const OgrGeometry &sub = g.parts[i];
                if (sub.parts.empty() || sub.parts[0].coords.empty())
                    continue;
                if (!first)
                    inner += ',';
                first = false;
                inner += '(';
                bool rfirst = true;
                for (size_t j = 0; j < sub.parts.size(); j++)
                {
                    if (sub.parts[j].coords.empty())
                        continue;
                    if (!rfirst)
                        inner += ',';
                    rfirst = false;
                    wktCoordList(inner, sub.parts[j], hasZ);
                }
                inner += ')';
            }
            if (first)
                out += "EMPTY";
            else
                out += '(' + inner + ')';
            break;
        }
        case 7:
        {
            if (g.parts.empty())
            {
                out += "EMPTY";
                break;
            }
            out += '(';
            for (size_t i = 0; i < g.parts.size(); i++)
            {
                if (i)
                    out += ',';
                out += legacy ? ogrWktLegacy(g.parts[i])
                              : ogrWkt(g.parts[i]);
            }
            out += ')';
            break;
        }
    }
}

static std::string wktCompose(const OgrGeometry &g, bool legacy)
{
    static const char *names[] = {"",
                                  "POINT",
                                  "LINESTRING",
                                  "POLYGON",
                                  "MULTIPOINT",
                                  "MULTILINESTRING",
                                  "MULTIPOLYGON",
                                  "GEOMETRYCOLLECTION"};
    if (g.type < 1 || g.type > 7)
        return "";
    std::string out = names[g.type];
    if (!legacy)
    {
        if (g.hasZ && g.hasM)
            out += " ZM";
        else if (g.hasZ)
            out += " Z";
        else if (g.hasM)
            out += " M";
    }
    std::string body;
    wktBody(body, g, g.hasZ, legacy);
    if (body == "EMPTY")
        out += " EMPTY";
    else
    {
        out += ' ';
        out += body;
    }
    return out;
}

std::string ogrWkt(const OgrGeometry &g)
{
    return wktCompose(g, false);
}

// GeoJSON export failure scan, first hit wins: 1 = empty point (no
// representation, fails quietly), 2 = non-finite coordinate (warns)
int geomJsonExportFail(const OgrGeometry &g)
{
    if (g.type == 1 && (g.empty || g.coords.empty()))
        return 1;
    for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
        for (int k = 0; k < 3; k++)
            if (!std::isfinite(g.coords[i + k]))
                return 2;
    for (const OgrGeometry &p : g.parts)
    {
        if (g.type == 4 && p.coords.empty())
            return 1;
        if (int r = geomJsonExportFail(p))
            return r;
    }
    return 0;
}

static bool gDiagOnceEmitted = false;

bool diagOnceGate(bool once)
{
    if (!once)
        return true;
    if (gDiagOnceEmitted)
        return false;
    gDiagOnceEmitted = true;
    return true;
}

std::string ogrWktLegacy(const OgrGeometry &g)
{
    return wktCompose(g, true);
}

// Replicates OGRParseDate for the formats GeoJSON strings can carry
bool ogrParseDate(const std::string &s, OgrDateTime &dt)
{
    const char *p = s.c_str();
    dt = OgrDateTime();

    auto parseTime = [&](const char *&q) -> bool {
        char *end = nullptr;
        long h = strtol(q, &end, 10);
        if (end == q || *end != ':' || end - q > 2 || h < 0 || h > 23)
            return false;
        q = end + 1;
        long mi = strtol(q, &end, 10);
        if (end == q || end - q > 2 || mi < 0 || mi > 59)
            return false;
        q = end;
        dt.hour = (int)h;
        dt.minute = (int)mi;
        dt.sec = 0;
        if (*q == ':')
        {
            q++;
            double sec = strtod(q, &end);
            if (end == q || sec < 0 || sec >= 61)
                return false;
            dt.sec = sec;
            q = end;
        }
        dt.hasTime = true;
        return true;
    };

    // leading date?
    char *end = nullptr;
    long y = strtol(p, &end, 10);
    if (end != p && (*end == '-' || *end == '/') && end - p <= 4 &&
        end - p >= 1)
    {
        char sep = *end;
        const char *q = end + 1;
        long mo = strtol(q, &end, 10);
        if (end == q || *end != sep || end - q > 2 || mo < 1 || mo > 12)
            return false;
        q = end + 1;
        long d = strtol(q, &end, 10);
        if (end == q || end - q > 2 || d < 1 || d > 31)
            return false;
        dt.year = (int)y;
        dt.month = (int)mo;
        dt.day = (int)d;
        dt.hasDate = true;
        q = end;
        if (*q == '\0')
        {
            dt.tzFlag = 0;
            return true;
        }
        if (*q != 'T' && *q != ' ')
            return false;
        q++;
        if (!parseTime(q))
            return false;
        if (*q == '\0')
        {
            dt.tzFlag = 0;
            return true;
        }
        if (*q == 'Z')
        {
            dt.tzFlag = 100;
            return q[1] == '\0';
        }
        if (*q == '+' || *q == '-')
        {
            int sign = *q == '+' ? 1 : -1;
            q++;
            if (!isdigit((unsigned char)q[0]) ||
                !isdigit((unsigned char)q[1]))
                return false;
            int oh = (q[0] - '0') * 10 + (q[1] - '0');
            q += 2;
            int om = 0;
            if (*q == ':')
                q++;
            if (isdigit((unsigned char)q[0]) && isdigit((unsigned char)q[1]))
            {
                om = (q[0] - '0') * 10 + (q[1] - '0');
                q += 2;
            }
            if (*q != '\0')
                return false;
            dt.tzFlag = 100 + sign * (oh * 4 + om / 15);
            return true;
        }
        return false;
    }

    // bare time
    const char *q = p;
    if (parseTime(q) && *q == '\0')
    {
        dt.tzFlag = 0;
        return true;
    }
    return false;
}

std::string ogrDateTimeToString(const OgrDateTime &dt, int fieldType)
{
    std::string out;
    if (fieldType == OFTDate)
        return strPrintf("%04d/%02d/%02d", dt.year, dt.month, dt.day);
    if (fieldType == OFTTime)
    {
        if (dt.sec != std::floor(dt.sec))
            return strPrintf("%02d:%02d:%06.3f", dt.hour, dt.minute, dt.sec);
        return strPrintf("%02d:%02d:%02d", dt.hour, dt.minute, (int)dt.sec);
    }
    if (dt.sec != std::floor(dt.sec))
        out = strPrintf("%04d/%02d/%02d %02d:%02d:%06.3f", dt.year, dt.month,
                        dt.day, dt.hour, dt.minute, dt.sec);
    else
        out = strPrintf("%04d/%02d/%02d %02d:%02d:%02d", dt.year, dt.month,
                        dt.day, dt.hour, dt.minute, (int)dt.sec);
    if (dt.tzFlag > 1)
    {
        int off = (dt.tzFlag - 100) * 15;
        char sign = off < 0 ? '-' : '+';
        int a = off < 0 ? -off : off;
        if (a % 60)
            out += strPrintf("%c%02d%02d", sign, a / 60, a % 60);
        else
            out += strPrintf("%c%02d", sign, a / 60);
    }
    return out;
}

