#pragma once
#include "json.h"
#include <string>

// Serializes with json-c pretty conventions: 2-space indent, no space
// after ':', ".0" suffix for integral doubles, shortest round-trip %g.
std::string jsoncSerialize(const JVal &v, bool escapeSlashes = false);
std::string jsoncFormatDouble(double d);
// json-c tokener error emulation: true when parsing fails, filling the
// json-c error descriptor and byte offset
bool jsoncTokenerError(const std::string &content, std::string &desc,
                       size_t &off);
