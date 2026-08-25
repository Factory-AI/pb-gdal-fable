#include "xml_min.h"

#include <cctype>

namespace
{

struct P
{
    const std::string &s;
    size_t i = 0;

    bool eof() const { return i >= s.size(); }
    char c() const { return i < s.size() ? s[i] : '\0'; }
    void skipWs()
    {
        while (!eof() && isspace((unsigned char)s[i]))
            ++i;
    }
    bool lit(const char *t)
    {
        size_t n = strlen(t);
        if (s.compare(i, n, t) == 0)
        {
            i += n;
            return true;
        }
        return false;
    }
    size_t strlen(const char *t) const
    {
        size_t n = 0;
        while (t[n])
            ++n;
        return n;
    }
};

std::string decodeEntities(const std::string &in)
{
    std::string out;
    for (size_t i = 0; i < in.size();)
    {
        if (in[i] == '&')
        {
            if (in.compare(i, 4, "&lt;") == 0)
            {
                out += '<';
                i += 4;
                continue;
            }
            if (in.compare(i, 4, "&gt;") == 0)
            {
                out += '>';
                i += 4;
                continue;
            }
            if (in.compare(i, 5, "&amp;") == 0)
            {
                out += '&';
                i += 5;
                continue;
            }
            if (in.compare(i, 6, "&quot;") == 0)
            {
                out += '"';
                i += 6;
                continue;
            }
            if (in.compare(i, 6, "&apos;") == 0)
            {
                out += '\'';
                i += 6;
                continue;
            }
            if (in.compare(i, 2, "&#") == 0)
            {
                size_t j = in.find(';', i);
                if (j != std::string::npos)
                {
                    int code = atoi(in.substr(i + 2, j - i - 2).c_str());
                    if (in[i + 2] == 'x' || in[i + 2] == 'X')
                        code = (int)strtol(in.substr(i + 3, j - i - 3).c_str(),
                                           nullptr, 16);
                    if (code > 0 && code < 128)
                        out += (char)code;
                    i = j + 1;
                    continue;
                }
            }
        }
        out += in[i++];
    }
    return out;
}

// CPL flavour, applied to the already XML-decoded text: known entities
// (incl. &amp; and &#38;) decode normally, but a bare '&' that does not
// start a valid entity ends the value; the rest of the run is dropped
std::string decodeEntitiesCpl(const std::string &in, bool &truncated)
{
    std::string out;
    for (size_t i = 0; i < in.size();)
    {
        if (in[i] == '&')
        {
            if (in.compare(i, 4, "&lt;") == 0)
            {
                out += '<';
                i += 4;
                continue;
            }
            if (in.compare(i, 4, "&gt;") == 0)
            {
                out += '>';
                i += 4;
                continue;
            }
            if (in.compare(i, 5, "&amp;") == 0)
            {
                out += '&';
                i += 5;
                continue;
            }
            if (in.compare(i, 6, "&quot;") == 0)
            {
                out += '"';
                i += 6;
                continue;
            }
            if (in.compare(i, 6, "&apos;") == 0)
            {
                out += '\'';
                i += 6;
                continue;
            }
            if (in.compare(i, 2, "&#") == 0)
            {
                size_t j = in.find(';', i);
                if (j != std::string::npos)
                {
                    int code = atoi(in.substr(i + 2, j - i - 2).c_str());
                    if (in[i + 2] == 'x' || in[i + 2] == 'X')
                        code = (int)strtol(in.substr(i + 3, j - i - 3).c_str(),
                                           nullptr, 16);
                    if (code > 0 && code < 128)
                        out += (char)code;
                    i = j + 1;
                    continue;
                }
            }
            truncated = true;
            return out;
        }
        out += in[i++];
    }
    return out;
}

std::string parseName(P &p)
{
    std::string n;
    while (!p.eof() &&
           (isalnum((unsigned char)p.c()) || p.c() == '_' || p.c() == ':' ||
            p.c() == '-' || p.c() == '.'))
        n += p.s[p.i++];
    return n;
}

bool parseElement(P &p, XmlNode &node)
{
    if (p.c() != '<')
        return false;
    ++p.i;
    node.name = parseName(p);
    if (node.name.empty())
        return false;
    for (;;)
    {
        p.skipWs();
        if (p.lit("/>"))
            return true;
        if (p.c() == '>')
        {
            ++p.i;
            break;
        }
        std::string an = parseName(p);
        if (an.empty())
            return false;
        p.skipWs();
        if (p.c() != '=')
            return false;
        ++p.i;
        p.skipWs();
        char q = p.c();
        if (q != '"' && q != '\'')
            return false;
        ++p.i;
        size_t start = p.i;
        while (!p.eof() && p.c() != q)
            ++p.i;
        node.attrs.emplace_back(
            an, decodeEntities(p.s.substr(start, p.i - start)));
        if (p.eof())
            return false;
        ++p.i;
    }
    // content
    for (;;)
    {
        if (p.eof())
            return false;
        if (p.lit("</"))
        {
            parseName(p);
            p.skipWs();
            if (p.c() == '>')
                ++p.i;
            return true;
        }
        if (p.lit("<!--"))
        {
            size_t j = p.s.find("-->", p.i);
            if (j == std::string::npos)
                return false;
            p.i = j + 3;
            continue;
        }
        if (p.lit("<![CDATA["))
        {
            size_t j = p.s.find("]]>", p.i);
            if (j == std::string::npos)
                return false;
            node.text += p.s.substr(p.i, j - p.i);
            if (!node.cplTruncated)
                node.textCpl += p.s.substr(p.i, j - p.i);
            p.i = j + 3;
            continue;
        }
        if (p.c() == '<')
        {
            XmlNode child;
            if (!parseElement(p, child))
                return false;
            node.children.push_back(std::move(child));
            continue;
        }
        size_t start = p.i;
        while (!p.eof() && p.c() != '<')
            ++p.i;
        std::string run = p.s.substr(start, p.i - start);
        std::string decoded = decodeEntities(run);
        node.text += decoded;
        if (!node.cplTruncated)
        {
            bool trunc = false;
            node.textCpl += decodeEntitiesCpl(decoded, trunc);
            node.cplTruncated = trunc;
        }
    }
}

}  // namespace

bool xmlParse(const std::string &content, XmlNode &root)
{
    P p{content};
    p.skipWs();
    while (p.lit("<?"))
    {
        size_t j = p.s.find("?>", p.i);
        if (j == std::string::npos)
            return false;
        p.i = j + 2;
        p.skipWs();
    }
    while (p.lit("<!--"))
    {
        size_t j = p.s.find("-->", p.i);
        if (j == std::string::npos)
            return false;
        p.i = j + 3;
        p.skipWs();
    }
    return parseElement(p, root);
}

std::string xmlEsc(const std::string &s, bool attr)
{
    std::string r;
    r.reserve(s.size());
    for (char c : s)
    {
        if (c == '&')
            r += "&amp;";
        else if (c == '<')
            r += "&lt;";
        else if (c == '>')
            r += "&gt;";
        else if (c == '"' && attr)
            r += "&quot;";
        else
            r += c;
    }
    return r;
}

void xmlSerialize(const XmlNode &n, std::string &out, int depth)
{
    std::string ind(depth * 2, ' ');
    out += ind + "<" + n.name;
    for (const auto &a : n.attrs)
        out += " " + a.first + "=\"" + xmlEsc(a.second, true) + "\"";
    if (n.children.empty() && n.text.empty())
    {
        out += " />\n";
        return;
    }
    if (n.children.empty())
    {
        out += ">" + xmlEsc(n.text) + "</" + n.name + ">\n";
        return;
    }
    out += ">\n";
    for (const auto &c : n.children)
        xmlSerialize(c, out, depth + 1);
    out += ind + "</" + n.name + ">\n";
}
