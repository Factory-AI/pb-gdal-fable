#include "util.h"
#include "vsi.h"
#include <sys/stat.h>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <algorithm>

std::string strPrintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string out(n, '\0');
    vsnprintf(&out[0], n + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

std::vector<std::string> strSplit(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos)
        {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool strStartsWith(const std::string &s, const std::string &pfx)
{
    return s.size() >= pfx.size() && s.compare(0, pfx.size(), pfx) == 0;
}

bool strEndsWith(const std::string &s, const std::string &sfx)
{
    return s.size() >= sfx.size() &&
           s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}

std::string strToLower(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string strToUpper(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}

std::string strJoin(const std::vector<std::string> &v, const std::string &sep)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i)
            out += sep;
        out += v[i];
    }
    return out;
}

std::string strTrim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool strEqualNoCase(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) !=
            std::tolower((unsigned char)b[i]))
            return false;
    return true;
}

bool readFileToString(const std::string &path, std::string &out)
{
    if (vsiIsVirtual(path))
    {
        std::string errKind;
        return vsiReadWhole(path, out, errKind);
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)std::max(0L, sz));
    size_t rd = sz > 0 ? fread(&out[0], 1, (size_t)sz, f) : 0;
    fclose(f);
    return rd == (size_t)sz;
}

bool writeStringToFile(const std::string &path, const std::string &content)
{
    if (vsiIsVirtual(path))
        return vsiWriteWhole(path, content);
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    size_t w = content.empty()
                   ? 0
                   : fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return w == content.size();
}

size_t osaDistance(const std::string &a, const std::string &b)
{
    const size_t n = a.size(), m = b.size();
    std::vector<std::vector<size_t>> d(n + 1, std::vector<size_t>(m + 1));
    for (size_t i = 0; i <= n; i++)
        d[i][0] = i;
    for (size_t j = 0; j <= m; j++)
        d[0][j] = j;
    for (size_t i = 1; i <= n; i++)
        for (size_t j = 1; j <= m; j++)
        {
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            size_t v = std::min(std::min(d[i - 1][j] + 1, d[i][j - 1] + 1),
                                d[i - 1][j - 1] + cost);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] &&
                a[i - 2] == b[j - 1])
                v = std::min(v, d[i - 2][j - 2] + 1);
            d[i][j] = v;
        }
    return d[n][m];
}

int cplValueType(const std::string &s)
{
    size_t i = 0, n = s.size();
    while (i < n && isspace((unsigned char)s[i]))
        i++;
    if (i == n)
        return 0;
    if (s[i] == '+' || s[i] == '-')
        i++;
    // zero-padded values stay strings ("03"), as do "0x"/"0e" forms;
    // "0" and "0.xxx" pass
    if (i < n && s[i] == '0' && i + 1 < n && s[i + 1] != '.')
        return 0;
    bool dot = false, expo = false, lastExp = false, real = false;
    bool digits = false;
    for (; i < n; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if (isdigit(c))
        {
            digits = true;
            lastExp = false;
        }
        else if (isspace(c))
        {
            size_t j = i;
            while (j < n && isspace((unsigned char)s[j]))
                j++;
            if (j != n)
                return 0;
            break;
        }
        else if (c == '+' || c == '-')
        {
            if (!lastExp)
                return 0;
            lastExp = false;
        }
        else if (c == '.')
        {
            if (dot || expo)
                return 0;
            dot = true;
            real = true;
            lastExp = false;
        }
        else if (c == 'e' || c == 'E' || c == 'd' || c == 'D')
        {
            if (expo)
                return 0;
            if (i + 1 >= n || !(s[i + 1] == '+' || s[i + 1] == '-' ||
                                isdigit((unsigned char)s[i + 1])))
                return 0;
            expo = true;
            lastExp = true;
            real = true;
        }
        else
            return 0;
    }
    if (!digits)
        return 0;
    return real ? 2 : 1;
}

bool numLooksHex(const std::string &s)
{
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i]))
        i++;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
        i++;
    return i + 1 < s.size() && s[i] == '0' &&
           (s[i + 1] == 'x' || s[i + 1] == 'X');
}
