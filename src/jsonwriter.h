#pragma once
#include <string>
#include <vector>

// Replica of CPLJSonStreamingWriter output formatting:
// 2-space indent, ": " after keys, no trailing newline, "[]"/"{}" for empty.
class JsonStreamWriter
{
  public:
    std::string result() const { return out_; }

    void startArray();
    void endArray();
    void startObject();
    void endObject();
    void addKey(const std::string &key);
    void addString(const std::string &v);
    void addInt(long long v);
    void addDouble(double v, const char *fmt = "%.17g");
    void addBool(bool v);
    void addNull();
    void addRaw(const std::string &raw);

    // CPLJSonStreamingWriter::SetNewline replica: while disabled, values
    // are separated by ", " on the current line; returns previous state
    bool setNewline(bool enabled)
    {
        bool prev = newline_;
        newline_ = enabled;
        return prev;
    }

    static std::string escape(const std::string &s);

  private:
    struct Level
    {
        bool nonEmpty = false;
    };
    std::string out_;
    std::vector<Level> levels_;
    bool pendingKey_ = false;
    bool newline_ = true;

    void beforeValue();
    void indent();
};
