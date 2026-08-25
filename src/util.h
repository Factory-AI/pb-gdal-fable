#pragma once
#include <string>
#include <vector>

std::string strPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> strSplit(const std::string &s, char sep);
bool strStartsWith(const std::string &s, const std::string &pfx);
bool strEndsWith(const std::string &s, const std::string &sfx);
std::string strToLower(const std::string &s);
std::string strToUpper(const std::string &s);
std::string strJoin(const std::vector<std::string> &v, const std::string &sep);
std::string strTrim(const std::string &s);
bool strEqualNoCase(const std::string &a, const std::string &b);
size_t osaDistance(const std::string &a, const std::string &b);
bool readFileToString(const std::string &path, std::string &out);
bool writeStringToFile(const std::string &path, const std::string &content);
// CPLGetValueType clone: 0 = string, 1 = integer, 2 = real; tolerates
// surrounding whitespace, rejects zero-padded numbers and hexadecimal,
// accepts d/D exponents
int cplValueType(const std::string &s);
// strtod parses hexadecimal but GDAL's real-value checks do not
bool numLooksHex(const std::string &s);
