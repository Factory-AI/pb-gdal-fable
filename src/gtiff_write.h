#pragma once
#include "dataset.h"
#include "srs.h"

#include <string>
#include <vector>

struct GeoTags
{
    bool any = false;
    bool zScaleOne = false;
    std::vector<uint16_t> dir;
    std::vector<double> doubles;
    std::string ascii;
};

bool buildGeoTags(const Srs &srs, GeoTags &out, bool point = false,
                  bool forceDir = false, int gtVersion = 0);

struct GmdItem
{
    std::string name;
    std::string value;
    int sample = -1;
    std::string role;
    std::string domain;
};

class RasterDatasetBase;

std::vector<GmdItem> buildGmdItems(const RasterDatasetBase &ds,
                                   int photometric,
                                   const std::vector<uint16_t> &extras,
                                   bool sortMd = true,
                                   int allDomains = 0);

struct GTiffCreateParams
{
    int width = 0;
    int height = 0;
    int bands = 1;
    DType type = DType::Byte;

    // creation options
    int compression = 1;  // TIFF compression code
    int predictor = 0;    // 0 = unset
    int zlevel = 6;
    int zstdLevel = 9;
    bool tiled = false;
    int blockX = 0;  // 0 = auto; in strip mode blockY overrides RowsPerStrip
    int blockY = 0;
    bool bandInterleave = false;
    bool sparse = false;
    bool bigtiff = false;
    std::string profile = "GDALGeoTIFF";

    bool hasNodata = false;
    double nodata = 0;
    std::string nodataText;
    // warp-created outputs set nodata after georeferencing, so the
    // GDAL_NODATA payload lands after the geo payloads
    bool nodataLate = false;
    bool geoDoubleOrphan = false;
    bool hasGT = false;
    double gt[6] = {0, 1, 0, 0, 0, 1};
    const Srs *srs = nullptr;
    const std::vector<GcpEntry> *gcps = nullptr;  // tiepoints (33922)
    const Srs *gcpSrs = nullptr;  // geokey source when GCPs are present
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<double> burn;  // per-band fill; empty = zeros

    bool append = false;

    // CreateCopy-style extensions
    const std::vector<std::vector<uint8_t>> *pixels = nullptr;  // per band
    bool useGmdItems = false;
    std::vector<GmdItem> gmdItems;
    const std::vector<ColorEntry> *colorTable = nullptr;
    int photometric = -1;  // -1 = derive from bands/type (create rule)
    bool extrasSet = false;
    std::vector<uint16_t> extraSamples;
    // recognized TIFFTAG_* metadata, in metadata-sorted placement order
    std::vector<std::pair<uint16_t, std::string>> asciiTags;
    bool hasXRes = false, hasYRes = false;
    float xres = 0, yres = 0;
    int minSample = -1, maxSample = -1, resUnit = 0;
    bool hasXform = false;
    double xform[16] = {};
    bool pointPixel = false;   // RasterTypeGeoKey = Point
    bool forceGeoDir = false;  // GK dir even without usable SRS
    int nbits = 0;             // sub-native BitsPerSample; 0 = native
    bool bigEndian = false;
    int gtVersion = 0;          // 0 AUTO, 10, 11
    int jpegQuality = 75;
    int jpegTablesMode = 1;
    int webpLevel = 75;
    bool webpLossless = false;
    // failed-JPEG leftovers: header/IFD only, JPEGTables only when the
    // in-memory tables dataset got past JPEGSetupEncode
    bool jpegStub = false;
    bool jpegStubTables = false;
    bool jpegStubNoStrips = false;  // pre-copy failure: strile arrays
                                    // never crystallized
    bool omitPhotometric = false;
    bool synthPalette = false;  // ramp color map from PHOTOMETRIC=PALETTE
    // NBITS clipping during the copy loop fails the convert algorithm
    // even though the file is fully written
    bool *clipNote = nullptr;
    // vector grid choreography: the reference crystallizes a pre-geo
    // directory at offset 8 (zeroed strip pointers), streams the strips,
    // then relocates the grown directory to EOF leaving the orphan behind
    bool gridOrphanIfd = false;
    // dataset mask plane (one byte per pixel, nonzero = valid): written
    // as a chained 1-bit deflate mask IFD after the main directory
    const std::vector<uint8_t> *maskPixels = nullptr;
};

bool gtiffWrite(const std::string &path, const GTiffCreateParams &p,
                std::string &err);

// COG (cloud optimized) layout: ghost header, all IFDs ahead of the
// data, row-major tiles with size leaders and last-4-bytes trailers,
// overview data before full resolution
struct CogOverview
{
    int w = 0, h = 0;
    const std::vector<std::vector<uint8_t>> *pixels = nullptr;
};

bool cogWrite(const std::string &path, const GTiffCreateParams &p,
              const std::vector<CogOverview> &ovrs, std::string &err);

// overview kernels shared with the overview verbs (store-rounded grids)
void cogResampleLevel(const std::vector<std::vector<double>> &src, int sw,
                      int sh, std::vector<std::vector<double>> &dst,
                      int dw, int dh, const std::string &method, DType dt,
                      bool hasNodata, double nodata);

struct CreationOptions
{
    int compression = 1;
    int predictor = 0;
    int zlevel = 6;
    bool tiled = false;
    int blockX = 0;
    int blockY = 0;
    bool bandInterleave = false;
    bool interleaveSet = false;
    bool sparse = false;
    int bigtiffMode = 0;  // 0 NO, 1 YES, 2 IF_NEEDED, 3 IF_SAFER
    std::string profile = "GDALGeoTIFF";
    bool fatal = false;

    bool compressBad = false;
    bool missingCodec = false;
    std::string compressRaw;
    bool hasPredictor = false;
    std::string predictorRaw;
    bool zlevelBad = false;
    std::string zlevelRaw;
    int zstdLevel = 9;
    bool zstdBad = false;
    std::string zstdRaw;
    bool jpegQualityBad = false;
    std::string jpegQualityRaw;
    int jpegQuality = 75;
    int jpegTablesMode = 1;
    int webpLevel = 75;
    bool webpLevelBad = false;
    std::string webpLevelRaw;
    bool webpLevelSpecified = false;
    bool webpLossless = false;
    bool hasPhotometric = false;
    std::string photometricVal;  // uppercased
    std::string photometricRaw;
    bool hasNbits = false;
    int nbits = 0;
    bool hasEndian = false;
    std::string endianVal;  // uppercased
    int gtVersion = 0;      // 0 AUTO, 10, 11

    // filled by finalizeCreationOptions
    int nbitsFinal = 0;
    bool halfFloat = false;
    bool endianBig = false;
    int resolvedPhot = -1;   // -1 = default derivation
    bool photApplied = false;  // valid PHOTOMETRIC actually applied
    bool photOmit = false;
    bool synthPalette = false;
    bool extrasSet = false;
    std::vector<uint16_t> extras;
    bool gmdColorinterp = false;  // create-side COLORINTERP items
    bool sumPhotWarn = false;
    int blockXFinal = 0;
    int blockYFinal = 0;
    int predictorFinal = 0;
};

CreationOptions parseCreationOptions(
    const std::vector<std::pair<std::string, std::string>> &cos,
    const std::string &filename, const std::string &algName);

// driver-stage creation-option resolution: emits the reference's
// diagnostics in its fixed check order; false = hard error (rc 1)
bool finalizeCreationOptions(CreationOptions &o, const std::string &output,
                             int bands, DType type,
                             bool isConvert = false);

// CPLGetFilename(): the path with any directory part stripped, as used
// by driver diagnostics
std::string fnameOf(const std::string &p);

// one JPEG strip/tile from interleaved raw samples (or one plane when
// plane >= 0); photometric 6 takes RGB input and downsamples to YCbCr.
// tablesMode is the JPEGTABLESMODE bitmask: bit0 = quant tables live in
// JPEGTables (else embedded per block), bit1 = standard Annex K Huffman
// coding with the tables in JPEGTables (else optimized per block).
std::vector<uint8_t> jpegBlock(const std::vector<uint8_t> &raw, int w,
                               int rows, int spp, uint16_t photometric,
                               int plane, int quality, bool warn = true,
                               int tablesMode = 1);
bool jpegCoarseTables(uint16_t photometric, int spp, int quality);
void jpegCoarseWarn();
