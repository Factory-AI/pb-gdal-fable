#include "cpl.h"
#include "dataset.h"
#include "jpeg_ijg.h"
#include "progress.h"
#include "tiff.h"
#include "util.h"
#include "vrt.h"
#include "vsi.h"
#include "webp_shim.h"
#include "xml_min.h"
#include "zstd_min.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sys/stat.h>

namespace
{

constexpr uint16_t TAG_WIDTH = 256, TAG_HEIGHT = 257, TAG_BPS = 258,
                   TAG_COMPRESSION = 259, TAG_PHOTOMETRIC = 262,
                   TAG_STRIP_OFFSETS = 273, TAG_SPP = 277,
                   TAG_ROWS_PER_STRIP = 278, TAG_STRIP_COUNTS = 279,
                   TAG_PLANAR = 284, TAG_PREDICTOR = 317,
                   TAG_COLORMAP = 320, TAG_TILE_WIDTH = 322,
                   TAG_TILE_LENGTH = 323, TAG_TILE_OFFSETS = 324,
                   TAG_TILE_COUNTS = 325, TAG_EXTRASAMPLES = 338,
                   TAG_SAMPLE_FORMAT = 339, TAG_PIXEL_SCALE = 33550,
                   TAG_TIEPOINT = 33922, TAG_TRANSFORM = 34264,
                   TAG_GEO_DIR = 34735, TAG_GEO_DOUBLES = 34736,
                   TAG_GEO_ASCII = 34737, TAG_GDAL_METADATA = 42112,
                   TAG_GDAL_NODATA = 42113;

constexpr char kGtGeogMismatchFmt[] =
    "The definition of geographic CRS EPSG:%d got from GeoTIFF keys is "
    "not the same as the one from the EPSG registry, which may cause "
    "issues during reprojection operations. Set GTIFF_SRS_SOURCE "
    "configuration option to EPSG to use official parameters (overriding "
    "the ones from GeoTIFF keys), or to GEOKEYS to use custom values "
    "from GeoTIFF keys and drop the EPSG code.";

DType dtypeFrom(uint32_t bps, uint32_t sf)
{
    switch (bps)
    {
        case 8:
            return sf == 2 ? DType::Int8 : DType::Byte;
        case 16:
            if (sf == 2)
                return DType::Int16;
            if (sf == 3)
                return DType::Float16;
            return DType::UInt16;
        case 32:
            if (sf == 2)
                return DType::Int32;
            if (sf == 3)
                return DType::Float32;
            if (sf == 5)
                return DType::CInt16;
            return DType::UInt32;
        case 64:
            if (sf == 3)
                return DType::Float64;
            if (sf == 5)
                return DType::CInt32;
            if (sf == 6)
                return DType::CFloat32;
            if (sf == 2)
                return DType::Int64;
            return DType::UInt64;
        case 128:
            return DType::CFloat64;
        default:
            if (bps < 8 && (sf == 1 || sf == 4))
                return DType::Byte;
            if (bps > 8 && bps < 16 && (sf == 1 || sf == 4))
                return DType::UInt16;
            if (bps > 16 && bps < 32 && (sf == 1 || sf == 4))
                return DType::UInt32;
            return DType::Unknown;
    }
}

class GTiffDataset final : public RasterDatasetBase
{
  public:
    TiffFile tf;
    const TiffIfd *ifd = nullptr;
    uint32_t compression = 1, predictor = 1, planar = 1, spp = 1;
    uint32_t photometricTag = 1;
    std::vector<uint8_t> jpegTables;
    bool tiled = false;
    uint32_t tileW = 0, tileH = 0, rowsPerStrip = 0;
    std::vector<uint64_t> chunkOffsets, chunkCounts;
    int bytesPerSample = 1;
    uint32_t bitsPerSample = 8;
    DType dt = DType::Byte;
    bool pamHasSrs = false, pamHasGt = false;
    Srs pamSrsObj;
    double pamGt[6] = {0, 1, 0, 0, 0, 1};

    bool packedBits() const
    {
        return bitsPerSample != (uint32_t)bytesPerSample * 8;
    }
    TiffFile *tiffFile() override { return &tf; }
    uint64_t tiffIfdOffset() const override
    {
        return ifd ? ifd->offset : 0;
    }
    std::unique_ptr<RasterDatasetBase> openOverviewPage(int page) override;
    std::unique_ptr<RasterDatasetBase> openOverviewEntry(
        const OvrEntry &e) override;
    // returns false and fills codecMsg on a read/decode failure; strict
    // mode (per-block reads) also fails on a lone block past EOF, which
    // the whole-image path silently zero-fills
    bool decodeChunkEx(size_t idx, std::vector<uint8_t> &out,
                       size_t expectedSize, bool strict, size_t chunksPerBand,
                       uint32_t scanline0, std::string &codecMsg);
    void emitChunkError(const std::string &codecMsg, int band, int bx,
                        int by);
    uint32_t chunkRowCount() const;
    bool readChunkRow(int band, uint32_t rowIdx, std::vector<uint8_t> &out,
                      bool strict);
    bool readBand(int band, std::vector<double> &out) override;
    bool readBandRaw(int band, std::vector<uint8_t> &out) override;
    bool readBandRawStrict(int band, std::vector<uint8_t> &out) override
    {
        return readBandRawMode(band, out, true);
    }
    bool readBandRawMode(int band, std::vector<uint8_t> &out, bool strict);
    bool readAllBands(std::vector<std::vector<uint8_t>> &out,
                      TermProgress *tp, bool strict = false) override;
};

void unpackBitRows(const std::vector<uint8_t> &packed,
                   std::vector<uint8_t> &out, uint32_t bits,
                   size_t rowSamples, uint32_t rows, int outBytes)
{
    out.assign(rowSamples * rows * outBytes, 0);
    size_t packedRowBytes = (rowSamples * bits + 7) / 8;
    for (uint32_t r = 0; r < rows; ++r)
    {
        size_t base = (size_t)r * packedRowBytes * 8;
        for (size_t s = 0; s < rowSamples; ++s)
        {
            uint32_t v = 0;
            size_t bit = base + s * bits;
            for (uint32_t k = 0; k < bits; ++k, ++bit)
            {
                size_t byteIdx = bit >> 3;
                uint32_t b = byteIdx < packed.size()
                                 ? (packed[byteIdx] >> (7 - (bit & 7))) & 1
                                 : 0;
                v = (v << 1) | b;
            }
            uint8_t *dst = &out[((size_t)r * rowSamples + s) * outBytes];
            for (int k = 0; k < outBytes; ++k)
                dst[k] = (uint8_t)(v >> (8 * k));
        }
    }
}

bool inflateBuf(const uint8_t *in, size_t inLen, std::vector<uint8_t> &out,
                size_t *produced = nullptr)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK)
        return false;
    zs.next_in = const_cast<Bytef *>(in);
    zs.avail_in = (uInt)inLen;
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    int r = inflate(&zs, Z_FINISH);
    if (produced)
        *produced = out.size() - zs.avail_out;
    inflateEnd(&zs);
    return r == Z_STREAM_END || r == Z_OK;
}

struct LzwResult
{
    bool corrupt = false;
    bool sawEoi = false;
    size_t produced = 0;
};

bool lzwDecode(const uint8_t *in, size_t inLen, std::vector<uint8_t> &out,
               LzwResult *res = nullptr)
{
    // TIFF LZW with early change
    std::vector<std::pair<int, uint8_t>> table;  // prefix, suffix
    table.reserve(4096);
    auto reset = [&] {
        table.clear();
        for (int i = 0; i < 256; ++i)
            table.emplace_back(-1, (uint8_t)i);
        table.emplace_back(-1, 0);  // 256 clear
        table.emplace_back(-1, 0);  // 257 eoi
    };
    reset();
    size_t outPos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    size_t pos = 0;
    int codeSize = 9;
    int prev = -1;
    std::vector<uint8_t> tmp;
    auto emit = [&](int code) {
        tmp.clear();
        int c = code;
        while (c >= 0)
        {
            tmp.push_back(table[c].second);
            c = table[c].first;
        }
        for (size_t i = tmp.size(); i > 0; --i)
        {
            if (outPos < out.size())
                out[outPos++] = tmp[i - 1];
        }
        return tmp.empty() ? 0 : tmp.back();
    };
    auto firstByte = [&](int code) {
        int c = code;
        uint8_t f = 0;
        while (c >= 0)
        {
            f = table[c].second;
            c = table[c].first;
        }
        return f;
    };
    while (outPos < out.size())
    {
        while (bitCount < codeSize && pos < inLen)
        {
            bitBuf = (bitBuf << 8) | in[pos++];
            bitCount += 8;
        }
        if (bitCount < codeSize)
            break;
        int code = (int)((bitBuf >> (bitCount - codeSize)) &
                         ((1u << codeSize) - 1));
        bitCount -= codeSize;
        if (code == 256)
        {
            reset();
            codeSize = 9;
            prev = -1;
            continue;
        }
        if (code == 257)
        {
            if (res)
                res->sawEoi = true;
            break;
        }
        if (prev < 0)
        {
            if (code >= (int)table.size())
            {
                if (res)
                {
                    res->corrupt = true;
                    res->produced = outPos;
                }
                return false;
            }
            emit(code);
            prev = code;
            continue;
        }
        if (code < (int)table.size())
        {
            uint8_t f = firstByte(code);
            table.emplace_back(prev, f);
            emit(code);
        }
        else if (code == (int)table.size())
        {
            uint8_t f = firstByte(prev);
            table.emplace_back(prev, f);
            emit((int)table.size() - 1);
        }
        else
        {
            if (res)
            {
                res->corrupt = true;
                res->produced = outPos;
            }
            return false;
        }
        prev = code;
        if ((int)table.size() >= (1 << codeSize) - 1 && codeSize < 12)
            ++codeSize;
    }
    if (res)
        res->produced = outPos;
    return true;
}

bool packbitsDecode(const uint8_t *in, size_t inLen, std::vector<uint8_t> &out)
{
    size_t o = 0, i = 0;
    auto warnOverrun = [&](size_t want) {
        if (o + want > out.size())
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        strPrintf("PackBitsDecode:Discarding %llu bytes to "
                                  "avoid buffer overrun",
                                  (unsigned long long)(o + want -
                                                       out.size())));
    };
    while (i < inLen && o < out.size())
    {
        int8_t n = (int8_t)in[i++];
        if (n >= 0)
        {
            warnOverrun((size_t)n + 1);
            for (int k = 0; k <= n && i < inLen && o < out.size(); ++k)
                out[o++] = in[i++];
        }
        else if (n != -128)
        {
            if (i >= inLen)
                break;
            uint8_t v = in[i++];
            warnOverrun((size_t)(1 - n));
            for (int k = 0; k < 1 - n && o < out.size(); ++k)
                out[o++] = v;
        }
    }
    return true;
}

bool GTiffDataset::decodeChunkEx(size_t idx, std::vector<uint8_t> &out,
                                 size_t expectedSize, bool strict,
                                 size_t chunksPerBand, uint32_t row0,
                                 std::string &codecMsg)
{
    codecMsg.clear();
    out.assign(expectedSize, 0);
    if (idx >= chunkOffsets.size())
        return true;
    uint64_t off = chunkOffsets[idx];
    uint64_t cnt = idx < chunkCounts.size() ? chunkCounts[idx] : 0;
    if (off == 0 || cnt == 0)
        return true;  // sparse
    uint64_t fsize = tf.data.size();
    const char *op = tiled ? "TIFFReadEncodedTile" : "TIFFReadEncodedStrip";
    // libtiff reads the full geometric chunk size for uncompressed data,
    // ignoring a smaller StripByteCounts value
    uint64_t want = compression == 1 ? (uint64_t)expectedSize : cnt;
    if (off + want > fsize)
    {
        uint64_t got = off < fsize ? fsize - off : 0;
        // the whole-image direct-read path silently zero-fills a lone
        // uncompressed strip/tile lying entirely past EOF
        if (!strict && compression == 1 && chunksPerBand == 1 && got == 0)
            return true;
        if (tiled)
            codecMsg = strPrintf(
                "%s:Read error at row 4294967295, col 4294967295; got "
                "%llu bytes, expected %llu",
                op, (unsigned long long)got, (unsigned long long)want);
        else
            codecMsg = strPrintf(
                "%s:Read error at scanline 4294967295; got %llu bytes, "
                "expected %llu",
                op, (unsigned long long)got, (unsigned long long)want);
        return false;
    }
    const uint8_t *src = tf.data.data() + off;
    switch (compression)
    {
        case 1:
            memcpy(out.data(), src, (size_t)want);
            break;
        case 8:
        case 32946:
        {
            size_t produced = 0;
            if (!inflateBuf(src, cnt, out, &produced))
            {
                codecMsg = strPrintf(
                    "ZIPDecode:Decoding error at scanline %u", row0);
                return false;
            }
            if (produced < expectedSize)
            {
                codecMsg = strPrintf(
                    "ZIPDecode:Not enough data at scanline %u (short "
                    "%llu bytes)",
                    row0, (unsigned long long)(expectedSize - produced));
                return false;
            }
            break;
        }
        case 5:
        {
            LzwResult res;
            lzwDecode(src, cnt, out, &res);
            if (res.corrupt)
            {
                codecMsg = strPrintf(
                    "LZWDecode:Corrupted LZW table at scanline %u", row0);
                return false;
            }
            if (res.produced < expectedSize)
            {
                if (!res.sawEoi)
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        strPrintf("LZWDecode:LZWDecode: Strip %llu not "
                                  "terminated with EOI code",
                                  (unsigned long long)idx));
                codecMsg = strPrintf(
                    "LZWDecode:Not enough data at scanline %u (short "
                    "%llu bytes)",
                    row0,
                    (unsigned long long)(expectedSize - res.produced));
                return false;
            }
            break;
        }
        case 32773:
            packbitsDecode(src, cnt, out);
            break;
        case 7:
        {
            JpegDecoded dec;
            if (!jpegDecodeStream(src, cnt, jpegTables.data(),
                                  jpegTables.size(), photometricTag == 6,
                                  dec))
            {
                codecMsg = strPrintf(
                    "JPEGDecode:Decoding error at scanline %u", row0);
                return false;
            }
            size_t nb = dec.pixels.size();
            if (nb > expectedSize)
                nb = expectedSize;
            memcpy(out.data(), dec.pixels.data(), nb);
            break;
        }
        case 50001:
        {
            int cw = tiled ? (int)tileW : width;
            int ch;
            if (tiled)
                ch = (int)tileH;
            else
            {
                uint32_t rps = rowsPerStrip ? rowsPerStrip
                                            : (uint32_t)height;
                uint32_t rowStart =
                    (uint32_t)((idx % (chunksPerBand ? chunksPerBand : 1)) *
                               (uint64_t)rps);
                ch = (int)(rowStart + rps <= (uint32_t)height
                               ? rps
                               : (uint32_t)height - rowStart);
            }
            int fileSpp = spp == 4 ? 4 : 3;
            if (cw > 0 &&
                expectedSize % ((size_t)cw * fileSpp) != 0)
            {
                codecMsg =
                    "WebPDecode:Fractional scanlines cannot be read";
                return false;
            }
            std::vector<uint8_t> dec((size_t)cw * ch * fileSpp);
            if (cw <= 0 || ch <= 0 ||
                !webpDecodeBlock(src, (size_t)cnt, cw, ch, fileSpp,
                                 dec.data()))
            {
                codecMsg = "WebPDecode:Unrecognized error.";
                return false;
            }
            size_t nb = dec.size() < expectedSize ? dec.size()
                                                  : expectedSize;
            memcpy(out.data(), dec.data(), nb);
            break;
        }
        case 50000:
        {
            ZSTD_DStream *ds = ZSTD_createDStream();
            if (!ds)
                return false;
            ZSTD_initDStream(ds);
            ZSTD_inBuffer ib = {src, (size_t)cnt, 0};
            ZSTD_outBuffer ob = {out.data(), out.size(), 0};
            size_t r = 0;
            while (ib.pos < ib.size && ob.pos < ob.size)
            {
                r = ZSTD_decompressStream(ds, &ob, &ib);
                if (ZSTD_isError(r))
                    break;
            }
            size_t produced = ob.pos;
            ZSTD_freeDStream(ds);
            if (ZSTD_isError(r))
            {
                codecMsg = strPrintf(
                    "ZSTDDecode:Error in ZSTD_decompressStream()");
                return false;
            }
            if (produced < expectedSize)
            {
                codecMsg = strPrintf(
                    "ZSTDDecode:Not enough data at scanline %u (short "
                    "%llu bytes)",
                    row0, (unsigned long long)(expectedSize - produced));
                return false;
            }
            break;
        }
        default:
            return false;
    }
    return true;
}

void GTiffDataset::emitChunkError(const std::string &codecMsg, int band,
                                  int bx, int by)
{
    const char *op = tiled ? "TIFFReadEncodedTile" : "TIFFReadEncodedStrip";
    cplErrorStr(CE_Failure, CPLE_AppDefined, codecMsg);
    cplErrorStr(CE_Failure, CPLE_AppDefined, strPrintf("%s() failed.", op));
    std::string base = path;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    cplErrorStr(CE_Failure, CPLE_AppDefined,
                strPrintf("%s, band %d: IReadBlock failed at X offset %d, "
                          "Y offset %d: %s() failed.",
                          base.c_str(), band, bx, by, op));
}



void applyPredictor(std::vector<uint8_t> &buf, uint32_t predictor,
                    size_t rowBytes, int sampleBytes, int samples,
                    bool isFloat)
{
    if (predictor == 2)
    {
        for (size_t row = 0; row * rowBytes + rowBytes <= buf.size(); ++row)
        {
            uint8_t *p = buf.data() + row * rowBytes;
            size_t n = rowBytes / sampleBytes;
            if (sampleBytes == 1)
            {
                for (size_t i = (size_t)samples; i < n; ++i)
                    p[i] = (uint8_t)(p[i] + p[i - samples]);
            }
            else if (sampleBytes == 2)
            {
                uint16_t *q = (uint16_t *)p;
                for (size_t i = (size_t)samples; i < n; ++i)
                    q[i] = (uint16_t)(q[i] + q[i - samples]);
            }
            else if (sampleBytes == 4)
            {
                uint32_t *q = (uint32_t *)p;
                for (size_t i = (size_t)samples; i < n; ++i)
                    q[i] = q[i] + q[i - samples];
            }
            else if (sampleBytes == 8)
            {
                uint64_t *q = (uint64_t *)p;
                for (size_t i = (size_t)samples; i < n; ++i)
                    q[i] = q[i] + q[i - samples];
            }
        }
    }
    else if (predictor == 3 && isFloat)
    {
        std::vector<uint8_t> tmp(rowBytes);
        for (size_t row = 0; row * rowBytes + rowBytes <= buf.size(); ++row)
        {
            uint8_t *p = buf.data() + row * rowBytes;
            for (size_t i = 1; i < rowBytes; ++i)
                p[i] = (uint8_t)(p[i] + p[i - 1]);
            size_t nvals = rowBytes / sampleBytes;
            memcpy(tmp.data(), p, rowBytes);
            for (size_t i = 0; i < nvals; ++i)
                for (int b = 0; b < sampleBytes; ++b)
                    p[i * sampleBytes + b] =
                        tmp[(size_t)(sampleBytes - 1 - b) * nvals + i];
        }
    }
}

double sampleToDouble(const uint8_t *p, DType t)
{
    switch (t)
    {
        case DType::Byte:
            return *p;
        case DType::Int8:
            return *(const int8_t *)p;
        case DType::UInt16:
            return *(const uint16_t *)p;
        case DType::Int16:
            return *(const int16_t *)p;
        case DType::UInt32:
            return *(const uint32_t *)p;
        case DType::Int32:
            return *(const int32_t *)p;
        case DType::UInt64:
            return (double)*(const uint64_t *)p;
        case DType::Int64:
            return (double)*(const int64_t *)p;
        case DType::Float16:
            return tailHalfToFloat(*(const uint16_t *)p);
        case DType::Float32:
            return *(const float *)p;
        case DType::Float64:
            return *(const double *)p;
        case DType::CInt16:
            return *(const int16_t *)p;
        case DType::CInt32:
            return *(const int32_t *)p;
        case DType::CFloat32:
            return *(const float *)p;
        case DType::CFloat64:
            return *(const double *)p;
        default:
            return 0;
    }
}

uint32_t GTiffDataset::chunkRowCount() const
{
    if (tiled)
        return tileH ? (height + tileH - 1) / tileH : 0;
    uint32_t rps = rowsPerStrip ? rowsPerStrip : (uint32_t)height;
    return rps ? (height + rps - 1) / rps : 0;
}

bool GTiffDataset::readChunkRow(int band, uint32_t rowIdx,
                                std::vector<uint8_t> &out, bool strict)
{
    int bpsamp = bytesPerSample;
    int bandIdx = band - 1;
    std::string msg;

    if (tiled)
    {
        uint32_t tilesX = (width + tileW - 1) / tileW;
        uint32_t tilesY = (height + tileH - 1) / tileH;
        uint32_t tilesPerBand = tilesX * tilesY;
        int chunkSamples = planar == 1 ? (int)spp : 1;
        size_t tileBytes =
            packedBits()
                ? ((size_t)tileW * chunkSamples * bitsPerSample + 7) / 8 *
                      tileH
                : (size_t)tileW * tileH * chunkSamples * bpsamp;
        std::vector<uint8_t> buf, packed;
        uint32_t ty = rowIdx;
        for (uint32_t tx = 0; tx < tilesX; ++tx)
        {
            size_t idx = ty * tilesX + tx;
            if (planar == 2)
                idx += (size_t)bandIdx * tilesPerBand;
            if (packedBits())
            {
                if (!decodeChunkEx(idx, packed, tileBytes, strict,
                                   tilesPerBand, ty * tileH, msg))
                {
                    emitChunkError(msg, band, (int)tx, (int)ty);
                    return false;
                }
                unpackBitRows(packed, buf, bitsPerSample,
                              (size_t)tileW * chunkSamples, tileH, bpsamp);
            }
            else if (!decodeChunkEx(idx, buf, tileBytes, strict,
                                    tilesPerBand, ty * tileH, msg))
            {
                emitChunkError(msg, band, (int)tx, (int)ty);
                return false;
            }
            applyPredictor(buf, predictor,
                           (size_t)tileW * chunkSamples * bpsamp, bpsamp,
                           chunkSamples,
                           dt == DType::Float32 || dt == DType::Float64);
            for (uint32_t r = 0; r < tileH; ++r)
            {
                uint32_t y = ty * tileH + r;
                if (y >= (uint32_t)height)
                    break;
                for (uint32_t ccol = 0; ccol < tileW; ++ccol)
                {
                    uint32_t x = tx * tileW + ccol;
                    if (x >= (uint32_t)width)
                        break;
                    size_t srcOff =
                        ((size_t)r * tileW + ccol) * chunkSamples;
                    if (planar == 1)
                        srcOff += bandIdx;
                    memcpy(&out[((size_t)y * width + x) * bpsamp],
                           &buf[srcOff * bpsamp], bpsamp);
                }
            }
        }
        return true;
    }

    uint32_t rps = rowsPerStrip ? rowsPerStrip : (uint32_t)height;
    uint32_t stripsPerBand = (height + rps - 1) / rps;
    int chunkSamples = planar == 1 ? (int)spp : 1;
    std::vector<uint8_t> buf, packed;
    uint32_t s = rowIdx;
    uint32_t rows = std::min<uint32_t>(rps, height - s * rps);
    size_t stripBytes =
        packedBits()
            ? ((size_t)width * chunkSamples * bitsPerSample + 7) / 8 * rows
            : (size_t)width * rows * chunkSamples * bpsamp;
    size_t idx = s;
    if (planar == 2)
        idx += (size_t)bandIdx * stripsPerBand;
    if (packedBits())
    {
        if (!decodeChunkEx(idx, packed, stripBytes, strict, stripsPerBand,
                           s * rps, msg))
        {
            emitChunkError(msg, band, 0, (int)s);
            return false;
        }
        unpackBitRows(packed, buf, bitsPerSample,
                      (size_t)width * chunkSamples, rows, bpsamp);
    }
    else if (!decodeChunkEx(idx, buf, stripBytes, strict, stripsPerBand,
                            s * rps, msg))
    {
        emitChunkError(msg, band, 0, (int)s);
        return false;
    }
    applyPredictor(buf, predictor, (size_t)width * chunkSamples * bpsamp,
                   bpsamp, chunkSamples,
                   dt == DType::Float32 || dt == DType::Float64);
    for (uint32_t r = 0; r < rows; ++r)
    {
        uint32_t y = s * rps + r;
        for (uint32_t x = 0; x < (uint32_t)width; ++x)
        {
            size_t srcOff = ((size_t)r * width + x) * chunkSamples;
            if (planar == 1)
                srcOff += bandIdx;
            memcpy(&out[((size_t)y * width + x) * bpsamp],
                   &buf[srcOff * bpsamp], bpsamp);
        }
    }
    return true;
}

bool GTiffDataset::readBandRawMode(int band, std::vector<uint8_t> &out,
                                   bool strict)
{
    gdalDebugCacheMaxOnce();
    out.assign((size_t)width * height * bytesPerSample, 0);
    uint32_t rows = chunkRowCount();
    for (uint32_t r = 0; r < rows; ++r)
        if (!readChunkRow(band, r, out, strict))
            return false;
    return true;
}

bool GTiffDataset::readBandRaw(int band, std::vector<uint8_t> &out)
{
    return readBandRawMode(band, out, false);
}

bool GTiffDataset::readAllBands(std::vector<std::vector<uint8_t>> &out,
                                TermProgress *tp, bool strict)
{
    gdalDebugCacheMaxOnce();
    out.resize(bands.size());
    for (auto &b : out)
        b.assign((size_t)width * height * bytesPerSample, 0);
    if (tp)
        tp->update(0.0);
    uint32_t rows = chunkRowCount();
    for (uint32_t r = 0; r < rows; ++r)
    {
        for (size_t b = 0; b < bands.size(); ++b)
            if (!readChunkRow((int)b + 1, r, out[b], strict))
                return false;
        if (tp)
            tp->update(0.5 * (double)(r + 1) / (double)rows);
    }
    return true;
}

bool GTiffDataset::readBand(int band, std::vector<double> &out)
{
    std::vector<uint8_t> raw;
    if (!readBandRaw(band, raw))
        return false;
    size_t pixels = (size_t)width * height;
    out.resize(pixels);
    int bpsamp = bytesPerSample;
    for (size_t i = 0; i < pixels; ++i)
        out[i] = sampleToDouble(&raw[i * bpsamp], dt);
    return true;
}

struct GeoKey
{
    uint16_t id, loc, count, valueOrOffset;
};

void parseGeoKeys(const TiffIfd &ifd, std::vector<GeoKey> &keys,
                  uint16_t &minorRev)
{
    const std::vector<uint64_t> *dir = ifd.getInts(TAG_GEO_DIR);
    if (!dir || dir->size() < 4)
        return;
    minorRev = (uint16_t)(*dir)[2];
    uint64_t n = (*dir)[3];
    for (uint64_t k = 0; k < n && 4 + k * 4 + 3 < dir->size(); ++k)
    {
        GeoKey g;
        g.id = (uint16_t)(*dir)[4 + k * 4];
        g.loc = (uint16_t)(*dir)[4 + k * 4 + 1];
        g.count = (uint16_t)(*dir)[4 + k * 4 + 2];
        g.valueOrOffset = (uint16_t)(*dir)[4 + k * 4 + 3];
        keys.push_back(g);
    }
}

const GeoKey *findKey(const std::vector<GeoKey> &keys, uint16_t id)
{
    for (auto &k : keys)
        if (k.id == id)
            return &k;
    return nullptr;
}

void parseGdalMetadata(const std::string &xml, GTiffDataset &ds)
{
    XmlNode root;
    if (!xmlParse(xml, root) || root.name != "GDALMetadata")
        return;
    for (auto &item : root.children)
    {
        if (item.name != "Item")
            continue;
        std::string name = item.attr("name");
        std::string sample = item.attr("sample");
        std::string role = item.attr("role");
        std::string domain = item.attr("domain");
        std::string value = item.textCpl;
        if (strEqualNoCase(domain, "IMAGE_STRUCTURE"))
        {
            // stored IMAGE_STRUCTURE items are dropped except the WEBP
            // codec state on WEBP-compressed files
            if (sample.empty() && ds.compression == 50001)
            {
                if (strEqualNoCase(name, "WEBP_LEVEL"))
                {
                    int lv = atoi(value.c_str());
                    if (lv >= 1 && lv <= 100)
                        ds.setMd("IMAGE_STRUCTURE",
                                 "COMPRESSION_REVERSIBILITY", "LOSSY");
                    ds.setMd("IMAGE_STRUCTURE", "WEBP_LEVEL", value);
                }
                else if (strEqualNoCase(name,
                                        "COMPRESSION_REVERSIBILITY"))
                    ds.setMd("IMAGE_STRUCTURE",
                             "COMPRESSION_REVERSIBILITY", value);
            }
            continue;
        }
        if (sample.empty())
        {
            ds.setMd(domain, name, value);
            continue;
        }
        int bandIdx = atoi(sample.c_str());
        if (bandIdx < 0 || bandIdx >= (int)ds.bands.size())
            continue;
        Band &b = ds.bands[bandIdx];
        if (role == "scale")
        {
            b.hasScale = true;
            b.scale = atof(value.c_str());
        }
        else if (role == "offset")
        {
            b.hasOffset = true;
            b.offset = atof(value.c_str());
        }
        else if (role == "unittype")
            b.unitType = value;
        else if (role == "description")
            b.description = value;
        else if (role == "colorinterp")
        {
            static const char *kInterpNames[] = {
                "Undefined", "Gray",     "Palette",  "Red",
                "Green",     "Blue",     "Alpha",    "Hue",
                "Saturation", "Lightness", "Cyan",   "Magenta",
                "Yellow",    "Black",    "YCbCr_Y",  "YCbCr_Cb",
                "YCbCr_Cr",  "Pan",      "Coastal",  "RedEdge",
                "NIR",       "SWIR",     "MWIR",     "LWIR",
                "TIR",       "OtherIR",  "SAR_Ka",   "SAR_K",
                "SAR_Ku",    "SAR_X",    "SAR_C",    "SAR_S",
                "SAR_L",     "SAR_P"};
            const char *match = nullptr;
            for (const char *n : kInterpNames)
                if (strEqualNoCase(value, n))
                {
                    match = n;
                    break;
                }
            if (match)
                b.colorInterp = match;
            else
            {
                b.colorInterp = "Undefined";
                b.setMd("", "COLOR_INTERPRETATION", value);
            }
        }
        else
            b.setMd(domain, name, value);
    }
}

void loadPam(const std::string &pamPath, GTiffDataset &ds)
{
    std::string content;
    if (!readFileToString(pamPath, content))
        return;
    XmlNode root;
    if (!xmlParse(content, root) || root.name != "PAMDataset")
        return;
    ds.pamExists = true;
    for (auto &c : root.children)
    {
        if (c.name == "Metadata")
        {
            std::string domain = c.attr("domain");
            if (c.attr("format") == "xml")
            {
                std::string s;
                for (auto &el : c.children)
                    xmlSerialize(el, s, 0);
                ds.xmlDomains[domain] = s;
                ds.noteDomain(domain);
                ds.pamXmlDomains.emplace_back(domain, s);
                continue;
            }
            ds.markPamSorted(domain);
            std::vector<std::pair<std::string, std::string>> items;
            for (auto &mdi : c.children)
                if (mdi.name == "MDI")
                    items.emplace_back(mdi.attr("key"), mdi.text);
            std::stable_sort(items.begin(), items.end(),
                             [](const auto &a, const auto &b) {
                                 return a.first < b.first;
                             });
            for (auto &kv : items)
            {
                ds.setMd(domain, kv.first, kv.second);
                ds.pamMdi.push_back({domain, kv.first, kv.second});
            }
        }
        else if (c.name == "SRS")
        {
            ds.pamSrsRaw = c.text;
            ds.pamSrsMapping = c.attr("dataAxisToSRSAxisMapping");
            bool ok = false;
            Srs s = Srs::fromUserInput(c.text, ok);
            if (ok)
            {
                ds.pamSrsObj = std::move(s);
                ds.pamHasSrs = true;
            }
        }
        else if (c.name == "GeoTransform")
        {
            ds.pamGtRaw = c.text;
            double v[6];
            const char *p = c.text.c_str();
            char *end = nullptr;
            int i = 0;
            for (; i < 6; ++i)
            {
                v[i] = strtod(p, &end);
                if (end == p)
                    break;
                p = end;
                while (*p == ',' || *p == ' ')
                    ++p;
            }
            if (i == 6)
            {
                memcpy(ds.pamGt, v, sizeof(v));
                ds.pamHasGt = true;
            }
        }
        else if (c.name == "PAMRasterBand")
        {
            int bi = atoi(c.attr("band", "0").c_str());
            if (bi < 1 || bi > (int)ds.bands.size())
                continue;
            Band &b = ds.bands[bi - 1];
            PamBandState &pb = ds.pamBands[bi];
            for (auto &bc : c.children)
            {
                if (bc.name == "Metadata")
                {
                    std::string domain = bc.attr("domain");
                    if (bc.attr("format") == "xml")
                    {
                        std::string s;
                        for (auto &el : bc.children)
                            xmlSerialize(el, s, 0);
                        b.xmlDomains[domain] = s;
                        b.noteDomain(domain);
                        pb.xmlDomains.emplace_back(domain, s);
                        continue;
                    }
                    b.markPamSorted(domain);
                    if (domain.empty())
                        pb.mdiSorted = true;
                    std::vector<std::pair<std::string, std::string>> items;
                    for (auto &mdi : bc.children)
                        if (mdi.name == "MDI")
                            items.emplace_back(mdi.attr("key"), mdi.text);
                    std::stable_sort(items.begin(), items.end(),
                                     [](const auto &a, const auto &e) {
                                         return a.first < e.first;
                                     });
                    for (auto &kv : items)
                    {
                        b.setMd(domain, kv.first, kv.second);
                        if (domain.empty())
                            pb.mdi.emplace_back(kv.first, kv.second);
                        else
                            pb.extraMdi.push_back(
                                {domain, kv.first, kv.second});
                    }
                }
                else if (bc.name == "Histograms")
                {
                    for (auto &hi : bc.children)
                    {
                        if (hi.name != "HistItem")
                            continue;
                        HistItem h;
                        for (auto &f : hi.children)
                        {
                            if (f.name == "HistMin")
                                h.mn = atof(f.text.c_str());
                            else if (f.name == "HistMax")
                                h.mx = atof(f.text.c_str());
                            else if (f.name == "BucketCount")
                                h.buckets = atoll(f.text.c_str());
                            else if (f.name == "Approximate")
                                h.approx = atoi(f.text.c_str()) != 0;
                            else if (f.name == "HistCounts")
                            {
                                const char *p = f.text.c_str();
                                while (*p)
                                {
                                    h.counts.push_back(atoll(p));
                                    const char *bar = strchr(p, '|');
                                    if (!bar)
                                        break;
                                    p = bar + 1;
                                }
                            }
                        }
                        b.pamHists.push_back(h);
                        pb.hists.push_back(std::move(h));
                    }
                }
                else if (bc.name == "Description")
                    b.description = bc.text;
                else if (bc.name == "NoDataValue")
                {
                    b.hasNodata = true;
                    b.nodata = atof(bc.text.c_str());
                    pb.nodataRaw = bc.text;
                }
                else if (bc.name == "UnitType")
                    b.unitType = bc.text;
                else if (bc.name == "Offset")
                {
                    b.hasOffset = true;
                    b.offset = atof(bc.text.c_str());
                    pb.offsetRaw = bc.text;
                }
                else if (bc.name == "Scale")
                {
                    b.hasScale = true;
                    b.scale = atof(bc.text.c_str());
                    pb.scaleRaw = bc.text;
                }
            }
        }
    }
}

}  // namespace

std::string baseName(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

const char *kGTiffOpenOptions[] = {"NUM_THREADS", "GEOREF_SOURCES",
                                   "SPARSE_OK", "IGNORE_COG_LAYOUT_BREAK",
                                   "COLOR_TABLE_MULTIPLIER"};

void warnOpenOptions(const OpenOptions &oo)
{
    auto boolish = [](const std::string &v) {
        static const char *vals[] = {"YES", "NO",  "TRUE", "FALSE",
                                     "ON",  "OFF", "0",    "1"};
        for (const char *b : vals)
            if (strEqualNoCase(v, b))
                return true;
        return false;
    };
    for (const auto &kv : oo.raw)
    {
        bool known = false;
        for (const char *k : kGTiffOpenOptions)
            if (strEqualNoCase(kv.first, k))
                known = true;
        if (!known)
        {
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "driver GTiff does not support open option " +
                            kv.first);
            continue;
        }
        if ((strEqualNoCase(kv.first, "SPARSE_OK") ||
             strEqualNoCase(kv.first, "IGNORE_COG_LAYOUT_BREAK")) &&
            !boolish(kv.second))
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "'" + kv.second + "' is an unexpected value for " +
                            kv.first + " open option of type boolean.");
        if (strEqualNoCase(kv.first, "COLOR_TABLE_MULTIPLIER"))
        {
            static const char *vals[] = {"AUTO", "1", "256", "257"};
            bool ok = false;
            for (const char *c : vals)
                if (strEqualNoCase(kv.second, c))
                    ok = true;
            if (!ok)
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "'" + kv.second +
                                "' is an unexpected value for " + kv.first +
                                " open option of type string-select.");
        }
    }
}

std::unique_ptr<RasterDatasetBase> openGTiff(const std::string &path,
                                             std::string &err,
                                             const OpenOptions &oo,
                                             const std::string &conn,
                                             int dirIndex, bool useOff,
                                             uint64_t dirOffset,
                                             bool quiet = false)
{
    warnOpenOptions(oo);
    bool sub = dirIndex > 0 || useOff;
    auto ds = std::make_unique<GTiffDataset>();
    if (!TiffFile::open(path, ds->tf, err))
    {
        if (err == "libtiff")
        {
            std::string base = baseName(path);
            for (const auto &d : ds->tf.diags)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            base + ": " + d.text);
            err = "reported";
        }
        return nullptr;
    }
    if (path == "/vsistdin/")
    {
        uint64_t want = ds->tf.bigTiff ? 16 : 8;
        if (!ds->tf.ifds.empty() && ds->tf.ifds[0].offset != want)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        ds->tf.bigTiff ? "IFD start should be at offset 16 "
                                         "for a streamed BigTIFF"
                                       : "IFD start should be at offset 8 "
                                         "for a streamed TIFF");
            err = "reported";
            return nullptr;
        }
    }
    int ifdIdx = 0;
    if (useOff)
    {
        size_t diagCount = ds->tf.diags.size();
        ifdIdx = ds->tf.findOrParseIfdAt(dirOffset);
        if (ifdIdx < 0)
        {
            for (size_t i = 0; i < diagCount; ++i)
                if (!ds->tf.diags[i].prefixed &&
                    ds->tf.diags[i].page == 0)
                    cplErrorStr(ds->tf.diags[i].warning ? CE_Warning
                                                        : CE_Failure,
                                CPLE_AppDefined, ds->tf.diags[i].text);
            for (size_t i = diagCount; i < ds->tf.diags.size(); ++i)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            ds->tf.diags[i].text);
            err = "reported";
            return nullptr;
        }
    }
    else if (dirIndex > 0)
    {
        if (dirIndex > (int)ds->tf.ifds.size())
        {
            // the failed directory seek walks the whole chain, reading
            // every page once
            for (int p = 0; p < (int)ds->tf.ifds.size(); ++p)
                for (const auto &d : ds->tf.diags)
                    if (!d.prefixed && d.page == p)
                        cplErrorStr(d.warning ? CE_Warning : CE_Failure,
                                    CPLE_AppDefined, d.text);
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        strPrintf("%s: Requested directory %d not found.",
                                  baseName(path).c_str(), dirIndex));
            err = "reported";
            return nullptr;
        }
        ifdIdx = dirIndex - 1;
    }
    {
        // page re-read multipliers mirror libtiff directory traffic: the
        // opening read is prefixed (full opens only), every later
        // SetDirectory replays the diagnostic unprefixed; overview
        // candidates are read twice more and accepted ones three times
        uint32_t mainSpp =
            (uint32_t)ds->tf.ifds[ifdIdx].getInt(TAG_SPP, 1);
        auto pageStype = [&](int p) -> uint64_t
        {
            return p >= 0 && p < (int)ds->tf.ifds.size()
                       ? ds->tf.ifds[p].getInt(254, 0)
                       : 0;
        };
        auto deferCount = [&](int p) -> int
        {
            if (sub)
            {
                if (p == ifdIdx)
                    return 2;
                if (p == 0)
                    return 1;
                return !useOff && p < ifdIdx ? 1 : 0;
            }
            if (p <= 0 || p >= (int)ds->tf.ifds.size())
                return 1;
            uint64_t st = pageStype(p);
            if ((st & 1) && !(st & 4) &&
                (uint32_t)ds->tf.ifds[p].getInt(TAG_SPP, 1) == mainSpp)
                return 3;
            return (st & 1) || (st & 4) ? 2 : 1;
        };
        for (const auto &d : ds->tf.diags)
        {
            if (!d.warning)
                ds->openHadErrors = true;
            if (d.prefixed)
            {
                if (!sub)
                    cplErrorStr(d.warning ? CE_Warning : CE_Failure,
                                CPLE_AppDefined,
                                baseName(path) + ": " + d.text);
                continue;
            }
            const char *kExtrasFix = "TIFFReadDirectory:Sum of Photometric";
            if (d.warning && d.page == ifdIdx &&
                d.text.compare(0, strlen(kExtrasFix), kExtrasFix) == 0)
                ds->rewriteWarnings.push_back(d.text);
        }
        // deferred replay order follows the directory walk: extra pages
        // in file order, then the return to the opened directory
        auto pushPage = [&](int p)
        {
            int n = deferCount(p);
            for (int k = 0; k < n; ++k)
                for (const auto &d : ds->tf.diags)
                    if (!d.prefixed && d.page == p)
                        ds->deferredWarnings.push_back(
                            {d.warning, d.text, false, p == ifdIdx});
        };
        if (sub)
        {
            for (int p = 0; p <= ifdIdx && p < (int)ds->tf.ifds.size();
                 ++p)
                pushPage(p);
        }
        else
        {
            // scan traffic per reduced/mask candidate: read it, hop back
            // to the opened directory, read it again to open (Opened
            // trace when accepted); full pages read once; one trailing
            // return to the opened directory
            auto pushOnce = [&](int p, bool asMain)
            {
                for (const auto &d : ds->tf.diags)
                    if (!d.prefixed && d.page == p)
                        ds->deferredWarnings.push_back(
                            {d.warning, d.text, false, asMain});
            };
            for (int p = 1; p < (int)ds->tf.ifds.size(); ++p)
            {
                uint64_t st = pageStype(p);
                if (!(st & 1) && !(st & 4))
                {
                    pushOnce(p, false);
                    continue;
                }
                pushOnce(p, false);
                pushOnce(0, false);
                pushOnce(p, false);
                if (deferCount(p) == 3)
                    ds->deferredWarnings.push_back(
                        {true,
                         strPrintf("Opened %dx%d overview.",
                                   (int)ds->tf.ifds[p].getInt(TAG_WIDTH, 0),
                                   (int)ds->tf.ifds[p].getInt(TAG_HEIGHT,
                                                              0)),
                         true, false});
            }
            pushOnce(0, true);
        }
        // diagnostics raised past the last page (broken next-IFD links)
        // replay once
        for (const auto &d : ds->tf.diags)
            if (!d.prefixed && d.page >= (int)ds->tf.ifds.size())
                ds->deferredWarnings.push_back({d.warning, d.text});
    }
    const TiffIfd &ifd = ds->tf.ifds[ifdIdx];
    ds->isSubdataset = sub;
    ds->subName = conn;
    ds->ifd = &ifd;
    ds->path = conn;
    ds->driverShort = "GTiff";
    ds->driverLong = "GeoTIFF";
    ds->width = (int)ifd.getInt(TAG_WIDTH);
    ds->height = (int)ifd.getInt(TAG_HEIGHT);
    ds->spp = (uint32_t)ifd.getInt(TAG_SPP, 1);
    ds->compression = (uint32_t)ifd.getInt(TAG_COMPRESSION, 1);
    ds->predictor = (uint32_t)ifd.getInt(TAG_PREDICTOR, 1);
    ds->planar = (uint32_t)ifd.getInt(TAG_PLANAR, 1);
    uint32_t bps = (uint32_t)ifd.getInt(TAG_BPS, 1);
    uint32_t sf = (uint32_t)ifd.getInt(TAG_SAMPLE_FORMAT, 1);
    ds->dt = dtypeFrom(bps, sf);
    ds->bytesPerSample = dtypeSizeBytes(ds->dt);
    ds->bitsPerSample = bps;
    uint32_t photometric = (uint32_t)ifd.getInt(TAG_PHOTOMETRIC, 1);
    ds->photometricTag = photometric;
    if (ds->compression == 7)
    {
        auto jt = ifd.tags.find(347);
        if (jt != ifd.tags.end())
            ds->jpegTables = jt->second.raw;
    }

    int blockX, blockY;
    if (ifd.has(TAG_TILE_WIDTH))
    {
        ds->tiled = true;
        ds->tileW = (uint32_t)ifd.getInt(TAG_TILE_WIDTH);
        ds->tileH = (uint32_t)ifd.getInt(TAG_TILE_LENGTH);
        blockX = (int)ds->tileW;
        blockY = (int)ds->tileH;
        if (auto *v = ifd.getInts(TAG_TILE_OFFSETS))
            ds->chunkOffsets = *v;
        if (auto *v = ifd.getInts(TAG_TILE_COUNTS))
            ds->chunkCounts = *v;
    }
    else
    {
        uint64_t rpsTag =
            ifd.getInt(TAG_ROWS_PER_STRIP, (uint64_t)ds->height);
        ds->rowsPerStrip = (uint32_t)rpsTag;
        if (ds->rowsPerStrip > (uint32_t)ds->height)
            ds->rowsPerStrip = (uint32_t)ds->height;
        blockX = ds->width;
        blockY = (int)ds->rowsPerStrip;
        if (ds->compression == 1 && rpsTag >= (uint64_t)ds->height &&
            ds->height > 0)
        {
            // single uncompressed strip: GDAL exposes the default block
            int chunkSamples = ds->planar == 1 ? (int)ds->spp : 1;
            size_t scanline =
                ((size_t)ds->width * chunkSamples * bps + 7) / 8;
            int fake = scanline ? (int)(8192 / scanline) : 1;
            if (fake < 1)
                fake = 1;
            if (fake > ds->height)
                fake = ds->height;
            blockY = fake;
        }
        if (auto *v = ifd.getInts(TAG_STRIP_OFFSETS))
            ds->chunkOffsets = *v;
        bool scFromTag = false;
        if (auto *v = ifd.getInts(TAG_STRIP_COUNTS))
        {
            ds->chunkCounts = *v;
            scFromTag = true;
        }
        else if (!ds->chunkOffsets.empty() && ds->compression == 1)
        {
            size_t scanline = ((size_t)ds->width * ds->spp * bps + 7) / 8;
            if (ds->planar == 2)
                scanline = ((size_t)ds->width * bps + 7) / 8;
            uint32_t rps = ds->rowsPerStrip;
            for (size_t i = 0; i < ds->chunkOffsets.size(); i++)
            {
                uint32_t rows = rps;
                uint32_t yOff =
                    (uint32_t)((i % ((ds->height + rps - 1) / rps)) * rps);
                if (yOff + rows > (uint32_t)ds->height)
                    rows = (uint32_t)ds->height - yOff;
                ds->chunkCounts.push_back((uint64_t)scanline * rows);
            }
        }
        if (scFromTag && ds->chunkOffsets.size() == 1 &&
            ds->chunkCounts.size() == 1 && ds->chunkOffsets[0] != 0)
        {
            uint64_t fsize = ds->tf.data.size();
            bool bogus =
                ds->chunkCounts[0] == 0 ||
                (ds->compression == 1 && ds->chunkOffsets[0] <= fsize &&
                 ds->chunkCounts[0] > fsize - ds->chunkOffsets[0]);
            if (bogus)
            {
                size_t scanline =
                    ((size_t)ds->width * ds->spp * bps + 7) / 8;
                if (ds->planar == 2)
                    scanline = ((size_t)ds->width * bps + 7) / 8;
                uint32_t rows = ds->rowsPerStrip;
                if (rows > (uint32_t)ds->height)
                    rows = (uint32_t)ds->height;
                ds->chunkCounts[0] = (uint64_t)scanline * rows;
            }
        }
    }

    const std::vector<uint64_t> *extras = ifd.getInts(TAG_EXTRASAMPLES);
    for (uint32_t b = 0; b < ds->spp; ++b)
    {
        Band band;
        band.index = (int)b + 1;
        band.type = ds->dt;
        band.blockX = blockX;
        band.blockY = blockY;
        if (photometric == 2 ||
            (photometric == 6 && ds->compression == 7))
        {
            static const char *rgb[3] = {"Red", "Green", "Blue"};
            if (b < 3)
                band.colorInterp = rgb[b];
            else
                band.colorInterp = "Undefined";
        }
        else if (photometric == 3 && b == 0)
            band.colorInterp = "Palette";
        else if (photometric == 0 && b == 0)
            band.colorInterp = bps <= 16 ? "Palette" : "Undefined";
        else if (photometric == 1 && b == 0)
            band.colorInterp = bps == 1 ? "Palette" : "Gray";
        else
            band.colorInterp = "Undefined";
        if (ds->dt != DType::Unknown && bps != 8 && bps != 16 && bps != 32 &&
            bps != 64 && bps != 128)
            band.setMd("IMAGE_STRUCTURE", "NBITS", strPrintf("%u", bps));
        uint32_t baseSamples =
            photometric == 2 ||
                    (photometric == 6 && ds->compression == 7)
                ? 3
                : 1;
        if (b >= baseSamples && extras)
        {
            size_t ei = b - baseSamples;
            if (ei < extras->size() &&
                ((*extras)[ei] == 1 || (*extras)[ei] == 2))
                band.colorInterp = "Alpha";
        }
        ds->bands.push_back(std::move(band));
    }

    // MINISWHITE at 16 bits or less reads as a synthetic inverted
    // grayscale palette; MINISBLACK only for bilevel data
    if (((photometric == 0 && bps <= 16) ||
         (photometric == 1 && bps == 1)) &&
        !ds->bands.empty())
    {
        int n = 1 << bps;
        for (int i = 0; i < n; ++i)
        {
            int idx = photometric == 0 ? n - 1 - i : i;
            short v = (short)((long)idx * 255 / (n - 1));
            ds->bands[0].colorTable.push_back(ColorEntry{v, v, v, 255});
        }
    }

    if (photometric == 3 && ifd.has(TAG_COLORMAP))
    {
        const std::vector<uint64_t> *cm = ifd.getInts(TAG_COLORMAP);
        size_t n = cm->size() / 3;
        long mult = oo.ctMult;
        if (mult == 0)
        {
            uint64_t maxV = 0;
            for (uint64_t v : *cm)
                if (v > maxV)
                    maxV = v;
            mult = maxV <= 255 ? 1 : 257;
        }
        for (size_t i = 0; i < n; ++i)
        {
            ColorEntry e;
            e.c1 = (short)((*cm)[i] / mult);
            e.c2 = (short)((*cm)[n + i] / mult);
            e.c3 = (short)((*cm)[2 * n + i] / mult);
            e.c4 = 255;
            ds->bands[0].colorTable.push_back(e);
        }
    }

    // nodata
    std::string nd = ifd.getAscii(TAG_GDAL_NODATA);
    if (!nd.empty())
    {
        double v;
        if (strEqualNoCase(strTrim(nd), "nan"))
            v = std::nan("");
        else
            v = atof(nd.c_str());
        if (ds->dt == DType::Float32)
            v = (double)(float)v;
        bool i64 = ds->dt == DType::Int64, u64 = ds->dt == DType::UInt64;
        long long ll = 0;
        unsigned long long ull = 0;
        if (i64)
        {
            ll = strtoll(nd.c_str(), nullptr, 10);
            v = (double)ll;
        }
        else if (u64)
        {
            ull = strtoull(nd.c_str(), nullptr, 10);
            v = (double)ull;
        }
        for (auto &b : ds->bands)
        {
            b.hasNodata = true;
            b.nodata = v;
            b.nodataIsI64 = i64;
            b.nodataI64 = ll;
            b.nodataIsU64 = u64;
            b.nodataU64 = ull;
        }
    }

    std::vector<std::string> srcs =
        oo.georefSet
            ? oo.georefSources
            : std::vector<std::string>{"PAM", "INTERNAL", "TABFILE",
                                       "WORLDFILE"};
    auto srcEnabled = [&](const char *s) {
        for (const auto &t : srcs)
            if (strEqualNoCase(t, s))
                return true;
        return false;
    };
    bool internalOn = srcEnabled("INTERNAL");

    // geotransform (internal candidate)
    bool hasInternalGT = false;
    double internalGT[6] = {0, 1, 0, 0, 0, 1};
    const std::vector<double> *scale = ifd.getDoubles(TAG_PIXEL_SCALE);
    const std::vector<double> *tie = ifd.getDoubles(TAG_TIEPOINT);
    const std::vector<double> *mat = ifd.getDoubles(TAG_TRANSFORM);
    if (mat && mat->size() >= 16)
    {
        hasInternalGT = true;
        internalGT[0] = (*mat)[3];
        internalGT[1] = (*mat)[0];
        internalGT[2] = (*mat)[1];
        internalGT[3] = (*mat)[7];
        internalGT[4] = (*mat)[4];
        internalGT[5] = (*mat)[5];
    }
    else if (scale && scale->size() >= 2 && tie && tie->size() >= 6)
    {
        hasInternalGT = true;
        if ((*scale)[1] < 0)
            ds->deferredWarnings.push_back(
                {true,
                 path +
                     ": File with negative value for ScaleY in "
                     "GeoPixelScale tag. This is rather unusual. GDAL, "
                     "contrary to the GeoTIFF specification, assumes that "
                     "the file was intended to be north-up, and will "
                     "treat this file as if ScaleY was positive. You may "
                     "override this behavior by setting the "
                     "GTIFF_HONOUR_NEGATIVE_SCALEY configuration option "
                     "to YES"});
        internalGT[1] = (*scale)[0];
        internalGT[5] = -std::fabs((*scale)[1]);
        internalGT[2] = internalGT[4] = 0;
        internalGT[0] = (*tie)[3] - (*tie)[0] * internalGT[1];
        internalGT[3] = (*tie)[4] - (*tie)[1] * internalGT[5];
    }
    // tiepoints without a resolved geotransform become GCPs
    if (internalOn && !hasInternalGT && tie && tie->size() >= 6)
    {
        size_t n = tie->size() / 6;
        for (size_t i = 0; i < n; ++i)
        {
            GcpEntry e;
            e.id = strPrintf("%d", (int)(i + 1));
            e.pixel = (*tie)[i * 6 + 0];
            e.line = (*tie)[i * 6 + 1];
            e.x = (*tie)[i * 6 + 3];
            e.y = (*tie)[i * 6 + 4];
            e.z = (*tie)[i * 6 + 5];
            ds->gcps.push_back(std::move(e));
        }
    }

    // Degenerate geokey sets still produce a CRS in libgeotiff: local
    // engineering, unknown-geographic, or geocentric fallbacks.
    static const char *kEngcrsUnknown =
        "ENGCRS[\"unnamed\",EDATUM[\"\"],CS[Cartesian,2],"
        "AXIS[\"(E)\",east,ORDER[1],LENGTHUNIT[\"unknown\",1]],"
        "AXIS[\"(N)\",north,ORDER[2],LENGTHUNIT[\"unknown\",1]]]";
    static const char *kEngcrsMetre =
        "ENGCRS[\"unnamed\",EDATUM[\"\"],CS[Cartesian,2],"
        "AXIS[\"(E)\",east,ORDER[1],LENGTHUNIT[\"metre\",1,"
        "ID[\"EPSG\",9001]]],"
        "AXIS[\"(N)\",north,ORDER[2],LENGTHUNIT[\"metre\",1,"
        "ID[\"EPSG\",9001]]]]";
    static const char *kGeogcrsUnknown =
        "GEOGCRS[\"unknown\",DATUM[\"unnamed\",ELLIPSOID[\"unretrievable - "
        "using WGS84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1,"
        "ID[\"EPSG\",9001]]]],PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\","
        "0.0174532925199433,ID[\"EPSG\",9122]]],CS[ellipsoidal,2],"
        "AXIS[\"latitude\",north,ORDER[1],ANGLEUNIT[\"unknown\","
        "0.0174532925199433]],AXIS[\"longitude\",east,ORDER[2],"
        "ANGLEUNIT[\"unknown\",0.0174532925199433]]]";
    static const char *kGeodcrsUnknown =
        "GEODCRS[\"unnamed\",DATUM[\"unnamed\",ELLIPSOID[\"unretrievable - "
        "using WGS84\",6378137,298.257223563,LENGTHUNIT[\"metre\",1,"
        "ID[\"EPSG\",9001]]]],PRIMEM[\"Greenwich\",0,ANGLEUNIT[\"degree\","
        "0.0174532925199433,ID[\"EPSG\",9122]]],CS[Cartesian,3],"
        "AXIS[\"(X)\",geocentricX,ORDER[1],LENGTHUNIT[\"unknown\",1]],"
        "AXIS[\"(Y)\",geocentricY,ORDER[2],LENGTHUNIT[\"unknown\",1]],"
        "AXIS[\"(Z)\",geocentricZ,ORDER[3],LENGTHUNIT[\"unknown\",1]]]";

    // geokeys (internal SRS candidate)
    bool hasInternalSrs = false;
    bool internalSrsSynthetic = false;
    Srs internalSrs;
    std::vector<GeoKey> keys;
    uint16_t gkRev = 0;
    if (internalOn)
        parseGeoKeys(ifd, keys, gkRev);
    if (!keys.empty())
    {
        // libgeotiff bounds validation: double and directory references
        // must fit their arrays, ascii offsets must land before the first
        // NUL; any violation drops the whole key set
        const std::vector<double> *gdv = ifd.getDoubles(TAG_GEO_DOUBLES);
        const std::vector<uint64_t> *dirv = ifd.getInts(TAG_GEO_DIR);
        std::string gav = ifd.getAscii(TAG_GEO_ASCII);
        size_t asciiLen = gav.find('\0');
        if (asciiLen == std::string::npos)
            asciiLen = gav.size();
        bool corrupt = false;
        for (const GeoKey &k : keys)
        {
            if (k.loc == TAG_GEO_DOUBLES &&
                (!gdv ||
                 (size_t)k.valueOrOffset + k.count > gdv->size()))
                corrupt = true;
            else if (k.loc == TAG_GEO_ASCII &&
                     (size_t)k.valueOrOffset > asciiLen)
                corrupt = true;
            else if (k.loc == TAG_GEO_DIR &&
                     (!dirv ||
                      (size_t)k.valueOrOffset + k.count > dirv->size()))
                corrupt = true;
        }
        if (corrupt)
        {
            std::string fn = path.substr(path.find_last_of("/\\") + 1);
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        strPrintf("%s: GeoTIFF tags apparently corrupt, "
                                  "they are being ignored.",
                                  fn.c_str()));
            keys.clear();
        }
    }
    if (!keys.empty())
    {
        const GeoKey *modelType = findKey(keys, 1024);
        uint16_t mt = modelType ? modelType->valueOrOffset : 0;
        // an absent model type with a PCS key present reads as projected
        if (!modelType && findKey(keys, 3072))
            mt = 1;
        int epsg = 0;
        if (mt == 2)
        {
            const GeoKey *g = findKey(keys, 2048);
            if (g && g->valueOrOffset > 0 && g->valueOrOffset < 32767)
                epsg = g->valueOrOffset;
            else if (!g)
            {
                // geographic directories carrying only a PCS code fall
                // back to its database geodetic CRS
                const GeoKey *p = findKey(keys, 3072);
                if (p && p->valueOrOffset > 0 && p->valueOrOffset < 32767)
                    epsg = Srs::gcsCodeOfPcs(p->valueOrOffset);
            }
        }
        else if (mt == 1)
        {
            const GeoKey *g = findKey(keys, 3072);
            if (g && g->valueOrOffset > 0 && g->valueOrOffset < 32767)
                epsg = g->valueOrOffset;
        }
        std::string gtCitText;
        {
            const GeoKey *ck = findKey(keys, 1026);
            if (ck && ck->loc == TAG_GEO_ASCII && ck->count > 0)
            {
                std::string ga = ifd.getAscii(TAG_GEO_ASCII);
                if (ck->valueOrOffset < ga.size())
                    gtCitText = ga.substr(
                        ck->valueOrOffset,
                        std::min<size_t>(ck->count - 1,
                                         ga.size() - ck->valueOrOffset));
                if (!gtCitText.empty() && gtCitText.back() == '|')
                    gtCitText.pop_back();
            }
        }
        // vertical geokeys: a geographic 3D code in 4096 promotes the
        // horizontal CRS; GeoTIFF 1.1 directories (key revision 1.1)
        // build compound CRSs while 1.0 ones round-trip the horizontal
        // through WKT1 and only keep the vertical unit on the bands
        const GeoKey *vert = findKey(keys, 4096);
        const GeoKey *vDatK = findKey(keys, 4098);
        const GeoKey *vUnitK = findKey(keys, 4099);
        int vcode = vert ? vert->valueOrOffset : 0;
        int vunit = vUnitK && vUnitK->valueOrOffset > 0 &&
                            vUnitK->valueOrOffset < 32767
                        ? vUnitK->valueOrOffset
                        : 0;
        int vdat = vDatK && vDatK->valueOrOffset > 0 &&
                           vDatK->valueOrOffset < 32767
                       ? vDatK->valueOrOffset
                       : 0;
        bool promotePending = false;
        Srs promoteGeog3D;
        if (vert && vcode > 0 && vcode < 32767)
        {
            bool okv = false;
            Srs cand = Srs::fromEpsg(vcode, okv);
            if (okv && cand.isGeographic3D())
            {
                if (mt == 2 && epsg > 0)
                {
                    internalSrs = std::move(cand);
                    hasInternalSrs = true;
                    epsg = 0;
                }
                else if (mt == 1 && epsg > 0)
                {
                    promotePending = true;
                    promoteGeog3D = std::move(cand);
                }
            }
            else if (okv && cand.isVertical() && epsg > 0)
            {
                if (gkRev >= 1)
                {
                    bool okC = false;
                    Srs comp = Srs::fromGTiffCompound(epsg, mt == 1, vcode,
                                                      okC, gtCitText);
                    if (okC)
                    {
                        std::string vUnit = comp.verticalUnitName();
                        if (!vUnit.empty())
                            for (auto &b : ds->bands)
                            {
                                b.unitType = vUnit;
                                b.unitImplicit = true;
                            }
                        internalSrs = std::move(comp);
                        hasInternalSrs = true;
                        epsg = 0;
                    }
                }
                else
                {
                    if (mt == 1)
                    {
                        // a GTCitation naming anything but the catalogued
                        // "<horiz> + <vert>" pair keeps the text-parsed
                        // base axes; otherwise the programmatic
                        // Latitude/lat flavor shows through
                        bool latAxes =
                            gtCitText.empty() ||
                            gtCitText ==
                                Srs::gtiffCompoundNameFor(epsg, vcode);
                        bool okR = false;
                        Srs rt = Srs::fromGTiffWkt1RoundTrip(epsg, okR,
                                                             latAxes);
                        if (okR)
                        {
                            internalSrs = std::move(rt);
                            hasInternalSrs = true;
                            epsg = 0;
                        }
                    }
                    std::string vUnit =
                        Srs::gtiffVerticalUnitName(vcode, vunit);
                    for (auto &b : ds->bands)
                    {
                        b.unitType = vUnit;
                        b.unitImplicit = true;
                    }
                }
            }
            else if (!okv)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    "PROJ: proj_create_from_database: crs not found");
        }
        else if (((vert && vcode == 32767) ||
                  (!vert && (vDatK || vUnitK))) &&
                 epsg > 0)
        {
            if (gkRev >= 1)
            {
                bool okC = false;
                Srs comp = Srs::fromGTiffCompound(epsg, mt == 1, 0, okC,
                                                  gtCitText, vunit, vdat);
                if (okC)
                {
                    std::string vUnit = comp.verticalUnitName();
                    if (!vUnit.empty())
                        for (auto &b : ds->bands)
                        {
                            b.unitType = vUnit;
                            b.unitImplicit = true;
                        }
                    internalSrs = std::move(comp);
                    hasInternalSrs = true;
                    epsg = 0;
                }
            }
            else
            {
                bool okR = false;
                Srs rt = Srs::fromGTiffWkt1RoundTrip(epsg, okR);
                if (okR)
                {
                    internalSrs = std::move(rt);
                    hasInternalSrs = true;
                    epsg = 0;
                }
                std::string vUnit = Srs::gtiffVerticalUnitName(0, vunit);
                for (auto &b : ds->bands)
                {
                    b.unitType = vUnit;
                    b.unitImplicit = true;
                }
            }
        }
        // short override keys on a coded PCS force a GTIFGetDefn-style
        // component rebuild instead of the pristine database object
        if (epsg > 0 && mt == 1 && !promotePending &&
            (findKey(keys, 2048) || findKey(keys, 2050) ||
             findKey(keys, 2056) || findKey(keys, 3074) ||
             findKey(keys, 3075)))
        {
            GTiffKeyValues kvv;
            const std::vector<double> *gd = ifd.getDoubles(TAG_GEO_DOUBLES);
            std::string ga = ifd.getAscii(TAG_GEO_ASCII);
            for (const GeoKey &k : keys)
            {
                if (k.loc == 0)
                    kvv.shorts[k.id] = k.valueOrOffset;
                else if (k.loc == TAG_GEO_DOUBLES && gd && k.count >= 1 &&
                         k.valueOrOffset < gd->size())
                    kvv.dbls[k.id] = (*gd)[k.valueOrOffset];
                else if (k.loc == TAG_GEO_ASCII && k.count > 0 &&
                         k.valueOrOffset < ga.size())
                    kvv.asciis[k.id] =
                        ga.substr(k.valueOrOffset,
                                  std::min<size_t>(k.count - 1,
                                                   ga.size() -
                                                       k.valueOrOffset));
            }
            bool okA = false;
            Srs a = Srs::fromGTiffProjectedRebuild(kvv, epsg, okA);
            if (okA)
            {
                internalSrs = std::move(a);
                hasInternalSrs = true;
                epsg = 0;
            }
        }
        if (epsg > 0)
        {
            bool ok = false;
            const GeoKey *gtCit = findKey(keys, 1026);
            internalSrs = Srs::fromEpsgGTiff(
                epsg, mt == 1, ok,
                gtCit && gtCit->loc == TAG_GEO_ASCII);
            hasInternalSrs = ok;
            if (ok && mt == 2 && !internalSrs.isGeographic() &&
                !internalSrs.isProjected())
            {
                // geocentric codes rebuild textually via the key path
                hasInternalSrs = false;
                epsg = 0;
            }
            else if (ok && ((mt == 2 && !internalSrs.isGeographic()) ||
                            (mt == 1 && internalSrs.isGeographic())))
            {
                // the reference decodes GeoTIFF SRS lazily, so this
                // mismatch surfaces only when the CRS is consumed (never
                // for pure GDALG serialization); deferring to the replay
                // points models that
                ds->deferredWarnings.push_back(
                    {true,
                     strPrintf(
                         "The definition of %s CRS EPSG:%d got from "
                         "GeoTIFF keys is not the same as the one from "
                         "the EPSG registry, which may cause issues "
                         "during reprojection operations. Set "
                         "GTIFF_SRS_SOURCE configuration option to EPSG "
                         "to use official parameters (overriding the "
                         "ones from GeoTIFF keys), or to GEOKEYS to use "
                         "custom values from GeoTIFF keys and drop the "
                         "EPSG code.",
                         mt == 2 ? "geographic" : "projected", epsg),
                     false, true, true});
                bool ok2 = false;
                Srs reg = Srs::fromEpsg(epsg, ok2);
                if (ok2)
                    internalSrs = std::move(reg);
            }
            // unit keys disagreeing with the database definition: a
            // mismatched angular unit round-trips the CRS through WKT1, a
            // mismatched linear unit converts parameters and axes in
            // place (GDAL's SetLinearUnitsAndUpdateParameters)
            if (ok && mt == 1)
            {
                const GeoKey *au = findKey(keys, 2054);
                if (au && au->loc == 0 &&
                    Srs::pcsAngularUnitDiffers(epsg, au->valueOrOffset))
                {
                    int gcsOfPcs = Srs::gcsCodeOfPcs(epsg);
                    if (gcsOfPcs > 0)
                        cplErrorStr(CE_Warning, CPLE_AppDefined,
                                    strPrintf(kGtGeogMismatchFmt,
                                              gcsOfPcs));
                    bool okR = false;
                    Srs rt = Srs::fromGTiffWkt1RoundTrip(epsg, okR);
                    if (okR)
                        internalSrs = std::move(rt);
                }
                const GeoKey *lk = findKey(keys, 3076);
                std::string luName;
                double luFactor = 0.0;
                if (lk && lk->loc == 0 &&
                    Srs::pcsLinearUnitDiffers(epsg, lk->valueOrOffset,
                                              luName, luFactor))
                    internalSrs.alterLinearUnit(luName, luFactor,
                                                lk->valueOrOffset);
            }
        }
        if (hasInternalSrs && promotePending)
            internalSrs = internalSrs.promotedTo3D(promoteGeog3D);
        if (!hasInternalSrs && epsg == 0)
        {
            GTiffKeyValues kvv;
            const std::vector<double> *gd = ifd.getDoubles(TAG_GEO_DOUBLES);
            std::string ga = ifd.getAscii(TAG_GEO_ASCII);
            for (const GeoKey &k : keys)
            {
                if (k.loc == 0)
                    kvv.shorts[k.id] = k.valueOrOffset;
                else if (k.loc == TAG_GEO_DOUBLES && gd && k.count >= 1 &&
                         k.valueOrOffset < gd->size())
                    kvv.dbls[k.id] = (*gd)[k.valueOrOffset];
                else if (k.loc == TAG_GEO_ASCII && k.count > 0 &&
                         k.valueOrOffset < ga.size())
                    kvv.asciis[k.id] =
                        ga.substr(k.valueOrOffset,
                                  std::min<size_t>(k.count - 1,
                                                   ga.size() -
                                                       k.valueOrOffset));
            }
            bool okK = false;
            Srs cand = Srs::fromGTiffKeys(kvv, okK);
            if (okK)
            {
                internalSrs = std::move(cand);
                hasInternalSrs = true;
            }
        }
        if (!hasInternalSrs)
        {
            std::string wkt;
            if (mt == 2)
                wkt = kGeogcrsUnknown;
            else if (mt == 3)
                wkt = kGeodcrsUnknown;
            else if (mt == 1)
            {
                // a coded GeographicType riding along resolves in the
                // registry: a projected code draws the mismatch warning,
                // a geocentric one spells out the local axis names
                int gcsCls = 0;
                const GeoKey *g2048 = findKey(keys, 2048);
                if (g2048 && g2048->loc == 0 &&
                    g2048->valueOrOffset > 0 &&
                    g2048->valueOrOffset < 32767)
                    gcsCls =
                        Srs::gtiffGcsTypeClass(g2048->valueOrOffset);
                if (gcsCls == 3)
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                strPrintf(kGtGeogMismatchFmt,
                                          (int)g2048->valueOrOffset));
                // method-less projected directories become a local CRS
                // named from the PCS citation (unless it holds recognized
                // "Key = value" members), then the GT citation
                std::string ga = ifd.getAscii(TAG_GEO_ASCII);
                std::string nm;
                for (int cid : {3073, 1026})
                {
                    const GeoKey *ck = findKey(keys, cid);
                    if (ck && ck->loc == TAG_GEO_ASCII && ck->count > 0 &&
                        ck->valueOrOffset < ga.size())
                        nm = ga.substr(ck->valueOrOffset,
                                       std::min<size_t>(
                                           ck->count - 1,
                                           ga.size() -
                                               ck->valueOrOffset));
                    if (!nm.empty() && nm.back() == '|')
                        nm.pop_back();
                    if (cid == 3073 && !nm.empty())
                    {
                        static const char *const kMarkers[] = {
                            "PCS Name = ", "Projection Name = ",
                            "LUnits = ",   "GCS Name = ",
                            "Datum = ",    "Ellipsoid = ",
                            "Primem = ",   "AUnits = "};
                        for (const char *m : kMarkers)
                            if (nm.find(m) != std::string::npos)
                            {
                                nm.clear();
                                break;
                            }
                    }
                    if (!nm.empty())
                        break;
                }
                if (nm.empty())
                    nm = "unnamed";
                std::string esc;
                for (char c : nm)
                {
                    esc += c;
                    if (c == '"')
                        esc += '"';
                }
                const char *aE = gcsCls == 2 ? "easting" : "(E)";
                const char *aN = gcsCls == 2 ? "northing" : "(N)";
                wkt = "ENGCRS[\"" + esc +
                      "\",EDATUM[\"\"],CS[Cartesian,2],AXIS[\"" + aE +
                      "\",east,ORDER[1],LENGTHUNIT[\"metre\",1,"
                      "ID[\"EPSG\",9001]]],AXIS[\"" + aN +
                      "\",north,ORDER[2],LENGTHUNIT[\"metre\",1,"
                      "ID[\"EPSG\",9001]]]]";
            }
            else
            {
                const GeoKey *lu = findKey(keys, 3076);
                wkt = (lu && lu->valueOrOffset == 9001) ? kEngcrsMetre
                                                        : kEngcrsUnknown;
            }
            bool ok = false;
            Srs cand = Srs::fromUserInput(wkt, ok);
            if (ok)
            {
                if (mt == 2)
                    cand.setCustomAngularUnit(
                        "unknown", strtod("0.0174532925199433", nullptr));
                internalSrs = std::move(cand);
                hasInternalSrs = true;
                internalSrsSynthetic = true;
            }
        }
    }

    // a COG ghost area right after the header surfaces LAYOUT=COG ahead
    // of every other IMAGE_STRUCTURE key; layout-detected COGs (no ghost
    // area) get it after the 42112-derived items instead
    {
        static const char kGhost[] = "GDAL_STRUCTURAL_METADATA_SIZE=";
        size_t ghOff = ds->tf.bigTiff ? 16 : 8;
        const std::vector<uint8_t> &raw = ds->tf.data;
        if (raw.size() >= ghOff + sizeof(kGhost) - 1 &&
            memcmp(raw.data() + ghOff, kGhost, sizeof(kGhost) - 1) == 0)
            ds->setMd("IMAGE_STRUCTURE", "LAYOUT", "COG");
    }

    switch (ds->compression)
    {
        case 5:
            ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "LZW");
            break;
        case 8:
        case 32946:
            ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "DEFLATE");
            break;
        case 32773:
            ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "PACKBITS");
            break;
        case 7:
            if (photometric == 6)
            {
                ds->setMd("IMAGE_STRUCTURE", "SOURCE_COLOR_SPACE",
                          "YCbCr");
                ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "YCbCr JPEG");
            }
            else
                ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "JPEG");
            break;
        case 50000:
            ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "ZSTD");
            break;
        case 50001:
            ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "WEBP");
            break;
        case 34925:
            ds->setMd("IMAGE_STRUCTURE", "COMPRESSION", "LZMA");
            break;
        default:
            break;
    }
    ds->setMd("IMAGE_STRUCTURE", "INTERLEAVE",
              (ds->spp > 1 && ds->planar == 1) ? "PIXEL" : "BAND");
    if (photometric == 0 && !ds->bands.empty())
        ds->setMd("IMAGE_STRUCTURE", "MINISWHITE", "YES");
    if (ds->compression == 7 && !ds->jpegTables.empty())
    {
        // quality recovered by regenerating candidate tables; the
        // reference generator does not force baseline clamping
        int guessed = -1;
        for (int q = 1; q <= 100 && guessed < 0; q++)
        {
            std::vector<uint16_t> lum = jpegQuantTable(false, q);
            std::vector<uint16_t> chr = jpegQuantTable(true, q);
            // DQT payloads carry the values in zigzag order
            const std::vector<uint8_t> &t = ds->jpegTables;
            bool match = true, any = false;
            size_t i = 2;
            while (i + 4 <= t.size() && t[i] == 0xFF)
            {
                if (t[i + 1] == 0xD9)
                    break;
                size_t len = ((size_t)t[i + 2] << 8) | t[i + 3];
                if (len < 2 || i + 2 + len > t.size())
                    break;
                if (t[i + 1] == 0xDB)
                {
                    size_t k = i + 4, endSeg = i + 2 + len;
                    while (k < endSeg)
                    {
                        int prec = t[k] >> 4, id = t[k] & 15;
                        k++;
                        size_t need = prec ? 128 : 64;
                        if (k + need > endSeg || id > 3)
                        {
                            match = false;
                            break;
                        }
                        const std::vector<uint16_t> &ref =
                            id == 0 ? lum : chr;
                        for (int j = 0; j < 64; j++)
                        {
                            unsigned v = prec
                                             ? ((unsigned)t[k + 2 * j]
                                                << 8) |
                                                   t[k + 2 * j + 1]
                                             : t[k + j];
                            if (v != ref[jpegZigzagIndex(j)])
                            {
                                match = false;
                                break;
                            }
                        }
                        any = true;
                        k += need;
                        if (!match)
                            break;
                    }
                }
                if (!match)
                    break;
                i += 2 + len;
            }
            if (match && any)
                guessed = q;
        }
        if (guessed > 0)
        {
            ds->setMd("IMAGE_STRUCTURE", "JPEG_QUALITY",
                      strPrintf("%d", guessed));
            bool hasDht = false;
            const std::vector<uint8_t> &t = ds->jpegTables;
            for (size_t i = 2; i + 4 <= t.size() && t[i] == 0xFF;)
            {
                if (t[i + 1] == 0xD9)
                    break;
                if (t[i + 1] == 0xC4)
                    hasDht = true;
                size_t len = ((size_t)t[i + 2] << 8) | t[i + 3];
                if (len < 2 || i + 2 + len > t.size())
                    break;
                i += 2 + len;
            }
            ds->setMd("IMAGE_STRUCTURE", "JPEGTABLESMODE",
                      hasDht ? "3" : "1");
        }
    }
    if ((ds->compression == 5 || ds->compression == 8 ||
         ds->compression == 32946 || ds->compression == 50000) &&
        ifd.has(TAG_PREDICTOR) && ds->predictor > 1)
        ds->setMd("IMAGE_STRUCTURE", "PREDICTOR",
                  strPrintf("%u", (unsigned)ds->predictor));
    {
        static const struct
        {
            uint16_t id;
            const char *name;
        } kAsciiTags[] = {
            {269, "TIFFTAG_DOCUMENTNAME"}, {270, "TIFFTAG_IMAGEDESCRIPTION"},
            {305, "TIFFTAG_SOFTWARE"},     {306, "TIFFTAG_DATETIME"},
            {315, "TIFFTAG_ARTIST"},       {316, "TIFFTAG_HOSTCOMPUTER"},
            {33432, "TIFFTAG_COPYRIGHT"},
        };
        for (const auto &t : kAsciiTags)
            if (ifd.has(t.id))
                ds->setMd("", t.name, ifd.getAscii(t.id));
        bool hasXRes = ifd.has(282), hasYRes = ifd.has(283);
        if (hasXRes || hasYRes)
        {
            auto resVal = [&](uint16_t id) {
                const std::vector<double> *v = ifd.getDoubles(id);
                float f = v && !v->empty() ? (float)(*v)[0] : 0.0f;
                return strPrintf("%.8g", (double)f);
            };
            ds->setMd("", "TIFFTAG_XRESOLUTION", resVal(282));
            ds->setMd("", "TIFFTAG_YRESOLUTION", resVal(283));
        }
        if (ifd.has(280))
            ds->setMd("", "TIFFTAG_MINSAMPLEVALUE",
                      strPrintf("%d", (int)ifd.getInt(280)));
        if (ifd.has(281))
            ds->setMd("", "TIFFTAG_MAXSAMPLEVALUE",
                      strPrintf("%d", (int)ifd.getInt(281)));
        uint64_t resUnit = ifd.getInt(296, 0);
        if (resUnit == 1)
            ds->setMd("", "TIFFTAG_RESOLUTIONUNIT", "1 (unitless)");
        else if (resUnit == 2)
            ds->setMd("", "TIFFTAG_RESOLUTIONUNIT", "2 (pixels/inch)");
        else if (resUnit == 3)
            ds->setMd("", "TIFFTAG_RESOLUTIONUNIT", "3 (pixels/cm)");
    }

    std::string gdalMd = ifd.getAscii(TAG_GDAL_METADATA);
    if (!gdalMd.empty())
        parseGdalMetadata(gdalMd, *ds);

    if (ds->tiled)
    {
        uint64_t firstData = 0;
        for (uint64_t o : ds->chunkOffsets)
            if (o && (firstData == 0 || o < firstData))
                firstData = o;
        if (firstData == 0 || firstData > ifd.offset)
            ds->setMd("IMAGE_STRUCTURE", "LAYOUT", "COG");
    }

    if (!sub && ds->tf.ifds.size() > 1)
    {
        // reduced-resolution pages with matching band count become band
        // overviews; other reduced or mask pages vanish entirely; full
        // pages keep their IFD number in the subdataset list (gaps stay)
        std::vector<int> fullPages;
        for (size_t i = 0; i < ds->tf.ifds.size(); ++i)
        {
            const TiffIfd &pg = ds->tf.ifds[i];
            uint64_t stype = pg.getInt(254, 0);
            if (i > 0 && (stype & 1) && !(stype & 4) &&
                (uint32_t)pg.getInt(TAG_SPP, 1) == ds->spp)
            {
                ds->overviews.push_back({(int)pg.getInt(TAG_WIDTH),
                                         (int)pg.getInt(TAG_HEIGHT),
                                         (int)i + 1});
                continue;
            }
            if (i > 0 && ((stype & 1) || (stype & 4)))
                continue;
            fullPages.push_back((int)i);
        }
        if (fullPages.size() > 1)
        {
            for (int i : fullPages)
            {
                const TiffIfd &pg = ds->tf.ifds[i];
                int idx = i + 1;
                ds->setMd("SUBDATASETS",
                          strPrintf("SUBDATASET_%d_NAME", idx),
                          strPrintf("GTIFF_DIR:%d:%s", idx, path.c_str()));
                ds->setMd("SUBDATASETS",
                          strPrintf("SUBDATASET_%d_DESC", idx),
                          strPrintf("Page %d (%dP x %dL x %dB)", idx,
                                    (int)pg.getInt(TAG_WIDTH),
                                    (int)pg.getInt(TAG_HEIGHT),
                                    (int)pg.getInt(TAG_SPP, 1)));
            }
        }
    }

    ds->files.push_back(path);
    ds->pamPath = path + ".aux.xml";
    bool hasWfGT = false;
    double wfGT[6] = {0, 1, 0, 0, 0, 1};
    std::string wfPath;
    if (!sub)
    {
        struct stat st;
        if (!configTestBool("GDAL_PAM_ENABLED", true))
        {
            // sidecar neither read nor listed; the disabled note is
            // emitted after the GDALOpen trace below
        }
        else if (stat((path + ".aux.xml").c_str(), &st) == 0)
        {
            loadPam(path + ".aux.xml", *ds);
            ds->files.push_back(path + ".aux.xml");
        }
        if (stat((path + ".ovr").c_str(), &st) == 0)
        {
            ds->files.push_back(path + ".ovr");
            TiffFile otf;
            std::string oerr;
            if (TiffFile::open(path + ".ovr", otf, oerr))
            {
                ds->extOvrPath = path + ".ovr";
                for (size_t i = 0; i < otf.ifds.size(); ++i)
                {
                    const TiffIfd &pg = otf.ifds[i];
                    uint64_t stype = pg.getInt(254, 0);
                    if (i > 0 &&
                        (!(stype & 1) || (stype & 4) ||
                         (uint32_t)pg.getInt(TAG_SPP, 1) != ds->spp))
                        continue;
                    ds->extOverviews.push_back(
                        {(int)pg.getInt(TAG_WIDTH),
                         (int)pg.getInt(TAG_HEIGHT), (int)i + 1, true});
                }
            }
        }
        if (stat((path + ".msk").c_str(), &st) == 0)
            ds->files.push_back(path + ".msk");
        if (srcEnabled("WORLDFILE"))
        {
            size_t slash = path.find_last_of('/');
            size_t dot = path.find_last_of('.');
            std::string stem =
                (dot != std::string::npos &&
                 (slash == std::string::npos || dot > slash))
                    ? path.substr(0, dot)
                    : path;
            static const char *exts[] = {".tfw",  ".TFW", ".tifw",
                                         ".TIFW", ".wld", ".WLD"};
            for (const char *e : exts)
            {
                std::string cand = stem + e;
                std::string content;
                if (stat(cand.c_str(), &st) != 0 ||
                    !readFileToString(cand, content))
                    continue;
                double v[6];
                const char *p = content.c_str();
                char *end = nullptr;
                int i = 0;
                for (; i < 6; ++i)
                {
                    v[i] = strtod(p, &end);
                    if (end == p)
                        break;
                    p = end;
                }
                if (i < 6)
                    continue;
                wfGT[1] = v[0];
                wfGT[4] = v[1];
                wfGT[2] = v[2];
                wfGT[5] = v[3];
                wfGT[0] = v[4] - 0.5 * wfGT[1] - 0.5 * wfGT[2];
                wfGT[3] = v[5] - 0.5 * wfGT[4] - 0.5 * wfGT[5];
                hasWfGT = true;
                wfPath = cand;
                break;
            }
        }
    }

    // AREA_OR_POINT enters the default domain lazily, after PAM load, so
    // the (default) domain lists after PAM-created domains; only an
    // explicit GTRasterTypeGeoKey produces it
    const GeoKey *rasterType = findKey(keys, 1025);
    if (hasInternalGT && rasterType && rasterType->valueOrOffset == 2)
    {
        internalGT[0] -= internalGT[1] * 0.5 + internalGT[2] * 0.5;
        internalGT[3] -= internalGT[4] * 0.5 + internalGT[5] * 0.5;
    }
    // an explicit GTRasterTypeGeoKey overwrites any GDALMetadata item of
    // the same name in place
    if (rasterType)
        ds->setMd("", "AREA_OR_POINT",
                  rasterType->valueOrOffset == 2 ? "Point" : "Area");
    if (rasterType && rasterType->valueOrOffset == 2)
        for (auto &g : ds->gcps)
        {
            g.pixel += 0.5;
            g.line += 0.5;
        }

    for (const auto &s : srcs)
    {
        if (strEqualNoCase(s, "PAM") && ds->pamHasSrs)
        {
            ds->srs = std::move(ds->pamSrsObj);
            ds->hasSrs = true;
            ds->srsFromPam = true;
            break;
        }
        if (strEqualNoCase(s, "INTERNAL") && hasInternalSrs)
        {
            ds->srs = std::move(internalSrs);
            ds->hasSrs = true;
            ds->srsSynthetic = internalSrsSynthetic;
            break;
        }
    }
    for (const auto &s : srcs)
    {
        if (strEqualNoCase(s, "PAM") && ds->pamHasGt)
        {
            memcpy(ds->gt, ds->pamGt, sizeof(ds->gt));
            ds->hasGT = true;
            break;
        }
        if (strEqualNoCase(s, "INTERNAL") && hasInternalGT)
        {
            memcpy(ds->gt, internalGT, sizeof(ds->gt));
            ds->hasGT = true;
            break;
        }
        if (strEqualNoCase(s, "WORLDFILE") && hasWfGT)
        {
            memcpy(ds->gt, wfGT, sizeof(ds->gt));
            ds->hasGT = true;
            ds->files.push_back(wfPath);
            break;
        }
    }

    // with GCPs present the SRS is exposed through the GCP channel only
    if (!ds->gcps.empty() && ds->hasSrs)
    {
        ds->gcpSrs = std::move(ds->srs);
        ds->hasGcpSrs = true;
        ds->hasSrs = false;
        ds->srs = Srs();
    }

    if (!quiet)
    {
        ds->debugPtr = cplDebugPtr();
        cplDebug("GDAL", "GDALOpen(" + conn + ", this=" + ds->debugPtr +
                             ") succeeds as GTiff.");
        (void)gdalPamEnabled();
    }

    return ds;
}

std::unique_ptr<RasterDatasetBase> GTiffDataset::openOverviewEntry(
    const OvrEntry &e)
{
    if (!e.ext)
        return openOverviewPage(e.page);
    std::string err;
    OpenOptions oo;
    if (e.page <= 1)
        return openGTiff(extOvrPath, err, oo, extOvrPath, 0, false, 0,
                         true);
    std::string conn = strPrintf("GTIFF_DIR:%d:", e.page) + extOvrPath;
    return openGTiff(extOvrPath, err, oo, conn, e.page, false, 0, true);
}

std::unique_ptr<RasterDatasetBase> GTiffDataset::openOverviewPage(int page)
{
    std::string err;
    OpenOptions oo;
    std::string conn = strPrintf("GTIFF_DIR:%d:", page) + tf.path;
    return openGTiff(tf.path, err, oo, conn, page, false, 0, true);
}

std::unique_ptr<RasterDatasetBase> openRaster(const std::string &path,
                                              std::string &err,
                                              const OpenOptions &oo)
{
    bool gtiffAllowed = oo.allowedDrivers.empty();
    for (const auto &d : oo.allowedDrivers)
        if (strEqualNoCase(d, "GTiff"))
            gtiffAllowed = true;
    if (gdalSkipHas("GTiff"))
        gtiffAllowed = false;
    if (path.rfind("GTIFF_DIR:", 0) == 0)
    {
        if (!gtiffAllowed)
        {
            err = "unrecognized";
            return nullptr;
        }
        warnOpenOptions(oo);
        std::string rest = path.substr(10);
        bool useOff = rest.rfind("off:", 0) == 0;
        if (useOff)
            rest = rest.substr(4);
        size_t colon = rest.find(':');
        bool ok = colon != std::string::npos && colon > 0;
        uint64_t num = 0;
        if (ok)
        {
            for (size_t i = 0; i < colon; ++i)
            {
                if (rest[i] < '0' || rest[i] > '9')
                {
                    ok = false;
                    break;
                }
                num = num * 10 + (rest[i] - '0');
            }
        }
        std::string file = ok ? rest.substr(colon + 1) : std::string();
        if (!ok || file.empty() || num < 1)
        {
            cplErrorStr(
                CE_Failure, CPLE_OpenFailed,
                baseName(path) +
                    ": Unable to extract offset or filename, should take "
                    "the form:\nGTIFF_DIR:<dir>:filename or "
                    "GTIFF_DIR:off:<dir_offset>:filename");
            err = "reported";
            return nullptr;
        }
        struct stat st;
        if (stat(file.c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(path));
            err = "reported";
            return nullptr;
        }
        if (!TiffFile::identify(file))
        {
            std::string content;
            readFileToString(file, content);
            content.resize(4, '\0');
            uint16_t magic =
                (uint16_t)((uint8_t)content[0] |
                           ((uint16_t)(uint8_t)content[1] << 8));
            if (magic == 0x4949 || magic == 0x4D4D)
            {
                bool be = magic == 0x4D4D;
                uint16_t ver =
                    be ? (uint16_t)(((uint8_t)content[2] << 8) |
                                    (uint8_t)content[3])
                       : (uint16_t)((uint8_t)content[2] |
                                    ((uint16_t)(uint8_t)content[3] << 8));
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("%s:Not a TIFF file, bad version "
                                      "number %u (0x%x)",
                                      file.c_str(), ver, ver));
            }
            else
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("%s:Not a TIFF or MDI file, bad "
                                      "magic number %u (0x%x)",
                                      file.c_str(), magic, magic));
            err = "reported";
            return nullptr;
        }
        OpenOptions noWarn = oo;
        noWarn.raw.clear();
        return openGTiff(file, err, noWarn, path, useOff ? 0 : (int)num,
                         useOff, useOff ? num : 0);
    }
    if (gtiffAllowed && TiffFile::identify(path))
        return openGTiff(path, err, oo, path, 0, false, 0);
    bool vrtAllowed = oo.allowedDrivers.empty();
    for (const auto &d : oo.allowedDrivers)
        if (strEqualNoCase(d, "VRT"))
            vrtAllowed = true;
    if (gdalSkipHas("VRT"))
        vrtAllowed = false;
    if (vrtAllowed)
    {
        std::string content;
        if (readFileToString(path, content) && vrtDetect(content))
            return openVrtContent(path, content, err, oo);
    }
    err = "unrecognized";
    return nullptr;
}
