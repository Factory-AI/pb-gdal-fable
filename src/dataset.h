#pragma once
#include "srs.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

enum class DType
{
    Unknown,
    Byte,
    Int8,
    UInt16,
    Int16,
    UInt32,
    Int32,
    UInt64,
    Int64,
    Float16,
    Float32,
    Float64,
    CInt16,
    CInt32,
    CFloat32,
    CFloat64
};

const char *dtypeName(DType t);
DType dtypeFromName(const std::string &name);
int dtypeSizeBytes(DType t);
bool dtypeIsComplex(DType t);
const char *dtypeStacName(DType t);
float tailHalfToFloat(uint16_t h);
uint16_t tailFloatToHalf(float f);
// GDALCopyWords-style double -> native conversions shared with the
// pixel-function verbs (defined next to their calibration in the tail file)
double rasterFinishReal(double v, DType t);
void rasterEncodeReal(DType t, uint8_t *q, double re, double im);

using MetaDomain = std::vector<std::pair<std::string, std::string>>;

struct ColorEntry
{
    short c1 = 0, c2 = 0, c3 = 0, c4 = 255;
};

struct HistItem
{
    double mn = 0, mx = 0;
    long long buckets = 0;
    std::vector<long long> counts;
    bool approx = false;
};

struct StatsResult
{
    bool ok = false;
    double mn = 0, mx = 0, mean = 0, stddev = 0;
    double validPct = 0;
    bool subsampled = false;
    long long count = 0;
};

class RasterDatasetBase;

struct Band
{
    int index = 1;
    DType type = DType::Byte;
    int blockX = 0, blockY = 0;
    std::string colorInterp = "Undefined";
    bool hasNodata = false;
    double nodata = 0;
    // Int64/UInt64 bands carry the GDAL_NODATA text parsed as exact
    // integers (strtoll semantics: "9.2e18" reads back as 9)
    bool nodataIsI64 = false;
    long long nodataI64 = 0;
    bool nodataIsU64 = false;
    unsigned long long nodataU64 = 0;
    std::string description;
    std::map<std::string, MetaDomain> metadata;
    std::vector<ColorEntry> colorTable;
    bool hasOffset = false, hasScale = false;
    double offset = 0, scale = 1;
    std::string unitType;
    // derived from the vertical CRS at read time: displayed but never
    // serialized as a GDALMetadata item
    bool unitImplicit = false;
    std::vector<HistItem> pamHists;
    std::vector<std::string> domainOrder;
    std::vector<std::string> sortedDomains;
    std::map<std::string, std::string> xmlDomains;

    MetaDomain &md(const std::string &domain) { return metadata[domain]; }
    void setMd(const std::string &domain, const std::string &k,
               const std::string &v);
    const std::string *getMd(const std::string &domain,
                             const std::string &k) const;
    void removeMd(const std::string &domain, const std::string &k);
    void noteDomain(const std::string &domain);
    void markPamSorted(const std::string &domain);
};

struct GcpEntry
{
    std::string id, info;
    double pixel = 0, line = 0, x = 0, y = 0, z = 0;
};

struct PamBandState
{
    std::string description;
    std::string nodataRaw, scaleRaw, offsetRaw;
    MetaDomain mdi;
    bool mdiSorted = false;
    std::vector<std::array<std::string, 3>> extraMdi;  // domain, key, value
    std::vector<std::pair<std::string, std::string>> xmlDomains;
    std::vector<HistItem> hists;
};

class RasterDatasetBase
{
  public:
    virtual ~RasterDatasetBase() = default;

    std::string path;         // as passed by the user
    std::string driverShort;
    std::string driverLong;
    std::string debugPtr;     // this=%p stand-in used by debug traces
    int width = 0, height = 0;
    std::vector<Band> bands;
    bool hasGT = false;
    // hillshade of an ungeoreferenced source: streamed info reports no
    // geotransform, yet the written file carries the default one
    bool demWriteDefaultGt = false;
    // warp-produced outputs are created via Create + SetNoDataValue after
    // the georeferencing, so their GDAL_NODATA payload lands last
    virtual bool warpProduced() const { return false; }
    // pansharpen forces PHOTOMETRIC=RGB (with an unassociated alpha
    // sample on 4 bands) for 3/4-band Byte outputs and MINISBLACK
    // otherwise, whatever the band interpretations say
    virtual bool pansharpenProduced() const { return false; }
    // --bit-depth surfaces as an implicit NBITS creation option
    virtual int pansharpenNbits() const { return 0; }
    double gt[6] = {0, 1, 0, 0, 0, 1};
    Srs srs;
    bool hasSrs = false;
    bool srsSynthetic = false;
    bool srsFromPam = false;
    std::map<std::string, MetaDomain> metadata;
    std::vector<std::string> domainOrder;
    std::vector<std::string> sortedDomains;
    std::map<std::string, std::string> xmlDomains;
    std::vector<std::string> files;
    std::vector<GcpEntry> gcps;
    Srs gcpSrs;
    bool hasGcpSrs = false;
    std::vector<int> gcpMapping;  // echo of dataAxisToSRSAxisMapping
    // libtiff re-emits unprefixed directory diagnostics on later lazy
    // reads; they surface after argument validation, not at open
    struct DeferredDiag
    {
        bool warning = true;
        std::string text;
        bool debug = false;  // "GTiff: Opened WxH overview." trace entries
        bool mainPage = true;
        // lazy SRS decode diagnostics: also flushed alone at CRS
        // consumption points that never replay the full list
        bool srsDecode = false;
    };
    std::vector<DeferredDiag> deferredWarnings;
    void replayDeferred();
    void replaySrsDecodeWarnings();
    // diagnostics that fire again when the directory is rewritten in place
    std::vector<std::string> rewriteWarnings;
    // failure-class diagnostics were reported while opening (the dataset
    // still opened); dataset check treats this as a failed check
    bool openHadErrors = false;

    // PAM (.aux.xml) sidecar state, kept for rewriting
    std::string pamPath;
    bool pamExists = false, pamDirty = false;
    // wrapper datasets keep stats display-only (no sidecar writes)
    bool pamSuppressItems = false;
    std::string pamSrsRaw, pamSrsMapping, pamGtRaw;
    std::vector<std::array<std::string, 3>> pamMdi;  // domain, key, value
    std::vector<std::pair<std::string, std::string>> pamXmlDomains;
    std::map<int, PamBandState> pamBands;

    // subdataset (GTIFF_DIR:) state
    bool isSubdataset = false;
    std::string subName;  // connection string used to open

    // nameless VRT copy created by the pipeline edit step; already
    // presented (driver VRT, files = sources), so the tail materialize
    // hook must not re-present it
    bool inMemoryVrtCopy = false;

    // internal reduced-resolution IFDs surfaced as band overviews
    struct OvrEntry
    {
        int w = 0, h = 0;
        int page = 0;  // 1-based IFD index (in the .ovr file when ext)
        bool ext = false;
    };
    std::vector<OvrEntry> overviews;
    // levels carried by a .ovr sidecar; displayed only when no internal
    // overviews exist
    std::vector<OvrEntry> extOverviews;
    std::string extOvrPath;
    std::string extOvrDebugPtr;
    bool extOvrOpened = false;
    const std::vector<OvrEntry> &dispOverviews() const
    {
        return overviews.empty() && !extOverviews.empty() ? extOverviews
                                                          : overviews;
    }
    // "GDALDefaultOverviews::OverviewScan()" trace; a .ovr sidecar is
    // opened by the scan, with its own trace block
    void debugOverviewScan();
    virtual std::unique_ptr<RasterDatasetBase> openOverviewPage(int)
    {
        return nullptr;
    }
    virtual std::unique_ptr<RasterDatasetBase> openOverviewEntry(
        const OvrEntry &e)
    {
        return e.ext ? nullptr : openOverviewPage(e.page);
    }

    void setMd(const std::string &domain, const std::string &k,
               const std::string &v);
    const std::string *getMd(const std::string &domain,
                             const std::string &k) const;
    void removeMd(const std::string &domain, const std::string &k);
    void noteDomain(const std::string &domain);
    void markPamSorted(const std::string &domain);

    // reads whole band as doubles (real part; imag ignored except checksum)
    virtual bool readBand(int band, std::vector<double> &out) = 0;
    // raw pixel bytes in native dtype, row-major
    virtual bool readBandRaw(int band, std::vector<uint8_t> &out) = 0;
    // per-block read that fails (with libtiff-style errors) even where
    // the whole-image path would silently zero-fill
    virtual bool readBandRawStrict(int band, std::vector<uint8_t> &out)
    {
        return readBandRaw(band, out);
    }
    // reads every band, emitting copy-style progress ticks and
    // libtiff-style errors on read failure
    virtual bool readAllBands(std::vector<std::vector<uint8_t>> &out,
                              struct TermProgress *tp, bool strict = false);
    // color-map's soft map-load failure writes its all-zero result
    // without any progress bar
    virtual bool suppressWriteBar() const { return false; }
    // real on-disk block dims (for block-ordered scans)
    virtual void realBlockDims(int &bw, int &bh) const
    {
        bw = width;
        bh = height;
    }
    virtual struct TiffFile *tiffFile() { return nullptr; }
    virtual uint64_t tiffIfdOffset() const { return 0; }
    // dataset-level mask plane selected by band verbs (one byte per
    // pixel, nonzero = valid); false = no mask requested
    virtual bool selectMaskBand(std::vector<uint8_t> &out)
    {
        (void)out;
        return false;
    }
    // side-effect hooks for drivers whose lazy source opens surface
    // diagnostics at specific points (VRT)
    virtual void infoBandTouch(int band) { (void)band; }
    // driver-provided GetMinimum/GetMaximum (VRT sourceless bands)
    virtual bool bandMinMaxHint(int band, double &mn, double &mx)
    {
        (void)band;
        (void)mn;
        (void)mx;
        return false;
    }
    // -2 = compute normally; anything else is the checksum to report
    virtual int checksumHook(int band)
    {
        (void)band;
        return -2;
    }
    // histogram PAM persistence: -1 = plain dataset (persist normally),
    // 0 = wrapper without persistence, 1 = wrapper persisting into the
    // inherited (source) sidecar
    virtual int histPamMode(int band)
    {
        (void)band;
        return -1;
    }
    // innermost dataset a value-identity wrapper band forwards histogram
    // computation to (nullptr = compute locally)
    virtual RasterDatasetBase *histDelegateWrap(int band)
    {
        (void)band;
        return nullptr;
    }
    // wrapper datasets describe how their bands serialize as VRT sources:
    // source dtype for SourceProperties, Simple vs Complex tag, and extra
    // child elements rendered after DstRect
    virtual bool vrtWrapperSource(int band, bool &complexTag,
                                  DType &srcType, std::string &childrenXml)
    {
        (void)band;
        (void)complexTag;
        (void)srcType;
        (void)childrenXml;
        return false;
    }
    // SourceBand element body for wrapper serialization ("mask,N" for
    // mask-derived bands)
    virtual std::string vrtWrapperSourceBandText(int band)
    {
        return std::to_string(bands[(size_t)band - 1].index);
    }
    // geometry override for wrapper serialization: source dims/blocks and
    // SrcRect/DstRect when they differ from the 1:1 full-cover default
    struct WrapRects
    {
        int srcW = 0, srcH = 0, srcBlockX = 0, srcBlockY = 0;
        long long sx = 0, sy = 0, sw = 0, sh = 0;
        long long dx = 0, dy = 0, dw = 0, dh = 0;
        std::string resampling;
    };
    virtual bool vrtWrapperRects(WrapRects &wr)
    {
        (void)wr;
        return false;
    }
    // stats/histogram forwarding: full-cover single-SimpleSource VRT bands
    // compute on the source band (stats land in the source dataset's PAM)
    virtual RasterDatasetBase *statsDelegate(int band, int &delegateBand)
    {
        delegateBand = band;
        return this;
    }
    // full replacement XML for VRT output (warped datasets)
    virtual std::string customVrtXml(const std::string &input,
                                     const std::string &output)
    {
        (void)input;
        (void)output;
        return std::string();
    }
    // a warp with an empty source window leaves an orphaned 0.0 slot in
    // the written GeoDoubleParams tag; wrappers forward to their inner
    // dataset
    virtual bool geoDoubleOrphanHint()
    {
        return false;
    }
    // whether forwarded results may also be adopted as this band's own
    // (nodata settings must agree)
    virtual bool statsAdopt(int band)
    {
        (void)band;
        return true;
    }
    virtual void flushSourcePams() {}
    virtual void persistPam();
    // warped datasets emit the reference's warp-operation debug block in
    // place of the plain copy traces
    virtual bool warpDebugEmit(const std::string &outPath)
    {
        (void)outPath;
        return false;
    }
};

struct OpenOptions
{
    long ctMult = 0;  // 0 = AUTO
    bool georefSet = false;
    std::vector<std::string> georefSources;  // upper-cased tokens
    std::vector<std::string> allowedDrivers;  // empty = all
    std::vector<std::pair<std::string, std::string>> raw;  // as given
};

// Detects driver by content/extension; returns nullptr if unrecognized
std::unique_ptr<RasterDatasetBase> openRaster(const std::string &path,
                                              std::string &err,
                                              const OpenOptions &oo = {});

int checksumBand(RasterDatasetBase &ds, int band);
bool readBandValues(RasterDatasetBase &ds, int band,
                    std::vector<double> &vals);
StatsResult computeBandStats(RasterDatasetBase &ds, const Band &b,
                             bool approxOK);
// forced GetStatistics semantics with VRT delegation/adoption/mosaic rules;
// stores results (band md + PAM) on whichever datasets they belong to.
// tp (when given) is driven across [p0,p1] once per compute performed
StatsResult vrtAwareForcedStats(RasterDatasetBase &ds, Band &b, bool approx,
                                struct TermProgress *tp = nullptr,
                                double p0 = 0.0, double p1 = 1.0);
// forced GetDefaultHistogram semantics with VRT source forwarding and
// cache reuse; false = no histogram (failure error already emitted)
bool vrtAwareHistogram(RasterDatasetBase &ds, Band &b, HistItem &out,
                       bool &fresh);
bool computeHistogram(RasterDatasetBase &ds, const Band &b, HistItem &h);
void writePam(RasterDatasetBase &ds);
// "GDAL: GDALClose(<path>, this=<ptr>)" trace
void debugCloseDataset(const RasterDatasetBase &ds);
void emitHistogramsXml(std::string &out, const std::vector<HistItem> &hists,
                       const std::string &ind);
// canonical <GCPList> serialization shared by the VRT writer and editor
std::string gcpListXml(const RasterDatasetBase &ds,
                       const std::string &indent);
