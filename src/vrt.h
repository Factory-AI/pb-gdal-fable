#pragma once
#include "dataset.h"
#include "xml_min.h"

struct VrtSource
{
    bool complex = false;
    std::string rawName;
    int relativeToVRT = 0;
    std::string resolved;
    int sourceBand = 1;
    bool maskBand = false;
    bool hasSrcRect = false, hasDstRect = false;
    double sx = 0, sy = 0, sw = 0, sh = 0;
    double dx = 0, dy = 0, dw = 0, dh = 0;
    bool hasScaleRatio = false, hasScaleOffset = false;
    double scaleRatio = 1, scaleOffset = 0;
    bool hasExponent = false, expClip = false;
    double exponent = 1, expSrcMin = 0, expSrcMax = 0, expDstMin = 0,
           expDstMax = 0;
    bool hasLut = false;
    std::vector<std::pair<double, double>> lut;
    int ctComponent = 0;
    // inline <VRTDataset> source: kept as a parsed subtree so the
    // serialized copy re-emits it CPL-formatted, plus an eagerly opened
    // dataset for evaluation
    bool hasNested = false;
    XmlNode nestedNode;
    std::shared_ptr<RasterDatasetBase> nestedDs;
    // 0 unknown, 1 open, 2 missing file, 3 illegal band, 4 unrecognized
    int state = 0;
    std::shared_ptr<RasterDatasetBase> ds;
    std::string failMsg;
    int failClass = 4;

    bool transforming() const
    {
        return hasScaleRatio || hasScaleOffset || hasExponent || hasLut ||
               ctComponent > 0 || maskBand || hasNested;
    }
};

class VrtDataset : public RasterDatasetBase
{
  public:
    std::vector<std::vector<VrtSource>> bandSources;
    std::vector<int> axisMapping;  // dataAxisToSRSAxisMapping attr echo

    bool readBand(int band, std::vector<double> &out) override;
    bool readBandRaw(int band, std::vector<uint8_t> &out) override;
    bool readAllBands(std::vector<std::vector<uint8_t>> &out,
                      struct TermProgress *tp, bool strict) override;
    void infoBandTouch(int band) override;
    int checksumHook(int band) override;
    bool bandMinMaxHint(int band, double &mn, double &mx) override;
    RasterDatasetBase *statsDelegate(int band, int &delegateBand) override;
    bool statsAdopt(int band) override;
    // full-raster non-overlapping in-bounds SimpleSources whose stats can
    // be computed per source and merged
    bool mosaicStatsParts(int band, std::vector<VrtSource *> &parts);
    // histogram forwarding: lone unscaled source reading its whole raster
    VrtSource *histDelegate(int band);
    void flushSourcePams() override;
    void persistPam() override;

    RasterDatasetBase *sourceAttempt(VrtSource &s);
    VrtSource *firstFailing(int band);
    bool computeBandDouble(int band, std::vector<double> &out);
    bool computeBandPlanes(int band, std::vector<double> &re,
                           std::vector<double> *im,
                           bool smallIntDest = false);

  private:
    // shared-open semantics: sources naming the same file share one dataset
    std::map<std::string, std::shared_ptr<RasterDatasetBase>> srcCache;
};

bool vrtDetect(const std::string &content);
std::unique_ptr<RasterDatasetBase> openVrtContent(const std::string &path,
                                                  const std::string &content,
                                                  std::string &err,
                                                  const OpenOptions &oo);
std::string vrtSerializeXml(RasterDatasetBase &ds, const std::string &input,
                            const std::string &output);
