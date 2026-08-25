#pragma once
#include "dataset.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct WarpParams
{
    std::string srcCrs, dstCrs, bboxCrs, like;
    std::string resampling = "nearest";
    bool resamplingSet = false;
    bool hasRes = false;
    double resX = 0, resY = 0;
    bool hasSize = false;
    long long sizeW = 0, sizeH = 0;
    bool hasBbox = false;
    double bbox[4] = {0, 0, 0, 0};
    bool tap = false;
    bool hasSrcNodata = false, hasDstNodata = false;
    std::vector<std::string> srcNodata, dstNodata;
    bool addAlpha = false;
    std::vector<std::string> wo, to;
    bool hasEt = false;
    double et = 0;
    bool hasNumThreads = false;
    std::string numThreads;
    // sample destination coords on a global pixel grid anchored at
    // (pgOX, pgOY) with this warp offset by (pgIX, pgIY) pixels, as a
    // windowed read on a larger warped dataset would
    bool pixGrid = false;
    double pgOX = 0, pgOY = 0;
    long long pgIX = 0, pgIY = 0;
};

using WarpGetter =
    std::function<const std::vector<std::string> *(const std::string &)>;

WarpParams warpFillParams(const WarpGetter &get);
// canonical " --arg value" echo used for GDALG command lines
std::string warpArgsEcho(const WarpParams &p);
// integer or ALL_CPUS (case-insensitive)
bool warpNumThreadsValid(const std::string &v);
// wraps ds in the warped dataset; emits its own errors; 0 = ok.
// leafUsage selects leaf abort semantics for the -tap check.
int warpWrap(const WarpParams &p, std::unique_ptr<RasterDatasetBase> &ds,
             bool leafUsage);

struct XmlNode;
// opens a VRTDataset subClass="VRTWarpedDataset"; emits its own errors
// and sets err="reported" on failure
std::unique_ptr<RasterDatasetBase> openWarpedVrt(const XmlNode &root,
                                                 const std::string &path,
                                                 std::string &err);
