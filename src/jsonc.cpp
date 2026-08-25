#include "jsonc.h"
#include "util.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

std::string jsoncFormatDouble(double d)
{
    if (std::isnan(d))
        return "NaN";
    if (std::isinf(d))
        return d > 0 ? "Infinity" : "-Infinity";
    char buf[64];
    for (int prec = 15; prec <= 17; ++prec)
    {
        snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (strtod(buf, nullptr) == d)
            break;
    }
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&
        !strchr(buf, 'n') && !strchr(buf, 'i'))
        strcat(buf, ".0");
    return buf;
}

static void escapeTo(std::string &out, const std::string &s,
                     bool escapeSlashes)
{
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '/':
                out += escapeSlashes ? "\\/" : "/";
                break;
            default:
                if (c < 0x20)
                    out += strPrintf("\\u%04x", c);
                else
                    out += static_cast<char>(c);
        }
    }
    out += '"';
}

static void ser(std::string &out, const JVal &v, int depth, bool esc)
{
    std::string ind(static_cast<size_t>(depth) * 2, ' ');
    std::string ind2(static_cast<size_t>(depth + 1) * 2, ' ');
    switch (v.type)
    {
        case JVal::NUL:
            out += "null";
            break;
        case JVal::BOOL:
            out += v.b ? "true" : "false";
            break;
        case JVal::INT:
            out += strPrintf("%lld", v.i);
            break;
        case JVal::DOUBLE:
            out += v.s.empty() ? jsoncFormatDouble(v.d) : v.s;
            break;
        case JVal::STRING:
            escapeTo(out, v.s, esc);
            break;
        case JVal::ARRAY:
            if (v.arr.empty())
            {
                out += "[\n" + ind + "]";
                break;
            }
            out += "[\n";
            for (size_t i = 0; i < v.arr.size(); ++i)
            {
                out += ind2;
                ser(out, v.arr[i], depth + 1, esc);
                if (i + 1 < v.arr.size())
                    out += ',';
                out += '\n';
            }
            out += ind + "]";
            break;
        case JVal::OBJECT:
            if (v.obj.empty())
            {
                out += "{\n" + ind + "}";
                break;
            }
            out += "{\n";
            for (size_t i = 0; i < v.obj.size(); ++i)
            {
                out += ind2;
                escapeTo(out, v.obj[i].first, esc);
                out += ':';
                ser(out, v.obj[i].second, depth + 1, esc);
                if (i + 1 < v.obj.size())
                    out += ',';
                out += '\n';
            }
            out += ind + "}";
            break;
    }
}

std::string jsoncSerialize(const JVal &v, bool escapeSlashes)
{
    std::string out;
    ser(out, v, 0, escapeSlashes);
    return out;
}
