#pragma once
// IJG-compatible baseline JPEG encoder (islow FDCT, two-pass optimized
// Huffman) reproducing libjpeg output bit for bit, as used by libtiff's
// abbreviated-datastream TIFF/JPEG mode.
#include <cstddef>
#include <cstdint>
#include <vector>

std::vector<uint16_t> jpegQuantTable(bool chroma, int quality);

// natural-order index of zigzag position i
int jpegZigzagIndex(int i);

// abbreviated tables-only stream (SOI + DQTs + EOI) for tag 347;
// withHuff appends the standard Annex K Huffman tables per quant table
// and withQuant=false drops the DQTs (JPEGTABLESMODE quant|huff)
std::vector<uint8_t> jpegTablesStream(
    const std::vector<std::vector<uint16_t>> &qts, bool withHuff = false,
    bool withQuant = true);

struct JpegScanComp
{
    // samples row-major, dims = the component's own (possibly
    // downsampled) size before block padding
    std::vector<uint8_t> samples;
    int w = 0, h = 0;
    uint8_t id = 0;
    int hs = 1, vs = 1;
    int qtsel = 0;
    int tabsel = 0;
};

// one strip/tile as an abbreviated image stream: SOI + SOF + optimized
// DHTs + SOS + entropy + EOI. w,h are the SOF dims. stdHuff encodes with
// the standard Annex K tables and emits no DHT markers (the tables live
// in the JPEGTables stream); embedQuant emits the DQTs before the SOF
// when the JPEGTables stream doesn't carry them.
std::vector<uint8_t> jpegStripStream(
    const std::vector<JpegScanComp> &comps, int w, int h, int ntables,
    const std::vector<std::vector<uint16_t>> &qts, bool stdHuff = false,
    bool embedQuant = false);

void jpegRgbToYcc(uint8_t r, uint8_t g, uint8_t b, uint8_t &y,
                  uint8_t &cb, uint8_t &cr);

// h2v2 downsample with alternating bias; input dims (2*ow) x (2*oh),
// caller pre-pads odd edges by duplication
std::vector<uint8_t> jpegH2v2Downsample(const std::vector<uint8_t> &in,
                                        int ow, int oh);

struct JpegDecoded
{
    int w = 0, h = 0, ncomp = 0;
    std::vector<uint8_t> pixels;  // interleaved, w * h * ncomp
};

// baseline decoder matching libjpeg islow IDCT bit for bit; tables is an
// optional abbreviated JPEGTables stream. yccToRgb reproduces libtiff's
// JPEGCOLORMODE_RGB path (merged/box upsampling, integer YCC->RGB).
bool jpegDecodeStream(const uint8_t *data, size_t len,
                      const uint8_t *tables, size_t tablesLen,
                      bool yccToRgb, JpegDecoded &out);
