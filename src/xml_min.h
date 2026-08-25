#pragma once
#include <string>
#include <vector>

struct XmlNode
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;
    // CPL-parser flavour: text truncated at the first '&' produced by
    // entity decoding (&amp;, &#38;, unknown entities); used by the
    // GDAL_METADATA tag reader, while PAM keeps the full text
    std::string textCpl;
    bool cplTruncated = false;

    std::string attr(const std::string &key,
                     const std::string &def = "") const
    {
        for (auto &a : attrs)
            if (a.first == key)
                return a.second;
        return def;
    }
    const XmlNode *child(const std::string &n) const
    {
        for (auto &c : children)
            if (c.name == n)
                return &c;
        return nullptr;
    }
};

bool xmlParse(const std::string &content, XmlNode &root);
std::string xmlEsc(const std::string &s, bool attr = false);
void xmlSerialize(const XmlNode &n, std::string &out, int depth);
