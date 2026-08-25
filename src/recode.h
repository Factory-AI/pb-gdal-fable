#pragma once

#include <string>

// LDID language driver byte -> encoding name ("" when unmapped)
std::string recodeFromLdid(int ldid);
// .cpg file content (or ENCODING lco value) -> encoding name
std::string recodeFromCpg(const std::string &cpg);
// CPLCanRecode("test", enc, UTF-8) equivalent
bool recodeSupported(const std::string &enc);
// CPLRecode replica (stub for ISO-8859-1<->UTF-8, iconv otherwise).
// failed set when any warning fired during the conversion.
std::string cplRecode(const std::string &s, const std::string &src,
                      const std::string &dst, bool *failed = nullptr);
// no warnings emitted, warn-once flags untouched
std::string cplRecodeSilent(const std::string &s, const std::string &src,
                            const std::string &dst);
void recodeClearWarnFlags();

std::string recodeLatin1ToUtf8(const std::string &s);
std::string recodeUtf8ToLatin1(const std::string &s, bool &lossy);
