// gdal vsi sozip create/list/validate/optimize: SOZip archives over a
// minimal in-memory zip writer/reader calibrated against the reference.

#include "cpl.h"
#include "engine.h"
#include "progress.h"
#include "util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <ctime>
#include <set>
#include <string>
#include <vector>

extern "C"
{
    struct libdeflate_decompressor;
    struct libdeflate_decompressor *libdeflate_alloc_decompressor(void);
    int libdeflate_deflate_decompress(struct libdeflate_decompressor *,
                                      const void *in, size_t in_nbytes,
                                      void *out, size_t out_nbytes_avail,
                                      size_t *actual_out_nbytes_ret);
    void libdeflate_free_decompressor(struct libdeflate_decompressor *);
}

namespace
{

void zw16(std::string &s, uint16_t v)
{
    s.push_back((char)(v & 0xff));
    s.push_back((char)(v >> 8));
}

void zw32(std::string &s, uint32_t v)
{
    zw16(s, (uint16_t)(v & 0xffff));
    zw16(s, (uint16_t)(v >> 16));
}

void zw64(std::string &s, uint64_t v)
{
    zw32(s, (uint32_t)(v & 0xffffffffu));
    zw32(s, (uint32_t)(v >> 32));
}

uint16_t zr16(const std::string &s, size_t p)
{
    return (uint16_t)((uint8_t)s[p] | ((uint8_t)s[p + 1] << 8));
}

uint32_t zr32(const std::string &s, size_t p)
{
    return (uint32_t)zr16(s, p) | ((uint32_t)zr16(s, p + 2) << 16);
}

uint64_t zr64(const std::string &s, size_t p)
{
    return (uint64_t)zr32(s, p) | ((uint64_t)zr32(s, p + 4) << 32);
}

// minizip's DOS timestamp rule: raw tm_year kept when outside both the
// >=1980 and >=80 windows, which is how pre-1980 mtimes land in 2055+
void sozDosTime(time_t when, uint16_t &dtime, uint16_t &ddate)
{
    struct tm tmv;
    localtime_r(&when, &tmv);
    unsigned year = (unsigned)tmv.tm_year;
    if (year >= 1980)
        year -= 1980;
    else if (year >= 80)
        year -= 80;
    ddate = (uint16_t)(((year & 0x7f) << 9) | ((tmv.tm_mon + 1) << 5) |
                       tmv.tm_mday);
    dtime = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) |
                       (tmv.tm_sec / 2));
}

uint64_t sozParseSize(const std::string &s, uint64_t def)
{
    if (s.empty())
        return def;
    char *end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (end && *end)
    {
        char c = *end;
        if (c == 'k' || c == 'K')
            v *= 1024LL;
        else if (c == 'm' || c == 'M')
            v *= 1024LL * 1024;
        else if (c == 'g' || c == 'G')
            v *= 1024LL * 1024 * 1024;
    }
    if (v < 0)
        v = 0;
    return (uint64_t)v;
}

struct SzEntry
{
    std::string name;
    uint16_t mth = 0, dtime = 0, ddate = 0;
    uint32_t crc = 0;
    uint64_t cs = 0, us = 0, lho = 0;
    std::string extra;
};

struct SzZip
{
    std::string raw;
    std::vector<SzEntry> es;
    uint64_t cdOff = 0, cdSize = 0;
    uint16_t count = 0;
};

bool szFindEocd(const std::string &raw, size_t &at)
{
    if (raw.size() < 22)
        return false;
    size_t scanStart = raw.size() > 65557 ? raw.size() - 65557 : 0;
    for (size_t i = raw.size() - 22 + 1; i-- > scanStart;)
    {
        if (raw[i] == 'P' && raw[i + 1] == 'K' && raw[i + 2] == 5 &&
            raw[i + 3] == 6)
        {
            at = i;
            return true;
        }
    }
    return false;
}

bool szOpen(const std::string &path, SzZip &z, bool requireEntries)
{
    if (!readFileToString(path, z.raw))
        return false;
    size_t eo = 0;
    if (!szFindEocd(z.raw, eo))
        return false;
    z.count = zr16(z.raw, eo + 10);
    z.cdSize = zr32(z.raw, eo + 12);
    z.cdOff = zr32(z.raw, eo + 16);
    if (requireEntries && z.count == 0)
        return false;
    if (z.cdOff + z.cdSize > z.raw.size())
        return false;
    size_t p = z.cdOff;
    for (uint16_t i = 0; i < z.count; ++i)
    {
        if (p + 46 > z.raw.size() || zr32(z.raw, p) != 0x02014b50u)
            return false;
        SzEntry e;
        e.mth = zr16(z.raw, p + 10);
        e.dtime = zr16(z.raw, p + 12);
        e.ddate = zr16(z.raw, p + 14);
        e.crc = zr32(z.raw, p + 16);
        e.cs = zr32(z.raw, p + 20);
        e.us = zr32(z.raw, p + 24);
        uint16_t nl = zr16(z.raw, p + 28);
        uint16_t el = zr16(z.raw, p + 30);
        uint16_t cl = zr16(z.raw, p + 32);
        e.lho = zr32(z.raw, p + 42);
        if (p + 46 + nl + el + cl > z.raw.size())
            return false;
        e.name = z.raw.substr(p + 46, nl);
        e.extra = z.raw.substr(p + 46 + nl, el);
        z.es.push_back(e);
        p += 46 + nl + el + cl;
    }
    return true;
}

bool szHidden(const std::string &name)
{
    if (!name.empty() && name.back() == '/')
        return true;
    if (name.size() >= 10 &&
        name.compare(name.size() - 10, 10, ".sozip.idx") == 0)
        return true;
    size_t start = 0;
    while (start <= name.size())
    {
        size_t sl = name.find('/', start);
        std::string comp = name.substr(
            start, sl == std::string::npos ? std::string::npos : sl - start);
        if (comp == "..")
            return true;
        if (sl == std::string::npos)
            break;
        start = sl + 1;
    }
    return false;
}

// data start of the entry's local record; false when the header is broken
bool szLocalData(const SzZip &z, const SzEntry &e, uint64_t &dataStart)
{
    if (e.lho + 30 > z.raw.size() || zr32(z.raw, e.lho) != 0x04034b50u)
        return false;
    uint16_t nl = zr16(z.raw, e.lho + 26);
    uint16_t el = zr16(z.raw, e.lho + 28);
    dataStart = e.lho + 30 + nl + el;
    return dataStart <= z.raw.size();
}

struct SzIdx
{
    uint32_t chunk = 0;
    uint64_t us = 0, cs = 0;
    std::vector<uint64_t> offsets;
};

std::string szIdxName(const std::string &name)
{
    size_t sl = name.find('/');
    if (sl == std::string::npos)
        return "." + name + ".sozip.idx";
    return name.substr(0, sl + 1) + "." + name.substr(sl + 1) +
           ".sozip.idx";
}

bool szFindIdx(const SzZip &z, const SzEntry &e, SzIdx &idx)
{
    uint64_t dataStart = 0;
    if (!szLocalData(z, e, dataStart))
        return false;
    uint64_t after = dataStart + e.cs;
    if (after + 30 > z.raw.size() || zr32(z.raw, after) != 0x04034b50u)
        return false;
    uint16_t nl = zr16(z.raw, after + 26);
    uint16_t el = zr16(z.raw, after + 28);
    if (after + 30 + nl + el > z.raw.size())
        return false;
    if (z.raw.substr(after + 30, nl) != szIdxName(e.name))
        return false;
    uint64_t payloadAt = after + 30 + nl + el;
    uint64_t payloadSize = zr32(z.raw, after + 22);  // uncompressed size
    if (payloadAt + payloadSize > z.raw.size() || payloadSize < 32)
        return false;
    if (zr32(z.raw, payloadAt) != 1)
        return false;
    idx.chunk = zr32(z.raw, payloadAt + 8);
    idx.us = zr64(z.raw, payloadAt + 16);
    idx.cs = zr64(z.raw, payloadAt + 24);
    size_t n = (size_t)((payloadSize - 32) / 8);
    idx.offsets.clear();
    for (size_t i = 0; i < n; ++i)
        idx.offsets.push_back(zr64(z.raw, payloadAt + 32 + 8 * i));
    return true;
}

std::string szKvExtra(const std::string &contentType)
{
    if (contentType.empty())
        return "";
    std::string kv = "KeyValuePairs";
    kv.push_back((char)1);
    zw16(kv, (uint16_t)strlen("Content-Type"));
    kv += "Content-Type";
    zw16(kv, (uint16_t)contentType.size());
    kv += contentType;
    std::string extra;
    zw16(extra, 0x564B);
    zw16(extra, (uint16_t)kv.size());
    extra += kv;
    return extra;
}

std::string szKvProps(const std::string &extra)
{
    std::string props;
    size_t p = 0;
    while (p + 4 <= extra.size())
    {
        uint16_t tag = zr16(extra, p);
        uint16_t len = zr16(extra, p + 2);
        if (p + 4 + len > extra.size())
            break;
        if (tag == 0x564B && len >= 14 &&
            extra.compare(p + 4, 13, "KeyValuePairs") == 0)
        {
            size_t q = p + 4 + 13;
            unsigned n = (uint8_t)extra[q];
            ++q;
            for (unsigned i = 0; i < n && q + 2 <= p + 4 + len; ++i)
            {
                uint16_t kl = zr16(extra, q);
                q += 2;
                std::string key = extra.substr(q, kl);
                q += kl;
                if (q + 2 > p + 4 + len)
                    break;
                uint16_t vl = zr16(extra, q);
                q += 2;
                std::string val = extra.substr(q, vl);
                q += vl;
                if (!props.empty())
                    props += ", ";
                props += key + "=" + val;
            }
        }
        p += 4 + (size_t)len;
    }
    return props;
}

std::string szDeflateWhole(const std::string &in)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK)
        return "";
    std::string out;
    out.resize(deflateBound(&zs, (uLong)in.size()) + 16);
    zs.next_in = (Bytef *)in.data();
    zs.avail_in = (uInt)in.size();
    zs.next_out = (Bytef *)&out[0];
    zs.avail_out = (uInt)out.size();
    deflate(&zs, Z_FINISH);
    out.resize(out.size() - zs.avail_out);
    deflateEnd(&zs);
    return out;
}

// per-chunk deflate: every full chunk is a fresh Z_FULL_FLUSH stream plus
// a manual empty stored block, the remainder (possibly empty) a fresh
// Z_FINISH stream; offsets land after each full chunk except a trailing
// one that closes the file exactly
std::string szDeflateChunked(const std::string &in, uint64_t chunk,
                             std::vector<uint64_t> &offsets,
                             const std::function<void(uint64_t)> &tick)
{
    offsets.clear();
    std::string out;
    uint64_t size = in.size();
    uint64_t full = size / chunk;
    uint64_t rem = size - full * chunk;
    for (uint64_t k = 0; k < full; ++k)
    {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY);
        size_t base = out.size();
        size_t cap = deflateBound(&zs, (uLong)chunk) + 32;
        out.resize(base + cap);
        zs.next_in = (Bytef *)in.data() + k * chunk;
        zs.avail_in = (uInt)chunk;
        zs.next_out = (Bytef *)&out[base];
        zs.avail_out = (uInt)cap;
        deflate(&zs, Z_FULL_FLUSH);
        out.resize(base + (cap - zs.avail_out));
        deflateEnd(&zs);
        out.append("\x00\x00\x00\xff\xff", 5);
        const bool lastChunk = (rem == 0 && k + 1 == full);
        if (!lastChunk)
            offsets.push_back(out.size());
        if (tick)
            tick((k + 1) * chunk);
    }
    {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY);
        size_t base = out.size();
        size_t cap = deflateBound(&zs, (uLong)rem) + 32;
        out.resize(base + cap);
        zs.next_in = (Bytef *)in.data() + full * chunk;
        zs.avail_in = (uInt)rem;
        zs.next_out = (Bytef *)&out[base];
        zs.avail_out = (uInt)cap;
        deflate(&zs, Z_FINISH);
        out.resize(base + (cap - zs.avail_out));
        deflateEnd(&zs);
        if (rem && tick)
            tick(size);
    }
    return out;
}

bool szInflateRaw(const char *in, size_t inLen, std::string &out,
                  uint64_t expect)
{
    out.assign((size_t)expect, '\0');
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK)
        return false;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inLen;
    zs.next_out = (Bytef *)(expect ? &out[0] : (char *)&zs);
    zs.avail_out = (uInt)expect;
    int ret = inflate(&zs, Z_FINISH);
    bool ok = (ret == Z_STREAM_END || ret == Z_OK || ret == Z_BUF_ERROR) &&
              zs.avail_out == 0;
    inflateEnd(&zs);
    return ok;
}

std::string szNormalizeStored(const std::string &path, bool junk)
{
    if (junk)
    {
        size_t sl = path.find_last_of('/');
        return sl == std::string::npos ? path : path.substr(sl + 1);
    }
    std::string s = path;
    for (;;)
    {
        if (s.compare(0, 2, "./") == 0)
            s = s.substr(2);
        else if (!s.empty() && s[0] == '/')
            s = s.substr(1);
        else
            break;
    }
    return s;
}

bool szExtIsZip(const std::string &path)
{
    size_t sl = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos ||
        (sl != std::string::npos && dot < sl))
        return false;
    return strEqualNoCase(path.substr(dot + 1), "zip");
}

struct SozItem
{
    std::string display;
    std::string stored;
    bool isDir = false;
    bool fromMemory = false;
    std::string path;     // disk items
    std::string content;  // memory items
    uint16_t dtime = 0, ddate = 0;
    bool dosSet = false;
    uint64_t size = 0;
};

void sozExpandDir(const std::string &dir, std::vector<SozItem> &items)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    std::vector<std::string> names;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        names.push_back(de->d_name);
    }
    closedir(d);
    for (const auto &n : names)
    {
        std::string full = dir + "/" + n;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            sozExpandDir(full, items);
        else
        {
            SozItem it;
            it.display = full;
            it.path = full;
            it.size = (uint64_t)st.st_size;
            items.push_back(it);
        }
    }
}

struct SozOpts
{
    bool quiet = false;
    int enable = 0;  // 0 auto / 1 yes / 2 no
    uint64_t chunk = 32768;
    uint64_t minSize = 1048576;
    std::string contentType;
};

int sozAddLoop(const std::string &word, std::vector<SozItem> &items,
               const std::string &output, const std::string &prefix,
               const std::string &oldCentral, uint16_t oldCount,
               std::set<std::string> names, const SozOpts &opts)
{
    FILE *f = fopen(output.c_str(), "wb");
    if (!f)
        return 1;
    std::string body = prefix;
    std::string central = oldCentral;
    uint16_t count = oldCount;
    uint64_t total = 0;
    for (const auto &it : items)
        total += it.size;
    TermProgress bar;
    uint64_t cum = 0;
    int rc = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        SozItem &it = items[i];
        if (!opts.quiet)
        {
            printf("Adding %s... (%zu/%zu)\n", it.display.c_str(), i + 1,
                   items.size());
            fflush(stdout);
        }
        if (it.isDir)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        word + ": " + it.display + " is a directory");
            rc = 1;
            break;
        }
        std::string content;
        uint16_t dtime = it.dtime, ddate = it.ddate;
        if (it.fromMemory)
            content.swap(it.content);
        else
        {
            struct stat st;
            if (stat(it.path.c_str(), &st) == 0 && !it.dosSet)
                sozDosTime(st.st_mtime, dtime, ddate);
            readFileToString(it.path, content);
        }
        if (names.count(it.stored))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        it.stored + " already exists in ZIP file");
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        word + ": Failed adding " + it.display);
            rc = 1;
            break;
        }
        const uint64_t size = content.size();
        const bool eligible =
            opts.enable != 2 && size > opts.chunk &&
            (opts.enable == 1 || size > opts.minSize);
        uint32_t crc = (uint32_t)crc32(
            0, (const Bytef *)content.data(), (uInt)content.size());
        std::string data;
        std::vector<uint64_t> offsets;
        if (eligible)
        {
            auto tick = [&](uint64_t done)
            {
                if (!opts.quiet && total > 0)
                    bar.update((double)(cum + done) / (double)total);
            };
            data = szDeflateChunked(content, opts.chunk, offsets, tick);
        }
        else
            data = szDeflateWhole(content);
        std::string extra = szKvExtra(opts.contentType);
        uint64_t lho = body.size();
        std::string local;
        zw32(local, 0x04034b50u);
        zw16(local, 20);
        zw16(local, 0);
        zw16(local, 8);
        zw16(local, dtime);
        zw16(local, ddate);
        zw32(local, crc);
        zw32(local, (uint32_t)data.size());
        zw32(local, (uint32_t)size);
        zw16(local, (uint16_t)it.stored.size());
        zw16(local, (uint16_t)extra.size());
        local += it.stored;
        local += extra;
        body += local;
        body += data;
        if (eligible)
        {
            std::string payload;
            zw32(payload, 1);
            zw32(payload, 0);
            zw32(payload, (uint32_t)opts.chunk);
            zw32(payload, 8);
            zw64(payload, size);
            zw64(payload, data.size());
            for (uint64_t off : offsets)
                zw64(payload, off);
            std::string idxName = szIdxName(it.stored);
            uint32_t icrc = (uint32_t)crc32(
                0, (const Bytef *)payload.data(), (uInt)payload.size());
            std::string ih;
            zw32(ih, 0x04034b50u);
            zw16(ih, 20);
            zw16(ih, 0);
            zw16(ih, 0);
            zw16(ih, dtime);
            zw16(ih, ddate);
            zw32(ih, icrc);
            zw32(ih, (uint32_t)payload.size());
            zw32(ih, (uint32_t)payload.size());
            zw16(ih, (uint16_t)idxName.size());
            zw16(ih, 0);
            ih += idxName;
            body += ih;
            body += payload;
        }
        std::string cd;
        zw32(cd, 0x02014b50u);
        zw16(cd, 0);
        zw16(cd, 20);
        zw16(cd, 0);
        zw16(cd, 8);
        zw16(cd, dtime);
        zw16(cd, ddate);
        zw32(cd, crc);
        zw32(cd, (uint32_t)data.size());
        zw32(cd, (uint32_t)size);
        zw16(cd, (uint16_t)it.stored.size());
        zw16(cd, (uint16_t)extra.size());
        zw16(cd, 0);
        zw16(cd, 0);
        zw16(cd, 0);
        zw32(cd, 0);
        zw32(cd, (uint32_t)lho);
        cd += it.stored;
        cd += extra;
        central += cd;
        ++count;
        names.insert(it.stored);
        cum += size;
        if (!opts.quiet && total > 0)
            bar.update((double)cum / (double)total);
    }
    uint64_t cdOff = body.size();
    body += central;
    zw32(body, 0x06054b50u);
    zw16(body, 0);
    zw16(body, 0);
    zw16(body, count);
    zw16(body, count);
    zw32(body, (uint32_t)central.size());
    zw32(body, (uint32_t)cdOff);
    zw16(body, 0);
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return rc;
}

SozOpts sozReadOpts(const ParseResult &r)
{
    SozOpts o;
    o.quiet = r.flag("quiet");
    std::string en = r.str("enable-sozip", "auto");
    o.enable = strEqualNoCase(en, "yes") ? 1
               : strEqualNoCase(en, "no") ? 2
                                          : 0;
    o.chunk = sozParseSize(r.str("sozip-chunk-size"), 32768);
    if (o.chunk < 1)
        o.chunk = 1;
    o.minSize = sozParseSize(r.str("sozip-min-file-size"), 1048576);
    o.contentType = r.str("content-type");
    return o;
}

int sozipCreateHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    const std::string output = r.str("output");
    if (!szExtIsZip(output))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "create: Extension of zip filename should be .zip");
        handlerPrintUsage();
        return 1;
    }
    SozOpts opts = sozReadOpts(r);
    const bool recursive = r.flag("recursive");
    const bool junk = r.flag("no-paths");
    const auto inputs = r.list("input");
    std::vector<std::pair<std::string, bool>> roots;  // path, isDir
    for (const auto &in : inputs)
    {
        struct stat st;
        if (stat(in.c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: " + in + " does not exist");
            return 1;
        }
        roots.emplace_back(in, S_ISDIR(st.st_mode));
    }
    std::vector<SozItem> items;
    for (const auto &rt : roots)
    {
        if (rt.second && recursive)
            sozExpandDir(rt.first, items);
        else
        {
            SozItem it;
            it.display = rt.first;
            it.path = rt.first;
            it.isDir = rt.second;
            struct stat st;
            if (stat(rt.first.c_str(), &st) == 0)
                it.size = (uint64_t)st.st_size;
            items.push_back(it);
        }
    }
    for (auto &it : items)
        if (!it.isDir)
            it.stored = szNormalizeStored(it.display, junk);
    std::string prefix, oldCentral;
    uint16_t oldCount = 0;
    std::set<std::string> names;
    struct stat st;
    if (!r.flag("overwrite") && stat(output.c_str(), &st) == 0)
    {
        SzZip z;
        if (!szOpen(output, z, false))
            return 1;  // existing non-zip output: silent failure
        prefix = z.raw.substr(0, z.cdOff);
        oldCentral = z.raw.substr(z.cdOff, z.cdSize);
        oldCount = z.count;
        for (const auto &e : z.es)
            names.insert(e.name);
    }
    return sozAddLoop("create", items, output, prefix, oldCentral,
                      oldCount, names, opts);
}

int sozipOptimizeHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    const std::string output = r.str("output");
    const std::string input =
        r.list("input").empty() ? "" : r.list("input")[0];
    if (!szExtIsZip(output))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "optimize: Extension of zip filename should be .zip");
        handlerPrintUsage();
        return 1;
    }
    SozOpts opts = sozReadOpts(r);
    SzZip z;
    if (!szOpen(input, z, true))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "optimize: " + input + " is not a valid .zip file");
        return 1;
    }
    struct stat ost;
    if (!r.flag("overwrite") && stat(output.c_str(), &ost) == 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "optimize: " + output + " already exists. Use "
                    "--overwrite");
        return 1;
    }
    std::vector<SozItem> items;
    {
        SozItem self;
        self.display = input;
        self.stored = szNormalizeStored(input, false);
        self.fromMemory = true;
        self.content = z.raw;
        self.size = z.raw.size();
        struct stat st;
        if (stat(input.c_str(), &st) == 0)
            sozDosTime(st.st_mtime, self.dtime, self.ddate);
        self.dosSet = true;
        items.push_back(std::move(self));
    }
    for (const auto &e : z.es)
    {
        if (szHidden(e.name))
            continue;
        SozItem it;
        it.display = "/vsizip/{" + input + "}/" + e.name;
        it.stored = e.name;
        it.fromMemory = true;
        it.dtime = e.dtime;
        it.ddate = e.ddate;
        it.dosSet = true;
        uint64_t dataStart = 0;
        if (szLocalData(z, e, dataStart) &&
            dataStart + e.cs <= z.raw.size())
        {
            if (e.mth == 0)
                it.content = z.raw.substr(dataStart, e.cs);
            else
                szInflateRaw(z.raw.data() + dataStart, (size_t)e.cs,
                             it.content, e.us);
        }
        it.size = it.content.size();
        items.push_back(std::move(it));
    }
    return sozAddLoop("optimize", items, output, "", "", 0, {}, opts);
}

int sozipListHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    const std::string input =
        r.list("input").empty() ? r.str("input") : r.list("input")[0];
    SzZip z;
    if (!szOpen(input, z, true))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "list: " + input + " is not a valid .zip file");
        return 1;
    }
    printf("  Length          DateTime        "
           "Seek-optimized / chunk size  Name               Properties\n");
    printf("-----------  -------------------  "
           "---------------------------  -----------------  "
           "--------------\n");
    for (const auto &e : z.es)
    {
        if (szHidden(e.name))
            continue;
        SzIdx idx;
        bool soz = szFindIdx(z, e, idx);
        std::string col;
        if (soz)
            col = strPrintf("   yes (%9u bytes)   ", idx.chunk);
        else
            col.assign(27, ' ');
        std::string props = szKvProps(e.extra);
        printf("%11llu  %04d-%02d-%02d %02d:%02d:%02d  %s  %s"
               "               %s\n",
               (unsigned long long)e.us, 1980 + (e.ddate >> 9),
               (e.ddate >> 5) & 15, e.ddate & 31, e.dtime >> 11,
               (e.dtime >> 5) & 63, (e.dtime & 31) * 2, col.c_str(),
               e.name.c_str(), props.c_str());
    }
    return 0;
}

int sozipValidateHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    const std::string input =
        r.list("input").empty() ? r.str("input") : r.list("input")[0];
    const bool verbose = r.flag("verbose");
    SzZip z;
    if (!szOpen(input, z, true))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "validate: " + input + " is not a valid .zip file");
        return 1;
    }
    int nSoz = 0;
    bool anyBad = false;
    struct libdeflate_decompressor *dc = libdeflate_alloc_decompressor();
    for (const auto &e : z.es)
    {
        if (szHidden(e.name))
            continue;
        if (verbose)
            printf("Testing %s...\n", e.name.c_str());
        SzIdx idx;
        if (!szFindIdx(z, e, idx))
            continue;
        if (verbose)
        {
            printf("  %s has an associated .sozip.idx file\n",
                   e.name.c_str());
            printf("  %s: checking index offset values...\n",
                   e.name.c_str());
            printf("  %s: checking if chunks can be independently "
                   "decompressed...\n",
                   e.name.c_str());
        }
        fflush(stdout);
        bool bad = false;
        uint64_t dataStart = 0;
        szLocalData(z, e, dataStart);
        uint64_t nChunks =
            idx.chunk ? (idx.us + idx.chunk - 1) / idx.chunk : 0;
        for (uint64_t k = 0; k < nChunks; ++k)
        {
            uint64_t start = k ? idx.offsets[(size_t)k - 1] : 0;
            uint64_t end =
                k + 1 < nChunks ? idx.offsets[(size_t)k] : idx.cs;
            bool bounds = start <= end && end <= e.cs &&
                          dataStart + end <= z.raw.size();
            if (k + 1 < nChunks)
            {
                static const char term[9] = {0, 0, '\xff', '\xff',
                                             0, 0, 0, '\xff', '\xff'};
                if (!bounds || end < 9 ||
                    memcmp(z.raw.data() + dataStart + end - 9, term, 9) !=
                        0)
                {
                    cplErrorStr(
                        CE_Failure, CPLE_AppDefined,
                        strPrintf("validate: Error: file %s, chunk[%d] is "
                                  "not terminated by "
                                  "\\x00\\x00\\xFF\\xFF\\x00\\x00\\x00"
                                  "\\xFF\\xFF.",
                                  e.name.c_str(), (int)k));
                    bad = true;
                }
            }
            uint64_t expect = k + 1 < nChunks
                                  ? idx.chunk
                                  : idx.us - (nChunks - 1) * idx.chunk;
            bool okChunk = false;
            if (bounds)
            {
                std::string in(z.raw, (size_t)(dataStart + start),
                               (size_t)(end - start));
                in.append("\x03\x00", 2);
                std::string out((size_t)expect, '\0');
                int ret = libdeflate_deflate_decompress(
                    dc, in.data(), in.size(),
                    expect ? &out[0] : (char *)&okChunk, (size_t)expect,
                    nullptr);
                okChunk = (ret == 0);
            }
            if (!okChunk)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("libdeflate_deflate_decompress() "
                                      "failed at pos %llu",
                                      (unsigned long long)(k * idx.chunk)));
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("libdeflate_deflate_decompress() "
                                      "failed at pos %llu",
                                      (unsigned long long)(k * idx.chunk)));
                cplErrorStr(
                    CE_Failure, CPLE_AppDefined,
                    strPrintf("validate: Error: file %s, chunk[%d] cannot "
                              "be fully read.",
                              e.name.c_str(), (int)k));
                bad = true;
            }
        }
        if (bad)
        {
            anyBad = true;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "validate: * File " + e.name +
                            " has a SOZip index, but is is invalid!");
        }
        else
        {
            ++nSoz;
            printf("* File %s has a valid SOZip index, using chunk_size = "
                   "%u.\n",
                   e.name.c_str(), idx.chunk);
            fflush(stdout);
        }
    }
    libdeflate_free_decompressor(dc);
    if (anyBad)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "validate: " + input + " is not a valid SOZip file!");
        return 1;
    }
    if (nSoz > 0)
    {
        printf("-----\n");
        printf("%s is a valid .zip file, and contains %d SOZip-enabled "
               "file(s).\n",
               input.c_str(), nSoz);
    }
    else
        printf("%s is a valid .zip file, but does not contain any "
               "SOZip-enabled files.\n",
               input.c_str());
    return 0;
}

}  // namespace

void registerVsiSozipHandlers()
{
    registerHandler("vsi_sozip_create", sozipCreateHandler);
    registerHandler("vsi_sozip_list", sozipListHandler);
    registerHandler("vsi_sozip_validate", sozipValidateHandler);
    registerHandler("vsi_sozip_optimize", sozipOptimizeHandler);
}
