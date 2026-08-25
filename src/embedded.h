#pragma once
#include <cstddef>
#include <string>

struct EmbeddedFile
{
    const char *name;
    const char *begin;
    const char *end;
};

const EmbeddedFile *embeddedFiles(size_t &count);

// Returns empty string if not found.
std::string embGet(const std::string &name);
bool embHas(const std::string &name);
