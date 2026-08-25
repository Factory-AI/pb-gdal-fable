#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "util.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{

// ceil-biased byte multiply used throughout the reference compositor
inline int mulc(int a, int b)
{
    return (a * b + 255) >> 8;
}

// round-half-up of p*255/A
inline int rhuScale(int p, int A)
{
    return (p * 255 * 2 + A) / (2 * A);
}

enum class BlendOp
{
    SrcOver,
    HsvValue,
    Multiply,
    Screen,
    Overlay,
    HardLight,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn
};

BlendOp blendOpFromName(const std::string &n)
{
    if (n == "hsv-value")
        return BlendOp::HsvValue;
    if (n == "multiply")
        return BlendOp::Multiply;
    if (n == "screen")
        return BlendOp::Screen;
    if (n == "overlay")
        return BlendOp::Overlay;
    if (n == "hard-light")
        return BlendOp::HardLight;
    if (n == "darken")
        return BlendOp::Darken;
    if (n == "lighten")
        return BlendOp::Lighten;
    if (n == "color-dodge")
        return BlendOp::ColorDodge;
    if (n == "color-burn")
        return BlendOp::ColorBurn;
    return BlendOp::SrcOver;
}

// premultiplied composite of one channel for the separable operators,
// before the union-alpha unpremultiply
int composePremult(BlendOp op, int c, int s, int ca, int sae)
{
    int cp = mulc(c, ca);
    int sp = mulc(s, sae);
    int d1a = 255 - sae;
    int s1a = 255 - ca;
    int rest = mulc(sp, s1a) + mulc(cp, d1a);
    switch (op)
    {
        case BlendOp::Multiply:
            return mulc(sp, cp) + rest;
        case BlendOp::Screen:
            return sp + cp - mulc(sp, cp);
        case BlendOp::Overlay:
            if (2 * cp < ca)
                return 2 * mulc(sp, cp) + rest;
            return mulc(sae, ca) - 2 * mulc(sae - sp, ca - cp) + rest;
        case BlendOp::HardLight:
            if (2 * sp < sae)
                return 2 * mulc(sp, cp) + rest;
            return mulc(sae, ca) - 2 * mulc(sae - sp, ca - cp) + rest;
        case BlendOp::Darken:
            return ((std::min(sp * ca, cp * sae) + 255) >> 8) + rest;
        case BlendOp::Lighten:
            return ((std::max(sp * ca, cp * sae) + 255) >> 8) + rest;
        case BlendOp::ColorDodge:
        {
            int u = mulc(cp, sae);
            int w = mulc(sp, ca);
            if (u + w >= mulc(sae, ca))
                return mulc(sae, ca) + rest;
            int den = 255 - (sae > 0 ? sp * 255 / sae : 0);
            return (den > 0 ? 255 * u / den : 0) + rest;
        }
        case BlendOp::ColorBurn:
        {
            int N = sp * ca + cp * sae - sae * ca;
            return (N > 0 ? N / 255 : 0) + rest;
        }
        default:
            return rest;
    }
}

// ------------------------------------------------------------------
// hsv-value: float chain replacing V with the composited overlay value
// ------------------------------------------------------------------

void hsvValuePixel(int r, int g, int b, int vnew, int out[3])
{
    int mx = std::max(r, std::max(g, b));
    int mn = std::min(r, std::min(g, b));
    int delta = mx - mn;
    float v = (float)vnew;
    if (delta == 0 || mx == 0)
    {
        int q = (int)std::floor(v + 0.5f);
        out[0] = out[1] = out[2] = q;
        return;
    }
    float d = (float)delta;
    float hh;
    if (mx == r)
        hh = (float)(g - b) / d;
    else if (mx == g)
        hh = 2.0f + (float)(b - r) / d;
    else
        hh = 4.0f + (float)(r - g) / d;
    if (hh < 0.0f)
        hh += 6.0f;
    float s = d / (float)mx;
    int i = ((int)hh) % 6;
    float f = hh - (float)(int)hh;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    float rr, gg, bb;
    switch (i)
    {
        case 0: rr = v; gg = t; bb = p; break;
        case 1: rr = q; gg = v; bb = p; break;
        case 2: rr = p; gg = v; bb = t; break;
        case 3: rr = p; gg = q; bb = v; break;
        case 4: rr = t; gg = p; bb = v; break;
        default: rr = v; gg = p; bb = q; break;
    }
    out[0] = (int)std::floor(rr + 0.5f);
    out[1] = (int)std::floor(gg + 0.5f);
    out[2] = (int)std::floor(bb + 0.5f);
}

// ------------------------------------------------------------------
// blended wrapper dataset
// ------------------------------------------------------------------

class BlendDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> color;
    std::unique_ptr<RasterDatasetBase> over;
    BlendOp op = BlendOp::SrcOver;
    int a8 = 255;
    int nColorC = 1, nOverC = 1;
    bool colorAlpha = false, overAlpha = false;
    int outColors = 1;
    bool outAlpha = false;
    bool computed = false;
    bool computeOk = true;
    std::vector<std::vector<double>> cache;

    bool swapped = false;

    BlendDataset(std::unique_ptr<RasterDatasetBase> c,
                 std::unique_ptr<RasterDatasetBase> o,
                 BlendOp oper, int opacity8)
        : color(std::move(c)), over(std::move(o)), op(oper), a8(opacity8)
    {
        path = color->path;
        driverShort = color->driverShort;
        driverLong = color->driverLong;
        width = color->width;
        height = color->height;
        hasGT = color->hasGT;
        memcpy(gt, color->gt, sizeof gt);
        srs = color->srs;
        hasSrs = color->hasSrs;

        // alpha is positional: 2- and 4-band inputs treat their last band
        // as alpha whatever its color interpretation says
        int ncb = (int)color->bands.size();
        int nob = (int)over->bands.size();
        bool unionStructure =
            op == BlendOp::Multiply || op == BlendOp::Screen ||
            op == BlendOp::Overlay || op == BlendOp::HardLight;
        // the union-structure operators put the wider dataset on the
        // destination side; the opacity stays attached to the original
        // overlay
        swapped = unionStructure && nob > ncb;
        colorAlpha = ncb == 2 || ncb == 4;
        overAlpha = nob == 2 || nob == 4;
        nColorC = ncb - (colorAlpha ? 1 : 0);
        nOverC = nob - (overAlpha ? 1 : 0);
        if (!unionStructure)
        {
            outColors = nColorC;
            outAlpha = colorAlpha;
        }
        else
        {
            // the alpha plane follows the destination (post-swap) side;
            // the other operand's alpha only feeds the compose
            outColors = std::max(nColorC, nOverC);
            outAlpha = swapped ? overAlpha : colorAlpha;
        }
        int nb = outColors + (outAlpha ? 1 : 0);
        static const char *kRgb[] = {"Red", "Green", "Blue"};
        for (int i = 0; i < nb; ++i)
        {
            Band b;
            b.index = i + 1;
            b.type = DType::Byte;
            b.blockX = width;
            b.blockY = height;
            if (outAlpha && i == nb - 1)
                b.colorInterp = "Alpha";
            else if (outColors == 3)
                b.colorInterp = kRgb[i];
            else
                b.colorInterp = "Gray";
            bands.push_back(std::move(b));
        }
    }

    bool computeAll()
    {
        if (computed)
            return computeOk;
        computed = true;
        size_t px = (size_t)width * height;
        int nb = (int)bands.size();
        cache.assign(nb, std::vector<double>(px, 0.0));

        int ncb = (int)color->bands.size();
        int nob = (int)over->bands.size();
        std::vector<std::vector<double>> cv(ncb), ov(nob);
        for (int i = 0; i < ncb; ++i)
            if (!color->readBand(i + 1, cv[i]))
                return computeOk = false;
        for (int i = 0; i < nob; ++i)
            if (!over->readBand(i + 1, ov[i]))
                return computeOk = false;

        const std::vector<double> *caBand =
            colorAlpha ? &cv[ncb - 1] : nullptr;
        const std::vector<double> *saBand =
            overAlpha ? &ov[nob - 1] : nullptr;

        if (op == BlendOp::HsvValue)
        {
            // single-band overlay at full opacity zeroes the output
            // alpha plane
            bool alphaZero = nob == 1 && a8 == 255;
            for (size_t k = 0; k < px; ++k)
            {
                int r = (int)cv[0][k], g = (int)cv[1][k],
                    b = (int)cv[2][k];
                int vo;
                if (nOverC >= 3)
                    vo = std::max((int)ov[0][k],
                                  std::max((int)ov[1][k], (int)ov[2][k]));
                else
                    vo = (int)ov[0][k];
                int ca = caBand ? (int)(*caBand)[k] : 255;
                int saRaw = saBand ? (int)(*saBand)[k] : 255;
                int sae = mulc(saRaw, a8);
                int vold = std::max(r, std::max(g, b));
                int v = (vo * sae + vold * (255 - sae) + 255) >> 8;
                int rgb[3];
                hsvValuePixel(r, g, b, v, rgb);
                cache[0][k] = rgb[0];
                cache[1][k] = rgb[1];
                cache[2][k] = rgb[2];
                if (outAlpha)
                    cache[3][k] = alphaZero ? 0 : ca;
            }
            return computeOk = true;
        }

        if (op == BlendOp::SrcOver)
        {
            // overlay wider than the color input collapses through luma
            bool lumaCollapse = nOverC > nColorC && nOverC >= 3;
            for (size_t k = 0; k < px; ++k)
            {
                int ca = caBand ? (int)(*caBand)[k] : 255;
                int saRaw = saBand ? (int)(*saBand)[k] : 255;
                int sae = mulc(saRaw, a8);
                auto blendPix = [&](int c, int s) -> int {
                    if (!colorAlpha)
                        return (s * sae + c * (255 - sae) + 255) >> 8;
                    int A = sae + mulc(ca, 255 - sae);
                    int p = mulc(s, sae) + mulc(mulc(c, ca), 255 - sae);
                    return A > 0 ? rhuScale(p, A) : 0;
                };
                for (int i = 0; i < outColors; ++i)
                {
                    int c = (int)cv[std::min(i, nColorC - 1)][k];
                    int s;
                    if (lumaCollapse)
                        s = (306 * (int)ov[0][k] + 601 * (int)ov[1][k] +
                             117 * (int)ov[2][k]) >>
                            10;
                    else
                        s = (int)ov[std::min(i, nOverC - 1)][k];
                    cache[i][k] = blendPix(c, s);
                }
                if (outAlpha)
                {
                    // 2-band inputs run their alpha plane through the
                    // per-band compositor against the replicated overlay
                    // band; 4-band inputs get the source-over union
                    if ((int)color->bands.size() == 2)
                    {
                        int s;
                        if (lumaCollapse)
                            s = (306 * (int)ov[0][k] +
                                 601 * (int)ov[1][k] +
                                 117 * (int)ov[2][k]) >>
                                10;
                        else
                            s = (int)ov[std::min(
                                outColors,
                                (int)over->bands.size() - 1)][k];
                        cache[outColors][k] = blendPix(ca, s);
                    }
                    else
                        cache[outColors][k] = sae + mulc(ca, 255 - sae);
                }
            }
            return computeOk = true;
        }

        // separable operators: replicated per-band compose with the
        // clamped slot write (narrow outputs let the trailing bands,
        // including a composited alpha plane, overwrite the last slot)
        int ncc = nColorC, noc = nOverC;
        const std::vector<double> *cB = caBand, *sB = saBand;
        std::vector<std::vector<double>> *cvp = &cv, *ovp = &ov;
        bool opacityOnColor = false;
        if (swapped)
        {
            std::swap(cvp, ovp);
            std::swap(cB, sB);
            std::swap(ncc, noc);
            opacityOnColor = true;
        }
        int steps = std::max(ncc, noc);
        // union-structure ops only carry D on a destination alpha plane;
        // the D-scaled ops let it overwrite the last slot whenever either
        // side brought an alpha band
        bool unionStructure =
            op == BlendOp::Multiply || op == BlendOp::Screen ||
            op == BlendOp::Overlay || op == BlendOp::HardLight;
        bool writeD = unionStructure ? outAlpha : (cB || sB) != 0;
        for (size_t k = 0; k < px; ++k)
        {
            int ca = cB ? (int)(*cB)[k] : 255;
            int saRaw = sB ? (int)(*sB)[k] : 255;
            int sae;
            if (opacityOnColor)
            {
                ca = mulc(ca, a8);
                sae = saRaw;
            }
            else
                sae = mulc(saRaw, a8);
            int D = ca + sae - mulc(ca, sae);
            for (int i = 0; i < steps; ++i)
            {
                int c = (int)(*cvp)[std::min(i, ncc - 1)][k];
                int s = (int)(*ovp)[std::min(i, noc - 1)][k];
                int p = composePremult(op, c, s, ca, sae);
                cache[std::min(i, nb - 1)][k] =
                    D > 0 ? (p * 255 / D) & 255 : 0;
            }
            if (writeD)
                cache[std::min(steps, nb - 1)][k] = D;
        }
        return computeOk = true;
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        if (!computeAll())
            return false;
        out = cache[band - 1];
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        std::vector<double> vals;
        if (!readBand(band, vals))
            return false;
        out.assign((size_t)width * height, 0);
        for (size_t i = 0; i < vals.size(); ++i)
            out[i] = (uint8_t)vals[i];
        return true;
    }

    bool readBandRawStrict(int band, std::vector<uint8_t> &out) override
    {
        return readBandRaw(band, out);
    }
};

// ------------------------------------------------------------------
// validation
// ------------------------------------------------------------------

int blendArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName == "output-format")
    {
        std::string drv;
        std::string issue =
            rasterOutFormatIssue(r.str("output-format"), drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "blend: " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

bool blSameFileAs(const std::string &a, const std::string &b)
{
    struct stat sa, sb;
    if (stat(a.c_str(), &sa) != 0 || stat(b.c_str(), &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

bool blendBandsSupported(const RasterDatasetBase &ds)
{
    size_t nb = ds.bands.size();
    if (nb < 1 || nb > 4)
        return false;
    for (const auto &b : ds.bands)
        if (b.type != DType::Byte)
            return false;
    return true;
}

int blendPreValidator(const CmdSpec &, ParseResult &r)
{
    bool fail = false;
    std::string input = r.str("input");
    std::string overlay = r.str("overlay");
    std::string output = r.str("output");
    bool ow = r.flag("overwrite");
    bool append = r.flag("append");

    bool inputOk = true, overlayOk = true;
    auto checkIn = [&fail](const std::string &path, bool &ok)
    {
        if (path.empty() || path.rfind("GTIFF_DIR:", 0) == 0)
            return;
        struct stat sb;
        if (stat(path.c_str(), &sb) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(path));
            fail = true;
            ok = false;
        }
        else if (!datasetIdentify(path, {"raster"}))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + path +
                            "' not recognized as being in a supported "
                            "file format.");
            fail = true;
            ok = false;
        }
    };
    if (r.get("input"))
        checkIn(input, inputOk);
    if (r.get("overlay"))
        checkIn(overlay, overlayOk);

    std::string of = r.str("output-format");
    bool skipOut = strEqualNoCase(of, "MEM") ||
                   strEqualNoCase(of, "Memory") ||
                   strEqualNoCase(of, "stream");
    const ArgValue *outv = r.get("output");
    struct stat ost;
    if (!skipOut && outv && !output.empty() &&
        stat(output.c_str(), &ost) == 0)
    {
        std::string kind = outputExistsKind(output);
        if (!ow && !append)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "blend: " + kind + " '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--append option.");
            fail = true;
        }
        else if (ow && !append)
        {
            if (kind == "Directory")
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "blend: Directory '" + output +
                                "' already exists, but is not recognized "
                                "as a valid GDAL dataset. Please manually "
                                "delete it before retrying");
                fail = true;
            }
            else if (!blSameFileAs(input, output))
                overwriteDeleteFileset(output);
        }
    }

    // dataset structure checks stack behind the open/exists reports and
    // fail fast among themselves
    if (inputOk && overlayOk && r.get("input") && r.get("overlay"))
    {
        std::string err;
        OpenOptions oo;
        oo.allowedDrivers = r.list("input-format");
        for (const auto &kv : r.list("open-option"))
        {
            size_t eq = kv.find('=');
            std::string key =
                eq == std::string::npos ? kv : kv.substr(0, eq);
            std::string val =
                eq == std::string::npos ? "" : kv.substr(eq + 1);
            oo.raw.emplace_back(key, val);
        }
        auto cds = openRaster(input, err, oo);
        auto ods = cds ? openRaster(overlay, err, oo) : nullptr;
        if (cds && ods)
        {
            std::string opName = r.str("operator", "src-over");
            if (!blendBandsSupported(*cds))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "blend: Only 1-band, 2-band, 3-band or "
                            "4-band Byte dataset supported as input");
                fail = true;
            }
            else if (!blendBandsSupported(*ods))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "blend: Only 1-band, 2-band, 3-band or "
                            "4-band Byte dataset supported as overlay");
                fail = true;
            }
            else if (cds->width != ods->width ||
                     cds->height != ods->height)
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            "blend: Input dataset and overlay dataset "
                            "must have the same dimensions");
                fail = true;
            }
            else if (opName == "hsv-value" &&
                     (cds->bands.size() < 3 || cds->bands.size() > 4))
            {
                cplErrorStr(CE_Failure, CPLE_IllegalArg,
                            strPrintf("blend: Input dataset has %d "
                                      "band(s), but operator hsv-value "
                                      "requires between 3 and 4 bands",
                                      (int)cds->bands.size()));
                fail = true;
            }
            else if (opName == "darken" || opName == "lighten")
            {
                int ncb = (int)cds->bands.size();
                int nob = (int)ods->bands.size();
                int ncc = ncb - (ncb == 2 || ncb == 4 ? 1 : 0);
                int noc = nob - (nob == 2 || nob == 4 ? 1 : 0);
                if (ncc != noc)
                {
                    cplErrorStr(
                        CE_Failure, CPLE_IllegalArg,
                        strPrintf("blend: For LIGHTEN and DARKEN "
                                  "operators, the source dataset and "
                                  "overlay dataset must have the same "
                                  "number of bands (without considering "
                                  "alpha). They have %d and %d bands "
                                  "respectively",
                                  ncc, noc));
                    fail = true;
                }
            }
        }
    }

    if (ow && append)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "blend: Argument 'append' is mutually exclusive "
                    "with 'overwrite'.");
        fail = true;
    }

    if (fail)
    {
        handlerPrintUsage();
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------
// handler
// ------------------------------------------------------------------

int rasterBlendHandler(const CmdSpec &, ParseResult &r)
{
    bool prefixWasEmpty = g_pipelineStepPrefix.empty();
    if (prefixWasEmpty)
        g_pipelineStepPrefix = "blend";
    struct PrefixReset
    {
        bool active;
        ~PrefixReset()
        {
            if (active)
                g_pipelineStepPrefix.clear();
        }
    } reset{prefixWasEmpty};

    std::string input = r.str("input");
    std::string overlay = r.str("overlay");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");
    std::string format = r.str("output-format");
    std::string opName = r.str("operator", "src-over");
    BlendOp op = blendOpFromName(opName);
    long long opacity = 100;
    if (r.get("opacity"))
        opacity = atoll(r.str("opacity").c_str());
    int a8 = (int)((opacity * 255 * 2 + 100) / 200);

    std::string drv;
    {
        std::string issue = rasterOutFormatIssue(format, drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + issue);
            handlerPrintUsage();
            return 1;
        }
    }

    OpenOptions oo;
    oo.allowedDrivers = r.list("input-format");
    for (const auto &kv : r.list("open-option"))
    {
        size_t eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
        oo.raw.emplace_back(key, val);
    }

    std::string err;
    auto cds = openRaster(input, err, oo);
    if (!cds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    auto odsUnique = openRaster(overlay, err, oo);
    if (!odsUnique)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + overlay +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    auto ods = std::make_shared<std::unique_ptr<RasterDatasetBase>>(
        std::move(odsUnique));

    std::string extra;
    if (r.get("operator") && opName != "src-over")
        extra += " --operator " + opName;
    if (r.get("opacity") && opacity != 100)
        extra += strPrintf(" --opacity %lld", opacity);

    std::string inputEcho = input + " --overlay " + overlay;

    auto materialize =
        [ods, op, a8](std::unique_ptr<RasterDatasetBase> &d) -> int {
        d = std::make_unique<BlendDataset>(std::move(d),
                                           std::move(*ods), op, a8);
        return 0;
    };

    return rasterConvertWriteOutput(cds, r, inputEcho, output, quiet,
                                    overwrite, append, drv, extra,
                                    materialize);
}

struct Reg
{
    Reg()
    {
        registerHandler("raster_blend", rasterBlendHandler);
        registerPreValidator("raster_blend", blendPreValidator);
        registerArgCheck("raster_blend", blendArgCheck);
    }
};

}  // namespace

void registerRasterBlendHandler()
{
    static Reg reg;
}
