#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "util.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct SelEntry
{
    int srcBand = 0;   // 1-based source band; 0 = mask entry
    int maskOf = 0;    // for mask entries: 0 = dataset mask, else band
};

class BandSelectDataset final : public RasterDatasetBase
{
  public:
    std::unique_ptr<RasterDatasetBase> src;
    std::vector<SelEntry> sel;

    // --mask selection: -1 unset, 0 dataset mask, >0 band values,
    // ~N (negative below -1 via -(N+1)) mask-of-band N
    int maskSel = -1;
    int maskOfBand = 0;

    BandSelectDataset(std::unique_ptr<RasterDatasetBase> s,
                      const std::vector<SelEntry> &entries)
        : src(std::move(s)), sel(entries)
    {
        path = src->path;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = src->width;
        height = src->height;
        hasGT = src->hasGT;
        memcpy(gt, src->gt, sizeof gt);
        srs = std::move(src->srs);
        hasSrs = src->hasSrs;
        srsSynthetic = src->srsSynthetic;
        metadata = src->metadata;
        domainOrder = src->domainOrder;
        sortedDomains = src->sortedDomains;
        xmlDomains = src->xmlDomains;
        files = src->files;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();
        for (size_t i = 0; i < sel.size(); ++i)
        {
            Band b;
            if (sel[i].srcBand > 0)
                b = src->bands[sel[i].srcBand - 1];
            else
            {
                b.type = DType::Byte;
                b.colorInterp = "Alpha";
                b.blockX = src->bands.empty() ? width
                                              : src->bands[0].blockX;
                b.blockY = src->bands.empty() ? height
                                              : src->bands[0].blockY;
            }
            // bands renumber sequentially; the source band survives in
            // the sel table for VRT serialization and stats delegation
            b.index = (int)i + 1;
            bands.push_back(std::move(b));
        }
    }

    std::string vrtWrapperSourceBandText(int band) override
    {
        const SelEntry &e = sel[(size_t)band - 1];
        if (e.srcBand > 0)
            return std::to_string(e.srcBand);
        return "mask," + std::to_string(e.maskOf > 0 ? e.maskOf : 1);
    }

    RasterDatasetBase *statsDelegate(int band, int &delegateBand) override
    {
        const SelEntry &e = sel[(size_t)band - 1];
        if (e.srcBand <= 0)
        {
            delegateBand = band;
            return this;
        }
        return src->statsDelegate(e.srcBand, delegateBand);
    }

    int alphaBand() const
    {
        for (size_t i = 0; i < src->bands.size(); ++i)
            if (src->bands[i].colorInterp == "Alpha")
                return (int)i + 1;
        return 0;
    }

    bool readMask(int ofBand, std::vector<uint8_t> &out)
    {
        const Band &b = src->bands[ofBand - 1];
        size_t n = (size_t)width * (size_t)height;
        if (b.hasNodata)
        {
            std::vector<double> vals;
            if (!src->readBand(ofBand, vals))
                return false;
            out.resize(n);
            for (size_t i = 0; i < n; ++i)
                out[i] = vals[i] == b.nodata ? 0 : 255;
            return true;
        }
        int ab = alphaBand();
        if (ab > 0 && ab != ofBand)
        {
            if (src->bands[ab - 1].type == DType::Byte)
                return src->readBandRaw(ab, out);
            std::vector<double> vals;
            if (!src->readBand(ab, vals))
                return false;
            out.resize(n);
            for (size_t i = 0; i < n; ++i)
                out[i] = vals[i] != 0 ? 255 : 0;
            return true;
        }
        out.assign(n, 255);
        return true;
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        const SelEntry &e = sel[band - 1];
        if (e.srcBand > 0)
            return src->readBand(e.srcBand, out);
        std::vector<uint8_t> m;
        if (!readMask(e.maskOf > 0 ? e.maskOf : 1, m))
            return false;
        out.assign(m.begin(), m.end());
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        const SelEntry &e = sel[band - 1];
        if (e.srcBand > 0)
            return src->readBandRaw(e.srcBand, out);
        return readMask(e.maskOf > 0 ? e.maskOf : 1, out);
    }

    bool readBandRawStrict(int band, std::vector<uint8_t> &out) override
    {
        const SelEntry &e = sel[band - 1];
        if (e.srcBand > 0)
            return src->readBandRawStrict(e.srcBand, out);
        return readMask(e.maskOf > 0 ? e.maskOf : 1, out);
    }

    void realBlockDims(int &bw, int &bh) const override
    {
        src->realBlockDims(bw, bh);
    }

    bool selectMaskBand(std::vector<uint8_t> &out) override
    {
        if (maskSel < 0)
            return false;
        if (maskSel == 0)
        {
            // dataset mask / mask-of-band: nodata and alpha rules
            return readMask(maskOfBand > 0 ? maskOfBand : 1, out);
        }
        // a plain band used as the mask: its values gate validity
        std::vector<double> vals;
        if (!src->readBand(maskSel, vals))
            return false;
        out.resize(vals.size());
        for (size_t i = 0; i < vals.size(); ++i)
            out[i] = vals[i] != 0 ? 255 : 0;
        return true;
    }
};

struct BandToken
{
    std::string raw;
    int num = 0;       // >0 numeric band
    int maskOf = -1;   // >=0 mask entry (0 = dataset mask)
    std::string color; // non-empty: color interpretation name
};

const char *kInterpNames[] = {
    "Undefined", "Gray",    "Palette", "Red",     "Green",
    "Blue",      "Alpha",   "Hue",     "Saturation", "Lightness",
    "Cyan",      "Magenta", "Yellow",  "Black",   "YCbCr_Y",
    "YCbCr_Cb",  "YCbCr_Cr"};

bool parseBandToken(const std::string &tok, BandToken &out)
{
    out.raw = tok;
    if (strEqualNoCase(tok, "mask"))
    {
        out.maskOf = 0;
        return true;
    }
    if (tok.size() > 5 && strEqualNoCase(tok.substr(0, 5), "mask:"))
    {
        std::string rest = tok.substr(5);
        if (rest.empty() ||
            rest.find_first_not_of("0123456789") != std::string::npos)
            return false;
        out.maskOf = atoi(rest.c_str());
        return out.maskOf >= 1;
    }
    if (!tok.empty() &&
        tok.find_first_not_of("0123456789") == std::string::npos)
    {
        out.num = atoi(tok.c_str());
        return out.num >= 1;
    }
    for (const char *n : kInterpNames)
        if (strEqualNoCase(tok, n))
        {
            out.color = tok;
            return true;
        }
    return false;
}

int selBuildAndWrap(std::unique_ptr<RasterDatasetBase> &d,
                    const std::vector<BandToken> &tokens, bool exclude)
{
    int nb = (int)d->bands.size();
    std::vector<SelEntry> entries;
    if (exclude)
    {
        // exclusion validates nothing: out-of-range bands, unmatched
        // colors and mask entries silently drop out of the request; the
        // all-bands refusal keys on the token count alone
        if ((int)tokens.size() >= nb)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() +
                            ": Cannot exclude all input bands");
            return 1;
        }
        std::vector<bool> drop((size_t)nb, false);
        for (const auto &t : tokens)
        {
            if (t.num > 0 && t.num <= nb)
                drop[t.num - 1] = true;
            else if (!t.color.empty())
                for (int bi = 0; bi < nb; ++bi)
                    if (strEqualNoCase(d->bands[bi].colorInterp, t.color))
                    {
                        drop[bi] = true;
                        break;
                    }
        }
        for (int bi = 0; bi < nb; ++bi)
            if (!drop[bi])
            {
                SelEntry e;
                e.srcBand = bi + 1;
                entries.push_back(e);
            }
        if (entries.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() +
                            ": Cannot exclude all input bands");
            return 1;
        }
        d = std::make_unique<BandSelectDataset>(std::move(d), entries);
        return 0;
    }
    for (const auto &t : tokens)
    {
        SelEntry e;
        if (t.num > 0)
        {
            if (t.num > nb)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("Band %d requested, but only "
                                      "bands 1 to %d available.",
                                      t.num, nb));
                return 1;
            }
            e.srcBand = t.num;
        }
        else if (t.maskOf >= 0)
        {
            if (t.maskOf > nb)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("Band %d requested, but only "
                                      "bands 1 to %d available.",
                                      t.maskOf, nb));
                return 1;
            }
            e.srcBand = 0;
            e.maskOf = t.maskOf;
        }
        else
        {
            int found = 0;
            for (int bi = 0; bi < nb; ++bi)
                if (strEqualNoCase(d->bands[bi].colorInterp, t.color))
                {
                    found = bi + 1;
                    break;
                }
            if (!found)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            convertMsgPrefix() +
                                ": No band has color interpretation " +
                                t.color);
                return 1;
            }
            e.srcBand = found;
        }
        entries.push_back(e);
    }
    d = std::make_unique<BandSelectDataset>(std::move(d), entries);
    return 0;
}

int rasterSelectHandler(const CmdSpec &, ParseResult &r)
{
    bool prefixWasEmpty = g_pipelineStepPrefix.empty();
    if (prefixWasEmpty)
        g_pipelineStepPrefix = "select";
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
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");
    bool exclude = r.flag("exclude");
    std::string maskOpt = r.str("mask");
    std::string format = r.str("output-format");

    std::vector<BandToken> tokens;
    for (const auto &v : r.list("band"))
    {
        size_t pos = 0;
        while (pos <= v.size())
        {
            size_t comma = v.find(',', pos);
            std::string tok =
                v.substr(pos, comma == std::string::npos
                                  ? std::string::npos
                                  : comma - pos);
            BandToken bt;
            if (!parseBandToken(tok, bt))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Invalid band specification.");
                handlerPrintUsage();
                return 1;
            }
            tokens.push_back(bt);
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
    }

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

    BandToken maskTok;
    bool haveMask = false;
    if (!maskOpt.empty() && !strEqualNoCase(maskOpt, "none"))
    {
        if (!parseBandToken(maskOpt, maskTok) || !maskTok.color.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid mask band specification.");
            handlerPrintUsage();
            return 1;
        }
        std::string effDrv = drv;
        if (effDrv.empty())
        {
            size_t slash = output.find_last_of('/');
            std::string base = slash == std::string::npos
                                   ? output
                                   : output.substr(slash + 1);
            size_t dot = base.find_last_of('.');
            std::string ext = dot == std::string::npos
                                  ? ""
                                  : strToLower(base.substr(dot + 1));
            std::string lbase = strToLower(base);
            if (lbase.size() > 11 &&
                lbase.compare(lbase.size() - 11, 11, ".gdalg.json") == 0)
                effDrv = "GDALG";
            else if (ext.empty() || ext == "tif" || ext == "tiff")
                effDrv = "GTiff";
        }
        if (effDrv != "GTiff" && effDrv != "GDALG")
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() +
                            ": --mask is not implemented for this "
                            "output in this build");
            return 1;
        }
        haveMask = true;
    }

    std::string err;
    auto ds = openRaster(input, err);
    if (!ds)
    {
        if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + input +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    std::string extra;
    {
        extra = " --band ";
        for (size_t i = 0; i < tokens.size(); ++i)
        {
            if (i)
                extra += ',';
            extra += tokens[i].raw;
        }
        if (exclude)
            extra += " --exclude";
        if (!maskOpt.empty())
            extra += " --mask " + maskOpt;
    }

    auto materialize =
        [&](std::unique_ptr<RasterDatasetBase> &d) -> int {
        int nb = (int)d->bands.size();
        if (selBuildAndWrap(d, tokens, exclude))
            return 1;
        int maskSel = -1, maskOfBand = 0;
        if (haveMask)
        {
            // out-of-range mask bands warn (GetRasterBand style) and
            // the output is simply written without a mask
            int need = maskTok.num > 0 ? maskTok.num : maskTok.maskOf;
            if (need > nb)
                cplErrorStr(
                    CE_Failure, CPLE_IllegalArg,
                    input + strPrintf(": GDALDataset::GetRasterBand("
                                      "%d) - Illegal band #\n",
                                      need));
            else if (maskTok.num > 0)
                maskSel = maskTok.num;
            else
            {
                maskSel = 0;
                maskOfBand = maskTok.maskOf;
            }
        }
        auto *sd = static_cast<BandSelectDataset *>(d.get());
        sd->maskSel = maskSel;
        sd->maskOfBand = maskOfBand;
        return 0;
    };

    return rasterConvertWriteOutput(ds, r, input, output, quiet, overwrite,
                                    append, drv, extra, materialize);
}

int rasterSelectPreValidator(const CmdSpec &, ParseResult &r)
{
    std::string format = r.str("output-format");
    {
        std::string drv;
        std::string issue = rasterOutFormatIssue(format, drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "select: " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    for (const auto &v : r.list("band"))
    {
        size_t pos = 0;
        while (pos <= v.size())
        {
            size_t comma = v.find(',', pos);
            std::string tok =
                v.substr(pos, comma == std::string::npos
                                  ? std::string::npos
                                  : comma - pos);
            BandToken bt;
            if (!parseBandToken(tok, bt))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Invalid band specification.");
                handlerPrintUsage();
                return 1;
            }
            if (comma == std::string::npos)
                break;
            pos = comma + 1;
        }
    }
    // the mask spec is validated before the input dataset is opened
    std::string maskOpt = r.str("mask");
    if (!maskOpt.empty() && !strEqualNoCase(maskOpt, "none"))
    {
        BandToken bt;
        if (!parseBandToken(maskOpt, bt) || !bt.color.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid mask band specification.");
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

}  // namespace

bool rasterSelectBandTokensValid(const std::vector<std::string> &vals)
{
    for (const auto &v : vals)
        for (const auto &tok : strSplit(v, ','))
        {
            BandToken bt;
            if (!parseBandToken(tok, bt))
                return false;
        }
    return true;
}

bool rasterSelectMaskTokenValid(const std::string &v)
{
    if (v.empty() || strEqualNoCase(v, "none"))
        return true;
    BandToken bt;
    return parseBandToken(v, bt) && bt.color.empty();
}

int rasterSelectApplyPipeStep(const PipeStepArgs &args,
                              std::unique_ptr<RasterDatasetBase> &ds)
{
    // the surrounding write/info delegation owns the prefix by now;
    // step errors still name the select step
    struct PrefixRestore
    {
        std::string prev = g_pipelineStepPrefix;
        ~PrefixRestore() { g_pipelineStepPrefix = prev; }
    } restore;
    g_pipelineStepPrefix = "select";
    std::vector<BandToken> tokens;
    auto it = args.find("band");
    if (it != args.end())
        for (const auto &v : it->second)
            for (const auto &tok : strSplit(v, ','))
            {
                BandToken bt;
                if (!parseBandToken(tok, bt))
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Invalid band specification.");
                    return 1;
                }
                tokens.push_back(bt);
            }
    bool exclude = args.count("exclude") != 0;
    std::string maskOpt;
    auto mi = args.find("mask");
    if (mi != args.end() && !mi->second.empty())
        maskOpt = mi->second[0];
    BandToken maskTok;
    bool haveMask = false;
    if (!maskOpt.empty() && !strEqualNoCase(maskOpt, "none"))
    {
        if (!parseBandToken(maskOpt, maskTok) || !maskTok.color.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid mask band specification.");
            return 1;
        }
        haveMask = true;
    }
    std::string srcName = ds->path;
    int nb = (int)ds->bands.size();
    if (selBuildAndWrap(ds, tokens, exclude))
        return 1;
    int maskSel = -1, maskOfBand = 0;
    if (haveMask)
    {
        // out-of-range mask bands warn (GetRasterBand style) and the
        // output is simply written without a mask
        int need = maskTok.num > 0 ? maskTok.num : maskTok.maskOf;
        if (need > nb)
            cplErrorStr(CE_Failure, CPLE_IllegalArg,
                        srcName + strPrintf(": GDALDataset::GetRasterBand("
                                            "%d) - Illegal band #\n",
                                            need));
        else if (maskTok.num > 0)
            maskSel = maskTok.num;
        else
        {
            maskSel = 0;
            maskOfBand = maskTok.maskOf;
        }
    }
    auto *sd = static_cast<BandSelectDataset *>(ds.get());
    sd->maskSel = maskSel;
    sd->maskOfBand = maskOfBand;
    return 0;
}

void registerRasterSelectHandler()
{
    registerHandler("raster_select", rasterSelectHandler);
    registerPreValidator("raster_select", rasterSelectPreValidator);
}
