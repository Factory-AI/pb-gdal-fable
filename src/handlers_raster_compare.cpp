// raster compare: structural and pixel diff between two datasets, exit
// code equals the number of reported differences
#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "progress.h"
#include "spec.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace
{

bool dtypeIntegral(DType t)
{
    switch (t)
    {
        case DType::Byte:
        case DType::Int8:
        case DType::UInt16:
        case DType::Int16:
        case DType::UInt32:
        case DType::Int32:
        case DType::UInt64:
        case DType::Int64:
        case DType::CInt16:
        case DType::CInt32:
            return true;
        default:
            return false;
    }
}

int maskFlagsOf(const RasterDatasetBase &ds, const Band &b)
{
    if (b.hasNodata)
        return 8;
    size_t n = ds.bands.size();
    if ((n == 2 || n == 4) && ds.bands.back().colorInterp == "Alpha" &&
        b.index != (int)n)
        return 6;
    return 1;
}

const MetaDomain *defaultDomain(const RasterDatasetBase &ds)
{
    auto it = ds.metadata.find("");
    return it == ds.metadata.end() ? nullptr : &it->second;
}

const std::string *domainValue(const MetaDomain *d, const std::string &k)
{
    if (!d)
        return nullptr;
    for (const auto &kv : *d)
        if (kv.first == k)
            return &kv.second;
    return nullptr;
}

std::unique_ptr<RasterDatasetBase> openOrFail(const std::string &path,
                                              int &rc)
{
    std::string err;
    auto ds = openRaster(path, err);
    if (!ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + path +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        rc = 1;
    }
    return ds;
}

int rasterCompareHandler(const CmdSpec &, ParseResult &r)
{
    std::string refPath = r.str("reference");
    std::string inPath = r.str("input");
    bool quiet = r.flag("quiet");
    bool skipAll = r.flag("skip-all-optional");
    bool skipBinary = skipAll || r.flag("skip-binary");
    bool skipCrs = skipAll || r.flag("skip-crs");
    bool skipGt = skipAll || r.flag("skip-geotransform");
    bool skipOvr = skipAll || r.flag("skip-overview");
    bool skipMd = skipAll || r.flag("skip-metadata");

    int rc = 0;
    auto ref = openOrFail(refPath, rc);
    if (!ref)
        return rc;
    auto in = openOrFail(inPath, rc);
    if (!in)
        return rc;

    std::vector<std::string> diffs;

    if (!skipBinary)
    {
        std::string refBytes, inBytes;
        bool okR = readFileToString(refPath, refBytes);
        bool okI = readFileToString(inPath, inBytes);
        if (okR && okI)
        {
            if (refBytes == inBytes)
                return 0;
            if (refBytes.size() != inBytes.size())
                diffs.push_back(strPrintf(
                    "Reference file has size %lld bytes, whereas input "
                    "file has size %lld bytes.",
                    (long long)refBytes.size(), (long long)inBytes.size()));
            else
                diffs.push_back("Reference file and input file differ at "
                                "the binary level.");
        }
    }

    if (!skipCrs)
    {
        ref->replaySrsDecodeWarnings();
        in->replaySrsDecodeWarnings();
        if (ref->hasSrs != in->hasSrs)
            diffs.push_back(ref->hasSrs
                                ? "Reference dataset has a CRS, but input "
                                  "dataset has none."
                                : "Reference dataset has no CRS, but input "
                                  "dataset has one.");
        else if (ref->hasSrs && !ref->srs.isEquivalentTo(in->srs))
            diffs.push_back("Reference and input CRS are not equivalent. "
                            "Reference one is '" +
                            ref->srs.wkt2SingleLine() + "'. Input one is '" +
                            in->srs.wkt2SingleLine() + "'");
    }

    if (!skipGt)
    {
        if (ref->hasGT != in->hasGT)
            diffs.push_back(ref->hasGT
                                ? "Reference dataset has a geotransform, "
                                  "but input one has none."
                                : "Reference dataset has no geotransform, "
                                  "but input one has one.");
        else if (ref->hasGT)
        {
            bool same = true;
            for (int i = 0; i < 6; ++i)
                if (ref->gt[i] != in->gt[i])
                    same = false;
            if (!same)
                diffs.push_back(strPrintf(
                    "Geotransform of reference and input dataset are not "
                    "equivalent. Reference geotransform is "
                    "(%f,%f,%f,%f,%f,%f). Input geotransform is "
                    "(%f,%f,%f,%f,%f,%f)",
                    ref->gt[0], ref->gt[1], ref->gt[2], ref->gt[3],
                    ref->gt[4], ref->gt[5], in->gt[0], in->gt[1], in->gt[2],
                    in->gt[3], in->gt[4], in->gt[5]));
        }
    }

    if (ref->bands.size() != in->bands.size())
        diffs.push_back(strPrintf(
            "Reference dataset has %d band(s), but input dataset has %d",
            (int)ref->bands.size(), (int)in->bands.size()));
    if (ref->width != in->width)
        diffs.push_back(strPrintf(
            "Reference dataset width is %d, but input dataset width is %d",
            ref->width, in->width));
    if (ref->height != in->height)
        diffs.push_back(strPrintf("Reference dataset height is %d, but "
                                  "input dataset height is %d",
                                  ref->height, in->height));

    if (!skipMd)
    {
        const MetaDomain *dr = defaultDomain(*ref);
        const MetaDomain *di = defaultDomain(*in);
        // iteration is key-sorted on both sides (std::map in the
        // reference implementation), unlike info's insertion order
        auto sortedItems = [](const MetaDomain *d)
        {
            std::vector<std::pair<std::string, std::string>> v;
            if (d)
                for (const auto &kv : *d)
                    v.push_back(kv);
            std::sort(v.begin(), v.end(),
                      [](const auto &a, const auto &b)
                      { return a.first < b.first; });
            return v;
        };
        for (const auto &kv : sortedItems(dr))
        {
            const std::string *iv = domainValue(di, kv.first);
            if (!iv)
                diffs.push_back("Reference metadata (dataset default "
                                "metadata domain) contains key '" +
                                kv.first +
                                "' but input metadata does not.");
            else if (*iv != kv.second)
                // reference/input values swapped, faithful to the
                // reference implementation
                diffs.push_back("Reference metadata (dataset default "
                                "metadata domain) has value '" +
                                *iv + "' for key '" + kv.first +
                                "' but input metadata has value '" +
                                kv.second + "'.");
        }
        for (const auto &kv : sortedItems(di))
            if (!domainValue(dr, kv.first))
                diffs.push_back("Input metadata (dataset default "
                                "metadata domain) contains key '" +
                                kv.first +
                                "' but reference metadata does not.");
    }

    if (ref->bands.size() == in->bands.size() &&
        ref->width == in->width && ref->height == in->height)
    {
        for (size_t bi = 0; bi < ref->bands.size(); ++bi)
        {
            const Band &br = ref->bands[bi];
            const Band &bn = in->bands[bi];
            int n = (int)bi + 1;
            if (br.type != bn.type)
                diffs.push_back(strPrintf(
                    "Reference band %d has data type %s, but input band "
                    "has data type %s",
                    n, dtypeName(br.type), dtypeName(bn.type)));
            if (br.hasNodata != bn.hasNodata)
                diffs.push_back(
                    br.hasNodata
                        ? strPrintf("Reference band %d has nodata value "
                                    "%f, but input band has none.",
                                    n, br.nodata)
                        : strPrintf("Reference band %d has no nodata "
                                    "value, but input band has no data "
                                    "value %f.",
                                    n, bn.nodata));
            else if (br.hasNodata && br.nodata != bn.nodata &&
                     !(std::isnan(br.nodata) && std::isnan(bn.nodata)))
                diffs.push_back(strPrintf(
                    "Reference band %d has nodata value %f, but input "
                    "band has no data value %f.",
                    n, br.nodata, bn.nodata));
            int mr = maskFlagsOf(*ref, br), mi = maskFlagsOf(*in, bn);
            if (mr != mi)
                diffs.push_back(strPrintf(
                    "Reference band %d has mask flags = %d , but input "
                    "band has mask flags = %d",
                    n, mr, mi));
            if (br.colorInterp != bn.colorInterp)
                diffs.push_back(strPrintf(
                    "Reference band %d has color interpretation %s, but "
                    "input band has color interpretation %s",
                    n, br.colorInterp.c_str(), bn.colorInterp.c_str()));
            if (!skipOvr)
            {
                int or_ = (int)ref->dispOverviews().size();
                int oi = (int)in->dispOverviews().size();
                if (or_ != oi)
                    diffs.push_back(strPrintf(
                        "Reference band %d has %d overview band(s), but "
                        "input band has %d",
                        n, or_, oi));
            }
        }

        if (ref->width == in->width && ref->height == in->height)
        {
            TermProgress tp;
            int nb = (int)ref->bands.size();
            for (int b = 1; b <= nb; ++b)
            {
                std::vector<double> vr, vi;
                bool okR = ref->readBand(b, vr);
                bool okI = in->readBand(b, vi);
                if (okR && okI && vr.size() == vi.size())
                {
                    long long ndiff = 0;
                    double maxd = 0;
                    for (size_t i = 0; i < vr.size(); ++i)
                    {
                        double a = vr[i], c = vi[i];
                        if (a == c || (std::isnan(a) && std::isnan(c)))
                            continue;
                        ++ndiff;
                        double d = std::fabs(a - c);
                        if (d > maxd)
                            maxd = d;
                    }
                    if (ndiff)
                    {
                        diffs.push_back(strPrintf(
                            "%d: pixels differing: %lld", b, ndiff));
                        bool intFmt = dtypeIntegral(ref->bands[b - 1].type) &&
                                      dtypeIntegral(in->bands[b - 1].type);
                        diffs.push_back(
                            intFmt ? strPrintf("%d: maximum pixel value "
                                               "difference: %lld",
                                               b, (long long)maxd)
                                   : strPrintf("%d: maximum pixel value "
                                               "difference: %f",
                                               b, maxd));
                    }
                }
                if (!quiet)
                    tp.update((double)b / nb);
            }
        }
    }

    for (const std::string &d : diffs)
        printf("%s\n", d.c_str());
    fflush(stdout);
    return (int)diffs.size();
}

}  // namespace

void registerRasterCompareHandler()
{
    registerHandler("raster_compare", rasterCompareHandler);
}
