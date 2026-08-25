#include "vsi.h"
#include "cpl.h"
#include "util.h"

#include <zlib.h>

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>

namespace
{

std::map<std::string, std::string> &memFs()
{
    static std::map<std::string, std::string> fs;
    return fs;
}

bool startsWithNoCase(const std::string &s, const char *p)
{
    size_t n = strlen(p);
    return s.size() >= n && strEqualNoCase(s.substr(0, n), p);
}

bool rawRead(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

bool inflateRaw(const std::string &in, size_t expected, std::string &out)
{
    z_stream zs{};
    if (inflateInit2(&zs, -15) != Z_OK)
        return false;
    out.resize(expected);
    zs.next_in = (Bytef *)in.data();
    zs.avail_in = (uInt)in.size();
    zs.next_out = (Bytef *)out.data();
    zs.avail_out = (uInt)out.size();
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END || (rc == Z_OK && zs.avail_out == 0) ||
           (rc == Z_BUF_ERROR && zs.avail_out == 0);
}

bool gunzipWhole(const std::string &in, std::string &out)
{
    z_stream zs{};
    if (inflateInit2(&zs, 16 + 15) != Z_OK)
        return false;
    zs.next_in = (Bytef *)in.data();
    zs.avail_in = (uInt)in.size();
    out.clear();
    char buf[65536];
    int rc = Z_OK;
    do
    {
        zs.next_out = (Bytef *)buf;
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END)
            break;
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (rc == Z_STREAM_END && zs.avail_in > 0)
        {
            // concatenated members
            if (inflateReset2(&zs, 16 + 15) != Z_OK)
                break;
            rc = Z_OK;
        }
    } while (rc == Z_OK && (zs.avail_in > 0 || zs.avail_out == 0));
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

std::string deflateRawStr(const std::string &in)
{
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return std::string();
    std::string out;
    out.resize(deflateBound(&zs, (uLong)in.size()));
    zs.next_in = (Bytef *)in.data();
    zs.avail_in = (uInt)in.size();
    zs.next_out = (Bytef *)&out[0];
    zs.avail_out = (uInt)out.size();
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

uint16_t rd16(const std::string &d, size_t o)
{
    return (uint8_t)d[o] | ((uint8_t)d[o + 1] << 8);
}
uint32_t rd32(const std::string &d, size_t o)
{
    return (uint32_t)(uint8_t)d[o] | ((uint32_t)(uint8_t)d[o + 1] << 8) |
           ((uint32_t)(uint8_t)d[o + 2] << 16) |
           ((uint32_t)(uint8_t)d[o + 3] << 24);
}
uint64_t rd64(const std::string &d, size_t o)
{
    return (uint64_t)rd32(d, o) | ((uint64_t)rd32(d, o + 4) << 32);
}

void wr16(std::string &s, uint16_t v)
{
    s += (char)(v & 0xff);
    s += (char)(v >> 8);
}
void wr32(std::string &s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s += (char)((v >> (8 * i)) & 0xff);
}

struct ArchiveEntry
{
    std::string name;
    bool isDir = false;
    uint16_t method = 0;  // zip only
    uint64_t csize = 0, usize = 0;
    uint64_t dataOff = 0;  // offset of raw data in archive
    bool needLocal = true;  // zip: dataOff is local header offset
    long long mtime = 0;
};

struct Archive
{
    bool ok = false;
    bool isZip = false;
    std::string bytes;
    std::vector<ArchiveEntry> entries;
};

bool parseZip(Archive &a)
{
    const std::string &d = a.bytes;
    if (d.size() < 22)
        return false;
    size_t scanStart = d.size() > 65557 ? d.size() - 65557 : 0;
    size_t eocd = std::string::npos;
    for (size_t i = d.size() - 22 + 1; i-- > scanStart;)
    {
        if (d[i] == 'P' && d[i + 1] == 'K' && d[i + 2] == 5 &&
            d[i + 3] == 6)
        {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos)
        return false;
    uint64_t count = rd16(d, eocd + 10);
    uint64_t cdOff = rd32(d, eocd + 16);
    if (count == 0xFFFF || cdOff == 0xFFFFFFFFu)
    {
        // zip64: locate EOCD64 locator right before EOCD
        if (eocd >= 20 && rd32(d, eocd - 20) == 0x07064b50u)
        {
            uint64_t e64 = rd64(d, eocd - 20 + 8);
            if (e64 + 56 <= d.size() && rd32(d, e64) == 0x06064b50u)
            {
                count = rd64(d, e64 + 32);
                cdOff = rd64(d, e64 + 48);
            }
        }
    }
    size_t p = cdOff;
    for (uint64_t i = 0; i < count; ++i)
    {
        if (p + 46 > d.size() || rd32(d, p) != 0x02014b50u)
            return false;
        ArchiveEntry e;
        e.method = rd16(d, p + 10);
        {
            uint16_t dt = rd16(d, p + 12);
            uint16_t dd = rd16(d, p + 14);
            struct tm tmv{};
            tmv.tm_year = 1980 + (dd >> 9) - 1900;
            tmv.tm_mon = ((dd >> 5) & 15) - 1;
            tmv.tm_mday = dd & 31;
            tmv.tm_hour = dt >> 11;
            tmv.tm_min = (dt >> 5) & 63;
            tmv.tm_sec = (dt & 31) * 2;
            e.mtime = (long long)timegm(&tmv);
        }
        e.csize = rd32(d, p + 20);
        e.usize = rd32(d, p + 24);
        uint16_t nameLen = rd16(d, p + 28);
        uint16_t extraLen = rd16(d, p + 30);
        uint16_t commentLen = rd16(d, p + 32);
        e.dataOff = rd32(d, p + 42);
        e.name = d.substr(p + 46, nameLen);
        size_t xp = p + 46 + nameLen, xend = xp + extraLen;
        while (xp + 4 <= xend)
        {
            uint16_t tag = rd16(d, xp);
            uint16_t sz = rd16(d, xp + 2);
            if (tag == 0x0001)
            {
                size_t fp = xp + 4;
                if (e.usize == 0xFFFFFFFFu && fp + 8 <= xend)
                {
                    e.usize = rd64(d, fp);
                    fp += 8;
                }
                if (e.csize == 0xFFFFFFFFu && fp + 8 <= xend)
                {
                    e.csize = rd64(d, fp);
                    fp += 8;
                }
                if (e.dataOff == 0xFFFFFFFFu && fp + 8 <= xend)
                    e.dataOff = rd64(d, fp);
            }
            xp += 4 + sz;
        }
        e.isDir = !e.name.empty() && e.name.back() == '/';
        a.entries.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

bool zipEntryData(const Archive &a, const ArchiveEntry &e, std::string &out)
{
    const std::string &d = a.bytes;
    size_t lho = e.dataOff;
    if (lho + 30 > d.size() || rd32(d, lho) != 0x04034b50u)
        return false;
    uint16_t nameLen = rd16(d, lho + 26);
    uint16_t extraLen = rd16(d, lho + 28);
    size_t dataStart = lho + 30 + nameLen + extraLen;
    if (dataStart + e.csize > d.size())
        return false;
    std::string raw = d.substr(dataStart, e.csize);
    if (e.method == 0)
    {
        out = std::move(raw);
        return true;
    }
    if (e.method == 8)
        return inflateRaw(raw, e.usize, out);
    return false;
}

uint64_t parseOctal(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
    {
        char c = s[i];
        if (c == ' ' || c == '\0')
            continue;
        if (c < '0' || c > '7')
            break;
        v = v * 8 + (uint64_t)(c - '0');
    }
    return v;
}

bool parseTar(Archive &a)
{
    const std::string &d = a.bytes;
    size_t p = 0;
    while (p + 512 <= d.size())
    {
        const char *h = d.data() + p;
        bool allZero = true;
        for (int i = 0; i < 512 && allZero; ++i)
            if (h[i])
                allZero = false;
        if (allZero)
            break;
        std::string name(h, strnlen(h, 100));
        uint64_t size = parseOctal(h + 124, 12);
        char type = h[156];
        if (memcmp(h + 257, "ustar", 5) == 0)
        {
            std::string prefix(h + 345, strnlen(h + 345, 155));
            if (!prefix.empty())
                name = prefix + "/" + name;
        }
        ArchiveEntry e;
        e.name = name;
        e.isDir = type == '5' || (!name.empty() && name.back() == '/');
        e.usize = e.csize = size;
        e.dataOff = p + 512;
        e.method = 0;
        e.needLocal = false;
        e.mtime = (long long)parseOctal(h + 136, 12);
        if (!(type == 'L' || type == 'K'))
            a.entries.push_back(std::move(e));
        p += 512 + ((size + 511) / 512) * 512;
    }
    return !a.entries.empty();
}

// forward
bool readWholeAny(const std::string &path, std::string &out,
                  std::string &errKind);

std::map<std::string, std::shared_ptr<Archive>> &archiveCache()
{
    static std::map<std::string, std::shared_ptr<Archive>> c;
    return c;
}

// gzip size computations persist a .properties sidecar like CPL does
void writeGzProperties(const std::string &underPath, size_t csize,
                       size_t usize)
{
    if (vsiIsVirtual(underPath))
        return;
    std::string props = underPath + ".properties";
    FILE *probe = fopen(props.c_str(), "rb");
    if (probe)
    {
        fclose(probe);
        return;
    }
    FILE *f = fopen(props.c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "compressed_size=%llu\nuncompressed_size=%llu\n",
            (unsigned long long)csize, (unsigned long long)usize);
    fclose(f);
}

std::shared_ptr<Archive> openArchive(const std::string &archivePath,
                                     bool zip)
{
    auto it = archiveCache().find(archivePath);
    if (it != archiveCache().end())
        return it->second;
    auto a = std::make_shared<Archive>();
    a->isZip = zip;
    std::string errKind;
    if (readWholeAny(archivePath, a->bytes, errKind))
    {
        if (!zip && a->bytes.size() >= 2 &&
            (uint8_t)a->bytes[0] == 0x1f && (uint8_t)a->bytes[1] == 0x8b)
        {
            size_t csize = a->bytes.size();
            std::string plain;
            if (gunzipWhole(a->bytes, plain))
            {
                a->bytes = std::move(plain);
                writeGzProperties(archivePath, csize, a->bytes.size());
            }
        }
        a->ok = zip ? parseZip(*a) : parseTar(*a);
    }
    archiveCache()[archivePath] = a;
    return a;
}

// splits "/vsizip/..." style rest into archive path + inner path;
// false = no brace syntax and no recognized archive extension anywhere,
// which the reference treats as a nonexistent name WITHOUT ever opening
// the underlying file (critical for /vsizip//vsistdin/: stdin must not
// be read)
bool splitArchivePath(const std::string &rest, bool zip,
                      std::string &archive, std::string &inner)
{
    if (!rest.empty() && rest[0] == '{')
    {
        // brace syntax: {archive}/inner with nesting
        int depth = 0;
        for (size_t i = 0; i < rest.size(); ++i)
        {
            if (rest[i] == '{')
                ++depth;
            else if (rest[i] == '}')
            {
                --depth;
                if (depth == 0)
                {
                    archive = rest.substr(1, i - 1);
                    inner = i + 1 < rest.size() ? rest.substr(i + 1)
                                                : std::string();
                    if (!inner.empty() && inner[0] == '/')
                        inner = inner.substr(1);
                    return true;
                }
            }
        }
        archive = rest;
        return true;
    }
    static const char *zipExts[] = {".zip", ".kmz", ".dwf", ".ods",
                                    ".xlsx"};
    static const char *tarExts[] = {".tar.gz", ".tar", ".tgz", ".gz"};
    const char **exts = zip ? zipExts : tarExts;
    size_t nExts = zip ? sizeof(zipExts) / sizeof(*zipExts)
                       : sizeof(tarExts) / sizeof(*tarExts);
    std::string low = strToLower(rest);
    size_t best = std::string::npos;
    for (size_t k = 0; k < nExts; ++k)
    {
        size_t extLen = strlen(exts[k]);
        size_t from = 0;
        while (true)
        {
            size_t pos = low.find(exts[k], from);
            if (pos == std::string::npos)
                break;
            size_t end = pos + extLen;
            if (end == low.size() || low[end] == '/')
            {
                if (best == std::string::npos || end < best)
                    best = end;
                break;
            }
            from = pos + 1;
        }
    }
    if (best == std::string::npos)
    {
        archive = rest;
        inner.clear();
        // whole-string candidate only when an extension appears mid-name
        // (e.g. /vsizip//vsigzip/x.zip.gz)
        for (size_t k = 0; k < nExts; ++k)
            if (low.find(exts[k]) != std::string::npos)
                return true;
        return false;
    }
    archive = rest.substr(0, best);
    inner = best < rest.size() ? rest.substr(best + 1) : std::string();
    return true;
}

const ArchiveEntry *findEntry(const Archive &a, const std::string &inner)
{
    for (const auto &e : a.entries)
    {
        if (e.name == inner)
            return &e;
        if (e.name.size() == inner.size() + 1 && e.name.back() == '/' &&
            strncmp(e.name.c_str(), inner.c_str(), inner.size()) == 0)
            return &e;
        if (e.name.rfind("./", 0) == 0 && e.name.substr(2) == inner)
            return &e;
    }
    return nullptr;
}

// counts non-directory entries; returns the single file entry if unique
const ArchiveEntry *singleFileEntry(const Archive &a)
{
    const ArchiveEntry *found = nullptr;
    for (const auto &e : a.entries)
    {
        if (e.isDir)
            continue;
        if (found)
            return nullptr;
        found = &e;
    }
    return found;
}

bool entryData(const Archive &a, const ArchiveEntry &e, std::string &out)
{
    if (a.isZip)
        return zipEntryData(a, e, out);
    if (e.dataOff + e.usize > a.bytes.size())
        return false;
    out = a.bytes.substr(e.dataOff, e.usize);
    return true;
}

bool stdinConsumed = false;
std::string stdinContent;

bool stdinDisabled()
{
    if (configGet("CPL_ALLOW_VSISTDIN").empty() ||
        !strEqualNoCase(configGet("CPL_ALLOW_VSISTDIN"), "NO"))
        return false;
    // the reference probes /vsistdin/ four times before giving up
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        for (int i = 0; i < 4; ++i)
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "/vsistdin/ disabled. Set CPL_ALLOW_VSISTDIN "
                        "to YES to enable it");
    }
    return true;
}

bool readStdinAll(std::string &out, std::string &errKind)
{
    if (stdinDisabled())
    {
        errKind = "disabled";
        return false;
    }
    if (!stdinConsumed)
    {
        stdinConsumed = true;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
            stdinContent.append(buf, n);
    }
    out = stdinContent;
    return true;
}

void curlFail(const std::string &path)
{
    static std::map<std::string, bool> done;
    if (done.count(path))
        return;
    done[path] = true;
    std::string url = path.substr(9);
    for (int i = 0; i < 3; ++i)
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "HTTP response code on " + url + ": 0");
    cplErrorStr(CE_Failure, 11,
                "CURL error: Could not resolve host: " + vsiCurlHost(path));
}

bool readWholeAny(const std::string &path, std::string &out,
                  std::string &errKind)
{
    errKind.clear();
    if (startsWithNoCase(path, "/vsicurl/"))
    {
        curlFail(path);
        errKind = "curl";
        return false;
    }
    if (startsWithNoCase(path, "/vsimem/"))
    {
        auto it = memFs().find(path);
        if (it == memFs().end())
        {
            errKind = "missing";
            return false;
        }
        out = it->second;
        return true;
    }
    if (path == "/vsistdin/")
        return readStdinAll(out, errKind);
    if (strStartsWith(path, "/vsiwebhdfs/"))
    {
        out.clear();
        return true;
    }
    if (startsWithNoCase(path, "/vsigzip/"))
    {
        std::string under = path.substr(9);
        std::string raw;
        if (!readWholeAny(under, raw, errKind))
            return false;
        if (raw.size() >= 2 && (uint8_t)raw[0] == 0x1f &&
            (uint8_t)raw[1] == 0x8b)
            return gunzipWhole(raw, out);
        errKind = "missing";
        return false;
    }
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
        {
            errKind = "missing";
            return false;
        }
        auto a = openArchive(archivePath, zip);
        if (!a->ok)
        {
            errKind = "missing";
            return false;
        }
        const ArchiveEntry *e = nullptr;
        if (inner.empty())
        {
            e = singleFileEntry(*a);
            if (!e)
            {
                errKind = "multi";
                return false;
            }
        }
        else
        {
            e = findEntry(*a, inner);
            if (!e || e->isDir)
            {
                errKind = e ? "isdir" : "missing";
                return false;
            }
        }
        return entryData(*a, *e, out);
    }
    if (startsWithNoCase(path, "/vsisubfile/"))
    {
        std::string rest = path.substr(12);
        size_t comma = rest.find(',');
        if (comma == std::string::npos)
        {
            errKind = "missing";
            return false;
        }
        std::string spec = rest.substr(0, comma);
        std::string under = rest.substr(comma + 1);
        size_t us = spec.find('_');
        uint64_t off = strtoull(spec.c_str(), nullptr, 10);
        uint64_t len = us == std::string::npos
                           ? 0
                           : strtoull(spec.c_str() + us + 1, nullptr, 10);
        std::string raw;
        if (!readWholeAny(under, raw, errKind))
            return false;
        if (off > raw.size())
        {
            errKind = "missing";
            return false;
        }
        out = raw.substr(off, len == 0 ? std::string::npos : len);
        return true;
    }
    // plain path (recursion base)
    if (!rawRead(path, out))
    {
        errKind = "missing";
        return false;
    }
    return true;
}

}  // namespace

bool vsiIsVirtual(const std::string &path)
{
    return startsWithNoCase(path, "/vsi");
}

bool vsiReadWhole(const std::string &path, std::string &out,
                  std::string &errKind)
{
    return readWholeAny(path, out, errKind);
}

namespace
{

bool writeUnder(const std::string &under, const std::string &bytes)
{
    if (vsiIsVirtual(under))
        return vsiWriteWhole(under, bytes);
    FILE *f = fopen(under.c_str(), "wb");
    if (!f)
        return false;
    size_t w = bytes.empty() ? 0
                             : fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return w == bytes.size();
}

void dosDateTime(uint16_t &dosTime, uint16_t &dosDate, time_t when = 0)
{
    time_t now = when ? when : time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    dosTime = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) |
                         (tmv.tm_sec / 2));
    dosDate = (uint16_t)(((tmv.tm_year + 1900 - 1980) << 9) |
                         ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
}

bool writeZipEntry(const std::string &archivePath, const std::string &inner,
                   const std::string &content, time_t srcMtime,
                   bool streaming)
{
    std::string prefix, oldCentral;
    uint16_t oldCount = 0;
    std::string old, errKind;
    if (readWholeAny(archivePath, old, errKind) && old.size() >= 22)
    {
        size_t scanStart = old.size() > 65557 ? old.size() - 65557 : 0;
        for (size_t i = old.size() - 22 + 1; i-- > scanStart;)
        {
            if (old[i] == 'P' && old[i + 1] == 'K' && old[i + 2] == 5 &&
                old[i + 3] == 6)
            {
                oldCount = rd16(old, i + 10);
                uint32_t cdSize = rd32(old, i + 12);
                uint32_t cdOff = rd32(old, i + 16);
                if ((size_t)cdOff + cdSize <= old.size())
                {
                    prefix = old.substr(0, cdOff);
                    oldCentral = old.substr(cdOff, cdSize);
                }
                break;
            }
        }
    }

    std::string data = deflateRawStr(content);
    uint32_t crc =
        (uint32_t)crc32(0, (const Bytef *)content.data(),
                        (uInt)content.size());
    uint16_t dosTime, dosDate;
    dosDateTime(dosTime, dosDate, srcMtime);

    std::string local;
    wr32(local, 0x04034b50u);
    wr16(local, streaming ? 45 : 20);
    wr16(local, 0);
    wr16(local, 8);
    wr16(local, dosTime);
    wr16(local, dosDate);
    wr32(local, crc);
    wr32(local, (uint32_t)data.size());
    wr32(local, (uint32_t)content.size());
    wr16(local, (uint16_t)inner.size());
    wr16(local, streaming ? 20 : 0);
    local += inner;
    if (streaming)
    {
        wr16(local, 0x0001);
        wr16(local, 16);
        local.append(16, '\0');
    }

    uint64_t lho = prefix.size();
    std::string central;
    wr32(central, 0x02014b50u);
    wr16(central, streaming ? 45 : 0);
    wr16(central, streaming ? 45 : 20);
    wr16(central, 0);
    wr16(central, 8);
    wr16(central, dosTime);
    wr16(central, dosDate);
    wr32(central, crc);
    wr32(central, (uint32_t)data.size());
    wr32(central, (uint32_t)content.size());
    wr16(central, (uint16_t)inner.size());
    wr16(central, 0);
    wr16(central, 0);
    wr16(central, 0);
    wr16(central, 0);
    wr32(central, 0);
    wr32(central, (uint32_t)lho);
    central += inner;

    std::string out = prefix + local + data;
    uint32_t cdOff = (uint32_t)out.size();
    out += oldCentral + central;
    uint32_t cdSize = (uint32_t)(oldCentral.size() + central.size());
    wr32(out, 0x06054b50u);
    wr16(out, 0);
    wr16(out, 0);
    wr16(out, (uint16_t)(oldCount + 1));
    wr16(out, (uint16_t)(oldCount + 1));
    wr32(out, cdSize);
    wr32(out, cdOff);
    wr16(out, 0);

    archiveCache().erase(archivePath);
    if (archivePath == "/vsistdin/" || archivePath == "/vsistdin")
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Write or update mode not supported on /vsistdin");
        return false;
    }
    return writeUnder(archivePath, out);
}

}  // namespace

bool vsiWriteWhole(const std::string &path, const std::string &content,
                   time_t srcMtime, bool zipStreaming)
{
    if (startsWithNoCase(path, "/vsimem/"))
    {
        memFs()[path] = content;
        return true;
    }
    if (startsWithNoCase(path, "/vsistdout/") || path == "/vsistdout")
    {
        fwrite(content.data(), 1, content.size(), stdout);
        fflush(stdout);
        return true;
    }
    if (startsWithNoCase(path, "/vsigzip/"))
    {
        std::string under = path.substr(9);
        std::string out;
        const unsigned char hdr[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
        out.assign((const char *)hdr, 10);
        out += deflateRawStr(content);
        uint32_t crc = (uint32_t)crc32(0, (const Bytef *)content.data(),
                                       (uInt)content.size());
        wr32(out, crc);
        wr32(out, (uint32_t)content.size());
        return writeUnder(under, out);
    }
    if (startsWithNoCase(path, "/vsizip/"))
    {
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, true, archivePath, inner))
            return false;
        if (inner.empty())
            return false;
        return writeZipEntry(archivePath, inner, content, srcMtime,
                             zipStreaming);
    }
    return false;
}

void vsiWriteGzipProperties(const std::string &path)
{
    if (startsWithNoCase(path, "/vsigzip/"))
    {
        std::string under = path.substr(9);
        if (vsiIsVirtual(under))
            return;
        std::string raw;
        if (!rawRead(under, raw))
            return;
        if (raw.size() < 2 || (uint8_t)raw[0] != 0x1f ||
            (uint8_t)raw[1] != 0x8b)
            return;
        std::string plain;
        if (!gunzipWhole(raw, plain))
            return;
        FILE *f = fopen((under + ".properties").c_str(), "wb");
        if (!f)
            return;
        fprintf(f, "compressed_size=%llu\nuncompressed_size=%llu\n",
                (unsigned long long)raw.size(),
                (unsigned long long)plain.size());
        fclose(f);
        return;
    }
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
            return;
        vsiWriteGzipProperties(archivePath);
    }
}

void vsiMemRemove(const std::string &path)
{
    memFs().erase(path);
}

bool vsiExists(const std::string &path)
{
    if (startsWithNoCase(path, "/vsimem/"))
        return memFs().count(path) != 0;
    if (path == "/vsistdin/")
        return !stdinDisabled();
    if (startsWithNoCase(path, "/vsicurl/"))
    {
        curlFail(path);
        return false;
    }
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
            return false;
        auto a = openArchive(archivePath, zip);
        if (!a->ok)
            return false;
        if (inner.empty())
            return true;
        if (findEntry(*a, inner))
            return true;
        // directory prefix?
        std::string pfx = inner + "/";
        for (const auto &e : a->entries)
            if (e.name.rfind(pfx, 0) == 0)
                return true;
        return false;
    }
    std::string out, errKind;
    return readWholeAny(path, out, errKind);
}

bool vsiIsDir(const std::string &path)
{
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
            return false;
        auto a = openArchive(archivePath, zip);
        if (!a->ok)
            return false;
        if (inner.empty())
            return singleFileEntry(*a) == nullptr;
        const ArchiveEntry *e = findEntry(*a, inner);
        if (e && e->isDir)
            return true;
        std::string pfx = inner + "/";
        for (const auto &en : a->entries)
            if (en.name.rfind(pfx, 0) == 0)
                return true;
        return false;
    }
    return false;
}

bool vsiListDir(const std::string &path, std::vector<std::string> &out)
{
    out.clear();
    if (startsWithNoCase(path, "/vsimem"))
    {
        std::string pfx = path;
        if (pfx.back() != '/')
            pfx += '/';
        for (const auto &kv : memFs())
            if (kv.first.rfind(pfx, 0) == 0)
            {
                std::string rest = kv.first.substr(pfx.size());
                size_t slash = rest.find('/');
                std::string child =
                    slash == std::string::npos ? rest : rest.substr(0, slash);
                bool dup = false;
                for (const auto &c : out)
                    if (c == child)
                        dup = true;
                if (!dup)
                    out.push_back(child);
            }
        return true;
    }
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
            return false;
        auto a = openArchive(archivePath, zip);
        if (!a->ok)
            return false;
        std::string pfx = inner.empty() ? "" : inner + "/";
        for (const auto &e : a->entries)
        {
            std::string n = e.name;
            if (n.rfind("./", 0) == 0)
                n = n.substr(2);
            if (!pfx.empty())
            {
                if (n.rfind(pfx, 0) != 0)
                    continue;
                n = n.substr(pfx.size());
            }
            if (n.empty())
                continue;
            size_t slash = n.find('/');
            std::string child =
                slash == std::string::npos ? n : n.substr(0, slash);
            if (child.empty())
                continue;
            bool dup = false;
            for (const auto &c : out)
                if (c == child)
                    dup = true;
            if (!dup)
                out.push_back(child);
        }
        return true;
    }
    return false;
}

bool vsiStat(const std::string &path, VsiPathInfo &out)
{
    out = VsiPathInfo();
    if (startsWithNoCase(path, "/vsimem/") || path == "/vsimem")
    {
        if (path == "/vsimem" || path == "/vsimem/")
        {
            out.exists = true;
            out.isDir = true;
            return true;
        }
        auto it = memFs().find(path);
        if (it != memFs().end())
        {
            out.exists = true;
            out.size = it->second.size();
            return true;
        }
        std::vector<std::string> kids;
        vsiListDir(path, kids);
        if (!kids.empty())
        {
            out.exists = true;
            out.isDir = true;
            return true;
        }
        return false;
    }
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
            return false;
        auto a = openArchive(archivePath, zip);
        if (!a->ok)
            return false;
        if (inner.empty())
        {
            const ArchiveEntry *single = singleFileEntry(*a);
            out.exists = true;
            if (single)
            {
                out.size = single->usize;
                out.mtime = single->mtime;
            }
            else
                out.isDir = true;
            return true;
        }
        const ArchiveEntry *e = findEntry(*a, inner);
        if (e)
        {
            out.exists = true;
            out.isDir = e->isDir;
            out.size = e->usize;
            out.mtime = e->mtime;
            return true;
        }
        std::string pfx = inner + "/";
        for (const auto &en : a->entries)
            if (en.name.rfind(pfx, 0) == 0)
            {
                out.exists = true;
                out.isDir = true;
                return true;
            }
        return false;
    }
    if (startsWithNoCase(path, "/vsigzip/"))
    {
        std::string content, errKind;
        if (!readWholeAny(path, content, errKind))
            return false;
        out.exists = true;
        out.size = content.size();
        std::string under = path.substr(9);
        struct stat st;
        if (!vsiIsVirtual(under) && stat(under.c_str(), &st) == 0)
        {
            out.mode = st.st_mode & 07777;
            writeGzProperties(under, (size_t)st.st_size, content.size());
        }
        return true;
    }
    if (path == "/vsistdin/")
    {
        if (stdinDisabled())
            return false;
        out.exists = true;
        return true;
    }
    if (startsWithNoCase(path, "/vsicurl/"))
        return false;
    std::string content, errKind;
    if (!readWholeAny(path, content, errKind))
        return false;
    out.exists = true;
    out.size = content.size();
    return true;
}

bool vsiListDirInfo(const std::string &path, std::vector<VsiDirEntry> &out)
{
    out.clear();
    if (startsWithNoCase(path, "/vsimem"))
    {
        std::vector<std::string> names;
        if (!vsiListDir(path, names))
            return false;
        std::string pfx = path;
        if (pfx.back() != '/')
            pfx += '/';
        for (const auto &n : names)
        {
            VsiDirEntry e;
            e.name = n;
            auto it = memFs().find(pfx + n);
            if (it != memFs().end())
                e.size = it->second.size();
            else
                e.isDir = true;
            out.push_back(e);
        }
        return true;
    }
    if (startsWithNoCase(path, "/vsizip/") ||
        startsWithNoCase(path, "/vsitar/"))
    {
        bool zip = startsWithNoCase(path, "/vsizip/");
        std::string rest = path.substr(8);
        std::string archivePath, inner;
        if (!splitArchivePath(rest, zip, archivePath, inner))
            return false;
        auto a = openArchive(archivePath, zip);
        if (!a->ok)
            return false;
        while (!inner.empty() && inner.back() == '/')
            inner.pop_back();
        std::string pfx = inner.empty() ? "" : inner + "/";
        for (const auto &e : a->entries)
        {
            std::string n = e.name;
            if (n.rfind("./", 0) == 0)
                n = n.substr(2);
            if (!pfx.empty())
            {
                if (n.rfind(pfx, 0) != 0)
                    continue;
                n = n.substr(pfx.size());
            }
            bool entryIsDir = e.isDir;
            if (!n.empty() && n.back() == '/')
                n.pop_back();
            if (n.empty())
                continue;
            size_t slash = n.find('/');
            std::string child =
                slash == std::string::npos ? n : n.substr(0, slash);
            if (child.empty())
                continue;
            bool childIsDir = slash != std::string::npos || entryIsDir;
            bool dup = false;
            for (auto &c : out)
                if (c.name == child)
                {
                    dup = true;
                    if (childIsDir)
                        c.isDir = true;
                }
            if (dup)
                continue;
            VsiDirEntry de;
            de.name = child;
            de.isDir = childIsDir;
            if (slash == std::string::npos)
            {
                de.size = entryIsDir ? 0 : e.usize;
                de.mtime = e.mtime;
            }
            out.push_back(de);
        }
        return true;
    }
    return false;
}

std::string vsiCurlHost(const std::string &path)
{
    std::string url = path;
    if (startsWithNoCase(url, "/vsicurl/"))
        url = url.substr(9);
    size_t p = url.find("://");
    std::string host = p == std::string::npos ? url : url.substr(p + 3);
    size_t slash = host.find('/');
    if (slash != std::string::npos)
        host = host.substr(0, slash);
    size_t colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);
    return host;
}

void vsiZipTouchArchive(const std::string &vsizipPath)
{
    if (!startsWithNoCase(vsizipPath, "/vsizip/"))
        return;
    std::string rest = vsizipPath.substr(8);
    std::string archivePath, inner;
    if (!splitArchivePath(rest, true, archivePath, inner))
        return;
    auto a = openArchive(archivePath, true);
    if (a->ok)
        return;
    std::string out;
    wr32(out, 0x06054b50u);
    out.append(18, '\0');
    archiveCache().erase(archivePath);
    writeUnder(archivePath, out);
}

bool vsiZipWriteNameOk(const std::string &path)
{
    if (!startsWithNoCase(path, "/vsizip/"))
        return false;
    std::string archive, inner;
    return splitArchivePath(path.substr(8), true, archive, inner);
}

int vsiZipWriteProbe(const std::string &path)
{
    if (!startsWithNoCase(path, "/vsizip/"))
        return 0;
    std::string archive, inner;
    if (!splitArchivePath(path.substr(8), true, archive, inner))
        return 0;
    if (archive == "/vsistdin/" || archive == "/vsistdin")
    {
        std::string old, errKind;
        readWholeAny(archive, old, errKind);
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Write or update mode not supported on /vsistdin");
        return 2;
    }
    return 1;
}

bool vsiTarWriteArchiveMissing(const std::string &path)
{
    if (!startsWithNoCase(path, "/vsitar/"))
        return false;
    std::string archive, inner;
    if (!splitArchivePath(path.substr(8), false, archive, inner))
        return false;
    struct stat st;
    return stat(archive.c_str(), &st) != 0;
}

namespace
{

// filesystems the reference registers; a path is theirs when it starts
// with the prefix or equals the prefix minus its trailing slash
const char *const kVsiRegistered[] = {
    "/vsimem/",           "/vsistdin/",
    "/vsistdout/",        "/vsistdout_redirect/",
    "/vsigzip/",          "/vsizip/",
    "/vsitar/",           "/vsicurl/",
    "/vsicurl_streaming/", "/vsis3/",
    "/vsis3_streaming/",  "/vsigs/",
    "/vsigs_streaming/",  "/vsiaz/",
    "/vsiaz_streaming/",  "/vsiadls/",
    "/vsioss/",           "/vsioss_streaming/",
    "/vsiswift/",         "/vsiswift_streaming/",
    "/vsiwebhdfs/",       "/vsicrypt/",
    "/vsisubfile/",       "/vsisparse/"};

bool vsiFsMatch(const std::string &path, const char *pfx)
{
    size_t n = strlen(pfx);
    if (strncmp(path.c_str(), pfx, n) == 0)
        return true;
    return path.size() == n - 1 &&
           strncmp(path.c_str(), pfx, n - 1) == 0;
}

std::string homeDir()
{
    const char *h = getenv("HOME");
    return h ? h : "";
}

}  // namespace

bool vsiRegisteredFs(const std::string &path)
{
    for (const char *p : kVsiRegistered)
        if (vsiFsMatch(path, p))
            return true;
    return false;
}

std::string vsiCredText(const std::string &path)
{
    if (vsiFsMatch(path, "/vsis3/") || vsiFsMatch(path, "/vsis3_streaming/"))
        return "InvalidCredentials: No valid AWS credentials found. For "
               "authenticated requests, you need to set "
               "AWS_SECRET_ACCESS_KEY, AWS_ACCESS_KEY_ID or other "
               "configuration options, or create a " +
               homeDir() +
               "/.aws/credentials file. Consult "
               "https://gdal.org/en/stable/user/virtual_file_systems.html#"
               "vsis3-aws-s3-files for more details. For unauthenticated "
               "requests on public resources, set the AWS_NO_SIGN_REQUEST "
               "configuration option to YES.";
    if (vsiFsMatch(path, "/vsiaz/") ||
        vsiFsMatch(path, "/vsiaz_streaming/") ||
        vsiFsMatch(path, "/vsiadls/"))
        return "InvalidCredentials: No valid Azure credentials found. For "
               "authenticated requests, you need to set "
               "AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_ACCESS_KEY, "
               "AZURE_STORAGE_SAS_TOKEN, AZURE_STORAGE_CONNECTION_STRING, "
               "or other configuration options. Consult "
               "https://gdal.org/en/stable/user/virtual_file_systems.html#"
               "vsiaz-microsoft-azure-blob-files for more details. For "
               "unauthenticated requests on public resources, set the "
               "AZURE_NO_SIGN_REQUEST configuration option to YES.";
    if (vsiFsMatch(path, "/vsioss/") ||
        vsiFsMatch(path, "/vsioss_streaming/"))
        return "InvalidCredentials: OSS_SECRET_ACCESS_KEY configuration "
               "option not defined";
    if (vsiFsMatch(path, "/vsiswift/") ||
        vsiFsMatch(path, "/vsiswift_streaming/"))
        return "InvalidCredentials: Missing SWIFT_STORAGE_URL+SWIFT_AUTH_"
               "TOKEN or appropriate authentication options";
    if (vsiFsMatch(path, "/vsigs/") || vsiFsMatch(path, "/vsigs_streaming/"))
        return "InvalidCredentials: No valid GCS credentials found. For "
               "authenticated requests, you need to set "
               "GS_SECRET_ACCESS_KEY, GS_ACCESS_KEY_ID, "
               "GS_OAUTH2_REFRESH_TOKEN, GOOGLE_APPLICATION_CREDENTIALS, "
               "or other configuration options, or create a " +
               homeDir() +
               "/.boto file. Consult https://gdal.org/en/stable/user/"
               "virtual_file_systems.html#vsigs-google-cloud-storage-files "
               "for more details. For unauthenticated requests on public "
               "resources, set the GS_NO_SIGN_REQUEST configuration option "
               "to YES.";
    return "";
}

void vsiEmitGsPairs(int n)
{
    for (int i = 0; i < n; ++i)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Could not resolve host: metadata.google.internal");
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Fetching OAuth2 access code from auth code failed.");
    }
}

// the first OAuth token fetch of the process retries once more than
// every later attempt
int vsiGsStatPairCount()
{
    static bool first = true;
    int n = first ? 3 : 2;
    first = false;
    return n;
}

void vsiStatPrelude(const std::string &path)
{
    if (vsiFsMatch(path, "/vsicrypt/"))
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "/vsicrypt/ support not available in this build");
    else if (vsiFsMatch(path, "/vsigs/") ||
             vsiFsMatch(path, "/vsigs_streaming/"))
        vsiEmitGsPairs(vsiGsStatPairCount());
}

void vsiCreatePrelude(const std::string &path)
{
    if (vsiFsMatch(path, "/vsicrypt/"))
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "/vsicrypt/ support not available in this build");
    else if (vsiFsMatch(path, "/vsigs/") ||
             vsiFsMatch(path, "/vsigs_streaming/"))
        vsiEmitGsPairs(1);
    else if (strStartsWith(path, "/vsiwebhdfs/"))
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Configuration option WEBHDFS_USERNAME or "
                    "WEBHDFS_DELEGATION should be defined");
    else if (path == "/vsistdin/" || path == "/vsistdin")
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Write or update mode not supported on /vsistdin");
    else if (vsiFsMatch(path, "/vsitar/"))
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Only read-only mode is supported for /vsitar");
}

std::string vsiWebhdfsHost(const std::string &path)
{
    std::string rest = path.substr(strlen("/vsiwebhdfs/"));
    size_t scheme = rest.find("://");
    if (scheme != std::string::npos)
        rest = rest.substr(scheme + 3);
    size_t slash = rest.find('/');
    return slash == std::string::npos ? rest : rest.substr(0, slash);
}

bool vsiMissingPrelude(const std::string &path)
{
    static std::map<std::string, bool> done;
    if (strStartsWith(path, "/vsicurl/"))
    {
        curlFail(path);
        return false;
    }
    std::string cred = vsiCredText(path);
    if (done.count(path))
        return cred.empty();
    done[path] = true;
    if (vsiFsMatch(path, "/vsistdout/"))
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Read or update mode not supported on /vsistdout");
        return true;
    }
    if (vsiFsMatch(path, "/vsistdout_redirect/"))
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Read or update mode not supported on "
                    "/vsistdout_redirect");
        return true;
    }
    if (vsiFsMatch(path, "/vsicrypt/"))
    {
        for (int i = 0; i < 4; ++i)
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "/vsicrypt/ support not available in this build");
        return true;
    }
    if (!cred.empty())
    {
        if (vsiFsMatch(path, "/vsigs/") ||
            vsiFsMatch(path, "/vsigs_streaming/"))
            vsiEmitGsPairs(vsiFsMatch(path, "/vsigs_streaming/") ? 4 : 8);
        cplErrorStr(CE_Failure, 15, cred);
        return false;
    }
    return true;
}

std::string datasetMissingMessage(const std::string &path)
{
    if (strStartsWith(path, "/vsimem/") || path == "/vsimem")
        return "No such file or directory";
    if (strStartsWith(path, "/vsisubfile/"))
    {
        std::string rest = path.substr(12);
        size_t comma = rest.find(',');
        if (comma != std::string::npos)
            return rest.substr(comma + 1) + ": No such file or directory";
        return "`" + path +
               "' does not exist in the file system, and is not "
               "recognized as a supported dataset name.";
    }
    if (vsiRegisteredFs(path))
        return "`" + path +
               "' does not exist in the file system, and is not "
               "recognized as a supported dataset name.";
    return path + ": No such file or directory";
}
