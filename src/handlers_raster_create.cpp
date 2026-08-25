#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "gtiff_write.h"
#include "srs.h"
#include "util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace
{

bool parseNodataValue(const std::string &s, double &out)
{
    std::string l = strToLower(s);
    if (l == "nan")
    {
        out = NAN;
        return true;
    }
    if (l == "inf" || l == "infinity")
    {
        out = INFINITY;
        return true;
    }
    if (l == "-inf" || l == "-infinity")
    {
        out = -INFINITY;
        return true;
    }
    char *end = nullptr;
    out = strtod(s.c_str(), &end);
    return end && *end == '\0' && end != s.c_str();
}

bool nodataRepresentable(DType t, double v, double &stored)
{
    stored = v;
    auto intRange = [&](double lo, double hi)
    {
        if (std::isnan(v) || !std::isfinite(v))
            return false;
        if (v != floor(v))
            return false;
        return v >= lo && v <= hi;
    };
    switch (t)
    {
        case DType::Byte:
            return intRange(0, 255);
        case DType::Int8:
            return intRange(-128, 127);
        case DType::UInt16:
            return intRange(0, 65535);
        case DType::Int16:
            return intRange(-32768, 32767);
        case DType::UInt32:
            return intRange(0, 4294967295.0);
        case DType::Int32:
            return intRange(-2147483648.0, 2147483647.0);
        case DType::Float32:
            stored = (double)(float)v;
            return true;
        case DType::Float16:
        {
            // exact half representation required (NaN/Inf pass)
            if (std::isnan(v) || std::isinf(v) || v == 0)
                return true;
            double a = fabs(v);
            if (a > 65504.0)
                return false;
            if (a < 6.103515625e-05)
            {
                double k = a * 16777216.0;
                return k == floor(k);
            }
            int e;
            double m = frexp(a, &e);
            double q = m * 2048.0;
            return q == floor(q);
        }
        default:
            // complex types are unvalidated and store the raw double
            return true;
    }
}

DType dtypeFromCli(const std::string &s)
{
    if (s == "UInt8")
        return DType::Byte;
    return dtypeFromName(s);
}
int rasterCreateHandler(const CmdSpec &, ParseResult &r)
{
    std::string output = r.str("output");
    const ArgValue *size = r.get("size");
    const ArgValue *like = r.get("input");
    const ArgValue *nodataArg = r.get("nodata");
    const ArgValue *crsArg = r.get("crs");
    const ArgValue *bboxArg = r.get("bbox");

    // ---- argument-stage validation (usage on error) ----
    std::string format = r.str("output-format");
    if (!format.empty())
    {
        std::string uf = strToLower(format);
        if ((uf != "gtiff" && uf != "cog" && uf != "vrt" && uf != "mem" &&
             uf != "gdalg") ||
            (uf != "gdalg" && gdalSkipHas(uf)))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Invalid value for argument "
                        "'output-format'. Driver '" +
                            format + "' does not exist.");
            handlerPrintUsage();
            return 1;
        }
        if (uf != "gtiff")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: only GTiff output is built in this "
                        "reimplementation");
            return 1;
        }
    }

    bool hasNodata = false;
    double nodata = 0;
    std::string nodataStr;
    bool nodataNone = false;
    if (nodataArg)
    {
        std::string s = nodataArg->str();
        if (strToLower(s) == "none")
            nodataNone = true;
        else if (parseNodataValue(s, nodata))
        {
            hasNodata = true;
            nodataStr = s;
        }
        else
        {
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        "create: Value of 'nodata' should be 'none', a "
                        "numeric value, 'nan', 'inf' or '-inf'");
            handlerPrintUsage();
            return 1;
        }
    }
    (void)nodataNone;

    Srs srs;
    bool hasSrs = false;
    if (crsArg)
    {
        std::string c = crsArg->str();
        bool ok = false;
        srs = Srs::fromCliInput(c, ok, true);
        if (!ok || !srs.valid())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Invalid value for 'crs' argument");
            handlerPrintUsage();
            return 1;
        }
        hasSrs = true;
    }

    double bbox[4] = {0, 0, 0, 0};
    bool hasBbox = false;
    if (bboxArg)
    {
        for (int i = 0; i < 4; i++)
            bbox[i] = atof(bboxArg->values[i].c_str());
        if (bbox[0] > bbox[2] || bbox[1] > bbox[3])
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Value of 'bbox' should be xmin,ymin,xmax,ymax with "
                        "xmin <= xmax and ymin <= ymax");
            handlerPrintUsage();
            return 1;
        }
        hasBbox = true;
    }

    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");
    struct stat st;
    bool exists = stat(output.c_str(), &st) == 0;
    if (exists && !overwrite && !append)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "create: Dataset '" + output +
                        "' already exists. You may specify the "
                        "--overwrite/--append option.");
        handlerPrintUsage();
        return 1;
    }

    // extension-based driver guess when no -f
    if (format.empty())
    {
        size_t slash = output.find_last_of('/');
        std::string basename =
            slash == std::string::npos ? output : output.substr(slash + 1);
        size_t dot = basename.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < basename.size())
        {
            std::string ext = strToLower(basename.substr(dot + 1));
            if ((ext != "tif" && ext != "tiff") || gdalSkipHas("GTiff"))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "create: Cannot guess driver for " + output);
                return 1;
            }
        }
        else if (gdalSkipHas("GTiff"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Cannot guess driver for " + output);
            return 1;
        }
    }

    // ---- run stage (no usage trailer) ----
    GTiffCreateParams p;

    std::unique_ptr<RasterDatasetBase> likeDs;
    if (like)
    {
        std::string err;
        likeDs = openRaster(like->str(), err);
        if (!likeDs)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + like->str() +
                            "' not recognized as being in a supported file "
                            "format.");
            return 1;
        }
        p.width = likeDs->width;
        p.height = likeDs->height;
        p.bands = (int)likeDs->bands.size();
        p.type = likeDs->bands.empty() ? DType::Byte : likeDs->bands[0].type;
        if (likeDs->hasSrs)
        {
            srs = std::move(likeDs->srs);
            hasSrs = true;
        }
        if (likeDs->hasGT)
        {
            memcpy(p.gt, likeDs->gt, sizeof(p.gt));
            p.hasGT = true;
        }
    }

    if (!size && !like)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "create: Argument 'size' should be specified, or 'like' "
                    "dataset should be specified");
        return 1;
    }
    if (size)
    {
        p.width = atoi(size->values[0].c_str());
        p.height = atoi(size->values[1].c_str());
    }
    const ArgValue *bc = r.get("band-count");
    if (bc)
        p.bands = atoi(bc->str().c_str());
    const ArgValue *dt = r.get("output-data-type");
    if (dt)
        p.type = dtypeFromCli(dt->str());

    const ArgValue *burn = r.get("burn");
    if (burn && !burn->values.empty())
    {
        if ((int)burn->values.size() != 1 &&
            (int)burn->values.size() != p.bands)
        {
            if (p.bands == 1)
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "create: One value should be provided for "
                            "argument 'burn', given there is one band");
            else
                cplErrorStr(
                    CE_Failure, CPLE_IllegalArg,
                    strPrintf("create: One or %d values should be provided "
                              "for argument 'burn', given there are %d bands",
                              p.bands, p.bands));
            return 1;
        }
        for (const auto &v : burn->values)
            p.burn.push_back(atof(v.c_str()));
    }

    if (p.width <= 0 || p.height <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("Attempt to create %dx%d dataset is illegal, "
                              "sizes must be larger than zero.",
                              p.width, p.height));
        return 1;
    }

    std::vector<std::pair<std::string, std::string>> cos;
    for (const auto &c : r.list("creation-option"))
    {
        size_t eq = c.find('=');
        cos.push_back({c.substr(0, eq), c.substr(eq + 1)});
    }
    CreationOptions o = parseCreationOptions(cos, output, "create");
    if (o.fatal)
        return 1;

    std::string dbgClosePtr;
    if (cplDebugEnabled("GDAL"))
    {
        dbgClosePtr = cplDebugPtr();
        cplDebug("GDAL",
                 strPrintf("GDALDriver::Create(GTiff,%s,%d,%d,%d,%s,%s)",
                           output.c_str(), p.width, p.height, p.bands,
                           dtypeName(p.type),
                           cos.empty() ? "(nil)" : cplDebugPtr().c_str()));
    }

    if (p.width <= 0 || p.height <= 0 || p.bands <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: Attempt to create %dx%dx%d TIFF file, "
                              "but width, height and bands must be positive.",
                              output.c_str(), p.width, p.height, p.bands));
        return 1;
    }

    if (!finalizeCreationOptions(o, output, p.bands, p.type))
        return 1;
    if (o.tiled)
    {
        p.tiled = true;
        p.blockX = o.blockXFinal;
        p.blockY = o.blockYFinal;
    }
    else
        p.blockY = o.blockYFinal;
    p.predictor = o.predictorFinal;
    p.compression = o.compression;
    p.zlevel = o.zlevel;
    p.zstdLevel = o.zstdLevel;
    p.bandInterleave = o.bandInterleave;
    p.sparse = o.sparse;
    p.profile = o.profile;
    {
        const double rawSize = (double)p.width * p.height * p.bands *
                               dtypeSizeBytes(p.type);
        p.bigtiff = o.bigtiffMode == 1 ||
                    (o.bigtiffMode == 2 && o.compression == 1 &&
                     rawSize > 4200000000.0) ||
                    (o.bigtiffMode == 3 && rawSize > 2000000000.0);
    }
    p.nbits = o.nbitsFinal;
    p.bigEndian = o.endianBig;
    p.gtVersion = o.gtVersion;
    if (o.resolvedPhot >= 0 || o.photOmit)
    {
        p.photometric = o.resolvedPhot >= 0 ? o.resolvedPhot : 1;
        p.omitPhotometric = o.photOmit;
        p.synthPalette = o.synthPalette;
        if (o.extrasSet)
        {
            p.extrasSet = true;
            p.extraSamples = o.extras;
        }
    }
    p.append = append && exists;

    if (hasNodata)
    {
        double stored = nodata;
        std::string ndText;
        bool ok;
        if (p.type == DType::Int64 || p.type == DType::UInt64)
        {
            // integralness checked on the double, range and stored text
            // from strtoll/strtoull of the raw argument ("1e3" stores 1)
            ok = std::isfinite(nodata) && nodata == floor(nodata);
            if (ok)
            {
                errno = 0;
                if (p.type == DType::Int64)
                {
                    long long ll =
                        strtoll(nodataStr.c_str(), nullptr, 10);
                    ok = errno != ERANGE;
                    ndText = strPrintf("%lld", ll);
                    stored = (double)ll;
                }
                else
                {
                    unsigned long long ull =
                        strtoull(nodataStr.c_str(), nullptr, 10);
                    ok = errno != ERANGE;
                    ndText = strPrintf("%llu", ull);
                    stored = (double)ull;
                }
            }
        }
        else
            ok = nodataRepresentable(p.type, nodata, stored);
        if (!ok)
        {
            // reference creates the file before failing to set nodata;
            // the burn happens after, so pixels stay zero
            p.burn.clear();
            std::string err;
            gtiffWrite(output, p, err);
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "create: Setting nodata value failed as it cannot "
                        "be represented on its data type");
            return 1;
        }
        p.hasNodata = true;
        p.nodata = stored;
        p.nodataText = ndText;
        // Create-flow nodata is set after the georeferencing
        p.nodataLate = true;
    }

    if (hasSrs)
        p.srs = &srs;
    if (hasBbox)
    {
        p.hasGT = true;
        p.gt[0] = bbox[0];
        p.gt[1] = (bbox[2] - bbox[0]) / p.width;
        p.gt[2] = 0;
        p.gt[3] = bbox[3];
        p.gt[4] = 0;
        p.gt[5] = -(bbox[3] - bbox[1]) / p.height;
    }
    for (const auto &m : r.list("metadata"))
    {
        size_t eq = m.find('=');
        p.metadata.push_back({m.substr(0, eq), m.substr(eq + 1)});
    }
    if (o.gmdColorinterp)
    {
        p.useGmdItems = true;
        for (const auto &kv : p.metadata)
            p.gmdItems.push_back({kv.first, kv.second, -1, "", ""});
        static const char *const cmyk[4] = {"Cyan", "Magenta", "Yellow",
                                            "Black"};
        for (int b = 0; b < p.bands; b++)
        {
            const char *ci = o.resolvedPhot == 5 && b < 4 ? cmyk[b]
                                                          : "Undefined";
            p.gmdItems.push_back(
                {"COLORINTERP", ci, b, "colorinterp", ""});
        }
    }

    // nodata snapping above still sees Float32; only the samples are half
    if (o.halfFloat)
        p.type = DType::Float16;

    p.jpegQuality = o.jpegQuality;
    p.jpegTablesMode = o.jpegTablesMode;
    p.webpLevel = o.webpLevel;
    p.webpLossless = o.webpLossless;
    if (p.compression == 7)
    {
        if (p.photometric == 6 && p.bandInterleave)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        fnameOf(output) +
                            ": PHOTOMETRIC=YCBCR requires INTERLEAVE=PIXEL");
            return 1;
        }
        const int bps = o.nbitsFinal > 0
                            ? o.nbitsFinal
                            : dtypeSizeBytes(p.type) * 8;
        const int mult = p.photometric == 6 ? 16 : 8;
        if (p.photometric == 3)
        {
            for (int i = 0; i < 2; i++)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "JPEGSetupEncode:PhotometricInterpretation 3 "
                            "not allowed for JPEG");
            p.jpegStub = true;
            p.burn.clear();
            std::string werr;
            gtiffWrite(output, p, werr);
            return 1;
        }
        if (bps != 8)
        {
            const int tmpBps = bps > 16 ? 16 : bps;
            if (bps > 16)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("gtiffdataset_jpg_tmp: NBITS=%d is invalid "
                              "for data type UInt16. Using NBITS=16",
                              bps));
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JPEGSetupEncode:BitsPerSample %d not "
                                  "allowed for JPEG",
                                  tmpBps));
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JPEGSetupEncode:BitsPerSample %d not "
                                  "allowed for JPEG",
                                  bps));
            p.jpegStub = true;
            p.burn.clear();
            std::string werr;
            gtiffWrite(output, p, werr);
            return 1;
        }
        if (!p.tiled && p.blockY > 0 && p.blockY < p.height &&
            p.blockY % mult)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JPEGSetupEncode:RowsPerStrip must be "
                                  "multiple of %d for JPEG",
                                  mult));
            p.jpegStub = true;
            p.jpegStubTables = true;
            p.burn.clear();
            std::string werr;
            gtiffWrite(output, p, werr);
            return 1;
        }
    }
    if (p.compression == 50001)
    {
        if (p.bandInterleave && p.bands > 1)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "TWebPFixupTags:TIFF WEBP requires data to be "
                        "stored contiguously in RGB e.g. RGBRGBRGB or "
                        "RGBARGBARGBA");
        const int bps = o.nbitsFinal > 0 ? o.nbitsFinal
                                         : dtypeSizeBytes(p.type) * 8;
        std::string setupMsg;
        if (p.bands != 3 && p.bands != 4)
            setupMsg = strPrintf(
                "WebPSetupEncode:WEBP driver doesn't support %d bands. "
                "Must be 3 (RGB) or 4 (RGBA) bands.",
                p.bands);
        else if (bps != 8 || p.type != DType::Byte)
            setupMsg =
                "WebPSetupEncode:WEBP driver requires 8 bit unsigned "
                "data";
        if (!setupMsg.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, setupMsg);
            // the failed directory never got its RGB photometric; the
            // leftover keeps MINISBLACK with unspecified extra samples
            p.jpegStub = true;
            p.photometric = 1;
            if (!p.extrasSet && p.bands > 1)
            {
                p.extrasSet = true;
                p.extraSamples.assign((size_t)p.bands - 1, 0);
            }
            p.burn.clear();
            std::string werr;
            gtiffWrite(output, p, werr);
            return 1;
        }
    }

    std::string err;
    if (!gtiffWrite(output, p, err))
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_AppDefined, "create: " + err);
        return 1;
    }
    if (!dbgClosePtr.empty())
        cplDebug("GDAL",
                 "GDALClose(" + output + ", this=" + dbgClosePtr + ")");
    return 0;
}

}  // namespace

void registerRasterCreateHandler()
{
    registerHandler("raster_create", rasterCreateHandler);
}
