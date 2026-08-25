#include "tiff.h"
#include "util.h"
#include "vsi.h"

#include <cstdio>
#include <cstring>

namespace
{

struct Cursor
{
    const std::vector<uint8_t> &d;
    bool be;

    uint64_t rd(size_t off, size_t n) const
    {
        if (off + n > d.size())
            return 0;
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i)
        {
            size_t idx = be ? off + i : off + n - 1 - i;
            v = (v << 8) | d[idx];
        }
        return v;
    }
    uint16_t u16(size_t off) const { return (uint16_t)rd(off, 2); }
    uint32_t u32(size_t off) const { return (uint32_t)rd(off, 4); }
    uint64_t u64(size_t off) const { return rd(off, 8); }
};

size_t typeSize(uint16_t t)
{
    switch (t)
    {
        case 1:
        case 2:
        case 6:
        case 7:
            return 1;
        case 3:
        case 8:
            return 2;
        case 4:
        case 9:
        case 11:
        case 13:
            return 4;
        case 5:
        case 10:
        case 12:
        case 16:
        case 17:
        case 18:
            return 8;
        default:
            return 1;
    }
}

}  // namespace

uint64_t TiffIfd::getInt(uint16_t id, uint64_t def) const
{
    auto it = tags.find(id);
    if (it == tags.end() || it->second.ints.empty())
        return def;
    return it->second.ints[0];
}

std::string TiffIfd::getAscii(uint16_t id) const
{
    auto it = tags.find(id);
    if (it == tags.end())
        return "";
    return it->second.ascii;
}

const std::vector<double> *TiffIfd::getDoubles(uint16_t id) const
{
    auto it = tags.find(id);
    if (it == tags.end())
        return nullptr;
    return &it->second.doubles;
}

const std::vector<uint64_t> *TiffIfd::getInts(uint16_t id) const
{
    auto it = tags.find(id);
    if (it == tags.end())
        return nullptr;
    return &it->second.ints;
}

static const char *asciiTagName(uint16_t id)
{
    switch (id)
    {
        case 269: return "DocumentName";
        case 270: return "ImageDescription";
        case 271: return "Make";
        case 272: return "Model";
        case 285: return "PageName";
        case 305: return "Software";
        case 306: return "DateTime";
        case 315: return "Artist";
        case 316: return "HostComputer";
        case 333: return "InkNames";
        case 337: return "TargetPrinter";
        case 33432: return "Copyright";
        case 34737: return "GeoASCIIParams";
        case 42112: return "GDALMetadata";
        case 42113: return "GDALNoDataValue";
        default: return nullptr;
    }
}

void TiffFile::parseIfdEntries(TiffFile &out, uint64_t ifdOff, TiffIfd &ifd,
                               bool firstDir)
{
    Cursor c{out.data, out.bigEndian};
    size_t entrySize = out.bigTiff ? 20 : 12;
    size_t cntSize = out.bigTiff ? 8 : 2;
    uint64_t n = out.bigTiff ? c.u64(ifdOff) : (uint64_t)c.u16(ifdOff);
    size_t base = ifdOff + cntSize;
    for (uint64_t i = 0; i < n; ++i)
    {
        size_t e = base + i * entrySize;
        TiffTag tag;
        tag.id = c.u16(e);
        tag.type = c.u16(e + 2);
        uint64_t count = out.bigTiff ? c.u64(e + 4) : c.u32(e + 4);
        tag.count = (uint32_t)count;
        size_t valFieldOff = e + (out.bigTiff ? 12 : 8);
        size_t inlineCap = out.bigTiff ? 8 : 4;
        size_t ts = typeSize(tag.type);
        uint64_t byteLen = count * ts;
        size_t valOff;
        if (byteLen <= inlineCap)
            valOff = valFieldOff;
        else
            valOff = out.bigTiff ? c.u64(valFieldOff) : c.u32(valFieldOff);
        if (valOff + byteLen > out.data.size())
        {
            ifd.tags[tag.id] = tag;
            continue;
        }
        tag.raw.assign(out.data.begin() + valOff,
                       out.data.begin() + valOff + byteLen);
        switch (tag.type)
        {
            case 1:
            case 6:
            case 7:
                for (uint64_t k = 0; k < count; ++k)
                    tag.ints.push_back(c.rd(valOff + k, 1));
                break;
            case 2:
            {
                tag.ascii.assign(
                    reinterpret_cast<const char *>(tag.raw.data()),
                    tag.raw.size());
                size_t nulPos = tag.ascii.find('\0');
                if (nulPos != std::string::npos && count > 0 &&
                    nulPos < count - 1)
                {
                    const char *fieldName = asciiTagName(tag.id);
                    if (fieldName)
                    {
                        std::string msg = strPrintf(
                            "TIFFFetchNormalTag:ASCII value for tag \"%s\" "
                            "contains null byte in value; value incorrectly "
                            "truncated during reading due to implementation "
                            "limitations",
                            fieldName);
                        if (firstDir)
                            out.diags.push_back({true, msg, true});
                        out.diags.push_back({true, msg, false});
                    }
                    tag.ascii.resize(nulPos);
                }
                while (!tag.ascii.empty() && tag.ascii.back() == '\0')
                    tag.ascii.pop_back();
                break;
            }
            case 3:
            case 8:
                for (uint64_t k = 0; k < count; ++k)
                    tag.ints.push_back(c.u16(valOff + k * 2));
                break;
            case 4:
            case 9:
            case 13:
                for (uint64_t k = 0; k < count; ++k)
                    tag.ints.push_back(c.u32(valOff + k * 4));
                break;
            case 16:
            case 17:
                for (uint64_t k = 0; k < count; ++k)
                    tag.ints.push_back(c.u64(valOff + k * 8));
                break;
            case 5:
            case 10:
                for (uint64_t k = 0; k < count; ++k)
                {
                    double num = (double)(int64_t)c.u32(valOff + k * 8);
                    double den = (double)(int64_t)c.u32(valOff + k * 8 + 4);
                    tag.doubles.push_back(den != 0 ? num / den : 0);
                }
                break;
            case 11:
                for (uint64_t k = 0; k < count; ++k)
                {
                    uint32_t bits = c.u32(valOff + k * 4);
                    float fv;
                    memcpy(&fv, &bits, 4);
                    tag.doubles.push_back(fv);
                }
                break;
            case 12:
                for (uint64_t k = 0; k < count; ++k)
                {
                    uint64_t bits = c.u64(valOff + k * 8);
                    double dv;
                    memcpy(&dv, &bits, 8);
                    tag.doubles.push_back(dv);
                }
                break;
            default:
                break;
        }
        if (tag.id == 296 && !tag.ints.empty() &&
            (tag.ints[0] < 1 || tag.ints[0] > 3))
        {
            std::string msg = strPrintf(
                "_TIFFVSetField:%s: Bad value %d for \"ResolutionUnit\" "
                "tag",
                out.path.c_str(), (int)tag.ints[0]);
            if (firstDir)
                out.diags.push_back({true, msg, true});
            out.diags.push_back({false, msg, false});
            continue;
        }
        ifd.tags[tag.id] = tag;
    }
}

int TiffFile::findOrParseIfdAt(uint64_t off)
{
    for (size_t i = 0; i < ifds.size(); ++i)
        if (ifds[i].offset == off)
            return (int)i;
    Cursor c{data, bigEndian};
    size_t entrySize = bigTiff ? 20 : 12;
    size_t cntSize = bigTiff ? 8 : 2;
    auto fail = [&]() {
        diags.push_back({false, strPrintf("TIFFReadDirectory:Failed to "
                                          "read directory at offset %llu",
                                          (unsigned long long)off)});
        return -1;
    };
    if (off + cntSize > data.size())
    {
        diags.push_back({false, strPrintf("TIFFFetchDirectory:%s: Can not "
                                          "read TIFF directory count",
                                          path.c_str())});
        return fail();
    }
    uint64_t n = bigTiff ? c.u64(off) : (uint64_t)c.u16(off);
    if (n == 0)
    {
        diags.push_back(
            {false, strPrintf("%s:Failed to allocate memory for to read "
                              "TIFF directory (0 elements of %d bytes each)",
                              path.c_str(), (int)entrySize)});
        return fail();
    }
    if (off + cntSize + n * entrySize > data.size())
    {
        diags.push_back({false, strPrintf("TIFFFetchDirectory:%s: Can not "
                                          "read TIFF directory",
                                          path.c_str())});
        return fail();
    }
    TiffIfd ifd;
    ifd.offset = off;
    parseIfdEntries(*this, off, ifd);
    ifds.push_back(std::move(ifd));
    return (int)ifds.size() - 1;
}

bool TiffFile::identify(const std::string &path)
{
    uint8_t h[4] = {0, 0, 0, 0};
    size_t n = 0;
    if (vsiIsVirtual(path))
    {
        std::string content, errKind;
        if (!vsiReadWhole(path, content, errKind))
            return false;
        n = content.size() < 4 ? content.size() : 4;
        memcpy(h, content.data(), n);
    }
    else
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
            return false;
        n = fread(h, 1, 4, f);
        fclose(f);
    }
    if (n != 4)
        return false;
    if (h[0] == 'I' && h[1] == 'I' && (h[2] == 42 || h[2] == 43) && h[3] == 0)
        return true;
    if (h[0] == 'M' && h[1] == 'M' && h[2] == 0 && (h[3] == 42 || h[3] == 43))
        return true;
    return false;
}

bool TiffFile::open(const std::string &path, TiffFile &out, std::string &err)
{
    if (vsiIsVirtual(path))
    {
        std::string content, errKind;
        if (!vsiReadWhole(path, content, errKind))
        {
            err = "cannot open";
            return false;
        }
        out.data.assign(content.begin(), content.end());
    }
    else
    {
        FILE *f = fopen(path.c_str(), "rb");
        if (!f)
        {
            err = "cannot open";
            return false;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        out.data.resize(static_cast<size_t>(sz));
        if (sz > 0 &&
            fread(out.data.data(), 1, (size_t)sz, f) != (size_t)sz)
        {
            fclose(f);
            err = "read error";
            return false;
        }
        fclose(f);
    }
    out.path = path;
    if (out.data.size() < 8)
    {
        err = "too small";
        return false;
    }
    if (out.data[0] == 'M' && out.data[1] == 'M')
        out.bigEndian = true;
    else if (out.data[0] == 'I' && out.data[1] == 'I')
        out.bigEndian = false;
    else
    {
        err = "bad magic";
        return false;
    }
    Cursor c{out.data, out.bigEndian};
    uint16_t magic = c.u16(2);
    if (magic == 43)
        out.bigTiff = true;
    else if (magic != 42)
    {
        err = "bad magic";
        return false;
    }

    uint64_t ifdOff;
    if (out.bigTiff)
    {
        if (c.u16(4) != 8)
        {
            err = "bad bigtiff";
            return false;
        }
        ifdOff = c.u64(8);
    }
    else
        ifdOff = c.u32(4);

    int guard = 0;
    std::vector<uint64_t> seenOffsets;
    while (ifdOff != 0 && ++guard < 64)
    {
        bool first = out.ifds.empty();
        size_t entrySize = out.bigTiff ? 20 : 12;
        size_t cntSize = out.bigTiff ? 8 : 2;
        for (size_t j = 0; j < seenOffsets.size(); ++j)
        {
            if (seenOffsets[j] == ifdOff)
            {
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "_TIFFCheckDirNumberAndOffset:TIFF directory %d "
                         "has IFD looping to directory %d at offset 0x%llx "
                         "(%llu)",
                         (int)out.ifds.size() - 1, (int)j,
                         (unsigned long long)ifdOff,
                         (unsigned long long)ifdOff);
                out.diags.push_back({true, buf});
                uint64_t prevOff = out.ifds.back().offset;
                snprintf(buf, sizeof(buf),
                         "TIFFReadDirectory:Didn't read next directory due "
                         "to IFD looping at offset 0x%llx (%llu) to offset "
                         "0x%llx (%llu)",
                         (unsigned long long)prevOff,
                         (unsigned long long)prevOff,
                         (unsigned long long)ifdOff,
                         (unsigned long long)ifdOff);
                out.diags.push_back({true, buf});
                ifdOff = 0;
                break;
            }
        }
        if (ifdOff == 0)
            break;
        auto dirReadFail = [&]() {
            out.diags.push_back(
                {false, strPrintf("TIFFReadDirectory:Failed to read "
                                  "directory at offset %llu",
                                  (unsigned long long)ifdOff)});
            if (first)
                err = "libtiff";
        };
        if (ifdOff + cntSize > out.data.size())
        {
            out.diags.push_back(
                {false, strPrintf("TIFFFetchDirectory:%s: Can not read "
                                  "TIFF directory count",
                                  path.c_str())});
            dirReadFail();
            if (first)
                return false;
            break;
        }
        uint64_t n =
            out.bigTiff ? c.u64(ifdOff) : (uint64_t)c.u16(ifdOff);
        if (n == 0)
        {
            out.diags.push_back(
                {false, strPrintf("%s:Failed to allocate memory for to "
                                  "read TIFF directory (0 elements of %d "
                                  "bytes each)",
                                  path.c_str(), (int)entrySize)});
            dirReadFail();
            if (first)
                return false;
            break;
        }
        size_t base = ifdOff + cntSize;
        if (base + n * entrySize > out.data.size())
        {
            out.diags.push_back(
                {false, strPrintf("TIFFFetchDirectory:%s: Can not read "
                                  "TIFF directory",
                                  path.c_str())});
            dirReadFail();
            if (first)
                return false;
            break;
        }
        seenOffsets.push_back(ifdOff);
        size_t pageDiagStart = out.diags.size();
        {
            bool sorted = true;
            uint16_t prev = 0;
            for (uint64_t i = 0; i < n && sorted; ++i)
            {
                uint16_t id = c.u16(base + i * entrySize);
                if (i > 0 && id < prev)
                    sorted = false;
                prev = id;
            }
            if (!sorted)
            {
                const char *msg =
                    "TIFFReadDirectoryCheckOrder:Invalid TIFF directory; "
                    "tags are not sorted in ascending order";
                if (first)
                    out.diags.push_back({true, msg, true});
                out.diags.push_back({true, msg, false});
            }
        }
        TiffIfd ifd;
        ifd.offset = ifdOff;
        parseIfdEntries(out, ifdOff, ifd, first);
        {
            uint64_t phot = ifd.getInt(262, 0);
            uint64_t spp = ifd.getInt(277, 1);
            uint64_t extraCount = 0;
            if (const std::vector<uint64_t> *v = ifd.getInts(338))
                extraCount = v->size();
            int colorChannels = 0;
            switch (phot)
            {
                case 0:
                case 1:
                case 3:
                case 4:
                    colorChannels = 1;
                    break;
                case 2:
                case 6:
                case 8:
                case 9:
                case 10:
                case 32845:
                    colorChannels = 3;
                    break;
                case 5:
                    colorChannels = 4;
                    break;
                default:
                    colorChannels = 0;
            }
            if (colorChannels &&
                spp > (uint64_t)colorChannels + extraCount)
            {
                const char *msg =
                    "TIFFReadDirectory:Sum of Photometric type-related "
                    "color channels and ExtraSamples doesn't match "
                    "SamplesPerPixel. Defining non-color channels as "
                    "ExtraSamples.";
                if (first)
                    out.diags.push_back({true, msg, true});
                out.diags.push_back({true, msg, false});
            }
        }
        {
            uint64_t comp = ifd.getInt(259, 1);
            const std::vector<uint64_t> *so = ifd.getInts(273);
            const std::vector<uint64_t> *sc = ifd.getInts(279);
            if (so && !so->empty() && !sc && comp == 1)
            {
                const char *msg =
                    "TIFFReadDirectory:TIFF directory is missing required "
                    "\"StripByteCounts\" field, calculating from "
                    "imagelength";
                if (first)
                    out.diags.push_back({true, msg, true});
                out.diags.push_back({true, msg, false});
            }
            else if (so && sc && so->size() == 1 && sc->size() == 1 &&
                     (*so)[0] != 0)
            {
                uint64_t fsize = out.data.size();
                bool bogus = (*sc)[0] == 0 ||
                             (comp == 1 && (*so)[0] <= fsize &&
                              (*sc)[0] > fsize - (*so)[0]);
                if (bogus)
                {
                    const char *msg =
                        "TIFFReadDirectory:Bogus \"StripByteCounts\" "
                        "field, ignoring and calculating from imagelength";
                    if (first)
                        out.diags.push_back({true, msg, true});
                    out.diags.push_back({true, msg, false});
                }
            }
        }
        // the WEBP codec's FixupTags rejects band-separate storage at
        // the end of every directory read: prefixed warning at open,
        // failure-class replay on later reads
        if (ifd.getInt(259, 1) == 50001 && ifd.getInt(284, 1) == 2 &&
            ifd.getInt(277, 1) > 1)
        {
            const char *msg =
                "TWebPFixupTags:TIFF WEBP requires data to be stored "
                "contiguously in RGB e.g. RGBRGBRGB or RGBARGBARGBA";
            if (first)
                out.diags.push_back({true, msg, true});
            out.diags.push_back({false, msg, false});
        }
        for (size_t i = pageDiagStart; i < out.diags.size(); ++i)
            out.diags[i].page = (int)out.ifds.size();
        out.ifds.push_back(std::move(ifd));
        size_t nextOff = base + n * entrySize;
        ifdOff = out.bigTiff ? c.u64(nextOff) : c.u32(nextOff);
    }
    if (out.ifds.empty())
    {
        err = "no ifd";
        return false;
    }
    return true;
}
