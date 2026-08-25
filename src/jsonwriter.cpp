#include "jsonwriter.h"
#include "util.h"

void JsonStreamWriter::indent()
{
    for (size_t i = 0; i < levels_.size(); ++i)
        out_ += "  ";
}

void JsonStreamWriter::beforeValue()
{
    if (pendingKey_)
    {
        pendingKey_ = false;
        return;
    }
    if (!levels_.empty())
    {
        if (!newline_)
        {
            if (levels_.back().nonEmpty)
                out_ += ", ";
            levels_.back().nonEmpty = true;
            return;
        }
        if (levels_.back().nonEmpty)
            out_ += ",";
        out_ += "\n";
        indent();
        levels_.back().nonEmpty = true;
    }
}

void JsonStreamWriter::startArray()
{
    beforeValue();
    out_ += "[";
    levels_.push_back(Level());
}

void JsonStreamWriter::endArray()
{
    bool nonEmpty = levels_.back().nonEmpty;
    levels_.pop_back();
    if (nonEmpty && newline_)
    {
        out_ += "\n";
        indent();
    }
    out_ += "]";
}

void JsonStreamWriter::startObject()
{
    beforeValue();
    out_ += "{";
    levels_.push_back(Level());
}

void JsonStreamWriter::endObject()
{
    bool nonEmpty = levels_.back().nonEmpty;
    levels_.pop_back();
    if (nonEmpty && newline_)
    {
        out_ += "\n";
        indent();
    }
    out_ += "}";
}

void JsonStreamWriter::addKey(const std::string &key)
{
    beforeValue();
    out_ += "\"" + escape(key) + "\": ";
    pendingKey_ = true;
}

void JsonStreamWriter::addString(const std::string &v)
{
    beforeValue();
    out_ += "\"" + escape(v) + "\"";
}

void JsonStreamWriter::addInt(long long v)
{
    beforeValue();
    out_ += strPrintf("%lld", v);
}

void JsonStreamWriter::addDouble(double v, const char *fmt)
{
    beforeValue();
    out_ += strPrintf(fmt, v);
}

void JsonStreamWriter::addBool(bool v)
{
    beforeValue();
    out_ += v ? "true" : "false";
}

void JsonStreamWriter::addNull()
{
    beforeValue();
    out_ += "null";
}

void JsonStreamWriter::addRaw(const std::string &raw)
{
    beforeValue();
    out_ += raw;
}

std::string JsonStreamWriter::escape(const std::string &s)
{
    std::string out;
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
            default:
                if (c < 0x20)
                    out += strPrintf("\\u%04x", c);
                else
                    out += static_cast<char>(c);
        }
    }
    return out;
}
