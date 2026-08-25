#include "gtiff_write.h"
#include "cpl.h"
#include "jpeg_ijg.h"
#include "proj_min.h"
#include "util.h"
#include "vsi.h"
#include "webp_shim.h"
#include "zstd_min.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

extern "C"
{
    struct libdeflate_compressor;
    struct libdeflate_compressor *libdeflate_alloc_compressor(int level);
    size_t libdeflate_zlib_compress(struct libdeflate_compressor *,
                                    const void *in, size_t in_nbytes,
                                    void *out, size_t out_nbytes_avail);
    void libdeflate_free_compressor(struct libdeflate_compressor *);
}

namespace
{

// ------------------------------------------------------------------
// low level TIFF assembly
// ------------------------------------------------------------------

enum : uint16_t
{
    TT_BYTE = 1,
    TT_ASCII = 2,
    TT_SHORT = 3,
    TT_LONG = 4,
    TT_DOUBLE = 12
};

int ttSize(uint16_t t)
{
    switch (t)
    {
        case TT_BYTE:
        case TT_ASCII:
            return 1;
        case TT_SHORT:
            return 2;
        case TT_LONG:
            return 4;
        case TT_DOUBLE:
        case 16:  // LONG8
            return 8;
    }
    return 1;
}

// insertion order = out-of-line data placement order (libtiff write order)
struct OutTag
{
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    std::vector<uint8_t> data;
};

void put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        v.push_back((x >> (8 * i)) & 0xff);
}

void put64(std::vector<uint8_t> &v, uint64_t x)
{
    for (int i = 0; i < 8; i++)
        v.push_back((x >> (8 * i)) & 0xff);
}

void putDouble(std::vector<uint8_t> &v, double d)
{
    uint8_t b[8];
    memcpy(b, &d, 8);
    v.insert(v.end(), b, b + 8);
}

OutTag mkShorts(uint16_t tag, const std::vector<uint16_t> &vals)
{
    OutTag t;
    t.tag = tag;
    t.type = TT_SHORT;
    t.count = vals.size();
    for (uint16_t v : vals)
        put16(t.data, v);
    return t;
}

OutTag mkShort(uint16_t tag, uint16_t v)
{
    return mkShorts(tag, {v});
}

OutTag mkLongs(uint16_t tag, const std::vector<uint32_t> &vals)
{
    OutTag t;
    t.tag = tag;
    t.type = TT_LONG;
    t.count = vals.size();
    for (uint32_t v : vals)
        put32(t.data, v);
    return t;
}

OutTag mkShortLong(uint16_t tag, uint32_t v)
{
    if (v <= 0xffff)
        return mkShort(tag, (uint16_t)v);
    return mkLongs(tag, {v});
}

OutTag mkDoubles(uint16_t tag, const std::vector<double> &vals)
{
    OutTag t;
    t.tag = tag;
    t.type = TT_DOUBLE;
    t.count = vals.size();
    for (double v : vals)
        putDouble(t.data, v);
    return t;
}

OutTag mkAscii(uint16_t tag, const std::string &s)
{
    OutTag t;
    t.tag = tag;
    t.type = TT_ASCII;
    t.count = s.size() + 1;
    t.data.assign(s.begin(), s.end());
    t.data.push_back(0);
    return t;
}

// libtiff stores RATIONAL fields as float; a dyadic denominator search
// reproduces its encoding for float-exact values
OutTag mkRationalF(uint16_t tag, float f)
{
    OutTag t;
    t.tag = tag;
    t.type = 5;
    t.count = 1;
    double v = f;
    uint32_t num = 0, den = 1;
    if (v >= 0 && std::isfinite(v))
    {
        uint64_t d = 1;
        while (d <= 0x80000000ull)
        {
            double scaled = v * (double)d;
            if (scaled == floor(scaled) && scaled <= 4294967295.0)
            {
                num = (uint32_t)scaled;
                den = (uint32_t)d;
                break;
            }
            d <<= 1;
        }
        if (d > 0x80000000ull)
        {
            num = (uint32_t)(v * 2147483648.0 + 0.5);
            den = 0x80000000u;
        }
    }
    put32(t.data, num);
    put32(t.data, den);
    return t;
}

// ------------------------------------------------------------------
// compressors
// ------------------------------------------------------------------

// The reference embeds a newer libdeflate; against the system 1.10 its
// levels map as 6->7, 7->8, 8->9, 9->10 (verified on varied corpora).
std::vector<uint8_t> deflateBlock(const std::vector<uint8_t> &in, int level)
{
    if (level >= 6 && level <= 9)
        level++;
    libdeflate_compressor *c = libdeflate_alloc_compressor(level);
    if (!c)
        return {};
    std::vector<uint8_t> out(in.size() + in.size() / 2 + 4096);
    size_t n = libdeflate_zlib_compress(c, in.data(), in.size(), out.data(),
                                        out.size());
    libdeflate_free_compressor(c);
    out.resize(n);
    return out;
}

// streamed like libtiff's zstd codec: the frame carries no
// content-size header, so the simple API would produce different bytes
std::vector<uint8_t> zstdBlock(const std::vector<uint8_t> &in, int level)
{
    ZSTD_CStream *cs = ZSTD_createCStream();
    if (!cs)
        return {};
    ZSTD_initCStream(cs, level);
    std::vector<uint8_t> out(ZSTD_compressBound(in.size()) + 64);
    ZSTD_inBuffer ib = {in.data(), in.size(), 0};
    ZSTD_outBuffer ob = {out.data(), out.size(), 0};
    ZSTD_compressStream(cs, &ob, &ib);
    ZSTD_endStream(cs, &ob);
    ZSTD_freeCStream(cs);
    out.resize(ob.pos);
    return out;
}

}  // namespace

bool jpegCoarseTables(uint16_t photometric, int spp, int quality)
{
    for (uint16_t v : jpegQuantTable(false, quality))
        if (v > 255)
            return true;
    if (photometric == 6 && spp == 3)
        for (uint16_t v : jpegQuantTable(true, quality))
            if (v > 255)
                return true;
    return false;
}

void jpegCoarseWarn()
{
    cplErrorStr(CE_Warning, CPLE_AppDefined,
                "JPEGLib:Caution: quantization tables are too coarse "
                "for baseline JPEG");
}

std::vector<uint8_t> jpegBlock(const std::vector<uint8_t> &raw, int w,
                               int rows, int spp, uint16_t photometric,
                               int plane, int quality, bool warn,
                               int tablesMode)
{
    const bool stdHuff = (tablesMode & 2) != 0;
    const bool embedQuant = (tablesMode & 1) == 0;
    if (warn && jpegCoarseTables(photometric, spp, quality))
        jpegCoarseWarn();
    std::vector<std::vector<uint16_t>> qts;
    qts.push_back(jpegQuantTable(false, quality));
    std::vector<JpegScanComp> comps;
    if (photometric == 6 && spp == 3)
    {
        qts.push_back(jpegQuantTable(true, quality));
        // the chroma input is expanded to the padded block width before
        // downsampling (libjpeg expand_right_edge), so the duplicated
        // columns still see the alternating downsample bias
        const int ew = (((w + 1) / 2 + 7) & ~7) * 2;
        const int eh = (rows + 1) & ~1;
        std::vector<uint8_t> lum((size_t)w * rows);
        std::vector<uint8_t> cb((size_t)ew * eh), cr((size_t)ew * eh);
        for (int j = 0; j < rows; j++)
        {
            for (int i = 0; i < w; i++)
            {
                const uint8_t *px = raw.data() + ((size_t)j * w + i) * 3;
                uint8_t y, u, v;
                jpegRgbToYcc(px[0], px[1], px[2], y, u, v);
                lum[(size_t)j * w + i] = y;
                cb[(size_t)j * ew + i] = u;
                cr[(size_t)j * ew + i] = v;
            }
            for (int i = w; i < ew; i++)
            {
                cb[(size_t)j * ew + i] = cb[(size_t)j * ew + w - 1];
                cr[(size_t)j * ew + i] = cr[(size_t)j * ew + w - 1];
            }
        }
        if (rows < eh)
            for (int i = 0; i < ew; i++)
            {
                cb[(size_t)rows * ew + i] =
                    cb[(size_t)(rows - 1) * ew + i];
                cr[(size_t)rows * ew + i] =
                    cr[(size_t)(rows - 1) * ew + i];
            }
        JpegScanComp cy;
        cy.samples = std::move(lum);
        cy.w = w;
        cy.h = rows;
        cy.id = 1;
        cy.hs = cy.vs = 2;
        cy.qtsel = cy.tabsel = 0;
        comps.push_back(std::move(cy));
        for (int k = 0; k < 2; k++)
        {
            JpegScanComp cc;
            cc.samples =
                jpegH2v2Downsample(k ? cr : cb, ew / 2, eh / 2);
            cc.w = ew / 2;
            cc.h = eh / 2;
            cc.id = (uint8_t)(2 + k);
            cc.hs = cc.vs = 1;
            cc.qtsel = cc.tabsel = 1;
            comps.push_back(std::move(cc));
        }
        return jpegStripStream(comps, w, rows, 2, qts, stdHuff,
                               embedQuant);
    }
    for (int s = 0; s < spp; s++)
    {
        JpegScanComp c;
        c.samples.resize((size_t)w * rows);
        for (size_t i = 0; i < (size_t)w * rows; i++)
            c.samples[i] = raw[i * spp + s];
        c.w = w;
        c.h = rows;
        if (plane >= 0)
            c.id = (uint8_t)plane;
        else if (spp == 1)
            c.id = 1;
        else if (photometric == 2 && spp == 3)
            c.id = (uint8_t)"RGB"[s];
        else if (photometric == 5 && spp == 4)
            c.id = (uint8_t)"CMYK"[s];
        else
            c.id = (uint8_t)s;
        c.hs = c.vs = 1;
        c.qtsel = c.tabsel = 0;
        comps.push_back(std::move(c));
    }
    return jpegStripStream(comps, w, rows, 1, qts, stdHuff, embedQuant);
}

namespace
{

class LzwEncoder
{
  public:
    std::vector<uint8_t> encode(const uint8_t *bp, size_t cc)
    {
        out_.clear();
        table_.clear();
        nbits_ = 9;
        maxcode_ = (1 << 9) - 1;
        freeEnt_ = 258;
        nextData_ = 0;
        nextBits_ = 0;
        ratio_ = 0;
        incount_ = 0;
        outcount_ = 0;
        checkpoint_ = kCheckGap;

        if (cc == 0)
        {
            putCode(256);
            putCode(257);
            flush();
            return out_;
        }
        putCode(256);
        int ent = *bp++;
        cc--;
        incount_++;
        while (cc > 0)
        {
            int c = *bp++;
            cc--;
            incount_++;
            int32_t fcode = ((int32_t)c << 12) + ent;
            auto it = table_.find(fcode);
            if (it != table_.end())
            {
                ent = it->second;
                continue;
            }
            putCode(ent);
            ent = c;
            table_[fcode] = freeEnt_++;
            if (freeEnt_ == 4094)  // CODE_MAX-1
            {
                clearTable();
            }
            else if (freeEnt_ > maxcode_)
            {
                nbits_++;
                maxcode_ = (1 << nbits_) - 1;
            }
            else if (incount_ >= checkpoint_)
            {
                // reference ratio checkpoint: bit-count denominator,
                // counters cleared on every voluntary reset while the
                // checkpoint itself survives the clear
                checkpoint_ = incount_ + kCheckGap;
                long rat;
                if (incount_ > 0x007fffff)
                {
                    rat = outcount_ >> 8;
                    rat = rat == 0 ? 0x7fffffff : incount_ / rat;
                }
                else
                    rat = (incount_ << 8) / outcount_;
                if (rat <= ratio_)
                    clearTable();
                else
                    ratio_ = rat;
            }
        }
        putCode(ent);
        // libtiff LZWPostEncode advances free_ent for the pending code,
        // which can widen (or reset) the code size before EOI
        freeEnt_++;
        if (freeEnt_ == 4094)
            clearTable();
        else if (freeEnt_ > maxcode_)
        {
            nbits_++;
            maxcode_ = (1 << nbits_) - 1;
        }
        putCode(257);
        flush();
        return out_;
    }

  private:
    static const long kCheckGap = 10000;

    void clearTable()
    {
        // libtiff cl_block: ratio counters reset, the checkpoint is NOT
        table_.clear();
        ratio_ = 0;
        incount_ = 0;
        outcount_ = 0;
        freeEnt_ = 258;
        putCode(256);
        nbits_ = 9;
        maxcode_ = (1 << 9) - 1;
    }

    void putCode(int code)
    {
        nextData_ = (nextData_ << nbits_) | (uint32_t)code;
        nextBits_ += nbits_;
        outcount_ += nbits_;
        while (nextBits_ >= 8)
        {
            out_.push_back((nextData_ >> (nextBits_ - 8)) & 0xff);
            nextBits_ -= 8;
        }
    }

    void flush()
    {
        if (nextBits_ > 0)
            out_.push_back((nextData_ << (8 - nextBits_)) & 0xff);
    }

    std::vector<uint8_t> out_;
    std::map<int32_t, int> table_;
    int nbits_ = 9;
    int maxcode_ = 511;
    int freeEnt_ = 258;
    uint32_t nextData_ = 0;
    int nextBits_ = 0;
    long ratio_ = 0, incount_ = 0, outcount_ = 0, checkpoint_ = kCheckGap;
};

// libtiff PackBitsEncode state machine, applied per scanline
void packBitsRow(const uint8_t *bp, long cc, std::vector<uint8_t> &out)
{
    enum
    {
        BASE,
        LITERAL,
        RUN,
        LITERAL_RUN
    } state = BASE;
    size_t lastliteral = 0;
    while (cc > 0)
    {
        uint8_t b = *bp++;
        cc--;
        long n = 1;
        for (; cc > 0 && b == *bp; cc--, bp++)
            n++;
    again:
        switch (state)
        {
            case BASE:
                if (n > 1)
                {
                    state = RUN;
                    if (n > 128)
                    {
                        out.push_back((uint8_t)-127);
                        out.push_back(b);
                        n -= 128;
                        goto again;
                    }
                    out.push_back((uint8_t)(-(n - 1)));
                    out.push_back(b);
                }
                else
                {
                    lastliteral = out.size();
                    out.push_back(0);
                    out.push_back(b);
                    state = LITERAL;
                }
                break;
            case LITERAL:
                if (n > 1)
                {
                    state = LITERAL_RUN;
                    if (n > 128)
                    {
                        out.push_back((uint8_t)-127);
                        out.push_back(b);
                        n -= 128;
                        goto again;
                    }
                    out.push_back((uint8_t)(-(n - 1)));
                    out.push_back(b);
                }
                else
                {
                    if (++out[lastliteral] == 127)
                        state = BASE;
                    out.push_back(b);
                }
                break;
            case RUN:
                if (n > 1)
                {
                    if (n > 128)
                    {
                        out.push_back((uint8_t)-127);
                        out.push_back(b);
                        n -= 128;
                        goto again;
                    }
                    out.push_back((uint8_t)(-(n - 1)));
                    out.push_back(b);
                }
                else
                {
                    lastliteral = out.size();
                    out.push_back(0);
                    out.push_back(b);
                    state = LITERAL;
                }
                break;
            case LITERAL_RUN:
                if (n == 1 && out[out.size() - 2] == (uint8_t)-1 &&
                    out[lastliteral] < 126)
                {
                    state =
                        ((out[lastliteral] += 2) == 127) ? BASE : LITERAL;
                    out[out.size() - 2] = out[out.size() - 1];
                }
                else
                    state = RUN;
                goto again;
        }
    }
}

// ------------------------------------------------------------------
// predictor
// ------------------------------------------------------------------

void horDiff(std::vector<uint8_t> &row, int sampleBytes, int stride)
{
    long n = row.size();
    if (sampleBytes == 1)
    {
        for (long i = n - 1; i >= stride; i--)
            row[i] = (uint8_t)(row[i] - row[i - stride]);
    }
    else if (sampleBytes == 2)
    {
        uint16_t *p = (uint16_t *)row.data();
        long cnt = n / 2;
        for (long i = cnt - 1; i >= stride; i--)
            p[i] = (uint16_t)(p[i] - p[i - stride]);
    }
    else if (sampleBytes == 4)
    {
        uint32_t *p = (uint32_t *)row.data();
        long cnt = n / 4;
        for (long i = cnt - 1; i >= stride; i--)
            p[i] = p[i] - p[i - stride];
    }
    else if (sampleBytes == 8)
    {
        uint64_t *p = (uint64_t *)row.data();
        long cnt = n / 8;
        for (long i = cnt - 1; i >= stride; i--)
            p[i] = p[i] - p[i - stride];
    }
}

// libtiff fpDiff: shuffle bytes MSB-first by significance then byte-diff.
// A big-endian target reverses the plane order: the reference feeds the
// predictor file-order samples, whose byte reversal flips significance.
void fpDiff(std::vector<uint8_t> &row, int sampleBytes, int stride,
            bool lsbFirst = false)
{
    long n = row.size();
    long wc = n / sampleBytes;
    std::vector<uint8_t> tmp(row);
    for (int b = 0; b < sampleBytes; b++)
        for (long c = 0; c < wc; c++)
            row[(size_t)b * wc + c] =
                tmp[(size_t)c * sampleBytes +
                    (lsbFirst ? b : sampleBytes - 1 - b)];
    (void)stride;
    for (long i = n - 1; i >= 1; i--)
        row[i] = (uint8_t)(row[i] - row[i - 1]);
}

// ------------------------------------------------------------------
// geokeys
// ------------------------------------------------------------------

int epsgOf(PJ *pj)
{
    const char *auth = proj_get_id_auth_name(pj, 0);
    const char *code = proj_get_id_code(pj, 0);
    if (auth && code && strcmp(auth, "EPSG") == 0)
        return atoi(code);
    return -1;
}

struct KeyEntry
{
    uint16_t id, loc, count, value;
};

int unitCodeOf(PJ *crs, int def)
{
    PJ_CONTEXT *ctx = projCtx();
    PJ *cs = proj_crs_get_coordinate_system(ctx, crs);
    int code = def;
    if (cs)
    {
        const char *unitCode = nullptr;
        const char *unitAuth = nullptr;
        if (proj_cs_get_axis_info(ctx, cs, 0, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, &unitAuth, &unitCode) &&
            unitCode && unitAuth && strcmp(unitAuth, "EPSG") == 0)
            code = atoi(unitCode);
        proj_destroy(cs);
    }
    if (code == 9122)  // "degree (supplier to define representation)"
        code = 9102;
    return code;
}

struct GeogParts
{
    std::string name = "unnamed", datumName = "unnamed",
                ellName = "unnamed", pmName = "Greenwich",
                unitName = "degree";
    int datumCode = -1, ellCode = -1;
    double a = 6378137, invf = 298.257223563, pmLon = 0;
    double pmConv = M_PI / 180.0;
    bool unitEpsg = true;
    int unitCode = 9102;
    double unitSize = 0.0174532925199433;
};

int datumCodeFromName(const std::string &raw)
{
    std::string n = raw;
    for (char &c : n)
        if (c == '_')
            c = ' ';
    if (n == "World Geodetic System 1984" || n == "WGS 1984" ||
        n == "WGS 84" || n == "WGS84")
        return 6326;
    if (n == "North American Datum 1927" || n == "NAD27" ||
        n == "NAD 27")
        return 6267;
    if (n == "North American Datum 1983" || n == "NAD83" ||
        n == "NAD 83")
        return 6269;
    if (n == "World Geodetic System 1972" || n == "WGS 1972" ||
        n == "WGS 72" || n == "WGS72")
        return 6322;
    return -1;
}

int angularCodeFromName(const std::string &name, double conv)
{
    std::string n = strToLower(name);
    if (n == "degree")
        return 9102;
    if (n == "grad")
        return 9105;
    if (n == "gon")
        return 9106;
    if (n == "radian")
        return 9101;
    if (n == "arc-second")
        return 9104;
    if (n == "arc-minute")
        return 9103;
    // matched by name only: a degree-sized unit under another name
    // ("unknown") stays custom (2055 + AUnits citation)
    (void)conv;
    return -1;
}

int linearCodeFromName(const std::string &name, double conv)
{
    std::string n = strToLower(name);
    if (n == "metre" || n == "meter")
        return 9001;
    if (n == "foot" || n == "international foot" || n == "feet")
        return 9002;
    if (n == "us survey foot" || n == "u.s. foot" || n == "us foot" ||
        n == "united states survey foot")
        return 9003;
    if (conv == 1.0)
        return 9001;
    if (std::fabs(conv - 0.3048) < 1e-15)
        return 9002;
    if (std::fabs(conv - 1200.0 / 3937.0) < 1e-12)
        return 9003;
    return -1;
}

struct LinearUnit
{
    int code = 9001;
    double size = 1.0;
    std::string name = "metre";
};

LinearUnit linearUnitOf(PJ_CONTEXT *ctx, PJ *crs)
{
    LinearUnit u;
    PJ *cs = proj_crs_get_coordinate_system(ctx, crs);
    if (!cs)
        return u;
    const char *unitName = nullptr, *uAuth = nullptr, *uCode = nullptr;
    double conv = 0;
    if (proj_cs_get_axis_info(ctx, cs, 0, nullptr, nullptr, nullptr,
                              &conv, &unitName, &uAuth, &uCode))
    {
        if (unitName)
            u.name = unitName;
        u.size = conv;
        if (uAuth && uCode && strcmp(uAuth, "EPSG") == 0)
            u.code = atoi(uCode);
        else
            u.code = linearCodeFromName(u.name, u.size);
    }
    proj_destroy(cs);
    return u;
}

GeogParts geogPartsOf(PJ_CONTEXT *ctx, PJ *gpj)
{
    GeogParts g;
    const char *nm = proj_get_name(gpj);
    if (nm)
        g.name = nm;
    PJ *datum = proj_crs_get_datum(ctx, gpj);
    if (!datum)
        datum = proj_crs_get_datum_ensemble(ctx, gpj);
    if (datum)
    {
        const char *dn = proj_get_name(datum);
        if (dn)
            g.datumName = dn;
        g.datumCode = epsgOf(datum);
        if (g.datumCode <= 0)
            g.datumCode = datumCodeFromName(g.datumName);
        proj_destroy(datum);
    }
    PJ *ell = proj_get_ellipsoid(ctx, gpj);
    if (ell)
    {
        const char *en = proj_get_name(ell);
        if (en)
            g.ellName = en;
        g.ellCode = epsgOf(ell);
        double b = 0;
        int comp = 0;
        proj_ellipsoid_get_parameters(ctx, ell, &g.a, &b, &comp, &g.invf);
        proj_destroy(ell);
    }
    PJ *pm = proj_get_prime_meridian(ctx, gpj);
    if (pm)
    {
        const char *pn = proj_get_name(pm);
        if (pn)
            g.pmName = pn;
        double conv = 0;
        const char *un = nullptr;
        proj_prime_meridian_get_parameters(ctx, pm, &g.pmLon, &conv, &un);
        if (conv > 0)
            g.pmConv = conv;
        proj_destroy(pm);
    }
    PJ *cs = proj_crs_get_coordinate_system(ctx, gpj);
    if (cs)
    {
        const char *unitName = nullptr, *uAuth = nullptr,
                   *uCode = nullptr;
        double conv = 0;
        if (proj_cs_get_axis_info(ctx, cs, 0, nullptr, nullptr, nullptr,
                                  &conv, &unitName, &uAuth, &uCode))
        {
            if (unitName)
                g.unitName = unitName;
            g.unitSize = conv;
            if (uAuth && uCode && strcmp(uAuth, "EPSG") == 0)
            {
                g.unitEpsg = true;
                g.unitCode = atoi(uCode);
                if (g.unitCode == 9122)
                    g.unitCode = 9102;
            }
            else
            {
                g.unitCode =
                    angularCodeFromName(g.unitName, g.unitSize);
                g.unitEpsg = g.unitCode > 0;
            }
        }
        proj_destroy(cs);
    }
    return g;
}

struct ConvInfo
{
    int method = -1;
    std::string methodName;
    std::map<int, double> byCode;
};

ConvInfo convInfoOf(PJ_CONTEXT *ctx, PJ *pj)
{
    ConvInfo ci;
    PJ *conv = proj_crs_get_coordoperation(ctx, pj);
    if (!conv)
        return ci;
    const char *mname = nullptr, *mauth = nullptr, *mcode = nullptr;
    proj_coordoperation_get_method_info(ctx, conv, &mname, &mauth,
                                        &mcode);
    if (mname)
        ci.methodName = mname;
    if (mauth && mcode && strcmp(mauth, "EPSG") == 0)
        ci.method = atoi(mcode);
    int n = proj_coordoperation_get_param_count(ctx, conv);
    for (int i = 0; i < n; i++)
    {
        const char *pname = nullptr, *pauth = nullptr, *pcode = nullptr;
        const char *ucat = nullptr;
        double val = 0, uconv = 0;
        int got = proj_coordoperation_get_param(
            ctx, conv, i, &pname, &pauth, &pcode, &val, nullptr, &uconv,
            nullptr, nullptr, nullptr, &ucat);
        if (!got)
            continue;
        // WKT1 parameter literals are degrees by GDAL convention; when
        // the CRS angular unit is not degree the reference's value
        // carries a deg->rad->deg roundtrip residue
        if (ucat && strcmp(ucat, "angular") == 0 && uconv > 0 &&
            std::fabs(uconv - M_PI / 180.0) > 1e-15)
            val = val * ((M_PI / 180.0) / 0.0174532925199433);
        if (pauth && pcode && strcmp(pauth, "EPSG") == 0)
            ci.byCode[atoi(pcode)] = val;
        else if (pname)
        {
            // proj-specific parameter names map onto their EPSG twins
            std::string pn = pname;
            int c = 0;
            if (pn == "Latitude of natural origin")
                c = 8801;
            else if (pn == "Longitude of natural origin" ||
                     pn == "Longitude of projection centre")
                c = 8802;
            else if (pn == "Scale factor at natural origin")
                c = 8805;
            else if (pn == "False easting")
                c = 8806;
            else if (pn == "False northing")
                c = 8807;
            else if (pn == "Latitude of 1st standard parallel")
                c = 8823;
            else if (pn == "Latitude of 2nd standard parallel")
                c = 8824;
            if (c)
                ci.byCode[c] = val;
        }
    }
    proj_destroy(conv);
    return ci;
}


// ------------------------------------------------------------------
// pixel generation
// ------------------------------------------------------------------

double clampToType(DType t, double v)
{
    auto clampI = [&](double lo, double hi)
    {
        double r = round(v);
        if (r < lo)
            r = lo;
        if (r > hi)
            r = hi;
        return r;
    };
    switch (t)
    {
        case DType::Byte:
            return clampI(0, 255);
        case DType::Int8:
            return clampI(-128, 127);
        case DType::UInt16:
            return clampI(0, 65535);
        case DType::Int16:
        case DType::CInt16:
            return clampI(-32768, 32767);
        case DType::UInt32:
            return clampI(0, 4294967295.0);
        case DType::Int32:
        case DType::CInt32:
            return clampI(-2147483648.0, 2147483647.0);
        case DType::UInt64:
        case DType::Int64:
            // bounds are not representable as double; writeSampleValue
            // applies the threshold semantics
            return v != v ? 0 : round(v);
        case DType::Float32:
        case DType::CFloat32:
            return (double)(float)v;
        default:
            return v;
    }
}

uint16_t floatToHalf(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff)
        return sign | 0x7c00 | (mant ? 0x200 : 0);
    if (exp <= 0)
        return sign;
    if (exp >= 31)
        return sign | 0x7c00;
    return sign | (exp << 10) | (mant >> 13);
}

void writeSampleValue(std::vector<uint8_t> &buf, size_t off, DType t,
                      double v)
{
    switch (t)
    {
        case DType::Byte:
            buf[off] = (uint8_t)v;
            break;
        case DType::Int8:
            buf[off] = (uint8_t)(int8_t)v;
            break;
        case DType::UInt16:
        {
            uint16_t x = (uint16_t)v;
            memcpy(&buf[off], &x, 2);
            break;
        }
        case DType::Int16:
        {
            int16_t x = (int16_t)v;
            memcpy(&buf[off], &x, 2);
            break;
        }
        case DType::UInt32:
        {
            uint32_t x = (uint32_t)v;
            memcpy(&buf[off], &x, 4);
            break;
        }
        case DType::Int32:
        {
            int32_t x = (int32_t)v;
            memcpy(&buf[off], &x, 4);
            break;
        }
        case DType::UInt64:
        {
            // reference semantics: above 2^64 saturates, exactly 2^64
            // wraps to 0 (its own overflowing cast), negatives clamp
            uint64_t x;
            if (v > 18446744073709551616.0)
                x = UINT64_MAX;
            else if (v >= 18446744073709551616.0)
                x = 0;
            else
                x = v <= 0 ? 0 : (uint64_t)v;
            memcpy(&buf[off], &x, 8);
            break;
        }
        case DType::Int64:
        {
            int64_t x = v >= 9223372036854775808.0
                            ? INT64_MAX
                            : (v <= -9223372036854775808.0
                                   ? INT64_MIN
                                   : (int64_t)v);
            memcpy(&buf[off], &x, 8);
            break;
        }
        case DType::Float16:
        {
            uint16_t x = floatToHalf((float)v);
            memcpy(&buf[off], &x, 2);
            break;
        }
        case DType::Float32:
        {
            float x = (float)v;
            memcpy(&buf[off], &x, 4);
            break;
        }
        case DType::Float64:
            memcpy(&buf[off], &v, 8);
            break;
        case DType::CInt16:
        {
            int16_t x = (int16_t)v;
            memcpy(&buf[off], &x, 2);
            int16_t z = 0;
            memcpy(&buf[off + 2], &z, 2);
            break;
        }
        case DType::CInt32:
        {
            int32_t x = (int32_t)v;
            memcpy(&buf[off], &x, 4);
            int32_t z = 0;
            memcpy(&buf[off + 4], &z, 4);
            break;
        }
        case DType::CFloat32:
        {
            float x = (float)v;
            memcpy(&buf[off], &x, 4);
            float z = 0;
            memcpy(&buf[off + 4], &z, 4);
            break;
        }
        case DType::CFloat64:
        {
            memcpy(&buf[off], &v, 8);
            double z = 0;
            memcpy(&buf[off + 8], &z, 8);
            break;
        }
        default:
            break;
    }
}

}  // namespace

// tile codecs shared with the overview builder
std::vector<uint8_t> gtiffDeflateBlock(const std::vector<uint8_t> &in,
                                       int level)
{
    return deflateBlock(in, level);
}
std::vector<uint8_t> gtiffZstdBlock(const std::vector<uint8_t> &in,
                                    int level)
{
    return zstdBlock(in, level);
}
std::vector<uint8_t> gtiffLzwEncode(const uint8_t *p, size_t n)
{
    LzwEncoder enc;
    return enc.encode(p, n);
}
void gtiffHorDiff(std::vector<uint8_t> &row, int sampleBytes, int stride)
{
    horDiff(row, sampleBytes, stride);
}
void gtiffFpDiff(std::vector<uint8_t> &row, int sampleBytes, int stride)
{
    fpDiff(row, sampleBytes, stride);
}
std::vector<uint8_t> gtiffPackbitsEncode(const uint8_t *p, size_t n,
                                         size_t rowBytes)
{
    std::vector<uint8_t> out;
    for (size_t off = 0; off < n; off += rowBytes)
        packBitsRow(p + off, (long)std::min(rowBytes, n - off), out);
    return out;
}

bool buildGeoTags(const Srs &srs, GeoTags &out, bool point,
                  bool forceDir, int gtVersion)
{
    PJ_CONTEXT *ctx = projCtx();
    PJ *pj = srs.pj();
    if (!pj && !forceDir)
        return false;

    PJ_TYPE type = pj ? proj_get_type(pj) : PJ_TYPE_UNKNOWN;
    PJ *ownedSub = nullptr;
    PJ *ownedSub2 = nullptr;
    bool oldCompound = false;
    int vertCode = 0, vertDatum = 0, vertUnit = 9001;
    std::string vertName;
    std::vector<KeyEntry> keys;
    uint16_t minor = 0;
    bool ok = false;
    const uint16_t rasterType = point ? 2 : 1;

    auto addShort = [&keys](uint16_t id, uint16_t v)
    { keys.push_back({id, 0, 1, v}); };
    auto addAscii = [&keys, &out](uint16_t id, const std::string &s)
    {
        keys.push_back({id, 34737, (uint16_t)(s.size() + 1),
                        (uint16_t)out.ascii.size()});
        out.ascii += s + "|";
    };
    auto addDouble = [&keys, &out](uint16_t id, double v)
    {
        keys.push_back({id, 34736, 1, (uint16_t)out.doubles.size()});
        out.doubles.push_back(v);
    };

    // shared user-defined geographic key section; the reference stores
    // ellipsoid doubles a-then-invf in geographic directories but
    // invf-then-a inside projected ones
    auto pushGeogUser = [&](const GeogParts &g, bool projectedCtx)
    {
        bool datumKnown = g.datumCode > 0;
        bool degree = g.unitEpsg && g.unitCode == 9102;
        addShort(2048, 32767);
        std::string cit = "GCS Name = " + g.name + "|";
        if (!datumKnown)
            cit += "Datum = " + g.datumName + "|";
        // the citation drops the ellipsoid whenever its EPSG code is
        // known, even though the 2056 key only appears for
        // user-defined datums
        if (!(g.ellCode > 0))
            cit += "Ellipsoid = " + g.ellName + "|";
        cit += "Primem = " + g.pmName + "|";
        if (!degree)
            cit += "AUnits = " + g.unitName + "|";
        addAscii(2049, cit);
        addShort(2050, (uint16_t)(datumKnown ? g.datumCode : 32767));
        if (g.unitEpsg)
            addShort(2054, (uint16_t)g.unitCode);
        if (!datumKnown)
            addShort(2056, (uint16_t)(g.ellCode > 0 ? g.ellCode : 32767));
        if (!g.unitEpsg)
            addDouble(2055, g.unitSize);
        if (datumKnown || (projectedCtx && g.ellCode > 0))
        {
            addDouble(2059, g.invf);
            addDouble(2057, g.a);
        }
        else
        {
            addDouble(2057, g.a);
            addDouble(2059, g.invf);
        }
        // the reference converts the prime meridian to degrees dividing
        // radians by the SRS_UA_DEGREE_CONV literal, then scales it back
        // by the CRS angular-unit size when that unit is not degree
        double pmDeg = (g.pmLon * g.pmConv) / 0.0174532925199433;
        addDouble(2061, degree ? pmDeg : pmDeg * g.unitSize);
    };
    auto gcsFromDatum = [](int datumCode)
    {
        switch (datumCode)
        {
            case 6326:
                return 4326;
            case 6322:
                return 4322;
            case 6269:
                return 4269;
            case 6267:
                return 4267;
        }
        return -1;
    };

    if (type == PJ_TYPE_COMPOUND_CRS)
    {
        PJ *horiz = proj_crs_get_sub_crs(ctx, pj, 0);
        PJ *vert = proj_crs_get_sub_crs(ctx, pj, 1);
        int hCode = horiz ? epsgOf(horiz) : -1;
        int vCode = vert ? epsgOf(vert) : -1;
        if (hCode > 0 && hCode <= 65535 && vCode > 0 && vCode <= 65535 &&
            gtVersion == 10)
        {
            // forced GeoTIFF 1.0: horizontal part rendered as usual but
            // cited with the compound name, vertical part in the
            // pre-1.1 vertical keys
            addAscii(1026, proj_get_name(pj));
            oldCompound = true;
            vertCode = vCode;
            const char *vn = proj_get_name(vert);
            vertName = vn ? vn : "unnamed";
            PJ *vd = proj_crs_get_datum(ctx, vert);
            if (vd)
            {
                vertDatum = epsgOf(vd);
                proj_destroy(vd);
            }
            vertUnit = unitCodeOf(vert, 9001);
            out.zScaleOne = true;
        }
        else if (hCode > 0 && hCode <= 65535 && vCode > 0 &&
                 vCode <= 65535)
        {
            bool prj = proj_get_type(horiz) == PJ_TYPE_PROJECTED_CRS;
            addShort(1024, prj ? 1 : 2);
            addShort(1025, rasterType);
            addAscii(1026, proj_get_name(pj));
            addShort(prj ? 3072 : 2048, (uint16_t)hCode);
            keys.push_back({4096, 0, 1, (uint16_t)vCode});
            minor = 1;
            out.zScaleOne = true;
            ok = true;
        }
        if (vert)
            proj_destroy(vert);
        if (!ok && horiz)
        {
            pj = horiz;
            ownedSub = horiz;
            type = proj_get_type(pj);
        }
        else if (horiz && ok)
            proj_destroy(horiz);
    }
    if (type == PJ_TYPE_GEOGRAPHIC_3D_CRS && gtVersion == 10)
    {
        // GeoTIFF 1.0 cannot carry the vertical axis: demote to 2D
        PJ *two = proj_crs_demote_to_2D(ctx, nullptr, pj);
        if (two)
        {
            pj = two;
            ownedSub2 = two;
            type = proj_get_type(pj);
        }
    }

    if (ok)
    {
    }
    else if (type == PJ_TYPE_PROJECTED_CRS)
    {
        int code = epsgOf(pj);
        PJ *geod = proj_crs_get_geodetic_crs(ctx, pj);
        GeogParts g;
        if (geod)
            g = geogPartsOf(ctx, geod);
        ConvInfo ci = convInfoOf(ctx, pj);
        auto P = [&ci](int c)
        {
            auto it = ci.byCode.find(c);
            return it == ci.byCode.end() ? 0.0 : it->second;
        };
        int utmProj = -1;
        if (code <= 0 && ci.method == 9807)
        {
            // libgeotiff's UTM shortcut: WGS84 both hemispheres,
            // NAD27/NAD83 north zones 3-22 as PCS codes; any other
            // recognized UTM gets a Proj_UTM_zone code in key 3074
            double lat0 = P(8801), lon0 = P(8802), k = P(8805);
            double fe = P(8806), fn = P(8807);
            if (lat0 == 0 && k == 0.9996 && fe == 500000 &&
                (fn == 0 || fn == 10000000))
            {
                double zf = (lon0 + 183.0) / 6.0;
                int z = (int)floor(zf + 0.5);
                if (fabs(zf - z) < 1e-9 && z >= 1 && z <= 60)
                {
                    bool north = fn == 0;
                    if (g.datumCode == 6326)
                        code = (north ? 32600 : 32700) + z;
                    else if (north && z >= 3 && z <= 22 &&
                             g.datumCode == 6269)
                        code = 26900 + z;
                    else if (north && z >= 3 && z <= 22 &&
                             g.datumCode == 6267)
                        code = 26700 + z;
                    else
                        utmProj = (north ? 16000 : 16100) + z;
                }
            }
        }
        if (code > 0 && code <= 65535)
        {
            addShort(1024, 1);
            addShort(1025, rasterType);
            if (gtVersion == 11)
            {
                // 1.1 keeps only the PCS code for known CRSs
                addShort(3072, (uint16_t)code);
            }
            else
            {
                if (!oldCompound)
                    addAscii(1026, proj_get_name(pj));
                if (geod)
                    addAscii(2049, g.name);
                addShort(2054, (uint16_t)(g.unitEpsg ? g.unitCode : 9102));
                addShort(3072, (uint16_t)code);
                addShort(3076, (uint16_t)unitCodeOf(pj, 9001));
            }
            ok = true;
        }
        else
        {
            struct KV
            {
                uint16_t key;
                double val;
            };
            std::vector<KV> params;
            int ct = -1;
            switch (ci.method)
            {
                case 9807:  // Transverse Mercator
                case 9808:  // TM South Oriented
                case 9801:  // LCC 1SP
                case 9809:  // Oblique Stereographic
                case 9804:  // Mercator variant A
                    ct = ci.method == 9807   ? 1
                         : ci.method == 9808 ? 27
                         : ci.method == 9801 ? 9
                         : ci.method == 9809 ? 16
                                             : 7;
                    params = {{3081, P(8801)},
                              {3080, P(8802)},
                              {3092, P(8805)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9805:  // Mercator variant B
                    ct = 7;
                    params = {{3081, 0.0},
                              {3080, P(8802)},
                              {3078, P(8823)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9810:  // Polar Stereographic variant A
                    ct = 15;
                    params = {{3081, P(8801)},
                              {3095, P(8802)},
                              {3092, P(8805)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9829:  // Polar Stereographic variant B
                    ct = 15;
                    params = {{3081, P(8832)},
                              {3095, P(8833)},
                              {3092, 1.0},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9820:  // Lambert Azimuthal Equal Area
                    ct = 10;
                    params = {{3089, P(8801)},
                              {3088, P(8802)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9822:  // Albers Equal Area
                    ct = 11;
                    params = {{3078, P(8823)},
                              {3079, P(8824)},
                              {3081, P(8821)},
                              {3080, P(8822)},
                              {3082, P(8826)},
                              {3083, P(8827)}};
                    break;
                case 9802:  // LCC 2SP
                    ct = 8;
                    params = {{3085, P(8821)},
                              {3084, P(8822)},
                              {3078, P(8823)},
                              {3079, P(8824)},
                              {3086, P(8826)},
                              {3087, P(8827)}};
                    break;
                case 9806:  // Cassini-Soldner
                    ct = 18;
                    params = {{3081, P(8801)},
                              {3080, P(8802)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9818:  // American Polyconic
                    ct = 22;
                    params = {{3081, P(8801)},
                              {3080, P(8802)},
                              {3092, ci.byCode.count(8805) ? P(8805)
                                                           : 1.0},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9812:  // Hotine Oblique Mercator variant A
                    ct = 3;
                    params = {{3089, P(8811)},
                              {3088, P(8812)},
                              {3094, P(8813)},
                              {3096, P(8814)},
                              {3093, P(8815)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9840:  // Orthographic
                    ct = 21;
                    params = {{3089, P(8801)},
                              {3088, P(8802)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9811:  // New Zealand Map Grid
                    ct = 26;
                    params = {{3089, P(8801)},
                              {3088, P(8802)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 9834:  // Lambert Cylindrical Equal Area
                case 9835:
                    ct = 28;
                    params = {{3080, P(8802)},
                              {3078, P(8823)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                case 1028:  // Equidistant Cylindrical
                case 1029:
                case 9823:
                case 9842:
                    ct = 17;
                    params = {{3089, P(8801)},
                              {3088, P(8802)},
                              {3078, P(8823)},
                              {3082, P(8806)},
                              {3083, P(8807)}};
                    break;
                default:
                    if (ci.methodName == "Stereographic")
                    {
                        ct = 14;
                        params = {{3089, P(8801)},
                                  {3088, P(8802)},
                                  {3092, P(8805)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName ==
                                 "Modified Azimuthal Equidistant" ||
                             ci.methodName == "Azimuthal Equidistant")
                    {
                        ct = 12;
                        params = {{3089, P(8801)},
                                  {3088, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName == "Miller Cylindrical")
                    {
                        ct = 20;
                        params = {{3089, P(8801)},
                                  {3088, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName == "Robinson")
                    {
                        ct = 23;
                        params = {{3088, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName == "Van Der Grinten")
                    {
                        ct = 25;
                        params = {{3088, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName == "Gnomonic")
                    {
                        ct = 19;
                        params = {{3089, P(8801)},
                                  {3088, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName == "Sinusoidal")
                    {
                        ct = 24;
                        params = {{3088, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    else if (ci.methodName == "Equidistant Conic")
                    {
                        ct = 13;
                        params = {{3078, P(8823)},
                                  {3079, P(8824)},
                                  {3081, P(8801)},
                                  {3080, P(8802)},
                                  {3082, P(8806)},
                                  {3083, P(8807)}};
                    }
                    break;
            }
            LinearUnit lu = linearUnitOf(ctx, pj);
            bool peString = utmProj < 0 && ct <= 0;
            addShort(1024, (uint16_t)(peString ? 32767 : 1));
            addShort(1025, rasterType);
            addAscii(1026, proj_get_name(pj));
            if (utmProj < 0 && !peString)
                for (const auto &kv : params)
                    addDouble(kv.key, kv.val);
            if (lu.code <= 0)
                addDouble(3077, lu.size);
            pushGeogUser(g, true);
            if (lu.code != 9001)
                addShort(3059, 1);
            if (utmProj > 0)
            {
                addShort(3072, 32767);
                addShort(3074, (uint16_t)utmProj);
            }
            else if (ct > 0)
            {
                addShort(3072, 32767);
                addShort(3074, 32767);
                addShort(3075, (uint16_t)ct);
            }
            else
            {
                // unmappable projection method: ESRI PE string citation
                addAscii(3073, "ESRI PE String = " + srs.wkt1Esri());
            }
            if (lu.code <= 0 && !peString)
                addAscii(3073, "LUnits = " + lu.name);
            addShort(3076, (uint16_t)(lu.code > 0 ? lu.code : 32767));
            ok = true;
        }
        if (geod)
            proj_destroy(geod);
    }
    else if (type == PJ_TYPE_GEOGRAPHIC_2D_CRS)
    {
        int code = epsgOf(pj);
        GeogParts g = geogPartsOf(ctx, pj);
        if (code <= 0 && g.unitEpsg && g.unitCode == 9102)
            code = gcsFromDatum(g.datumCode);
        if (code > 0 && code <= 65535)
        {
            addShort(1024, 2);
            addShort(1025, rasterType);
            addShort(2048, (uint16_t)code);
            if (gtVersion != 11)
            {
                addAscii(2049, g.name);
                addShort(2054, (uint16_t)(g.unitEpsg ? g.unitCode : 9102));
                // doubles are stored in set order: invf pushed first
                keys.push_back({2057, 34736, 1, 1});
                keys.push_back({2059, 34736, 1, 0});
                out.doubles.push_back(g.invf);
                out.doubles.push_back(g.a);
            }
            ok = true;
        }
        else
        {
            addShort(1024, 2);
            addShort(1025, rasterType);
            pushGeogUser(g, false);
            ok = true;
        }
    }
    else if (type == PJ_TYPE_GEOGRAPHIC_3D_CRS)
    {
        int code = epsgOf(pj);
        PJ *two = proj_crs_demote_to_2D(ctx, nullptr, pj);
        int code2 = two ? epsgOf(two) : -1;
        if (two)
            proj_destroy(two);
        if (code > 0 && code <= 65535 && code2 > 0)
        {
            addShort(1024, 2);
            addShort(1025, rasterType);
            addShort(2048, (uint16_t)code2);
            keys.push_back({4096, 0, 1, (uint16_t)code});
            minor = 1;
            ok = true;
        }
    }
    else if (type == PJ_TYPE_GEOCENTRIC_CRS)
    {
        GeogParts g = geogPartsOf(ctx, pj);
        addShort(1024, 3);
        addShort(1025, rasterType);
        const char *nm = proj_get_name(pj);
        addAscii(1026, nm ? nm : "unnamed");
        addShort(2048, 32767);
        addShort(2050,
                 (uint16_t)(g.datumCode > 0 ? g.datumCode : 32767));
        addShort(2052, 9001);
        addShort(2054, 9102);
        if (g.datumCode <= 0)
            addShort(2056,
                     (uint16_t)(g.ellCode > 0 ? g.ellCode : 32767));
        if (g.datumCode > 0)
        {
            addDouble(2059, g.invf);
            addDouble(2057, g.a);
        }
        else
        {
            addDouble(2057, g.a);
            addDouble(2059, g.invf);
        }
        ok = true;
    }

    if (!ok)
    {
        // local/engineering CRS: minimal unnamed directory; an absent
        // CRS (Point-only forced dir) stores the raster type alone
        addShort(1025, rasterType);
        if (pj)
        {
            const char *nm = proj_get_name(pj);
            addAscii(1026, nm && nm[0] ? nm : "unnamed");
            addShort(3076, 9001);
        }
    }

    if (oldCompound)
    {
        addShort(4096, (uint16_t)vertCode);
        addAscii(4097, vertName);
        addShort(4098, (uint16_t)(vertDatum > 0 ? vertDatum : 32767));
        addShort(4099, (uint16_t)vertUnit);
    }
    if (gtVersion == 11)
        minor = 1;
    else if (gtVersion == 10)
        minor = 0;

    std::stable_sort(keys.begin(), keys.end(),
                     [](const KeyEntry &a, const KeyEntry &b)
                     { return a.id < b.id; });
    out.dir.clear();
    out.dir.push_back(1);
    out.dir.push_back(1);
    out.dir.push_back(minor);
    out.dir.push_back((uint16_t)keys.size());
    for (const auto &k : keys)
    {
        out.dir.push_back(k.id);
        out.dir.push_back(k.loc);
        out.dir.push_back(k.count);
        out.dir.push_back(k.value);
    }
    out.any = true;
    if (ownedSub)
        proj_destroy(ownedSub);
    if (ownedSub2)
        proj_destroy(ownedSub2);
    return true;
}

namespace
{
bool coTestBool(const std::string &v)
{
    std::string u = strToUpper(v);
    return !(u == "NO" || u == "FALSE" || u == "OFF" || u == "0");
}

void coUnexpectedSelect(const std::string &key, const std::string &val,
                        const char *kind)
{
    cplErrorStr(CE_Warning, CPLE_NotSupported,
                strPrintf("'%s' is an unexpected value for %s creation "
                          "option of type %s.",
                          val.c_str(), key.c_str(), kind));
}

bool coIsInt(const std::string &v)
{
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t'))
        i++;
    if (i < v.size() && (v[i] == '+' || v[i] == '-'))
        i++;
    if (i >= v.size())
        return false;
    for (; i < v.size(); i++)
        if (!isdigit((unsigned char)v[i]))
            return false;
    return true;
}

}  // namespace

CreationOptions parseCreationOptions(
    const std::vector<std::pair<std::string, std::string>> &cos,
    const std::string &filename, const std::string &algName)
{
    CreationOptions o;
    std::vector<std::string> seen;
    for (const auto &kv : cos)
    {
        std::string key = strToUpper(kv.first);
        const std::string &val = kv.second;
        std::string uval = strToUpper(val);
        // CSLFetchNameValue semantics: the first occurrence of a key is
        // the one the driver reads, but every occurrence is validated
        bool first = std::find(seen.begin(), seen.end(), key) == seen.end();
        if (first)
            seen.push_back(key);
        if (key == "COMPRESS")
        {
            if (uval == "LZMA" ||
                uval == "CCITTRLE" || uval == "CCITTFAX3" ||
                uval == "CCITTFAX4")
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            algName + ": compression '" + uval +
                                "' is not built in this reimplementation");
                o.fatal = true;
                continue;
            }
            if (uval != "NONE" && uval != "LZW" && uval != "PACKBITS" &&
                uval != "DEFLATE" && uval != "ZSTD" && uval != "JPEG" &&
                uval != "WEBP")
                coUnexpectedSelect("COMPRESS", val, "string-select");
            if (!first)
                continue;
            if (uval == "JPEG")
                o.compression = 7;
            else if (uval == "LZW")
                o.compression = 5;
            else if (uval == "PACKBITS")
                o.compression = 32773;
            else if (uval == "DEFLATE")
                o.compression = 8;
            else if (uval == "ZSTD")
                o.compression = 50000;
            else if (uval == "WEBP")
                o.compression = 50001;
            else if (uval == "LERC" || uval == "LERC_DEFLATE" ||
                     uval == "LERC_ZSTD" || uval == "JXL")
            {
                // recognized compression name absent from this trimmed
                // build's codec set
                o.missingCodec = true;
                o.compressRaw = val;
            }
            else if (uval != "NONE")
            {
                o.compressBad = true;
                o.compressRaw = val;
            }
        }
        else if (key == "PREDICTOR")
        {
            if (!coIsInt(val))
                coUnexpectedSelect("PREDICTOR", val, "int");
            if (!first)
                continue;
            o.hasPredictor = true;
            o.predictorRaw = val;
            o.predictor = atoi(val.c_str());
        }
        else if (key == "ZLEVEL")
        {
            int z = atoi(val.c_str());
            if (!coIsInt(val))
                coUnexpectedSelect("ZLEVEL", val, "int");
            else if (z < 1)
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    strPrintf("'%s' is an unexpected value for ZLEVEL "
                              "creation option that should be >= 1.",
                              val.c_str()));
            else if (z > 12)
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    strPrintf("'%s' is an unexpected value for ZLEVEL "
                              "creation option that should be <= 12.",
                              val.c_str()));
            if (!first)
                continue;
            if (z < 1 || z > 12)
            {
                o.zlevelBad = true;
                o.zlevelRaw = val;
            }
            else
                o.zlevel = z > 9 ? 9 : z;
        }
        else if (key == "ZSTD_LEVEL")
        {
            int z = atoi(val.c_str());
            if (!coIsInt(val))
                coUnexpectedSelect("ZSTD_LEVEL", val, "int");
            else if (z < 1)
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    strPrintf("'%s' is an unexpected value for ZSTD_LEVEL "
                              "creation option that should be >= 1.",
                              val.c_str()));
            else if (z > 22)
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    strPrintf("'%s' is an unexpected value for ZSTD_LEVEL "
                              "creation option that should be <= 22.",
                              val.c_str()));
            if (!first)
                continue;
            if (z < 1 || z > 22)
            {
                o.zstdBad = true;
                o.zstdRaw = val;
            }
            else
                o.zstdLevel = z;
        }
        else if (key == "JPEG_QUALITY")
        {
            int q = atoi(val.c_str());
            if (!coIsInt(val))
                coUnexpectedSelect("JPEG_QUALITY", val, "int");
            else if (q < 1)
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    strPrintf("'%s' is an unexpected value for "
                              "JPEG_QUALITY creation option that should "
                              "be >= 1.",
                              val.c_str()));
            else if (q > 100)
                cplErrorStr(
                    CE_Warning, CPLE_NotSupported,
                    strPrintf("'%s' is an unexpected value for "
                              "JPEG_QUALITY creation option that should "
                              "be <= 100.",
                              val.c_str()));
            if (!first)
                continue;
            if (q < 1 || q > 100)
            {
                o.jpegQualityBad = true;
                o.jpegQualityRaw = val;
            }
            // out-of-range values are still fetched: too high clamps,
            // too low falls back to the default
            o.jpegQuality = q > 100 ? 100 : q >= 1 ? q : 75;
        }
        else if (key == "JPEGTABLESMODE")
        {
            if (!val.empty() && !coIsInt(val))
                coUnexpectedSelect("JPEGTABLESMODE", val, "int");
            if (first)
                o.jpegTablesMode = atoi(val.c_str()) & 3;
        }
        else if (key == "WEBP_LEVEL")
        {
            if (!val.empty() && !coIsInt(val))
                coUnexpectedSelect("WEBP_LEVEL", val, "int");
            if (!first)
                continue;
            o.webpLevelSpecified = true;
            int lv = atoi(val.c_str());
            if (lv < 1 || lv > 100)
            {
                o.webpLevelBad = true;
                o.webpLevelRaw = val;
            }
            else
                o.webpLevel = lv;
        }
        else if (key == "WEBP_LOSSLESS")
        {
            // recognized by the codec but absent from the driver's
            // declared option list, so it warns yet still applies
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "driver GTiff does not support creation option " +
                            kv.first);
            if (first)
                o.webpLossless = coTestBool(val);
        }
        else if (key == "TILED")
        {
            if (uval != "YES" && uval != "NO" && uval != "TRUE" &&
                uval != "FALSE" && uval != "ON" && uval != "OFF")
                coUnexpectedSelect("TILED", val, "boolean");
            if (first)
                o.tiled = coTestBool(val);
        }
        else if (key == "SPARSE_OK")
        {
            if (uval != "YES" && uval != "NO" && uval != "TRUE" &&
                uval != "FALSE" && uval != "ON" && uval != "OFF")
                coUnexpectedSelect("SPARSE_OK", val, "boolean");
            if (first)
                o.sparse = coTestBool(val);
        }
        else if (key == "BLOCKXSIZE")
        {
            if (!coIsInt(val))
                coUnexpectedSelect("BLOCKXSIZE", val, "int");
            if (first)
                o.blockX = atoi(val.c_str());
        }
        else if (key == "BLOCKYSIZE")
        {
            if (!coIsInt(val))
                coUnexpectedSelect("BLOCKYSIZE", val, "int");
            if (first)
                o.blockY = atoi(val.c_str());
        }
        else if (key == "NBITS")
        {
            if (!coIsInt(val))
                coUnexpectedSelect("NBITS", val, "int");
            if (first)
            {
                o.hasNbits = true;
                o.nbits = atoi(val.c_str());
            }
        }
        else if (key == "PHOTOMETRIC")
        {
            if (uval != "MINISBLACK" && uval != "MINISWHITE" &&
                uval != "PALETTE" && uval != "RGB" && uval != "CMYK" &&
                uval != "YCBCR" && uval != "CIELAB" && uval != "ICCLAB" &&
                uval != "ITULAB")
                coUnexpectedSelect("PHOTOMETRIC", val, "string-select");
            if (first)
            {
                o.hasPhotometric = true;
                o.photometricVal = uval;
                o.photometricRaw = val;
            }
        }
        else if (key == "ENDIANNESS")
        {
            if (uval != "NATIVE" && uval != "INVERTED" &&
                uval != "LITTLE" && uval != "BIG")
                coUnexpectedSelect("ENDIANNESS", val, "string-select");
            if (first)
            {
                o.hasEndian = true;
                o.endianVal = uval;
            }
        }
        else if (key == "GEOTIFF_VERSION")
        {
            if (uval != "AUTO" && uval != "1.0" && uval != "1.1")
                coUnexpectedSelect("GEOTIFF_VERSION", val,
                                   "string-select");
            if (first)
                o.gtVersion = uval == "1.0" ? 10 : uval == "1.1" ? 11 : 0;
        }
        else if (key == "INTERLEAVE")
        {
            if (uval != "BAND" && uval != "PIXEL")
            {
                coUnexpectedSelect("INTERLEAVE", val, "string-select");
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            fnameOf(filename) + ": INTERLEAVE=" + val +
                                " unsupported, value must be PIXEL or "
                                "BAND.");
                o.fatal = true;
            }
            else if (first)
            {
                o.bandInterleave = uval == "BAND";
                o.interleaveSet = true;
            }
        }
        else if (key == "PROFILE")
        {
            if (uval != "GDALGEOTIFF" && uval != "GEOTIFF" &&
                uval != "BASELINE")
            {
                coUnexpectedSelect("PROFILE", val, "string-select");
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "Unsupported value for PROFILE: " + val);
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "Unsupported value for PROFILE: " + val);
            }
            else if (first)
                o.profile = uval == "GDALGEOTIFF" ? "GDALGeoTIFF"
                            : uval == "GEOTIFF"   ? "GeoTIFF"
                                                  : "BASELINE";
        }
        else if (key == "BIGTIFF")
        {
            if (uval != "YES" && uval != "NO" && uval != "IF_NEEDED" &&
                uval != "IF_SAFER")
                coUnexpectedSelect("BIGTIFF", val, "string-select");
            if (first)
            {
                if (uval == "IF_NEEDED")
                    o.bigtiffMode = 2;
                else if (uval == "IF_SAFER")
                    o.bigtiffMode = 3;
                else
                    o.bigtiffMode = coTestBool(val) ? 1 : 0;
            }
        }
        else if (key == "NUM_THREADS" || key == "GEOTIFF_KEYS_FLAVOR")
        {
            // accepted silently, no observable effect for created outputs
        }
        else
        {
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "driver GTiff does not support creation option " +
                            kv.first);
        }
    }
    return o;
}

// driver diagnostics carry CPLGetFilename() of the dataset, so any
// directory part of the output path is stripped from messages
std::string fnameOf(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

bool finalizeCreationOptions(CreationOptions &o,
                             const std::string &outputPath, int bands,
                             DType type, bool isConvert)
{
    const std::string output = fnameOf(outputPath);
    // block geometry checks come first in the reference driver
    if (o.tiled)
    {
        int bx = o.blockX > 0 ? o.blockX : 256;
        int by = o.blockY > 0 ? o.blockY : 256;
        if (bx % 16)
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        output + ": BLOCKXSIZE must be a multiple of 16");
            return false;
        }
        if (by % 16)
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        output + ": BLOCKYSIZE must be a multiple of 16");
            return false;
        }
        o.blockXFinal = bx;
        o.blockYFinal = by;
    }
    else
    {
        if (o.blockX > 0)
            cplErrorStr(CE_Warning, CPLE_IllegalArg,
                        output +
                            ": BLOCKXSIZE can only be used with TILED=YES");
        o.blockYFinal = o.blockY;
    }

    if (o.missingCodec)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Cannot create TIFF file due to missing codec for " +
                        o.compressRaw + ".");
        return false;
    }
    if (o.compressBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "COMPRESS=" + o.compressRaw +
                        " value not recognised, ignoring.");

    // NBITS resolution
    if (o.hasNbits)
    {
        int lo = 0, hi = 0;
        switch (type)
        {
            case DType::Byte:
                lo = 1;
                hi = 8;
                break;
            case DType::UInt16:
                lo = 9;
                hi = 16;
                break;
            case DType::UInt32:
                lo = 17;
                hi = 32;
                break;
            default:
                break;
        }
        if (lo)
        {
            int n = o.nbits;
            if (n < lo || n > hi)
            {
                int used = n < lo ? lo : hi;
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            strPrintf("%s: NBITS=%d is invalid for data "
                                      "type %s. Using NBITS=%d",
                                      output.c_str(), n,
                                      dtypeName(type), used));
                n = used;
            }
            o.nbitsFinal = n == hi ? 0 : n;
        }
        else if (type == DType::Float32)
        {
            if (o.nbits == 16)
                o.halfFloat = true;
            else
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            output + ": Only NBITS=16 is supported for "
                                     "data type Float32");
        }
        else
        {
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        output + ": NBITS is not supported for data type " +
                            dtypeName(type));
        }
    }

    if (o.hasPredictor)
    {
        if (o.compression != 5 && o.compression != 8 &&
            o.compression != 50000)
        {
            cplErrorStr(
                CE_Warning, CPLE_NotSupported,
                output + ": PREDICTOR option is ignored for COMPRESS=" +
                    (o.compressBad
                         ? o.compressRaw
                         : std::string(o.compression == 32773 ? "PACKBITS"
                                       : o.compression == 7   ? "JPEG"
                                       : o.compression == 50001 ? "WEBP"
                                                                : "NONE")) +
                    ". Only valid for DEFLATE, LZW, LZMA or ZSTD");
        }
        else if (o.predictor == 3)
        {
            if (type != DType::Float16 && type != DType::Float32 &&
                type != DType::Float64)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            output + ": PREDICTOR=3 is only supported "
                                     "with Float16, Float32 or Float64.");
                return false;
            }
            o.predictorFinal = 3;
        }
        else if (o.predictor == 1 || o.predictor == 2)
        {
            if (o.predictor == 2 && dtypeSizeBytes(type) == 8)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            output + ": PREDICTOR=2 is supported on 64 "
                                     "bit samples starting with libtiff "
                                     "> 4.3.0.");
                return false;
            }
            o.predictorFinal = o.predictor;
        }
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        output + ": PREDICTOR=" + o.predictorRaw +
                            " is not supported.");
            return false;
        }
    }

    if (o.zlevelBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "ZLEVEL=" + o.zlevelRaw +
                        " value not recognised, ignoring.");
    if (o.zstdBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "ZSTD_LEVEL=" + o.zstdRaw +
                        " value not recognised, ignoring.");
    if (o.webpLevelBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "WEBP_LEVEL=" + o.webpLevelRaw +
                        " value not recognised, ignoring.");
    if (o.jpegQualityBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "JPEG_QUALITY=" + o.jpegQualityRaw +
                        " value not recognised, ignoring.");
    // the codec's own level fetch when a lossy WEBP directory actually
    // consumes the option, after all first-fetch warnings
    if (o.webpLevelBad && o.compression == 50001 && !o.webpLossless)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "WEBP_LEVEL=" + o.webpLevelRaw +
                        " value not recognised, ignoring.");

    if (o.hasEndian)
    {
        if (o.endianVal == "BIG" || o.endianVal == "INVERTED")
            o.endianBig = true;
        else if (o.endianVal != "LITTLE" && o.endianVal != "NATIVE")
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        output + ": ENDIANNESS=" + o.endianVal +
                            " not supported. Defaulting to NATIVE");
    }

    // PHOTOMETRIC resolution
    if (o.hasPhotometric)
    {
        const std::string &v = o.photometricVal;
        int phot = -1, channels = 1;
        if (v == "MINISBLACK")
            phot = 1;
        else if (v == "MINISWHITE")
            phot = 0;
        else if (v == "PALETTE")
            phot = 3;
        else if (v == "RGB")
        {
            phot = 2;
            channels = 3;
        }
        else if (v == "CMYK")
        {
            phot = 5;
            channels = 4;
        }
        else if (v == "YCBCR")
        {
            phot = 6;
            channels = 3;
        }
        else if (v == "CIELAB")
        {
            phot = 8;
            channels = 3;
        }
        else if (v == "ICCLAB")
        {
            phot = 9;
            channels = 3;
        }
        else if (v == "ITULAB")
        {
            phot = 10;
            channels = 3;
        }

        if (phot < 0)
        {
            cplErrorStr(CE_Warning, CPLE_IllegalArg,
                        output + ": PHOTOMETRIC=" + o.photometricRaw +
                            " value not recognised, ignoring.  Set the "
                            "Photometric Interpretation as MINISBLACK.");
            o.resolvedPhot = 1;
            o.extrasSet = true;
            for (int i = 1; i < bands; i++)
                o.extras.push_back(0);
        }
        else if (phot == 6 && o.compression != 7)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        output + ": Currently, PHOTOMETRIC=YCBCR "
                                 "requires COMPRESS=JPEG");
            return false;
        }
        else if (phot == 6 && bands != 3)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        strPrintf("%s: PHOTOMETRIC=YCBCR not supported "
                                  "on a %d-band raster: only compatible "
                                  "of a 3-band (RGB) raster",
                                  output.c_str(), bands));
            return false;
        }
        else if (phot == 3 &&
                 type != DType::Byte && type != DType::UInt16)
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        output + ": PHOTOMETRIC=PALETTE only compatible "
                                 "with Byte or UInt16");
            o.photOmit = true;
            o.extrasSet = true;
            for (int i = 1; i < bands; i++)
                o.extras.push_back(0);
        }
        else if (bands < channels)
        {
            cplErrorStr(CE_Warning, CPLE_IllegalArg,
                        strPrintf("%s: PHOTOMETRIC=%s value does not "
                                  "correspond to number of bands (%d), "
                                  "ignoring.  Set the Photometric "
                                  "Interpretation as MINISBLACK.",
                                  output.c_str(), o.photometricRaw.c_str(),
                                  bands));
            o.resolvedPhot = 1;
            o.extrasSet = true;  // no ExtraSamples tag at all
            o.sumPhotWarn = bands > 1;
        }
        else
        {
            o.resolvedPhot = phot;
            o.photApplied = true;
            o.extrasSet = true;
            for (int i = channels; i < bands; i++)
                o.extras.push_back(0);
            if (phot == 3)
                o.synthPalette = true;
            if (phot == 0 || phot == 5 || phot >= 8)
                o.gmdColorinterp = true;
        }
    }

    // the mismatched-photometric libtiff warning fires on directory
    // re-read: CreateCopy re-reads twice before the second option fetch,
    // Create once after it
    auto sumWarn = []()
    {
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "TIFFReadDirectory:Sum of Photometric type-related "
                    "color channels and ExtraSamples doesn't match "
                    "SamplesPerPixel. Defining non-color channels as "
                    "ExtraSamples.");
    };
    if (isConvert && o.sumPhotWarn)
    {
        sumWarn();
        sumWarn();
    }
    if (o.zlevelBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "ZLEVEL=" + o.zlevelRaw +
                        " value not recognised, ignoring.");
    if (o.zstdBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "ZSTD_LEVEL=" + o.zstdRaw +
                        " value not recognised, ignoring.");
    if (o.webpLevelBad)
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "WEBP_LEVEL=" + o.webpLevelRaw +
                        " value not recognised, ignoring.");
    if (o.jpegQualityBad)
    {
        cplErrorStr(CE_Warning, CPLE_IllegalArg,
                    "JPEG_QUALITY=" + o.jpegQualityRaw +
                        " value not recognised, ignoring.");
        // the in-memory JPEGTables dataset fetches the option once more
        if (o.compression == 7)
            cplErrorStr(CE_Warning, CPLE_IllegalArg,
                        "JPEG_QUALITY=" + o.jpegQualityRaw +
                            " value not recognised, ignoring.");
    }
    if (o.compression == 50001 && o.webpLossless && o.webpLevelSpecified)
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "WEBP_LEVEL is specified, but WEBP_LOSSLESS=YES. "
                    "WEBP_LEVEL will be ignored.");
    if (!isConvert && o.sumPhotWarn)
        sumWarn();
    return true;
}



// ------------------------------------------------------------------
// main entry
// ------------------------------------------------------------------

bool gtiffWrite(const std::string &path, const GTiffCreateParams &p,
                std::string &err)
{
    if (strncmp(path.c_str(), "/vsizip/", 8) == 0)
    {
        const int probe = vsiZipWriteProbe(path);
        if (probe == 1)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Random access not supported for writable file in "
                        "/vsizip");
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "Attempt to create new tiff file `" + path +
                        "' failed");
        if (probe == 1)
            vsiZipTouchArchive(path);
        err = "reported";
        return false;
    }
    if (strncmp(path.c_str(), "/vsitar/", 8) == 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Only read-only mode is supported for /vsitar");
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "Attempt to create new tiff file `" + path +
                        "' failed");
        err = "reported";
        return false;
    }
    const int bpp = dtypeSizeBytes(p.type);
    const bool planar = p.bandInterleave && p.bands > 1;
    const int planes = planar ? p.bands : 1;
    const int sppPerPlane = planar ? 1 : p.bands;

    bool bigtiff = p.bigtiff;
    if (p.append)
    {
        std::string hdr;
        if (readFileToString(path, hdr) && hdr.size() >= 4)
            bigtiff = (uint8_t)hdr[2] == 43;
    }

    // photometric / extra samples
    uint16_t photometric = 1;
    std::vector<uint16_t> extras;
    if (p.photometric >= 0)
    {
        photometric = (uint16_t)p.photometric;
        if (p.extrasSet)
            extras = p.extraSamples;
    }
    else if (p.type == DType::Byte && (p.bands == 3 || p.bands == 4))
    {
        photometric = 2;
        if (p.bands == 4)
            extras.push_back(2);  // unassociated alpha
    }
    else if (p.bands > 1)
    {
        extras.assign(p.bands - 1, 0);
    }

    // block geometry
    int rps = 0;
    int tilesX = 1, tilesY = 1, blocksPerPlane, blockW, blockH;
    if (p.tiled)
    {
        blockW = p.blockX > 0 ? p.blockX : 256;
        blockH = p.blockY > 0 ? p.blockY : 256;
        tilesX = (p.width + blockW - 1) / blockW;
        tilesY = (p.height + blockH - 1) / blockH;
        blocksPerPlane = tilesX * tilesY;
    }
    else
    {
        long scanline =
            p.nbits > 0
                ? (long)(((int64_t)p.width * sppPerPlane * p.nbits + 7) / 8)
                : (long)p.width * bpp * sppPerPlane;
        if (p.blockY > 0)
            rps = p.blockY;
        else if (p.compression == 7)
        {
            // libtiff JPEGDefaultStripSize: the usual 8k target rounded
            // up to a multiple of 16, on the subsampled scanline
            long sl = scanline;
            if (photometric == 6 && sppPerPlane == 3)
                sl = (long)((p.width + 1) & ~1) * 3 / 2;
            rps = sl > 0 ? (int)(8192 / sl) : 1;
            if (rps < 1)
                rps = 1;
            rps = (rps + 15) / 16 * 16;
        }
        else
        {
            rps = scanline > 0 ? (int)(8192 / scanline) : 1;
            if (rps < 1)
                rps = 1;
        }
        if (rps > p.height)
            rps = p.height;
        blockW = p.width;
        blockH = rps;
        blocksPerPlane = (p.height + rps - 1) / rps;
    }
    const int nblocks = blocksPerPlane * planes;

    // sample data. Explicit non-zero burns go through the block-write
    // path, which trims the last strip. A nodata-only fill goes through
    // the empty-tile filler: it trims when uncompressed, but for
    // compressed output it compresses one full-size blank block and
    // reuses the blob for every strip. All-zero content is written with
    // full-size strips (or skipped entirely when SPARSE_OK).
    std::vector<double> fill = p.burn;
    const bool fromNodata = fill.empty() && p.hasNodata && !p.pixels;
    if (fromNodata)
        fill.assign(1, p.nodata);
    auto pixelBytesFor = [&](const std::vector<double> &vals)
    {
        std::vector<uint8_t> px((size_t)bpp * p.bands, 0);
        for (int b = 0; b < p.bands; b++)
        {
            double v = vals.size() == 1 ? vals[0] : vals[(size_t)b];
            writeSampleValue(px, (size_t)b * bpp, p.type,
                             clampToType(p.type, v));
        }
        return px;
    };
    bool dirty = false;
    if (!fill.empty())
        for (uint8_t c : pixelBytesFor(fill))
            if (c)
                dirty = true;
    bool sparseSkip = !dirty;
    if (dirty && p.hasNodata &&
        pixelBytesFor(fill) == pixelBytesFor({p.nodata}))
        sparseSkip = true;
    const bool writeData =
        !p.jpegStub && (p.pixels || !p.sparse || !sparseSkip);
    const bool trimLast =
        p.pixels ? !p.tiled
                 : !p.tiled && dirty && (!fromNodata || p.compression == 1);
    // CreateCopy per-block empty handling: a block whose samples all
    // equal the nodata value (or zero without nodata) is skipped when
    // SPARSE_OK; otherwise, when uncompressed, it is deferred to the
    // empty-block filler which appends it after the real blocks (zero
    // fill full-size, nodata fill trimmed)
    std::vector<uint8_t> emptyPat((size_t)bpp, 0);
    if (p.hasNodata)
        writeSampleValue(emptyPat, 0, p.type, clampToType(p.type, p.nodata));
    bool patternZero = true;
    for (uint8_t c : emptyPat)
        if (c)
            patternZero = false;
    std::vector<int> blockClass(nblocks, 0);  // 0 real, 1 defer, 2 skip
    std::vector<std::vector<uint8_t>> blocks;
    int elemBytes = bpp;
    if (p.type == DType::CInt16 || p.type == DType::CInt32 ||
        p.type == DType::CFloat32 || p.type == DType::CFloat64)
        elemBytes = bpp / 2;
    bool clipWarned = false;
    if (p.compression == 7 && (!p.jpegStub || p.jpegStubTables) &&
        jpegCoarseTables(photometric, sppPerPlane, p.jpegQuality))
        jpegCoarseWarn();
    // the zero/nodata empty-block filler compresses one blank block and
    // reuses the blob, so its JPEG diagnostics fire only once
    std::vector<uint8_t> jpegFillCache;
    bool jpegFillCached = false;
    const bool jpegFillReuse =
        p.compression == 7 && !p.pixels && !(dirty && !fromNodata);
    if (writeData)
    {
        LzwEncoder lzw;
        blocks.reserve(nblocks);
        for (int plane = 0; plane < planes; plane++)
        {
            for (int bi = 0; bi < blocksPerPlane; bi++)
            {
                int rows = blockH;
                if (trimLast && bi == blocksPerPlane - 1)
                    rows = p.height - (blocksPerPlane - 1) * rps;
                size_t rowBytes = (size_t)blockW * bpp * sppPerPlane;
                std::vector<uint8_t> raw(rowBytes * rows, 0);
                if (p.pixels)
                {
                    int bx0 = 0, by0 = 0;
                    if (p.tiled)
                    {
                        bx0 = (bi % tilesX) * blockW;
                        by0 = (bi / tilesX) * blockH;
                    }
                    else
                        by0 = bi * rps;
                    int copyRows = std::min(rows, p.height - by0);
                    int copyCols = std::min(blockW, p.width - bx0);
                    for (int s = 0; s < sppPerPlane; s++)
                    {
                        int band = planar ? plane : s;
                        const std::vector<uint8_t> &src = (*p.pixels)[band];
                        for (int r = 0; r < copyRows; r++)
                        {
                            const uint8_t *sp =
                                src.data() +
                                ((size_t)(by0 + r) * p.width + bx0) * bpp;
                            uint8_t *dp = raw.data() + (size_t)r * rowBytes;
                            if (sppPerPlane == 1)
                                memcpy(dp, sp, (size_t)copyCols * bpp);
                            else
                                for (int x = 0; x < copyCols; x++)
                                    memcpy(dp + ((size_t)x * sppPerPlane +
                                                 s) *
                                                    bpp,
                                           sp + (size_t)x * bpp, bpp);
                        }
                    }
                    if (p.compression == 7 && p.tiled &&
                        (copyCols < blockW || copyRows < rows) &&
                        copyCols > 0 && copyRows > 0)
                    {
                        // GTiff WriteEncodedTile pads partial JPEG tiles
                        // by replicating the last column, then last row
                        size_t pxb = (size_t)bpp * sppPerPlane;
                        for (int r = 0; r < copyRows; r++)
                        {
                            uint8_t *rowp =
                                raw.data() + (size_t)r * rowBytes;
                            const uint8_t *lastPx =
                                rowp + (size_t)(copyCols - 1) * pxb;
                            for (int x = copyCols; x < blockW; x++)
                                memcpy(rowp + (size_t)x * pxb, lastPx,
                                       pxb);
                        }
                        for (int r = copyRows; r < rows; r++)
                            memcpy(raw.data() + (size_t)r * rowBytes,
                                   raw.data() +
                                       (size_t)(copyRows - 1) * rowBytes,
                                   rowBytes);
                    }
                    bool empty = true;
                    size_t pxBytes = (size_t)bpp * sppPerPlane;
                    for (int r = 0; r < copyRows && empty; r++)
                    {
                        const uint8_t *rowp =
                            raw.data() + (size_t)r * rowBytes;
                        size_t nb = (size_t)copyCols * pxBytes;
                        for (size_t i = 0; i < nb; i++)
                            if (rowp[i] != emptyPat[i % (size_t)bpp])
                            {
                                empty = false;
                                break;
                            }
                    }
                    int cls = 0;
                    if (empty && !p.gridOrphanIfd)
                    {
                        if (p.sparse)
                            cls = 2;
                        else if (p.compression == 1)
                            cls = 1;
                    }
                    blockClass[plane * blocksPerPlane + bi] = cls;
                    if (cls == 2)
                    {
                        blocks.emplace_back();
                        continue;
                    }
                    if (cls == 1)
                    {
                        if (patternZero)
                        {
                            if (rows != blockH)
                                raw.assign(rowBytes * blockH, 0);
                        }
                        else
                            for (size_t i = 0; i < raw.size(); i++)
                                raw[i] = emptyPat[i % (size_t)bpp];
                    }
                }
                else if (!fill.empty())
                {
                    std::vector<uint8_t> pixel((size_t)bpp * sppPerPlane);
                    for (int s = 0; s < sppPerPlane; s++)
                    {
                        int band = planar ? plane : s;
                        double v = fill.size() == 1 ? fill[0]
                                                    : fill[(size_t)band];
                        writeSampleValue(pixel, (size_t)s * bpp, p.type,
                                         clampToType(p.type, v));
                    }
                    size_t npix = (size_t)blockW * rows;
                    for (size_t i = 0; i < npix; i++)
                        memcpy(&raw[i * pixel.size()], pixel.data(),
                               pixel.size());
                }

                if (p.nbits > 0)
                {
                    size_t rowSamples = (size_t)blockW * sppPerPlane;
                    size_t prb = (rowSamples * p.nbits + 7) / 8;
                    size_t nrows = raw.size() / rowBytes;
                    const uint32_t maxv =
                        p.nbits >= 32 ? 0xffffffffu
                                      : ((1u << p.nbits) - 1);
                    std::vector<uint8_t> packed(prb * nrows, 0);
                    for (size_t r = 0; r < nrows; r++)
                    {
                        size_t base = r * prb * 8;
                        for (size_t s = 0; s < rowSamples; s++)
                        {
                            uint32_t v = 0;
                            const uint8_t *sp =
                                raw.data() + (r * rowSamples + s) * bpp;
                            for (int k = 0; k < bpp && k < 4; k++)
                                v |= (uint32_t)sp[k] << (8 * k);
                            if (v > maxv)
                            {
                                v = maxv;
                                if (!clipWarned)
                                {
                                    clipWarned = true;
                                    // the copy-phase error snapshot only
                                    // trips the exit code when the clip
                                    // is the first diagnostic of the run
                                    if (p.clipNote && cplErrorSeq() == 0)
                                        *p.clipNote = true;
                                    cplErrorStr(
                                        CE_Warning, CPLE_AppDefined,
                                        strPrintf(
                                            "%s, band %d: One or more "
                                            "pixels clipped to fit %d "
                                            "bit domain.",
                                            fnameOf(path).c_str(),
                                            planar ? plane + 1 : 1,
                                            p.nbits));
                                }
                            }
                            size_t bit = base + s * p.nbits;
                            for (int k = p.nbits - 1; k >= 0; --k, ++bit)
                                if ((v >> k) & 1)
                                    packed[bit >> 3] |=
                                        (uint8_t)(0x80 >> (bit & 7));
                        }
                    }
                    raw.swap(packed);
                    rowBytes = prb;
                }

                if (p.predictor >= 2)
                {
                    size_t nrows = raw.size() / rowBytes;
                    for (size_t r = 0; r < nrows; r++)
                    {
                        std::vector<uint8_t> row(
                            raw.begin() + r * rowBytes,
                            raw.begin() + (r + 1) * rowBytes);
                        if (p.predictor == 3)
                            fpDiff(row, bpp, sppPerPlane, p.bigEndian);
                        else
                            horDiff(row, bpp, sppPerPlane);
                        memcpy(&raw[r * rowBytes], row.data(), rowBytes);
                    }
                }
                if (p.bigEndian && p.nbits == 0 && elemBytes > 1 &&
                    p.predictor != 3)
                    for (size_t i = 0; i + elemBytes <= raw.size();
                         i += elemBytes)
                        std::reverse(raw.begin() + i,
                                     raw.begin() + i + elemBytes);

                switch (p.compression)
                {
                    case 7:  // JPEG
                    {
                        if (jpegFillReuse && jpegFillCached)
                        {
                            blocks.push_back(jpegFillCache);
                            break;
                        }
                        int rows = (int)(raw.size() / rowBytes);
                        blocks.push_back(jpegBlock(
                            raw, blockW, rows, sppPerPlane, photometric,
                            p.bandInterleave ? plane : -1,
                            p.jpegQuality, true, p.jpegTablesMode));
                        if (jpegFillReuse)
                        {
                            jpegFillCache = blocks.back();
                            jpegFillCached = true;
                        }
                        break;
                    }
                    case 8:  // DEFLATE
                        blocks.push_back(deflateBlock(raw, p.zlevel));
                        break;
                    case 50000:  // ZSTD
                        blocks.push_back(zstdBlock(raw, p.zstdLevel));
                        break;
                    case 50001:  // WEBP
                    {
                        int rows = (int)(raw.size() / rowBytes);
                        if (planar)
                        {
                            // libtiff keeps nSamples in the segment
                            // buffer for separate planes; the plane
                            // fills its head and the tail stays zero
                            std::vector<uint8_t> buf(
                                (size_t)blockW * rows * p.bands, 0);
                            memcpy(buf.data(), raw.data(), raw.size());
                            blocks.push_back(webpEncodeBlock(
                                buf.data(), blockW, rows, p.bands,
                                p.webpLevel, p.webpLossless));
                        }
                        else
                            blocks.push_back(webpEncodeBlock(
                                raw.data(), blockW, rows, sppPerPlane,
                                p.webpLevel, p.webpLossless));
                        break;
                    }
                    case 5:  // LZW
                        blocks.push_back(lzw.encode(raw.data(), raw.size()));
                        break;
                    case 32773:  // PACKBITS
                    {
                        std::vector<uint8_t> outb;
                        size_t nrows = raw.size() / rowBytes;
                        for (size_t r = 0; r < nrows; r++)
                            packBitsRow(raw.data() + r * rowBytes,
                                        (long)rowBytes, outb);
                        blocks.push_back(std::move(outb));
                        break;
                    }
                    default:
                        blocks.push_back(std::move(raw));
                        break;
                }
            }
        }
    }

    uint16_t sampleFormat = 1;
    switch (p.type)
    {
        case DType::Int8:
        case DType::Int16:
        case DType::Int32:
        case DType::Int64:
            sampleFormat = 2;
            break;
        case DType::Float16:
        case DType::Float32:
        case DType::Float64:
            sampleFormat = 3;
            break;
        case DType::CInt16:
        case DType::CInt32:
            sampleFormat = 5;
            break;
        case DType::CFloat32:
        case DType::CFloat64:
            sampleFormat = 6;
            break;
        default:
            sampleFormat = 1;
            break;
    }

    const bool geoProfile = p.profile != "BASELINE";
    const bool gdalMetaInTiff = p.profile == "GDALGeoTIFF";

    GeoTags geo;
    if (geoProfile)
    {
        if (p.srs && p.srs->valid())
            buildGeoTags(*p.srs, geo, p.pointPixel, false, p.gtVersion);
        else if (p.gcpSrs && p.gcpSrs->valid())
            buildGeoTags(*p.gcpSrs, geo, p.pointPixel, false,
                         p.gtVersion);
        else if (p.forceGeoDir)
        {
            Srs empty;
            buildGeoTags(empty, geo, p.pointPixel, true, p.gtVersion);
        }
    }
    const bool geoOrphanApplied = p.geoDoubleOrphan && geo.any;
    if (geoOrphanApplied)
    {
        geo.doubles.insert(geo.doubles.begin(), 0.0);
        for (size_t i = 4; i + 3 < geo.dir.size(); i += 4)
            if (geo.dir[i + 1] == 34736)
                geo.dir[i + 3] += 1;
    }

    // assemble tags in libtiff data placement order
    std::vector<OutTag> tags;
    tags.push_back(mkShortLong(256, (uint32_t)p.width));
    tags.push_back(mkShortLong(257, (uint32_t)p.height));
    if (p.hasXRes || p.hasYRes)
    {
        tags.push_back(mkRationalF(282, p.xres));
        tags.push_back(mkRationalF(283, p.yres));
    }
    tags.push_back(mkShorts(
        258, std::vector<uint16_t>(
                 p.bands,
                 (uint16_t)(p.nbits > 0 ? p.nbits : bpp * 8))));
    tags.push_back(mkShort(259, (uint16_t)p.compression));
    if (!p.omitPhotometric)
        tags.push_back(mkShort(262, photometric));
    tags.push_back(mkShort(277, (uint16_t)p.bands));
    if (!p.tiled)
        tags.push_back(mkShortLong(278, (uint32_t)rps));
    if (p.minSample >= 0)
        tags.push_back(mkShort(280, (uint16_t)p.minSample));
    if (p.maxSample >= 0)
        tags.push_back(mkShort(281, (uint16_t)p.maxSample));
    tags.push_back(mkShort(284, p.bandInterleave ? 2 : 1));
    if (p.resUnit > 0)
        tags.push_back(mkShort(296, (uint16_t)p.resUnit));
    if (p.tiled)
    {
        tags.push_back(mkShortLong(322, (uint32_t)blockW));
        tags.push_back(mkShortLong(323, (uint32_t)blockH));
    }

    // byte counts then offsets (placeholder offsets patched later)
    std::vector<uint32_t> counts(nblocks, 0);
    if (writeData)
        for (int i = 0; i < nblocks; i++)
            counts[i] = (uint32_t)blocks[i].size();
    uint32_t maxCount = 0;
    for (uint32_t c : counts)
        if (c > maxCount)
            maxCount = c;
    // libtiff sizes the byte-count entry before compressing: SHORT only
    // when the worst-case compressed block (raw + raw/4 + 2) fits the
    // 8192-byte write buffer; PackBits never qualifies.
    const size_t fullRaw =
        p.nbits > 0
            ? ((size_t)blockW * sppPerPlane * p.nbits + 7) / 8 * blockH
            : (size_t)blockW * blockH * bpp * sppPerPlane;
    bool countsShort = false;
    if (nblocks > 1)
    {
        if (p.compression == 1)
            countsShort = maxCount <= 0xffff;
        else if (p.compression == 5 || p.compression == 7 ||
                 p.compression == 8 || p.compression == 50000 ||
                 p.compression == 50001)
            countsShort = fullRaw + fullRaw / 4 + 2 <= 8192;
    }
    OutTag countTag;
    if (p.jpegStubNoStrips)
    {
        // strile arrays were still deferred when the copy failed
    }
    else if (countsShort)
    {
        std::vector<uint16_t> sc(counts.begin(), counts.end());
        countTag = mkShorts(p.tiled ? 325 : 279, sc);
    }
    else if (bigtiff &&
             (nblocks <= 1 ||
              !(p.compression == 1 || p.compression == 5 ||
                p.compression == 7 || p.compression == 8 ||
                p.compression == 50000 || p.compression == 50001)))
    {
        // BigTIFF byte counts stay LONG8 unless the classic estimate
        // path sized them (single block and PackBits never estimate)
        countTag.tag = p.tiled ? 325 : 279;
        countTag.type = 16;
        countTag.count = (uint32_t)nblocks;
        for (uint32_t c : counts)
            put64(countTag.data, c);
    }
    else
        countTag = mkLongs(p.tiled ? 325 : 279, counts);
    size_t offsetsTagIndex = (size_t)-1;
    if (!p.jpegStubNoStrips)
    {
        tags.push_back(std::move(countTag));
        offsetsTagIndex = tags.size();
        if (bigtiff)
        {
            OutTag ot;
            ot.tag = p.tiled ? 324 : 273;
            ot.type = 16;
            ot.count = (uint32_t)nblocks;
            ot.data.assign((size_t)nblocks * 8, 0);
            tags.push_back(std::move(ot));
        }
        else
            tags.push_back(mkLongs(p.tiled ? 324 : 273,
                                   std::vector<uint32_t>(nblocks, 0)));
    }

    if (p.colorTable)
    {
        int bits = p.nbits > 0 ? p.nbits : (bpp * 8 > 8 ? 16 : 8);
        size_t nEntries = (size_t)1 << bits;
        if (p.colorTable->size() > nEntries)
            nEntries = p.colorTable->size();
        std::vector<uint16_t> cmap(nEntries * 3, 0);
        for (size_t i = 0; i < p.colorTable->size() && i < nEntries; i++)
        {
            const ColorEntry &e = (*p.colorTable)[i];
            const int comps[3] = {e.c1, e.c2, e.c3};
            for (int j = 0; j < 3; j++)
            {
                int v = comps[j] * 257;
                if (v > 65535)
                {
                    cplError(CE_Warning, CPLE_AppDefined,
                             "Color table entry [%d][%d] = %d, clamped to "
                             "65535",
                             (int)i, j + 1, comps[j]);
                    v = 65535;
                }
                else if (v < 0)
                {
                    cplError(CE_Warning, CPLE_AppDefined,
                             "Color table entry [%d][%d] = %d, clamped to "
                             "0",
                             (int)i, j + 1, comps[j]);
                    v = 0;
                }
                cmap[(size_t)j * nEntries + i] = (uint16_t)v;
            }
        }
        tags.push_back(mkShorts(320, cmap));
    }
    else if (p.synthPalette)
    {
        // PHOTOMETRIC=PALETTE without a source table: grayscale ramp,
        // 8-bit-or-less entries scaled by 257, wider depths raw index
        int bits = p.nbits > 0 ? p.nbits : bpp * 8;
        size_t nEntries = (size_t)1 << bits;
        std::vector<uint16_t> cmap(nEntries * 3, 0);
        for (size_t i = 0; i < nEntries; i++)
        {
            uint16_t v = (uint16_t)(bits <= 8 ? i * 257 : i);
            cmap[i] = v;
            cmap[nEntries + i] = v;
            cmap[2 * nEntries + i] = v;
        }
        tags.push_back(mkShorts(320, cmap));
    }
    if (p.compression == 5 || p.compression == 8 ||
        p.compression == 50000)
        tags.push_back(
            mkShort(317, (uint16_t)(p.predictor >= 2 ? p.predictor : 1)));
    if (!extras.empty())
        tags.push_back(mkShorts(338, extras));
    tags.push_back(
        mkShorts(339, std::vector<uint16_t>(p.bands, sampleFormat)));
    if (p.compression == 7 && (!p.jpegStub || p.jpegStubTables))
    {
        if (photometric == 6 && !p.jpegStub)
        {
            tags.push_back(mkShorts(530, {2, 2}));
            OutTag rb;
            rb.tag = 532;
            rb.type = 5;
            rb.count = 6;
            const uint32_t refbw[12] = {0,   1, 255, 1, 128, 1,
                                        255, 1, 128, 1, 255, 1};
            for (uint32_t v : refbw)
                put32(rb.data, v);
            tags.push_back(std::move(rb));
        }
        if (p.jpegTablesMode & 3)
        {
            std::vector<std::vector<uint16_t>> qts;
            qts.push_back(jpegQuantTable(false, p.jpegQuality));
            if (photometric == 6 && !p.jpegStub)
                qts.push_back(jpegQuantTable(true, p.jpegQuality));
            OutTag jt;
            jt.tag = 347;
            jt.type = 7;
            jt.data = jpegTablesStream(qts, (p.jpegTablesMode & 2) != 0,
                                       (p.jpegTablesMode & 1) != 0);
            jt.count = (uint32_t)jt.data.size();
            tags.push_back(std::move(jt));
        }
    }
    for (const auto &at : p.asciiTags)
        tags.push_back(mkAscii(at.first, at.second));

    // custom tags in GDAL set order
    auto xesc = [](const std::string &s)
    {
        std::string r;
        for (char c : s)
        {
            switch (c)
            {
                case '<':
                    r += "&lt;";
                    break;
                case '>':
                    r += "&gt;";
                    break;
                case '&':
                    r += "&amp;";
                    break;
                case '"':
                    r += "&quot;";
                    break;
                default:
                    r += c;
            }
        }
        return r;
    };
    std::string webpItem;
    if (p.compression == 50001)
        webpItem =
            p.webpLossless
                ? std::string("  <Item name=\"COMPRESSION_REVERSIBILITY\" "
                              "domain=\"IMAGE_STRUCTURE\">LOSSLESS"
                              "</Item>\n")
                : strPrintf("  <Item name=\"WEBP_LEVEL\" "
                            "domain=\"IMAGE_STRUCTURE\">%d</Item>\n",
                            p.webpLevel);
    if (gdalMetaInTiff && p.useGmdItems)
    {
        if (!p.gmdItems.empty() || !webpItem.empty())
        {
            std::string xml = "<GDALMetadata>\n";
            for (const auto &it : p.gmdItems)
            {
                xml += "  <Item name=\"" + xesc(it.name) + "\"";
                if (it.sample >= 0)
                    xml += strPrintf(" sample=\"%d\"", it.sample);
                if (!it.role.empty())
                    xml += " role=\"" + it.role + "\"";
                if (!it.domain.empty())
                    xml += " domain=\"" + xesc(it.domain) + "\"";
                xml += ">" + xesc(xesc(it.value)) + "</Item>\n";
            }
            xml += webpItem;
            xml += "</GDALMetadata>\n";
            tags.push_back(mkAscii(42112, xml));
        }
    }
    else if (gdalMetaInTiff && (!p.metadata.empty() || !webpItem.empty()))
    {
        std::string xml = "<GDALMetadata>\n";
        for (const auto &kv : p.metadata)
            xml += "  <Item name=\"" + xesc(kv.first) + "\">" +
                   xesc(xesc(kv.second)) + "</Item>\n";
        xml += webpItem;
        xml += "</GDALMetadata>\n";
        tags.push_back(mkAscii(42112, xml));
    }
    // CreateCopy flows set GDAL_NODATA when the bands are created, so
    // its string payload precedes every georeferencing payload; warp
    // flows set it afterwards
    auto pushNodataTag = [&tags, &p]()
    {
        if (p.hasNodata)
            tags.push_back(mkAscii(42113,
                                   p.nodataText.empty()
                                       ? strPrintf("%.17g", p.nodata)
                                       : p.nodataText));
    };
    if (!p.nodataLate)
        pushNodataTag();

    // an orphan slot with no real doubles means the tag is created only
    // by the rewrite, so its data lands after everything else
    const bool orphanTrailingDoubles =
        geoOrphanApplied && geo.doubles.size() == 1;
    auto pushGeoKeyTags = [&tags, &geo, orphanTrailingDoubles]()
    {
        if (!geo.any)
            return;
        OutTag kd;
        kd.tag = 34735;
        kd.type = TT_SHORT;
        kd.count = geo.dir.size();
        for (uint16_t v : geo.dir)
            put16(kd.data, v);
        tags.push_back(std::move(kd));
        if (!geo.doubles.empty() && !orphanTrailingDoubles)
            tags.push_back(mkDoubles(34736, geo.doubles));
        if (!geo.ascii.empty())
            tags.push_back(mkAscii(34737, geo.ascii));
    };
    // the abandoned-double rewrite also flips the data placement: geokey
    // blocks land before the geo transform tags
    if (geoOrphanApplied)
        pushGeoKeyTags();
    if (geoProfile && p.gcps && !p.gcps->empty())
    {
        // CreateCopy stores Point GCPs a half pixel below the exposed
        // value, the opposite of the in-place SetGCPs shift
        double shift = p.pointPixel ? -0.5 : 0.0;
        std::vector<double> tps;
        for (const GcpEntry &g : *p.gcps)
        {
            tps.push_back(g.pixel + shift);
            tps.push_back(g.line + shift);
            tps.push_back(0.0);
            tps.push_back(g.x);
            tps.push_back(g.y);
            tps.push_back(g.z);
        }
        tags.push_back(mkDoubles(33922, tps));
    }
    else if (geoProfile && p.hasXform)
    {
        tags.push_back(
            mkDoubles(34264, std::vector<double>(p.xform, p.xform + 16)));
    }
    else if (geoProfile && p.hasGT)
    {
        tags.push_back(mkDoubles(
            33550,
            {p.gt[1], -p.gt[5],
             geo.zScaleOne && p.bands == 1 ? 1.0 : 0.0}));
        tags.push_back(
            mkDoubles(33922, {0, 0, 0, p.gt[0], p.gt[3], 0.0}));
    }
    if (!geoOrphanApplied)
        pushGeoKeyTags();
    if (p.nodataLate)
        pushNodataTag();
    if (orphanTrailingDoubles && geo.any)
        tags.push_back(mkDoubles(34736, geo.doubles));

    if (p.gridOrphanIfd && !bigtiff && !p.append && !p.bigEndian)
    {
        auto isGeoTag = [](uint16_t t)
        {
            return t == 33550 || t == 33922 || t == 34264 ||
                   t == 34735 || t == 34736 || t == 34737;
        };
        std::vector<OutTag> otags;
        for (const OutTag &t : tags)
            if (!isGeoTag(t.tag))
                otags.push_back(t);
        for (OutTag &t : otags)
            if (t.tag == 273 || t.tag == 279 || t.tag == 324 ||
                t.tag == 325)
                std::fill(t.data.begin(), t.data.end(), 0);
        auto sortIdx = [](const std::vector<OutTag> &v)
        {
            std::vector<size_t> ord(v.size());
            for (size_t i = 0; i < v.size(); i++)
                ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(),
                             [&v](size_t a, size_t b)
                             { return v[a].tag < v[b].tag; });
            return ord;
        };
        const size_t n0 = otags.size();
        uint32_t cur0 = 8 + 2 + (uint32_t)n0 * 12 + 4;
        std::vector<uint32_t> off0(n0, 0);
        for (size_t i = 0; i < n0; i++)
            if (otags[i].data.size() > 4)
            {
                if (cur0 & 1)
                    ++cur0;
                off0[i] = cur0;
                cur0 += (uint32_t)otags[i].data.size();
            }
        std::vector<uint32_t> boff(nblocks, 0);
        uint32_t curb = cur0;
        for (int i = 0; i < nblocks; i++)
        {
            boff[i] = curb;
            curb += (uint32_t)blocks[i].size();
        }
        const uint32_t fbase = curb + (curb & 1);
        if (offsetsTagIndex != (size_t)-1)
        {
            OutTag &ot = tags[offsetsTagIndex];
            ot.data.clear();
            for (int i = 0; i < nblocks; i++)
                put32(ot.data, boff[i]);
        }
        const size_t nf = tags.size();
        uint32_t fcur = fbase + 2 + (uint32_t)nf * 12 + 4;
        std::vector<uint32_t> foff(nf, 0);
        for (size_t i = 0; i < nf; i++)
            if (tags[i].data.size() > 4)
            {
                if (fcur & 1)
                    ++fcur;
                foff[i] = fcur;
                fcur += (uint32_t)tags[i].data.size();
            }
        std::string outBuf("II*\0", 4);
        {
            char b[4];
            memcpy(b, &fbase, 4);
            outBuf.append(b, 4);
        }
        auto emitIfd = [&outBuf](const std::vector<OutTag> &v,
                                 const std::vector<size_t> &ord,
                                 const std::vector<uint32_t> &off)
        {
            std::vector<uint8_t> ifd;
            put16(ifd, (uint16_t)v.size());
            for (size_t oi : ord)
            {
                const OutTag &t = v[oi];
                put16(ifd, t.tag);
                put16(ifd, t.type);
                put32(ifd, t.count);
                if (t.data.size() > 4)
                    put32(ifd, off[oi]);
                else
                {
                    std::vector<uint8_t> d(t.data);
                    d.resize(4, 0);
                    ifd.insert(ifd.end(), d.begin(), d.end());
                }
            }
            put32(ifd, 0);
            outBuf.append((const char *)ifd.data(), ifd.size());
        };
        emitIfd(otags, sortIdx(otags), off0);
        for (size_t i = 0; i < n0; i++)
            if (otags[i].data.size() > 4)
            {
                if (outBuf.size() & 1)
                    outBuf += '\0';
                outBuf.append((const char *)otags[i].data.data(),
                              otags[i].data.size());
            }
        for (int i = 0; i < nblocks; i++)
            outBuf.append((const char *)blocks[i].data(),
                          blocks[i].size());
        if (outBuf.size() & 1)
            outBuf += '\0';
        emitIfd(tags, sortIdx(tags), foff);
        for (size_t i = 0; i < nf; i++)
            if (tags[i].data.size() > 4)
            {
                if (outBuf.size() & 1)
                    outBuf += '\0';
                outBuf.append((const char *)tags[i].data.data(),
                              tags[i].data.size());
            }
        if (!writeStringToFile(path, outBuf))
        {
            err = "write failed";
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // layout
    // ------------------------------------------------------------------
    std::string existing;
    const bool bt = bigtiff;
    uint32_t base = bt ? 16 : 8;
    size_t patchNextAt = bt ? 8 : 4;  // where to store this IFD's offset
    if (p.append)
    {
        if (!readFileToString(path, existing))
        {
            err = "cannot read existing file";
            return false;
        }
        if (existing.size() < 8)
        {
            err = "invalid TIFF";
            return false;
        }
        if (bt)
        {
            uint64_t ifdOff;
            memcpy(&ifdOff, existing.data() + 8, 8);
            while (true)
            {
                if (ifdOff + 8 > existing.size())
                {
                    err = "invalid TIFF";
                    return false;
                }
                uint64_t n;
                memcpy(&n, existing.data() + ifdOff, 8);
                size_t nextPos = (size_t)(ifdOff + 8 + n * 20);
                uint64_t next;
                memcpy(&next, existing.data() + nextPos, 8);
                if (next == 0)
                {
                    patchNextAt = nextPos;
                    break;
                }
                ifdOff = next;
            }
        }
        else
        {
            uint32_t ifdOff;
            memcpy(&ifdOff, existing.data() + 4, 4);
            while (true)
            {
                if (ifdOff + 2 > existing.size())
                {
                    err = "invalid TIFF";
                    return false;
                }
                uint16_t n;
                memcpy(&n, existing.data() + ifdOff, 2);
                size_t nextPos = ifdOff + 2 + (size_t)n * 12;
                uint32_t next;
                memcpy(&next, existing.data() + nextPos, 4);
                if (next == 0)
                {
                    patchNextAt = nextPos;
                    break;
                }
                ifdOff = next;
            }
        }
        base = (uint32_t)((existing.size() + 1) & ~1ull);
    }

    std::vector<size_t> ifdOrder(tags.size());
    for (size_t i = 0; i < tags.size(); i++)
        ifdOrder[i] = i;
    std::stable_sort(ifdOrder.begin(), ifdOrder.end(),
                     [&tags](size_t a, size_t b)
                     { return tags[a].tag < tags[b].tag; });

    const size_t n = tags.size();
    const size_t inlineMax = bt ? 8 : 4;
    uint32_t ifdEnd = bt ? base + 8 + (uint32_t)n * 20 + 8
                         : base + 2 + (uint32_t)n * 12 + 4;
    uint32_t cur = ifdEnd;
    std::vector<uint32_t> dataOff(n, 0);
    for (size_t i = 0; i < n; i++)
    {
        size_t sz = tags[i].data.size();
        if (sz > inlineMax)
        {
            if (cur & 1)
                ++cur;  // libtiff word-aligns tag data
            dataOff[i] = cur;
            cur += (uint32_t)sz;
        }
    }

    // mask sub-IFD (clean-collar --add-mask): its directory and payloads
    // land after the main payloads, its deflate blocks after the data;
    // the block grid mirrors the main bands
    const bool wantMask =
        p.maskPixels && !bt && !p.append && !p.bigEndian && writeData;
    std::vector<OutTag> mtags;
    std::vector<std::vector<uint8_t>> maskBlocks;
    std::vector<uint32_t> mDataOff;
    uint32_t maskIfdOff = 0;
    size_t mOffsetsIdx = (size_t)-1;
    if (wantMask)
    {
        size_t mFullRaw;
        if (p.tiled)
        {
            const int tRowBytes = blockW / 8;
            mFullRaw = (size_t)tRowBytes * blockH;
            for (int ty = 0; ty < tilesY; ty++)
                for (int tx = 0; tx < tilesX; tx++)
                {
                    std::vector<uint8_t> raw(mFullRaw, 0);
                    for (int r = 0; r < blockH; r++)
                    {
                        int y = ty * blockH + r;
                        if (y >= p.height)
                            break;
                        for (int xx = 0; xx < blockW; xx++)
                        {
                            int x = tx * blockW + xx;
                            if (x >= p.width)
                                break;
                            if ((*p.maskPixels)[(size_t)y * p.width + x])
                                raw[(size_t)r * tRowBytes + xx / 8] |=
                                    (uint8_t)(0x80 >> (xx & 7));
                        }
                    }
                    maskBlocks.push_back(deflateBlock(raw, 6));
                }
        }
        else
        {
            const int mRowBytes = (p.width + 7) / 8;
            mFullRaw = (size_t)mRowBytes * rps;
            for (int bi = 0; bi < blocksPerPlane; bi++)
            {
                int y0 = bi * rps;
                int rows = y0 + rps > p.height ? p.height - y0 : rps;
                std::vector<uint8_t> raw((size_t)rows * mRowBytes, 0);
                for (int r = 0; r < rows; r++)
                    for (int x = 0; x < p.width; x++)
                        if ((*p.maskPixels)[(size_t)(y0 + r) * p.width + x])
                            raw[(size_t)r * mRowBytes + x / 8] |=
                                (uint8_t)(0x80 >> (x & 7));
                maskBlocks.push_back(deflateBlock(raw, 6));
            }
        }
        std::vector<uint32_t> mCounts;
        for (const auto &b : maskBlocks)
            mCounts.push_back((uint32_t)b.size());
        const bool mShort = maskBlocks.size() > 1 &&
                            mFullRaw + mFullRaw / 4 + 2 <= 8192;
        mtags.push_back(mkLongs(254, {4}));
        mtags.push_back(mkShortLong(256, (uint32_t)p.width));
        mtags.push_back(mkShortLong(257, (uint32_t)p.height));
        mtags.push_back(mkShort(258, 1));
        mtags.push_back(mkShort(259, 8));
        mtags.push_back(mkShort(262, 4));
        mtags.push_back(mkShort(277, 1));
        if (!p.tiled)
            mtags.push_back(mkShortLong(278, (uint32_t)rps));
        if (mShort)
            mtags.push_back(mkShorts(
                p.tiled ? 325 : 279,
                std::vector<uint16_t>(mCounts.begin(), mCounts.end())));
        else
            mtags.push_back(mkLongs(p.tiled ? 325 : 279, mCounts));
        mOffsetsIdx = mtags.size();
        mtags.push_back(mkLongs(
            p.tiled ? 324 : 273,
            std::vector<uint32_t>(maskBlocks.size(), 0)));
        mtags.push_back(mkShort(284, 1));
        mtags.push_back(mkShort(317, 1));
        if (p.tiled)
        {
            mtags.push_back(mkShortLong(322, (uint32_t)blockW));
            mtags.push_back(mkShortLong(323, (uint32_t)blockH));
        }
        mtags.push_back(mkShort(339, 1));

        if (cur & 1)
            ++cur;
        maskIfdOff = cur;
        cur += 2 + (uint32_t)mtags.size() * 12 + 4;
        mDataOff.assign(mtags.size(), 0);
        for (size_t i = 0; i < mtags.size(); i++)
        {
            size_t sz = mtags[i].data.size();
            if (sz > 4)
            {
                if (cur & 1)
                    ++cur;
                mDataOff[i] = cur;
                cur += (uint32_t)sz;
            }
        }
    }

    std::vector<uint32_t> blockOff(nblocks, 0);
    if (writeData)
    {
        for (int i = 0; i < nblocks; i++)
        {
            if (blockClass[i] != 0)
                continue;
            blockOff[i] = cur;
            cur += (uint32_t)blocks[i].size();
        }
        for (int i = 0; i < nblocks; i++)
        {
            if (blockClass[i] != 1)
                continue;
            blockOff[i] = cur;
            cur += (uint32_t)blocks[i].size();
        }
    }
    if (wantMask)
    {
        std::vector<uint32_t> mOffs;
        for (const auto &b : maskBlocks)
        {
            mOffs.push_back(cur);
            cur += (uint32_t)b.size();
        }
        OutTag &ot = mtags[mOffsetsIdx];
        ot.data.clear();
        for (uint32_t o : mOffs)
            put32(ot.data, o);
    }
    // patch offsets tag
    if (offsetsTagIndex != (size_t)-1)
    {
        OutTag &ot = tags[offsetsTagIndex];
        ot.data.clear();
        for (int i = 0; i < nblocks; i++)
        {
            if (bt)
                put64(ot.data, blockOff[i]);
            else
                put32(ot.data, blockOff[i]);
        }
    }
    if (p.bigEndian)
    {
        for (OutTag &t : tags)
        {
            int es = t.type == 5 ? 4 : ttSize(t.type);
            if (es <= 1)
                continue;
            for (size_t i = 0; i + (size_t)es <= t.data.size();
                 i += (size_t)es)
                std::reverse(t.data.begin() + i, t.data.begin() + i + es);
        }
    }

    // ------------------------------------------------------------------
    // emit
    // ------------------------------------------------------------------
    const bool be = p.bigEndian;
    auto put16e = [be](std::vector<uint8_t> &v, uint16_t x)
    {
        if (be)
        {
            v.push_back(x >> 8);
            v.push_back(x & 0xff);
        }
        else
            put16(v, x);
    };
    auto put32e = [be](std::vector<uint8_t> &v, uint32_t x)
    {
        if (be)
            for (int i = 3; i >= 0; i--)
                v.push_back((x >> (8 * i)) & 0xff);
        else
            put32(v, x);
    };
    auto put64e = [be](std::vector<uint8_t> &v, uint64_t x)
    {
        if (be)
            for (int i = 7; i >= 0; i--)
                v.push_back((x >> (8 * i)) & 0xff);
        else
            put64(v, x);
    };
    std::string outBuf;
    if (p.append)
    {
        existing.resize(base, '\0');
        if (bt)
        {
            uint64_t b = base;
            memcpy(&existing[patchNextAt], &b, 8);
        }
        else
        {
            uint32_t b = base;
            memcpy(&existing[patchNextAt], &b, 4);
        }
        outBuf = std::move(existing);
    }
    else if (bt)
    {
        outBuf = be ? std::string("MM\0+", 4) : std::string("II+\0", 4);
        std::vector<uint8_t> hdr;
        put16e(hdr, 8);
        put16e(hdr, 0);
        put64e(hdr, 16);
        outBuf.append((const char *)hdr.data(), hdr.size());
    }
    else if (be)
    {
        outBuf = std::string("MM\0*", 4);
        std::vector<uint8_t> ofs;
        put32e(ofs, 8);
        outBuf.append((const char *)ofs.data(), 4);
    }
    else
    {
        outBuf = std::string("II*\0", 4);
        char ofs[4];
        uint32_t v = 8;
        memcpy(ofs, &v, 4);
        outBuf.append(ofs, 4);
    }

    std::vector<uint8_t> ifd;
    if (bt)
        put64e(ifd, n);
    else
        put16e(ifd, (uint16_t)n);
    for (size_t oi : ifdOrder)
    {
        const OutTag &t = tags[oi];
        put16e(ifd, t.tag);
        put16e(ifd, t.type);
        if (bt)
            put64e(ifd, t.count);
        else
            put32e(ifd, t.count);
        if (t.data.size() > inlineMax)
        {
            if (bt)
                put64e(ifd, dataOff[oi]);
            else
                put32e(ifd, dataOff[oi]);
        }
        else
        {
            std::vector<uint8_t> v(t.data);
            v.resize(inlineMax, 0);
            ifd.insert(ifd.end(), v.begin(), v.end());
        }
    }
    if (bt)
        put64e(ifd, 0);  // next IFD
    else
        put32e(ifd, wantMask ? maskIfdOff : 0);
    outBuf.append((const char *)ifd.data(), ifd.size());

    for (size_t i = 0; i < n; i++)
        if (tags[i].data.size() > inlineMax)
        {
            if (outBuf.size() & 1)
                outBuf += '\0';
            outBuf.append((const char *)tags[i].data.data(),
                          tags[i].data.size());
        }
    if (wantMask)
    {
        if (outBuf.size() & 1)
            outBuf += '\0';
        std::vector<size_t> mOrder(mtags.size());
        for (size_t i = 0; i < mtags.size(); i++)
            mOrder[i] = i;
        std::stable_sort(mOrder.begin(), mOrder.end(),
                         [&](size_t a, size_t b)
                         { return mtags[a].tag < mtags[b].tag; });
        std::vector<uint8_t> mifd;
        put16(mifd, (uint16_t)mtags.size());
        for (size_t oi : mOrder)
        {
            const OutTag &t = mtags[oi];
            put16(mifd, t.tag);
            put16(mifd, t.type);
            put32(mifd, t.count);
            if (t.data.size() > 4)
                put32(mifd, mDataOff[oi]);
            else
            {
                std::vector<uint8_t> v(t.data);
                v.resize(4, 0);
                mifd.insert(mifd.end(), v.begin(), v.end());
            }
        }
        put32(mifd, 0);
        outBuf.append((const char *)mifd.data(), mifd.size());
        for (size_t i = 0; i < mtags.size(); i++)
            if (mtags[i].data.size() > 4)
            {
                if (outBuf.size() & 1)
                    outBuf += '\0';
                outBuf.append((const char *)mtags[i].data.data(),
                              mtags[i].data.size());
            }
    }
    if (writeData)
    {
        for (int i = 0; i < nblocks; i++)
            if (blockClass[i] == 0)
                outBuf.append((const char *)blocks[i].data(),
                              blocks[i].size());
        for (int i = 0; i < nblocks; i++)
            if (blockClass[i] == 1)
                outBuf.append((const char *)blocks[i].data(),
                              blocks[i].size());
    }
    if (wantMask)
        for (const auto &b : maskBlocks)
            outBuf.append((const char *)b.data(), b.size());

    if (!writeStringToFile(path, outBuf))
    {
        err = "write failed";
        return false;
    }

    // PAM sidecar for non-GDALGeoTIFF profiles
    if (p.profile != "GDALGeoTIFF" && gdalPamEnabled())
    {
        bool pamSrs = p.profile == "BASELINE" && p.srs && p.srs->valid();
        bool pamGt = p.profile == "BASELINE" && p.hasGT;
        bool pamMeta = !p.metadata.empty();
        if (pamSrs || pamGt || pamMeta)
        {
            std::string pam = "<PAMDataset>\n";
            if (pamSrs)
            {
                std::string mapping;
                for (int m : p.srs->dataAxisToSRSAxisMapping())
                {
                    if (!mapping.empty())
                        mapping += ",";
                    mapping += strPrintf("%d", m);
                }
                pam += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping +
                       "\">" + p.srs->wkt1Gdal() + "</SRS>\n";
            }
            if (pamGt)
            {
                pam += "  <GeoTransform>";
                for (int i = 0; i < 6; i++)
                {
                    if (i)
                        pam += ",";
                    pam += strPrintf("%24.16e", p.gt[i]);
                }
                pam += "</GeoTransform>\n";
            }
            if (pamMeta)
            {
                pam += "  <Metadata>\n";
                for (const auto &kv : p.metadata)
                    pam += "    <MDI key=\"" + kv.first + "\">" + kv.second +
                           "</MDI>\n";
                pam += "  </Metadata>\n";
            }
            pam += "</PAMDataset>\n";
            writeStringToFile(path + ".aux.xml", pam);
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// COG writer: classic little-endian layout with the GDAL ghost area,
// every IFD (main first, then overviews) ahead of the data, per-IFD
// value payloads right after their IFD, strile arrays after the last
// IFD, and row-major tile data from the smallest overview up to the
// full resolution image, each tile framed by a 4-byte size leader and
// a last-4-bytes-repeated trailer.
// ---------------------------------------------------------------------
bool cogWrite(const std::string &path, const GTiffCreateParams &p,
              const std::vector<CogOverview> &ovrs, std::string &err)
{
    const int bpp = dtypeSizeBytes(p.type);
    const int blockW = p.blockX > 0 ? p.blockX : 512;
    const int blockH = p.blockY > 0 ? p.blockY : 512;
    const uint16_t photometric =
        p.photometric >= 0 ? (uint16_t)p.photometric : 1;
    const std::vector<uint16_t> &extras = p.extraSamples;
    const int pred = p.predictor >= 2 ? p.predictor : 1;
    const bool predTag = p.compression == 5 || p.compression == 8 ||
                         p.compression == 50000;

    uint16_t sampleFormat = 1;
    switch (p.type)
    {
        case DType::Int8:
        case DType::Int16:
        case DType::Int32:
        case DType::Int64:
            sampleFormat = 2;
            break;
        case DType::Float16:
        case DType::Float32:
        case DType::Float64:
            sampleFormat = 3;
            break;
        default:
            sampleFormat = 1;
            break;
    }

    GeoTags geo;
    if (p.srs && p.srs->valid())
        buildGeoTags(*p.srs, geo, p.pointPixel, false, p.gtVersion);
    else if (p.forceGeoDir)
    {
        Srs empty;
        buildGeoTags(empty, geo, p.pointPixel, true, p.gtVersion);
    }

    LzwEncoder lzw;
    auto compressBlock = [&](std::vector<uint8_t> &&raw)
    {
        switch (p.compression)
        {
            case 8:
                return deflateBlock(raw, p.zlevel);
            case 50000:
                return zstdBlock(raw, p.zstdLevel);
            case 5:
                return lzw.encode(raw.data(), raw.size());
            case 32773:
            {
                std::vector<uint8_t> outb;
                size_t rowBytes = (size_t)blockW * bpp * p.bands;
                size_t nrows = raw.size() / rowBytes;
                for (size_t r = 0; r < nrows; r++)
                    packBitsRow(raw.data() + r * rowBytes,
                                (long)rowBytes, outb);
                return outb;
            }
            default:
                return std::move(raw);
        }
    };
    auto buildBlocks = [&](int w, int h,
                           const std::vector<std::vector<uint8_t>> &px,
                           std::vector<std::vector<uint8_t>> &out)
    {
        int tx = (w + blockW - 1) / blockW;
        int ty = (h + blockH - 1) / blockH;
        size_t rowBytes = (size_t)blockW * bpp * p.bands;
        for (int bi = 0; bi < tx * ty; ++bi)
        {
            int bx0 = (bi % tx) * blockW;
            int by0 = (bi / tx) * blockH;
            std::vector<uint8_t> raw(rowBytes * blockH, 0);
            int copyRows = std::min(blockH, h - by0);
            int copyCols = std::min(blockW, w - bx0);
            for (int b = 0; b < p.bands; ++b)
            {
                const std::vector<uint8_t> &src = px[(size_t)b];
                for (int r = 0; r < copyRows; ++r)
                {
                    const uint8_t *sp =
                        src.data() +
                        ((size_t)(by0 + r) * w + bx0) * bpp;
                    uint8_t *dp = raw.data() + (size_t)r * rowBytes;
                    if (p.bands == 1)
                        memcpy(dp, sp, (size_t)copyCols * bpp);
                    else
                        for (int x = 0; x < copyCols; ++x)
                            memcpy(dp + ((size_t)x * p.bands + b) * bpp,
                                   sp + (size_t)x * bpp, bpp);
                }
            }
            if (pred >= 2)
                for (int r = 0; r < blockH; ++r)
                {
                    std::vector<uint8_t> row(
                        raw.begin() + (size_t)r * rowBytes,
                        raw.begin() + (size_t)(r + 1) * rowBytes);
                    if (pred == 3)
                        fpDiff(row, bpp, p.bands, false);
                    else
                        horDiff(row, bpp, p.bands);
                    memcpy(&raw[(size_t)r * rowBytes], row.data(),
                           rowBytes);
                }
            out.push_back(std::move(raw));
        }
    };

    auto xesc = [](const std::string &s)
    {
        std::string r;
        for (char c : s)
        {
            switch (c)
            {
                case '<':
                    r += "&lt;";
                    break;
                case '>':
                    r += "&gt;";
                    break;
                case '&':
                    r += "&amp;";
                    break;
                case '"':
                    r += "&quot;";
                    break;
                default:
                    r += c;
            }
        }
        return r;
    };

    struct CogIfd
    {
        int w = 0, h = 0;
        std::vector<OutTag> tags;  // placement order, striles excluded
        OutTag offTag, cntTag;
        std::vector<std::vector<uint8_t>> blocks;
        std::vector<uint32_t> tagOff;  // external payload offsets
        uint32_t offArr = 0, cntArr = 0;
    };
    std::vector<CogIfd> ifds(1 + ovrs.size());

    std::string ndText;
    if (p.hasNodata)
        ndText = p.nodataText.empty() ? strPrintf("%.17g", p.nodata)
                                      : p.nodataText;

    for (size_t i = 0; i < ifds.size(); ++i)
    {
        CogIfd &f = ifds[i];
        const bool ovr = i > 0;
        f.w = ovr ? ovrs[i - 1].w : p.width;
        f.h = ovr ? ovrs[i - 1].h : p.height;
        buildBlocks(f.w, f.h,
                    ovr ? *ovrs[i - 1].pixels : *p.pixels, f.blocks);
    }
    // compress in file order (smallest overview first): the LZW
    // encoder carries libtiff's post-encode free_ent quirk between
    // blocks, so the stream depends on everything encoded before it
    for (size_t ri = ifds.size(); ri-- > 0;)
        for (auto &b : ifds[ri].blocks)
            b = compressBlock(std::move(b));

    for (size_t i = 0; i < ifds.size(); ++i)
    {
        CogIfd &f = ifds[i];
        const bool ovr = i > 0;
        const int nb = (int)f.blocks.size();
        if (ovr)
            f.tags.push_back(mkLongs(254, {1}));
        f.tags.push_back(mkShortLong(256, (uint32_t)f.w));
        f.tags.push_back(mkShortLong(257, (uint32_t)f.h));
        f.tags.push_back(mkShorts(
            258, std::vector<uint16_t>(p.bands, (uint16_t)(bpp * 8))));
        f.tags.push_back(mkShort(259, (uint16_t)p.compression));
        f.tags.push_back(mkShort(262, photometric));
        f.tags.push_back(mkShort(277, (uint16_t)p.bands));
        f.tags.push_back(mkShort(284, 1));
        if (predTag)
            f.tags.push_back(mkShort(317, (uint16_t)pred));
        {
            int baseSamples = photometric == 2 ? 3 : 1;
            if (p.bands > baseSamples)
                f.tags.push_back(mkShorts(
                    338, std::vector<uint16_t>(
                             (size_t)(p.bands - baseSamples), 0)));
        }
        f.tags.push_back(mkShortLong(322, (uint32_t)blockW));
        f.tags.push_back(mkShortLong(323, (uint32_t)blockH));
        f.offTag = mkLongs(324, std::vector<uint32_t>(nb, 0));
        std::vector<uint32_t> counts(nb);
        for (int b = 0; b < nb; ++b)
            counts[(size_t)b] = (uint32_t)f.blocks[(size_t)b].size();
        f.cntTag = mkLongs(325, counts);
        f.tags.push_back(mkShorts(
            339, std::vector<uint16_t>(p.bands, sampleFormat)));
        if (!ovr && p.useGmdItems && !p.gmdItems.empty())
        {
            std::string xml = "<GDALMetadata>\n";
            for (const auto &it : p.gmdItems)
            {
                xml += "  <Item name=\"" + xesc(it.name) + "\"";
                if (it.sample >= 0)
                    xml += strPrintf(" sample=\"%d\"", it.sample);
                if (!it.role.empty())
                    xml += " role=\"" + it.role + "\"";
                if (!it.domain.empty())
                    xml += " domain=\"" + xesc(it.domain) + "\"";
                xml += ">" + xesc(xesc(it.value)) + "</Item>\n";
            }
            xml += "</GDALMetadata>\n";
            f.tags.push_back(mkAscii(42112, xml));
        }
        if (p.hasNodata)
            f.tags.push_back(mkAscii(42113, ndText));
        if (!ovr)
        {
            if (geo.any)
            {
                // geo transform payloads precede the geokey blocks
            }
            if (p.hasXform)
                f.tags.push_back(mkDoubles(
                    34264,
                    std::vector<double>(p.xform, p.xform + 16)));
            else if (p.hasGT)
            {
                f.tags.push_back(mkDoubles(
                    33550, {p.gt[1], -p.gt[5],
                            geo.zScaleOne && p.bands == 1 ? 1.0 : 0.0}));
                f.tags.push_back(mkDoubles(
                    33922, {0, 0, 0, p.gt[0], p.gt[3], 0.0}));
            }
            if (geo.any)
            {
                OutTag kd;
                kd.tag = 34735;
                kd.type = TT_SHORT;
                kd.count = (uint32_t)geo.dir.size();
                for (uint16_t v : geo.dir)
                    put16(kd.data, v);
                f.tags.push_back(std::move(kd));
                if (!geo.doubles.empty())
                    f.tags.push_back(mkDoubles(34736, geo.doubles));
                if (!geo.ascii.empty())
                    f.tags.push_back(mkAscii(34737, geo.ascii));
            }
        }
    }

    // ghost header
    static const char *kGhostBody =
        "LAYOUT=IFDS_BEFORE_DATA\n"
        "BLOCK_ORDER=ROW_MAJOR\n"
        "BLOCK_LEADER=SIZE_AS_UINT4\n"
        "BLOCK_TRAILER=LAST_4_BYTES_REPEATED\n"
        "KNOWN_INCOMPATIBLE_EDITION=NO\n";
    std::string ghost = "GDAL_STRUCTURAL_METADATA_SIZE=000140 bytes\n";
    ghost += kGhostBody;
    ghost.resize(43 + 140, ' ');

    // layout pass
    uint32_t pos = 8 + (uint32_t)ghost.size();
    if (pos & 1)
        ++pos;
    const uint32_t ifd0 = pos;
    std::vector<uint32_t> ifdAt(ifds.size(), 0);
    for (size_t i = 0; i < ifds.size(); ++i)
    {
        CogIfd &f = ifds[i];
        if (pos & 1)
            ++pos;
        ifdAt[i] = pos;
        pos += 2 + (uint32_t)(f.tags.size() + 2) * 12 + 4;
        f.tagOff.assign(f.tags.size(), 0);
        for (size_t t = 0; t < f.tags.size(); ++t)
            if (f.tags[t].data.size() > 4)
            {
                if (pos & 1)
                    ++pos;
                f.tagOff[t] = pos;
                pos += (uint32_t)f.tags[t].data.size();
            }
    }
    for (CogIfd &f : ifds)
    {
        if (f.offTag.data.size() > 4)
        {
            if (pos & 1)
                ++pos;
            f.offArr = pos;
            pos += (uint32_t)f.offTag.data.size();
            f.cntArr = pos;
            pos += (uint32_t)f.cntTag.data.size();
        }
    }
    for (size_t ri = ifds.size(); ri-- > 0;)
    {
        CogIfd &f = ifds[ri];
        f.offTag.data.clear();
        for (const auto &b : f.blocks)
        {
            put32(f.offTag.data, pos + 4);
            pos += 4 + (uint32_t)b.size() + 4;
        }
    }

    // serialize
    std::string outBuf("II*\0", 4);
    {
        std::vector<uint8_t> tmp;
        put32(tmp, ifd0);
        outBuf.append((const char *)tmp.data(), tmp.size());
    }
    outBuf += ghost;
    while (outBuf.size() & 1)
        outBuf += '\0';
    for (size_t i = 0; i < ifds.size(); ++i)
    {
        CogIfd &f = ifds[i];
        struct Ent
        {
            const OutTag *t;
            uint32_t off;
        };
        std::vector<Ent> ents;
        for (size_t t = 0; t < f.tags.size(); ++t)
            ents.push_back({&f.tags[t], f.tagOff[t]});
        ents.push_back({&f.offTag, f.offArr});
        ents.push_back({&f.cntTag, f.cntArr});
        std::stable_sort(ents.begin(), ents.end(),
                         [](const Ent &a, const Ent &b)
                         { return a.t->tag < b.t->tag; });
        if (outBuf.size() & 1)
            outBuf += '\0';
        std::vector<uint8_t> ifd;
        put16(ifd, (uint16_t)ents.size());
        for (const Ent &e : ents)
        {
            put16(ifd, e.t->tag);
            put16(ifd, e.t->type);
            put32(ifd, e.t->count);
            if (e.t->data.size() > 4)
                put32(ifd, e.off);
            else
            {
                std::vector<uint8_t> d(e.t->data);
                d.resize(4, 0);
                ifd.insert(ifd.end(), d.begin(), d.end());
            }
        }
        put32(ifd, i + 1 < ifds.size() ? ifdAt[i + 1] : 0);
        outBuf.append((const char *)ifd.data(), ifd.size());
        for (const OutTag &t : f.tags)
            if (t.data.size() > 4)
            {
                if (outBuf.size() & 1)
                    outBuf += '\0';
                outBuf.append((const char *)t.data.data(),
                              t.data.size());
            }
    }
    for (const CogIfd &f : ifds)
        if (f.offTag.data.size() > 4)
        {
            if (outBuf.size() & 1)
                outBuf += '\0';
            outBuf.append((const char *)f.offTag.data.data(),
                          f.offTag.data.size());
            outBuf.append((const char *)f.cntTag.data.data(),
                          f.cntTag.data.size());
        }
    for (size_t ri = ifds.size(); ri-- > 0;)
        for (const auto &b : ifds[ri].blocks)
        {
            std::vector<uint8_t> lead;
            put32(lead, (uint32_t)b.size());
            outBuf.append((const char *)lead.data(), 4);
            outBuf.append((const char *)b.data(), b.size());
            size_t n = b.size() >= 4 ? 4 : b.size();
            outBuf.append((const char *)b.data() + b.size() - n, n);
        }

    if (!writeStringToFile(path, outBuf))
    {
        err = "write failed";
        return false;
    }
    return true;
}
