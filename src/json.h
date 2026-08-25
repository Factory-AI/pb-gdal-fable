#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

// Minimal JSON DOM with json-c compatible serialization.
class JVal
{
  public:
    enum Type
    {
        NUL,
        BOOL,
        INT,
        DOUBLE,
        STRING,
        ARRAY,
        OBJECT
    };
    Type type = NUL;
    bool b = false;
    long long i = 0;
    double d = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    static JVal parse(const std::string &text, bool *ok = nullptr);

    const JVal *get(const std::string &key) const;
    std::string getString(const std::string &key,
                          const std::string &def = "") const;
    bool getBool(const std::string &key, bool def = false) const;
    long long getInt(const std::string &key, long long def = 0) const;
    double getDouble(const std::string &key, double def = 0) const;
    std::vector<std::string> getStringList(const std::string &key) const;
};
