#include "json.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <strings.h>

namespace
{
struct Parser
{
    const char *p;
    const char *end;
    bool ok = true;

    void skipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    JVal parseValue()
    {
        skipWs();
        if (p >= end)
        {
            ok = false;
            return {};
        }
        switch (*p)
        {
            case '{':
                return parseObject();
            case '[':
                return parseArray();
            case '"':
                return parseString();
            case 't':
            case 'f':
            case 'T':
            case 'F':
                return parseBool();
            case 'n':
            case 'N':
                if (end - p >= 4 && strncasecmp(p, "null", 4) == 0)
                {
                    p += 4;
                    return {};
                }
                // fallthrough: "nan" in any case
            case 'I':
            case 'i':
            {
                JVal v;
                v.type = JVal::DOUBLE;
                if (end - p >= 8 && strncasecmp(p, "infinity", 8) == 0)
                {
                    v.d = std::numeric_limits<double>::infinity();
                    p += 8;
                }
                else if (end - p >= 3 && strncasecmp(p, "nan", 3) == 0)
                {
                    v.d = std::numeric_limits<double>::quiet_NaN();
                    p += 3;
                }
                else
                    ok = false;
                return v;
            }
            default:
                return parseNumber();
        }
    }

    JVal parseObject()
    {
        JVal v;
        v.type = JVal::OBJECT;
        ++p;
        skipWs();
        if (p < end && *p == '}')
        {
            ++p;
            return v;
        }
        while (ok)
        {
            skipWs();
            if (p >= end || *p != '"')
            {
                ok = false;
                break;
            }
            JVal key = parseString();
            skipWs();
            if (p >= end || *p != ':')
            {
                ok = false;
                break;
            }
            ++p;
            JVal val = parseValue();
            v.obj.emplace_back(key.s, std::move(val));
            skipWs();
            if (p < end && *p == ',')
            {
                ++p;
                continue;
            }
            if (p < end && *p == '}')
            {
                ++p;
                break;
            }
            ok = false;
            break;
        }
        return v;
    }

    JVal parseArray()
    {
        JVal v;
        v.type = JVal::ARRAY;
        ++p;
        skipWs();
        if (p < end && *p == ']')
        {
            ++p;
            return v;
        }
        while (ok)
        {
            v.arr.push_back(parseValue());
            skipWs();
            if (p < end && *p == ',')
            {
                ++p;
                continue;
            }
            if (p < end && *p == ']')
            {
                ++p;
                break;
            }
            ok = false;
            break;
        }
        return v;
    }

    JVal parseString()
    {
        JVal v;
        v.type = JVal::STRING;
        ++p;
        while (p < end && *p != '"')
        {
            if (*p == '\\' && p + 1 < end)
            {
                ++p;
                switch (*p)
                {
                    case 'n':
                        v.s += '\n';
                        break;
                    case 't':
                        v.s += '\t';
                        break;
                    case 'r':
                        v.s += '\r';
                        break;
                    case 'b':
                        v.s += '\b';
                        break;
                    case 'f':
                        v.s += '\f';
                        break;
                    case 'u':
                    {
                        if (end - p >= 5)
                        {
                            char hex[5] = {p[1], p[2], p[3], p[4], 0};
                            unsigned cp = strtoul(hex, nullptr, 16);
                            p += 4;
                            if (cp < 0x80)
                                v.s += static_cast<char>(cp);
                            else if (cp < 0x800)
                            {
                                v.s += static_cast<char>(0xC0 | (cp >> 6));
                                v.s += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            else
                            {
                                v.s += static_cast<char>(0xE0 | (cp >> 12));
                                v.s += static_cast<char>(0x80 |
                                                         ((cp >> 6) & 0x3F));
                                v.s += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                        }
                        break;
                    }
                    default:
                        v.s += *p;
                        break;
                }
                ++p;
            }
            else
            {
                v.s += *p;
                ++p;
            }
        }
        if (p < end)
            ++p;
        else
            ok = false;
        return v;
    }

    JVal parseBool()
    {
        JVal v;
        v.type = JVal::BOOL;
        if (end - p >= 4 && strncasecmp(p, "true", 4) == 0)
        {
            v.b = true;
            p += 4;
        }
        else if (end - p >= 5 && strncasecmp(p, "false", 5) == 0)
        {
            v.b = false;
            p += 5;
        }
        else
            ok = false;
        return v;
    }

    JVal parseNumber()
    {
        const char *start = p;
        bool isDouble = false;
        if (p < end && (*p == '-' || *p == '+'))
            ++p;
        if (p < end && (*p == 'I' || *p == 'N' || *p == 'i' || *p == 'n'))
        {
            JVal inner = parseValue();
            JVal v;
            v.type = JVal::DOUBLE;
            v.d = *start == '-' ? -inner.d : inner.d;
            return v;
        }
        while (p < end && (isdigit(static_cast<unsigned char>(*p)) ||
                           *p == '.' || *p == 'e' || *p == 'E' || *p == '-' ||
                           *p == '+'))
        {
            if (*p == '.' || *p == 'e' || *p == 'E')
                isDouble = true;
            ++p;
        }
        std::string num(start, p);
        JVal v;
        if (isDouble)
        {
            v.type = JVal::DOUBLE;
            v.d = strtod(num.c_str(), nullptr);
            v.s = num;  // raw literal, kept verbatim on re-serialization
        }
        else
        {
            v.type = JVal::INT;
            v.i = strtoll(num.c_str(), nullptr, 10);
        }
        return v;
    }
};
}  // namespace

JVal JVal::parse(const std::string &text, bool *okOut)
{
    Parser parser{text.data(), text.data() + text.size()};
    JVal v = parser.parseValue();
    if (okOut)
        *okOut = parser.ok;
    return v;
}

const JVal *JVal::get(const std::string &key) const
{
    for (const auto &kv : obj)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

std::string JVal::getString(const std::string &key,
                            const std::string &def) const
{
    const JVal *v = get(key);
    return v && v->type == STRING ? v->s : def;
}

bool JVal::getBool(const std::string &key, bool def) const
{
    const JVal *v = get(key);
    return v && v->type == BOOL ? v->b : def;
}

long long JVal::getInt(const std::string &key, long long def) const
{
    const JVal *v = get(key);
    if (!v)
        return def;
    if (v->type == INT)
        return v->i;
    if (v->type == DOUBLE)
        return static_cast<long long>(v->d);
    return def;
}

double JVal::getDouble(const std::string &key, double def) const
{
    const JVal *v = get(key);
    if (!v)
        return def;
    if (v->type == DOUBLE)
        return v->d;
    if (v->type == INT)
        return static_cast<double>(v->i);
    return def;
}

std::vector<std::string> JVal::getStringList(const std::string &key) const
{
    std::vector<std::string> out;
    const JVal *v = get(key);
    if (v && v->type == ARRAY)
        for (const auto &e : v->arr)
            if (e.type == STRING)
                out.push_back(e.s);
    return out;
}
