#include "recode.h"
#include "cpl.h"
#include "util.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iconv.h>

namespace
{

bool startsWithCI(const std::string &s, const char *pfx)
{
    size_t n = strlen(pfx);
    if (s.size() < n)
        return false;
    return strEqualNoCase(s.substr(0, n), pfx);
}

bool stubWarned = false;
bool iconvWarned = false;

}  // namespace

void recodeClearWarnFlags()
{
    stubWarned = false;
    iconvWarned = false;
}

std::string recodeFromLdid(int ldid)
{
    int cp = -1;
    switch (ldid)
    {
        case 1: cp = 437; break;
        case 2: cp = 850; break;
        case 3: cp = 1252; break;
        case 4: cp = 10000; break;
        case 8: cp = 865; break;
        case 10: cp = 850; break;
        case 11: cp = 437; break;
        case 13: cp = 437; break;
        case 14: cp = 850; break;
        case 15: cp = 437; break;
        case 16: cp = 850; break;
        case 17: cp = 437; break;
        case 18: cp = 850; break;
        case 19: cp = 932; break;
        case 20: cp = 850; break;
        case 21: cp = 437; break;
        case 22: cp = 850; break;
        case 23: cp = 865; break;
        case 24: cp = 437; break;
        case 25: cp = 437; break;
        case 26: cp = 850; break;
        case 27: cp = 437; break;
        case 28: cp = 863; break;
        case 29: cp = 850; break;
        case 31: cp = 852; break;
        case 34: cp = 852; break;
        case 35: cp = 852; break;
        case 36: cp = 860; break;
        case 37: cp = 850; break;
        case 38: cp = 866; break;
        case 55: cp = 850; break;
        case 64: cp = 852; break;
        case 77: cp = 936; break;
        case 78: cp = 949; break;
        case 79: cp = 950; break;
        case 80: cp = 874; break;
        case 87: return "ISO-8859-1";
        case 88: cp = 1252; break;
        case 89: cp = 1252; break;
        case 100: cp = 852; break;
        case 101: cp = 866; break;
        case 102: cp = 865; break;
        case 103: cp = 861; break;
        case 104: cp = 895; break;
        case 105: cp = 620; break;
        case 106: cp = 737; break;
        case 107: cp = 857; break;
        case 108: cp = 863; break;
        case 120: cp = 950; break;
        case 121: cp = 949; break;
        case 122: cp = 936; break;
        case 123: cp = 932; break;
        case 124: cp = 874; break;
        case 134: cp = 737; break;
        case 135: cp = 852; break;
        case 136: cp = 857; break;
        case 150: cp = 10007; break;
        case 151: cp = 10029; break;
        case 200: cp = 1250; break;
        case 201: cp = 1251; break;
        case 202: cp = 1254; break;
        case 203: cp = 1253; break;
        case 204: cp = 1257; break;
        default: break;
    }
    if (cp < 0)
        return "";
    return strPrintf("CP%d", cp);
}

std::string recodeFromCpg(const std::string &cpg)
{
    const int n = atoi(cpg.c_str());
    if ((n >= 437 && n <= 950) || (n >= 1250 && n <= 1258))
        return strPrintf("CP%d", n);
    if (startsWithCI(cpg, "8859"))
    {
        if (cpg.size() > 4 && cpg[4] == '-')
            return "ISO-8859-" + cpg.substr(5);
        return "ISO-8859-" + cpg.substr(4);
    }
    if (startsWithCI(cpg, "UTF-8") || startsWithCI(cpg, "UTF8"))
        return "UTF-8";
    if (startsWithCI(cpg, "ANSI 1251"))
        return "CP1251";
    return cpg;
}

std::string recodeLatin1ToUtf8(const std::string &in)
{
    std::string out;
    for (unsigned char c : in)
    {
        if (c < 0x80)
            out += (char)c;
        else
        {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string recodeUtf8ToLatin1(const std::string &in, bool &lossy)
{
    std::string out;
    size_t i = 0;
    while (i < in.size())
    {
        unsigned char c = in[i];
        if (c < 0x80)
        {
            out += (char)c;
            i++;
        }
        else if ((c & 0xE0) == 0xC0 && i + 1 < in.size())
        {
            unsigned cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F);
            if (cp <= 0xFF)
                out += (char)cp;
            else
            {
                out += '?';
                lossy = true;
            }
            i += 2;
        }
        else
        {
            out += '?';
            lossy = true;
            i++;
            while (i < in.size() && ((unsigned char)in[i] & 0xC0) == 0x80)
                i++;
        }
    }
    return out;
}

namespace
{
bool g_silent = false;
}

std::string cplRecodeSilent(const std::string &s, const std::string &src,
                            const std::string &dst)
{
    bool saveStub = stubWarned, saveIconv = iconvWarned;
    g_silent = true;
    std::string out = cplRecode(s, src, dst);
    g_silent = false;
    stubWarned = saveStub;
    iconvWarned = saveIconv;
    return out;
}

std::string cplRecode(const std::string &s, const std::string &src,
                      const std::string &dst, bool *failed)
{
    if (failed)
        *failed = false;
    if (strEqualNoCase(src, dst))
        return s;
    if (strEqualNoCase(src, "ISO-8859-1") && strEqualNoCase(dst, "UTF-8"))
        return recodeLatin1ToUtf8(s);
    if (strEqualNoCase(src, "UTF-8") && strEqualNoCase(dst, "ISO-8859-1"))
    {
        bool lossy = false;
        std::string out = recodeUtf8ToLatin1(s, lossy);
        if (lossy)
        {
            if (failed)
                *failed = true;
            if (!stubWarned && !g_silent)
            {
                stubWarned = true;
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "One or several characters couldn't be "
                            "converted correctly from " +
                                src + " to " + dst +
                                ".  This warning will not be emitted "
                                "anymore.");
            }
        }
        return out;
    }
    iconv_t cd = iconv_open(dst.c_str(), src.c_str());
    if (cd == (iconv_t)-1)
    {
        if (failed)
            *failed = true;
        if (!g_silent)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Recode from " + src + " to " + dst +
                            " failed with the error: \"" +
                            strerror(errno) + "\".");
        return s;
    }
    std::string out;
    size_t cap = s.size() * 4 + 8;
    out.resize(cap);
    char *ip = const_cast<char *>(s.data());
    size_t inLeft = s.size();
    char *op = &out[0];
    size_t outLeft = cap;
    while (inLeft > 0)
    {
        size_t r = iconv(cd, &ip, &inLeft, &op, &outLeft);
        if (r != (size_t)-1)
            break;
        if (errno == EILSEQ)
        {
            if (failed)
                *failed = true;
            if (!iconvWarned && !g_silent)
            {
                iconvWarned = true;
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "One or several characters couldn't be "
                            "converted correctly from " +
                                src + " to " + dst +
                                ".  This warning will not be emitted "
                                "anymore");
            }
            if (inLeft == 0)
                break;
            --inLeft;
            ++ip;
        }
        else if (errno == E2BIG)
        {
            size_t done = (size_t)(op - out.data());
            cap *= 2;
            out.resize(cap);
            op = &out[done];
            outLeft = cap - done;
        }
        else
            break;
    }
    iconv_close(cd);
    out.resize((size_t)(op - out.data()));
    return out;
}

bool recodeSupported(const std::string &enc)
{
    if (strEqualNoCase(enc, "UTF-8"))
        return true;
    if (strEqualNoCase(enc, "ISO-8859-1"))
        return true;
    iconv_t cd = iconv_open("UTF-8", enc.c_str());
    if (cd == (iconv_t)-1)
        return false;
    iconv_close(cd);
    return true;
}
