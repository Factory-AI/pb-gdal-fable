#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct TiffTag
{
    uint16_t id = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    std::vector<uint64_t> ints;
    std::vector<double> doubles;
    std::string ascii;
    std::vector<uint8_t> raw;
};

struct TiffIfd
{
    uint64_t offset = 0;
    std::map<uint16_t, TiffTag> tags;

    bool has(uint16_t id) const { return tags.count(id) != 0; }
    uint64_t getInt(uint16_t id, uint64_t def = 0) const;
    std::string getAscii(uint16_t id) const;
    const std::vector<double> *getDoubles(uint16_t id) const;
    const std::vector<uint64_t> *getInts(uint16_t id) const;
};

struct TiffDiag
{
    bool warning = false;
    std::string text;
    bool prefixed = false;
    int page = 0;  // 0-based IFD the diagnostic was raised for
};

struct TiffFile
{
    std::string path;
    std::vector<uint8_t> data;
    bool bigEndian = false;
    bool bigTiff = false;
    std::vector<TiffIfd> ifds;
    std::vector<TiffDiag> diags;

    static bool open(const std::string &path, TiffFile &out,
                     std::string &err);
    static bool identify(const std::string &path);
    static void parseIfdEntries(TiffFile &out, uint64_t ifdOff, TiffIfd &ifd,
                                bool firstDir = false);
    // Returns ifds index for the IFD at file offset off, parsing it if
    // needed; on failure appends libtiff-style diags and returns -1.
    int findOrParseIfdAt(uint64_t off);
};
