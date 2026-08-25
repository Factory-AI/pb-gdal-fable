#include "embedded.h"
#include <map>

static const std::map<std::string, std::pair<const char *, size_t>> &index_()
{
    static std::map<std::string, std::pair<const char *, size_t>> m = [] {
        std::map<std::string, std::pair<const char *, size_t>> r;
        size_t count = 0;
        const EmbeddedFile *files = embeddedFiles(count);
        for (size_t i = 0; i < count; ++i)
            r[files[i].name] = {files[i].begin,
                                static_cast<size_t>(files[i].end -
                                                    files[i].begin)};
        return r;
    }();
    return m;
}

std::string embGet(const std::string &name)
{
    auto &m = index_();
    auto it = m.find(name);
    if (it == m.end())
        return std::string();
    return std::string(it->second.first, it->second.second);
}

bool embHas(const std::string &name)
{
    return index_().count(name) != 0;
}
