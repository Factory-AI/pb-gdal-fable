#include "vrt.h"

#include "cpl.h"
#include "progress.h"
#include "util.h"
#include "vsi.h"
#include "warp.h"
#include "xml_min.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
const char *canonicalInterpVrt(const std::string &v)
{
    static const char *kInterpNames[] = {
        "Undefined", "Gray",     "Palette",  "Red",       "Green",
        "Blue",      "Alpha",    "Hue",      "Saturation", "Lightness",
        "Cyan",      "Magenta",  "Yellow",   "Black",     "YCbCr_Y",
        "YCbCr_Cb",  "YCbCr_Cr", "Pan",      "Coastal",   "RedEdge",
        "NIR",       "SWIR",     "MWIR",     "LWIR",      "TIR",
        "OtherIR",   "SAR_Ka",   "SAR_K",    "SAR_Ku",    "SAR_X",
        "SAR_C",     "SAR_S",    "SAR_L",    "SAR_P"};
    for (const char *n : kInterpNames)
        if (strEqualNoCase(v, n))
            return n;
    return nullptr;
}
}  // namespace

bool vrtDetect(const std::string &content)
{
    size_t n = content.size() < 1024 ? content.size() : 1024;
    std::string head = content.substr(0, n);
    return head.find("<VRTDataset") != std::string::npos;
}

namespace
{

std::string vrtDirName(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos)
        return "";
    return p.substr(0, slash);
}

// CPL minixml-flavoured structural scan: 0-based line numbers, the two
// messages the oracle emits for hand-written breakage
bool xmlScan(const std::string &s, std::string &errMsg)
{
    std::vector<std::string> stack;
    size_t i = 0;
    int line = 0;
    while (i < s.size())
    {
        char c = s[i];
        if (c == '\n')
        {
            line++;
            i++;
            continue;
        }
        if (c != '<')
        {
            i++;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0)
        {
            size_t e = s.find("-->", i);
            if (e == std::string::npos)
                break;
            for (size_t k = i; k < e; k++)
                if (s[k] == '\n')
                    line++;
            i = e + 3;
            continue;
        }
        if (i + 1 < s.size() && (s[i + 1] == '?' || s[i + 1] == '!'))
        {
            size_t e = s.find('>', i);
            if (e == std::string::npos)
                break;
            i = e + 1;
            continue;
        }
        bool closing = i + 1 < s.size() && s[i + 1] == '/';
        size_t nameStart = i + (closing ? 2 : 1);
        size_t k = nameStart;
        while (k < s.size() && !isspace((unsigned char)s[k]) &&
               s[k] != '>' && s[k] != '/')
            k++;
        std::string name = s.substr(nameStart, k - nameStart);
        size_t e = k;
        bool inQuote = false;
        char q = 0;
        while (e < s.size())
        {
            char ce = s[e];
            if (inQuote)
            {
                if (ce == q)
                    inQuote = false;
            }
            else if (ce == '"' || ce == '\'')
            {
                inQuote = true;
                q = ce;
            }
            else if (ce == '>')
                break;
            if (ce == '\n')
                line++;
            e++;
        }
        if (e >= s.size())
        {
            if (!closing && !name.empty())
                stack.push_back(name);
            break;
        }
        bool selfClose = e > i && s[e - 1] == '/';
        if (closing)
        {
            if (stack.empty() || stack.back() != name)
            {
                errMsg = strPrintf(
                    "Line %d: </%s> doesn't have matching <%s>.", line,
                    name.c_str(), name.c_str());
                return false;
            }
            stack.pop_back();
        }
        else if (!selfClose && !name.empty())
            stack.push_back(name);
        i = e + 1;
    }
    if (!stack.empty())
    {
        errMsg = "Parse error at EOF, not all elements have been closed, "
                 "starting with " +
                 stack.back();
        return false;
    }
    return true;
}

void parseMetadataNode(const XmlNode &n,
                       std::map<std::string, MetaDomain> &metadata,
                       std::vector<std::string> &domainOrder,
                       std::vector<std::string> &sortedDomains)
{
    std::string domain = n.attr("domain");
    bool seen = false;
    for (const auto &d : domainOrder)
        if (d == domain)
            seen = true;
    if (!seen)
        domainOrder.push_back(domain);
    for (const auto &c : n.children)
        if (c.name == "MDI" && !c.text.empty())
            metadata[domain].push_back({c.attr("key"), c.text});
    // metadata read from VRT XML lands in a sorted string list
    std::stable_sort(metadata[domain].begin(), metadata[domain].end(),
                     [](const std::pair<std::string, std::string> &a,
                        const std::pair<std::string, std::string> &b)
                     { return a.first < b.first; });
    bool marked = false;
    for (const auto &d : sortedDomains)
        if (d == domain)
            marked = true;
    if (!marked)
        sortedDomains.push_back(domain);
}

bool parseRect(const XmlNode *n, double &x, double &y, double &w, double &h)
{
    if (!n)
        return false;
    x = atof(n->attr("xOff", "0").c_str());
    y = atof(n->attr("yOff", "0").c_str());
    w = atof(n->attr("xSize", "0").c_str());
    h = atof(n->attr("ySize", "0").c_str());
    return true;
}

double packClampInt(double v, double lo, double hi)
{
    if (std::isnan(v))
        return 0;
    v = v >= 0 ? std::floor(v + 0.5) : std::ceil(v - 0.5);
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

}  // namespace

std::unique_ptr<RasterDatasetBase> openVrtContent(const std::string &path,
                                                  const std::string &content,
                                                  std::string &err,
                                                  const OpenOptions &oo)
{
    err = "reported";
    std::string scanErr;
    if (!xmlScan(content, scanErr))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, scanErr);
        return nullptr;
    }
    XmlNode root;
    if (!xmlParse(content, root) || root.name != "VRTDataset")
    {
        err = "unrecognized";
        return nullptr;
    }
    std::string subClass = root.attr("subClass");
    if (subClass == "VRTWarpedDataset")
        return openWarpedVrt(root, path, err);
    std::string xs = root.attr("rasterXSize"), ys = root.attr("rasterYSize");
    int nBands = 0;
    for (const auto &c : root.children)
        if (c.name == "VRTRasterBand")
            nBands++;
    if (xs.empty() || ys.empty() || nBands == 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Missing one of rasterXSize, rasterYSize or bands on"
                    " VRTDataset.");
        return nullptr;
    }
    int w = atoi(xs.c_str()), h = atoi(ys.c_str());
    if (w <= 0 || h <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("Invalid dataset dimensions : %d x %d", w, h));
        return nullptr;
    }

    auto ds = std::make_unique<VrtDataset>();
    ds->path = path;
    ds->driverShort = "VRT";
    ds->driverLong = "Virtual Raster";
    ds->width = w;
    ds->height = h;
    if (!path.empty())
        ds->files.push_back(path);
    std::string dir = vrtDirName(path);

    int bandIdx = 0;
    for (const auto &c : root.children)
    {
        if (c.name == "SRS")
        {
            bool ok = false;
            Srs s = Srs::fromUserInput(c.text, ok);
            if (ok)
            {
                ds->srs = std::move(s);
                ds->hasSrs = true;
            }
            std::string mapping = c.attr("dataAxisToSRSAxisMapping");
            if (!mapping.empty())
            {
                ds->axisMapping.clear();
                const char *p = mapping.c_str();
                while (*p)
                {
                    ds->axisMapping.push_back(atoi(p));
                    const char *comma = strchr(p, ',');
                    if (!comma)
                        break;
                    p = comma + 1;
                }
            }
        }
        else if (c.name == "GeoTransform")
        {
            const char *p = c.text.c_str();
            for (int i = 0; i < 6; i++)
            {
                ds->gt[i] = atof(p);
                const char *comma = strchr(p, ',');
                if (!comma)
                    break;
                p = comma + 1;
            }
            ds->hasGT = true;
        }
        else if (c.name == "GCPList")
        {
            std::string proj = c.attr("Projection");
            if (!proj.empty())
            {
                bool ok = false;
                Srs s = Srs::fromUserInput(proj, ok);
                if (ok)
                {
                    ds->gcpSrs = std::move(s);
                    ds->hasGcpSrs = true;
                }
            }
            std::string mapping = c.attr("dataAxisToSRSAxisMapping");
            if (!mapping.empty())
            {
                ds->gcpMapping.clear();
                const char *p = mapping.c_str();
                while (*p)
                {
                    ds->gcpMapping.push_back(atoi(p));
                    const char *comma = strchr(p, ',');
                    if (!comma)
                        break;
                    p = comma + 1;
                }
            }
            for (const auto &g : c.children)
            {
                if (g.name != "GCP")
                    continue;
                GcpEntry e;
                e.id = g.attr("Id");
                e.info = g.attr("Info");
                if (e.info.empty())
                    if (const XmlNode *in = g.child("Info"))
                        e.info = in->text;
                e.pixel = atof(g.attr("Pixel", "0.0").c_str());
                e.line = atof(g.attr("Line", "0.0").c_str());
                e.x = atof(g.attr("X", "0.0").c_str());
                e.y = atof(g.attr("Y", "0.0").c_str());
                std::string z = g.attr("Z");
                if (z.empty())
                    z = g.attr("GCPZ", "0.0");
                e.z = atof(z.c_str());
                ds->gcps.push_back(std::move(e));
            }
        }
        else if (c.name == "Metadata")
            parseMetadataNode(c, ds->metadata, ds->domainOrder,
                              ds->sortedDomains);
        else if (c.name == "VRTRasterBand")
        {
            bandIdx++;
            std::string bsub = c.attr("subClass");
            if (bsub == "VRTDerivedRasterBand")
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "PixelFunctionType missing");
                return nullptr;
            }
            Band b;
            b.index = bandIdx;
            std::string dt = c.attr("dataType");
            if (!dt.empty())
            {
                b.type = dtypeFromName(dt);
                if (b.type == DType::Unknown)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Invalid dataType = " + dt);
                    return nullptr;
                }
            }
            std::string bandAttr = c.attr("band");
            if (!bandAttr.empty() && atoi(bandAttr.c_str()) != bandIdx)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("Invalid band number. Got %d, expected %d. "
                              "Ignoring provided one, and using %d instead",
                              atoi(bandAttr.c_str()), bandIdx, bandIdx));
            b.blockX = std::min(ds->width, 128);
            b.blockY = std::min(ds->height, 128);
            std::string bx = c.attr("blockXSize"), by = c.attr("blockYSize");
            if (!bx.empty() && atoi(bx.c_str()) > 0)
                b.blockX = atoi(bx.c_str());
            if (!by.empty() && atoi(by.c_str()) > 0)
                b.blockY = atoi(by.c_str());

            std::vector<VrtSource> sources;
            for (const auto &e : c.children)
            {
                if (e.name == "Description")
                    b.description = e.text;
                else if (e.name == "NoDataValue")
                {
                    b.hasNodata = true;
                    b.nodata = atof(e.text.c_str());
                    if (b.type == DType::Int64)
                    {
                        b.nodataIsI64 = true;
                        b.nodataI64 =
                            strtoll(e.text.c_str(), nullptr, 10);
                        b.nodata = (double)b.nodataI64;
                    }
                    else if (b.type == DType::UInt64)
                    {
                        b.nodataIsU64 = true;
                        b.nodataU64 =
                            strtoull(e.text.c_str(), nullptr, 10);
                        b.nodata = (double)b.nodataU64;
                    }
                }
                else if (e.name == "ColorInterp")
                {
                    const char *m = canonicalInterpVrt(e.text);
                    b.colorInterp = m ? m : "Undefined";
                }
                else if (e.name == "Offset")
                {
                    b.hasOffset = true;
                    b.offset = atof(e.text.c_str());
                }
                else if (e.name == "Scale")
                {
                    b.hasScale = true;
                    b.scale = atof(e.text.c_str());
                }
                else if (e.name == "UnitType")
                    b.unitType = e.text;
                else if (e.name == "Metadata")
                    parseMetadataNode(e, b.metadata, b.domainOrder,
                                      b.sortedDomains);
                else if (e.name == "ColorTable")
                {
                    for (const auto &en : e.children)
                        if (en.name == "Entry")
                        {
                            ColorEntry ce;
                            ce.c1 = (short)atoi(en.attr("c1", "0").c_str());
                            ce.c2 = (short)atoi(en.attr("c2", "0").c_str());
                            ce.c3 = (short)atoi(en.attr("c3", "0").c_str());
                            ce.c4 = (short)atoi(en.attr("c4", "255").c_str());
                            b.colorTable.push_back(ce);
                        }
                }
                else if (e.name == "Histograms")
                {
                    for (const auto &hi : e.children)
                    {
                        if (hi.name != "HistItem")
                            continue;
                        HistItem h;
                        for (const auto &f : hi.children)
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
                    }
                }
                else if (e.name == "SimpleSource" ||
                         e.name == "ComplexSource")
                {
                    VrtSource s;
                    s.complex = e.name == "ComplexSource";
                    const XmlNode *fn = e.child("SourceFilename");
                    const XmlNode *nested = e.child("VRTDataset");
                    if ((!fn || fn->text.empty()) && nested)
                    {
                        s.hasNested = true;
                        s.nestedNode = *nested;
                        std::string nx;
                        xmlSerialize(s.nestedNode, nx, 0);
                        std::string nerr;
                        auto nds = openVrtContent(path, nx, nerr, oo);
                        if (!nds)
                        {
                            err = "unrecognized";
                            return nullptr;
                        }
                        s.nestedDs = std::move(nds);
                    }
                    else if (!fn || fn->text.empty())
                    {
                        cplErrorStr(CE_Warning, CPLE_AppDefined,
                                    "Missing <SourceFilename> or "
                                    "<VRTDataset> element in <" + e.name +
                                        ">.");
                        err = "unrecognized";
                        return nullptr;
                    }
                    else
                    {
                        s.rawName = fn->text;
                        s.relativeToVRT =
                            atoi(fn->attr("relativeToVRT", "0").c_str());
                        if (s.relativeToVRT == 1 && !dir.empty() &&
                            !s.rawName.empty() && s.rawName[0] != '/')
                            s.resolved = dir + "/" + s.rawName;
                        else
                            s.resolved = s.rawName;
                    }
                    const XmlNode *sb = e.child("SourceBand");
                    if (sb)
                    {
                        const char *bt = sb->text.c_str();
                        if (strncmp(bt, "mask", 4) == 0)
                        {
                            s.maskBand = true;
                            s.sourceBand =
                                bt[4] == ',' ? atoi(bt + 5) : 1;
                        }
                        else
                            s.sourceBand = atoi(bt);
                    }
                    s.hasSrcRect =
                        parseRect(e.child("SrcRect"), s.sx, s.sy, s.sw, s.sh);
                    s.hasDstRect =
                        parseRect(e.child("DstRect"), s.dx, s.dy, s.dw, s.dh);
                    if (const XmlNode *sr = e.child("ScaleRatio"))
                    {
                        s.hasScaleRatio = true;
                        s.scaleRatio = atof(sr->text.c_str());
                    }
                    if (const XmlNode *so = e.child("ScaleOffset"))
                    {
                        s.hasScaleOffset = true;
                        s.scaleOffset = atof(so->text.c_str());
                    }
                    if (const XmlNode *ex = e.child("Exponent"))
                    {
                        s.hasExponent = true;
                        s.exponent = atof(ex->text.c_str());
                        if (const XmlNode *c = e.child("SrcMin"))
                            s.expSrcMin = atof(c->text.c_str());
                        if (const XmlNode *c = e.child("SrcMax"))
                            s.expSrcMax = atof(c->text.c_str());
                        if (const XmlNode *c = e.child("DstMin"))
                            s.expDstMin = atof(c->text.c_str());
                        if (const XmlNode *c = e.child("DstMax"))
                            s.expDstMax = atof(c->text.c_str());
                        if (const XmlNode *c = e.child("Clip"))
                            s.expClip = c->text == "true";
                    }
                    if (const XmlNode *l = e.child("LUT"))
                    {
                        s.hasLut = true;
                        const char *p = l->text.c_str();
                        while (*p)
                        {
                            char *endp = nullptr;
                            double in = strtod(p, &endp);
                            double outv = 0;
                            if (endp && *endp == ':')
                                outv = strtod(endp + 1, &endp);
                            s.lut.push_back({in, outv});
                            const char *comma =
                                endp ? strchr(endp, ',') : nullptr;
                            if (!comma)
                                break;
                            p = comma + 1;
                        }
                    }
                    if (const XmlNode *ctc = e.child("ColorTableComponent"))
                        s.ctComponent = atoi(ctc->text.c_str());
                    std::vector<std::string> newFiles;
                    if (s.hasNested)
                        newFiles = s.nestedDs->files;
                    else
                        newFiles.push_back(s.resolved);
                    for (const auto &nf : newFiles)
                    {
                        bool seen = false;
                        for (const auto &f : ds->files)
                            if (f == nf)
                                seen = true;
                        if (!seen)
                            ds->files.push_back(nf);
                    }
                    sources.push_back(std::move(s));
                }
            }
            ds->bands.push_back(std::move(b));
            ds->bandSources.push_back(std::move(sources));
        }
    }
    (void)oo;
    err.clear();
    ds->debugPtr = cplDebugPtr();
    cplDebug("GDAL", "GDALOpen(" + ds->path + ", this=" + ds->debugPtr +
                         ") succeeds as VRT.");
    return ds;
}

RasterDatasetBase *VrtDataset::sourceAttempt(VrtSource &s)
{
    if (s.hasNested)
    {
        s.state = 1;
        if (!s.ds)
            s.ds = s.nestedDs;
        return s.nestedDs.get();
    }
    if (s.state == 0)
    {
        if (!vsiExists(s.resolved))
        {
            s.state = 2;
            s.failClass = CPLE_OpenFailed;
            s.failMsg = s.resolved + ": No such file or directory";
        }
        else
        {
            std::string oerr;
            std::shared_ptr<RasterDatasetBase> d;
            auto cit = srcCache.find(s.resolved);
            if (cit != srcCache.end())
                d = cit->second;
            else
            {
                cplPushQuietHandler();
                d = openRaster(s.resolved, oerr);
                cplPopHandler();
                if (d)
                {
                    // lazy geokey SRS decode diagnostics of the source
                    // surface at first source use, unlike its other
                    // open-time noise (even through quiet-wrapped reads)
                    int qd = cplSuspendQuiet();
                    d->replaySrsDecodeWarnings();
                    cplRestoreQuiet(qd);
                    srcCache[s.resolved] = d;
                }
            }
            if (!d)
            {
                s.state = 4;
                s.failClass = CPLE_OpenFailed;
                s.failMsg = "`" + s.resolved +
                            "' not recognized as being in a supported "
                            "file format.";
            }
            else if (s.sourceBand < 1 ||
                     s.sourceBand > (int)d->bands.size())
            {
                s.state = 3;
                s.ds = std::move(d);
                s.failClass = CPLE_IllegalArg;
                s.failMsg = s.resolved +
                            strPrintf(": GDALDataset::GetRasterBand(%d) - "
                                      "Illegal band #\n",
                                      s.sourceBand);
            }
            else
            {
                s.state = 1;
                s.ds = std::move(d);
            }
        }
    }
    if (s.state != 1)
    {
        cplErrorStr(CE_Failure, s.failClass, s.failMsg);
        return nullptr;
    }
    return s.ds.get();
}

VrtSource *VrtDataset::firstFailing(int band)
{
    for (auto &s : bandSources[(size_t)band - 1])
    {
        cplPushQuietHandler();
        RasterDatasetBase *d = sourceAttempt(s);
        cplPopHandler();
        if (!d)
            return &s;
    }
    return nullptr;
}

void VrtDataset::infoBandTouch(int band)
{
    auto &srcs = bandSources[(size_t)band - 1];
    if (srcs.empty())
        return;
    const Band &b = bands[(size_t)band - 1];
    if (b.getMd("", "STATISTICS_MINIMUM") &&
        b.getMd("", "STATISTICS_MAXIMUM"))
        return;
    VrtSource *bad = firstFailing(band);
    if (!bad)
        return;
    size_t idx = 0;
    while (idx < srcs.size() && &srcs[idx] != bad)
        ++idx;
    // GetMinimum/GetMaximum walk the sources; how many loud open
    // attempts surface depends on where the walk stops
    int louds;
    if (srcs.size() == 1)
        louds = 4;
    else if (idx == 0)
        louds = 2;
    else
    {
        bool prevStats = true;
        for (size_t i = 0; i < idx && prevStats; i++)
        {
            RasterDatasetBase *d = srcs[i].ds.get();
            if (!d)
            {
                prevStats = false;
                break;
            }
            const Band &sb = d->bands[(size_t)srcs[i].sourceBand - 1];
            if (!(sb.getMd("", "STATISTICS_MINIMUM") &&
                  sb.getMd("", "STATISTICS_MAXIMUM")))
                prevStats = false;
        }
        louds = prevStats ? 1 : 0;
    }
    for (int i = 0; i < louds; i++)
        sourceAttempt(*bad);
}

bool VrtDataset::bandMinMaxHint(int band, double &mn, double &mx)
{
    if (bandSources[(size_t)band - 1].empty())
    {
        mn = 0;
        mx = 0;
        return true;
    }
    int db = band;
    RasterDatasetBase *d = statsDelegate(band, db);
    if (d != this && statsAdopt(band))
    {
        const Band &sb = d->bands[(size_t)db - 1];
        const std::string *pmn = sb.getMd("", "STATISTICS_MINIMUM");
        const std::string *pmx = sb.getMd("", "STATISTICS_MAXIMUM");
        if (!pmn || !pmx)
            return false;
        mn = atof(pmn->c_str());
        mx = atof(pmx->c_str());
        return true;
    }
    auto &srcs = bandSources[(size_t)band - 1];
    if (srcs.size() < 2)
        return false;
    bool got = false;
    for (auto &s : srcs)
    {
        cplPushQuietHandler();
        RasterDatasetBase *sd = sourceAttempt(s);
        cplPopHandler();
        if (!sd)
            return false;
        const Band &sb = sd->bands[(size_t)s.sourceBand - 1];
        const std::string *pmn = sb.getMd("", "STATISTICS_MINIMUM");
        const std::string *pmx = sb.getMd("", "STATISTICS_MAXIMUM");
        if (!pmn || !pmx)
            return false;
        double smn = atof(pmn->c_str());
        double smx = atof(pmx->c_str());
        if (!got)
        {
            mn = smn;
            mx = smx;
            got = true;
        }
        else
        {
            if (smn < mn)
                mn = smn;
            if (smx > mx)
                mx = smx;
        }
    }
    return got;
}

RasterDatasetBase *VrtDataset::statsDelegate(int band, int &delegateBand)
{
    delegateBand = band;
    auto &srcs = bandSources[(size_t)band - 1];
    if (srcs.size() != 1 || srcs[0].transforming())
        return this;
    VrtSource &s = srcs[0];
    cplPushQuietHandler();
    RasterDatasetBase *d = sourceAttempt(s);
    cplPopHandler();
    if (!d)
        return this;
    if (d->width != width || d->height != height)
        return this;
    if (s.hasSrcRect &&
        (s.sx != 0 || s.sy != 0 || s.sw != d->width || s.sh != d->height))
        return this;
    if (s.hasDstRect &&
        (s.dx != 0 || s.dy != 0 || s.dw != width || s.dh != height))
        return this;
    delegateBand = s.sourceBand;
    return d;
}

VrtSource *VrtDataset::histDelegate(int band)
{
    auto &srcs = bandSources[(size_t)band - 1];
    if (srcs.size() != 1)
        return nullptr;
    VrtSource &s = srcs[0];
    if (s.transforming())
        return nullptr;
    cplPushQuietHandler();
    RasterDatasetBase *d = sourceAttempt(s);
    cplPopHandler();
    if (!d)
        return nullptr;
    if (s.hasSrcRect &&
        (s.sx != 0 || s.sy != 0 || s.sw != d->width || s.sh != d->height))
        return nullptr;
    return &s;
}

bool VrtDataset::mosaicStatsParts(int band, std::vector<VrtSource *> &parts)
{
    parts.clear();
    auto &srcs = bandSources[(size_t)band - 1];
    if (srcs.empty())
        return false;
    struct Rect
    {
        double x, y, w, h;
    };
    std::vector<Rect> placed;
    for (auto &s : srcs)
    {
        if (s.transforming())
            return false;
        cplPushQuietHandler();
        RasterDatasetBase *d = sourceAttempt(s);
        cplPopHandler();
        if (!d)
            return false;
        const Band &sb = d->bands[(size_t)s.sourceBand - 1];
        if (sb.hasNodata)
            return false;
        if (s.hasSrcRect && (s.sx != 0 || s.sy != 0 || s.sw != d->width ||
                             s.sh != d->height))
            return false;
        Rect r;
        if (s.hasDstRect)
            r = {s.dx, s.dy, s.dw, s.dh};
        else
            r = {0, 0, (double)width, (double)height};
        if (r.w != d->width || r.h != d->height)
            return false;
        if (r.x != floor(r.x) || r.y != floor(r.y) || r.w != floor(r.w) ||
            r.h != floor(r.h))
            return false;
        if (r.x < 0 || r.y < 0 || r.x + r.w > width || r.y + r.h > height)
            return false;
        for (const auto &p : placed)
            if (r.x < p.x + p.w && p.x < r.x + r.w && r.y < p.y + p.h &&
                p.y < r.y + r.h)
                return false;
        placed.push_back(r);
        parts.push_back(&s);
    }
    return true;
}

bool VrtDataset::statsAdopt(int band)
{
    int db = band;
    RasterDatasetBase *d = statsDelegate(band, db);
    if (d == this)
        return true;
    const Band &vb = bands[(size_t)band - 1];
    const Band &sb = d->bands[(size_t)db - 1];
    return vb.hasNodata == sb.hasNodata &&
           (!vb.hasNodata || vb.nodata == sb.nodata ||
            (std::isnan(vb.nodata) && std::isnan(sb.nodata)));
}

void VrtDataset::flushSourcePams()
{
    for (auto &kv : srcCache)
    {
        if (kv.second && kv.second->pamDirty)
        {
            writePam(*kv.second);
            kv.second->pamDirty = false;
        }
    }
}

void VrtDataset::persistPam()
{
    // a VRT's PAM is the .vrt file itself
    std::string x = vrtSerializeXml(*this, path, path);
    writeStringToFile(path, x);
}

int VrtDataset::checksumHook(int band)
{
    VrtSource *bad = firstFailing(band);
    if (!bad)
        return -2;
    if (bandSources[(size_t)band - 1].size() > 1)
    {
        sourceAttempt(*bad);
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    "Checksum value could not be computed due to I/O read "
                    "error.");
        return -1;
    }
    cplErrorStr(CE_Failure, CPLE_FileIO,
                "Checksum value could not be computed due to I/O read "
                "error.");
    sourceAttempt(*bad);
    sourceAttempt(*bad);
    return -1;
}

namespace
{

bool sourceMaskPlane(RasterDatasetBase *src, int band,
                     std::vector<double> &out)
{
    size_t n = (size_t)src->width * src->height;
    auto matches = [](double v, double nd)
    { return v == nd || (std::isnan(v) && std::isnan(nd)); };
    const std::string *nvs = src->getMd("", "NODATA_VALUES");
    if (nvs)
    {
        std::vector<double> nd;
        const char *p = nvs->c_str();
        char *endp = nullptr;
        while (*p)
        {
            nd.push_back(strtod(p, &endp));
            if (!endp || endp == p)
                break;
            p = endp;
            while (*p == ' ')
                ++p;
        }
        int nb = (int)src->bands.size();
        if ((int)nd.size() > nb)
            nd.resize((size_t)nb);
        std::vector<std::vector<double>> sv(nd.size());
        for (size_t i = 0; i < nd.size(); ++i)
            if (!src->readBand((int)i + 1, sv[i]))
                return false;
        out.assign(n, 255.0);
        for (size_t px = 0; px < n; ++px)
        {
            bool all = !nd.empty();
            for (size_t i = 0; i < nd.size() && all; ++i)
                if (!matches(sv[i][px], nd[i]))
                    all = false;
            if (all)
                out[px] = 0.0;
        }
        return true;
    }
    const Band &sb = src->bands[(size_t)band - 1];
    if (!sb.hasNodata)
    {
        out.assign(n, 255.0);
        return true;
    }
    std::vector<double> sv;
    if (!src->readBand(band, sv))
        return false;
    out.assign(n, 255.0);
    for (size_t px = 0; px < n; ++px)
        if (matches(sv[px], sb.nodata))
            out[px] = 0.0;
    return true;
}

double lutLookup(const std::vector<std::pair<double, double>> &lut,
                 double v)
{
    if (lut.empty())
        return v;
    if (v <= lut.front().first)
        return lut.front().second;
    if (v >= lut.back().first)
        return lut.back().second;
    size_t i = 1;
    while (i < lut.size() && lut[i].first < v)
        ++i;
    if (i >= lut.size())
        return lut.back().second;
    const auto &a = lut[i - 1];
    const auto &b = lut[i];
    if (b.first == a.first)
        return b.second;
    double t = (v - a.first) / (b.first - a.first);
    return a.second + t * (b.second - a.second);
}

bool sourceImagPlane(RasterDatasetBase *src, int band,
                     std::vector<double> &im)
{
    const Band &sb = src->bands[(size_t)band - 1];
    size_t n = (size_t)src->width * src->height;
    im.assign(n, 0.0);
    DType t = sb.type;
    if (t != DType::CInt16 && t != DType::CInt32 &&
        t != DType::CFloat32 && t != DType::CFloat64)
        return true;
    std::vector<uint8_t> raw;
    if (!src->readBandRaw(band, raw))
        return false;
    switch (t)
    {
        case DType::CInt16:
        {
            const int16_t *p = (const int16_t *)raw.data();
            for (size_t i = 0; i < n; i++)
                im[i] = p[2 * i + 1];
            break;
        }
        case DType::CInt32:
        {
            const int32_t *p = (const int32_t *)raw.data();
            for (size_t i = 0; i < n; i++)
                im[i] = p[2 * i + 1];
            break;
        }
        case DType::CFloat32:
        {
            const float *p = (const float *)raw.data();
            for (size_t i = 0; i < n; i++)
                im[i] = p[2 * i + 1];
            break;
        }
        default:
        {
            const double *p = (const double *)raw.data();
            for (size_t i = 0; i < n; i++)
                im[i] = p[2 * i + 1];
            break;
        }
    }
    return true;
}

}  // namespace

bool VrtDataset::computeBandDouble(int band, std::vector<double> &out)
{
    return computeBandPlanes(band, out, nullptr);
}

bool VrtDataset::computeBandPlanes(int band, std::vector<double> &out,
                                   std::vector<double> *imOut,
                                   bool smallIntDest)
{
    out.assign((size_t)width * height, 0.0);
    if (imOut)
        imOut->assign((size_t)width * height, 0.0);
    for (auto &s : bandSources[(size_t)band - 1])
    {
        RasterDatasetBase *src = sourceAttempt(s);
        if (!src)
            return false;
        std::vector<double> sv;
        if (s.maskBand)
        {
            if (!sourceMaskPlane(src, s.sourceBand, sv))
                return false;
        }
        else if (!src->readBand(s.sourceBand, sv))
            return false;
        std::vector<double> siv;
        if (imOut && !sourceImagPlane(src, s.sourceBand, siv))
            return false;
        const std::vector<ColorEntry> *sct = nullptr;
        if (s.ctComponent > 0)
            sct = &src->bands[(size_t)s.sourceBand - 1].colorTable;
        int srcW = src->width, srcH = src->height;
        double sx = s.hasSrcRect ? s.sx : 0;
        double sy = s.hasSrcRect ? s.sy : 0;
        double sw = s.hasSrcRect ? s.sw : srcW;
        double sh = s.hasSrcRect ? s.sh : srcH;
        double dx = s.hasDstRect ? s.dx : 0;
        double dy = s.hasDstRect ? s.dy : 0;
        double dw = s.hasDstRect ? s.dw : width;
        double dh = s.hasDstRect ? s.dh : height;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
            continue;
        // clip source window to the source extent, shifting the dst
        // window proportionally the way VRTSimpleSource does
        if (sx < 0)
        {
            dx += -sx * dw / sw;
            dw -= -sx * dw / sw;
            sw += sx;
            sx = 0;
        }
        if (sy < 0)
        {
            dy += -sy * dh / sh;
            dh -= -sy * dh / sh;
            sh += sy;
            sy = 0;
        }
        if (sx + sw > srcW)
        {
            double cut = sx + sw - srcW;
            dw -= cut * dw / sw;
            sw -= cut;
        }
        if (sy + sh > srcH)
        {
            double cut = sy + sh - srcH;
            dh -= cut * dh / sh;
            sh -= cut;
        }
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
            continue;
        int x0 = (int)std::floor(dx + 0.5);
        int y0 = (int)std::floor(dy + 0.5);
        int x1 = (int)std::floor(dx + dw + 0.5);
        int y1 = (int)std::floor(dy + dh + 0.5);
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > width)
            x1 = width;
        if (y1 > height)
            y1 = height;
        for (int py = y0; py < y1; py++)
        {
            double sYf = sy + (py + 0.5 - dy) * sh / dh;
            int sY = (int)std::floor(sYf);
            if (sY < 0)
                sY = 0;
            if (sY >= srcH)
                sY = srcH - 1;
            for (int px = x0; px < x1; px++)
            {
                double sXf = sx + (px + 0.5 - dx) * sw / dw;
                int sX = (int)std::floor(sXf);
                if (sX < 0)
                    sX = 0;
                if (sX >= srcW)
                    sX = srcW - 1;
                double v = sv[(size_t)sY * srcW + sX];
                if (s.complex)
                {
                    if (sct)
                    {
                        long long idx = (long long)v;
                        if (idx >= 0 && idx < (long long)sct->size())
                        {
                            const ColorEntry &ce = (*sct)[(size_t)idx];
                            v = s.ctComponent == 1   ? ce.c1
                                : s.ctComponent == 2 ? ce.c2
                                : s.ctComponent == 3 ? ce.c3
                                                     : ce.c4;
                        }
                        else
                            v = 0;
                    }
                    if (s.hasExponent)
                    {
                        double base = (v - s.expSrcMin) /
                                      (s.expSrcMax - s.expSrcMin);
                        if (s.expClip)
                        {
                            if (base < 0)
                                base = 0;
                            if (base > 1)
                                base = 1;
                        }
                        v = s.expDstMin + (s.expDstMax - s.expDstMin) *
                                              std::pow(base, s.exponent);
                    }
                    else
                        v = v * s.scaleRatio + s.scaleOffset;
                    if (s.hasLut)
                        v = lutLookup(s.lut, v);
                    // scaled pixels stream through a Float32 working
                    // buffer when the destination type fits one
                    if (smallIntDest &&
                        (s.hasExponent || s.hasScaleRatio ||
                         s.hasScaleOffset))
                        v = (double)(float)v;
                }
                out[(size_t)py * width + px] = v;
                if (imOut)
                    (*imOut)[(size_t)py * width + px] =
                        siv[(size_t)sY * srcW + sX];
            }
        }
    }
    return true;
}

bool VrtDataset::readBand(int band, std::vector<double> &out)
{
    return computeBandDouble(band, out);
}

bool VrtDataset::readBandRaw(int band, std::vector<uint8_t> &out)
{
    DType t = bands[(size_t)band - 1].type;
    bool cplx = t == DType::CInt16 || t == DType::CInt32 ||
                t == DType::CFloat32 || t == DType::CFloat64;
    std::vector<double> vals, ivals;
    bool smallInt = t == DType::Byte || t == DType::Int8 ||
                    t == DType::UInt16 || t == DType::Int16;
    if (!computeBandPlanes(band, vals, cplx ? &ivals : nullptr, smallInt))
        return false;
    size_t n = vals.size();
    out.resize(n * (size_t)dtypeSizeBytes(t));
    switch (t)
    {
        case DType::Byte:
        {
            uint8_t *p = out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (uint8_t)packClampInt(vals[i], 0, 255);
            break;
        }
        case DType::Int8:
        {
            int8_t *p = (int8_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (int8_t)packClampInt(vals[i], -128, 127);
            break;
        }
        case DType::UInt16:
        {
            uint16_t *p = (uint16_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (uint16_t)packClampInt(vals[i], 0, 65535);
            break;
        }
        case DType::Int16:
        {
            int16_t *p = (int16_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (int16_t)packClampInt(vals[i], -32768, 32767);
            break;
        }
        case DType::UInt32:
        {
            uint32_t *p = (uint32_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (uint32_t)packClampInt(vals[i], 0, 4294967295.0);
            break;
        }
        case DType::Int32:
        {
            int32_t *p = (int32_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (int32_t)packClampInt(vals[i], -2147483648.0,
                                             2147483647.0);
            break;
        }
        case DType::UInt64:
        {
            uint64_t *p = (uint64_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (uint64_t)packClampInt(vals[i], 0,
                                              18446744073709551615.0);
            break;
        }
        case DType::Int64:
        {
            int64_t *p = (int64_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (int64_t)packClampInt(vals[i],
                                             -9223372036854775808.0,
                                             9223372036854775807.0);
            break;
        }
        case DType::Float32:
        {
            float *p = (float *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = (float)vals[i];
            break;
        }
        case DType::Float64:
        {
            double *p = (double *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = vals[i];
            break;
        }
        case DType::Float16:
        {
            uint16_t *p = (uint16_t *)out.data();
            for (size_t i = 0; i < n; i++)
                p[i] = tailFloatToHalf((float)vals[i]);
            break;
        }
        case DType::CInt16:
        {
            int16_t *p = (int16_t *)out.data();
            for (size_t i = 0; i < n; i++)
            {
                p[2 * i] = (int16_t)packClampInt(vals[i], -32768, 32767);
                p[2 * i + 1] =
                    (int16_t)packClampInt(ivals[i], -32768, 32767);
            }
            break;
        }
        case DType::CInt32:
        {
            int32_t *p = (int32_t *)out.data();
            for (size_t i = 0; i < n; i++)
            {
                p[2 * i] = (int32_t)packClampInt(vals[i], -2147483648.0,
                                                 2147483647.0);
                p[2 * i + 1] = (int32_t)packClampInt(
                    ivals[i], -2147483648.0, 2147483647.0);
            }
            break;
        }
        case DType::CFloat32:
        {
            float *p = (float *)out.data();
            for (size_t i = 0; i < n; i++)
            {
                p[2 * i] = (float)vals[i];
                p[2 * i + 1] = (float)ivals[i];
            }
            break;
        }
        case DType::CFloat64:
        {
            double *p = (double *)out.data();
            for (size_t i = 0; i < n; i++)
            {
                p[2 * i] = vals[i];
                p[2 * i + 1] = ivals[i];
            }
            break;
        }
        default:
        {
            memset(out.data(), 0, out.size());
            break;
        }
    }
    return true;
}

bool VrtDataset::readAllBands(std::vector<std::vector<uint8_t>> &out,
                              TermProgress *tp, bool strict)
{
    VrtSource *bad = nullptr;
    for (int b = 1; b <= (int)bands.size() && !bad; b++)
        bad = firstFailing(b);
    if (!bad)
        return RasterDatasetBase::readAllBands(out, tp, strict);
    bool multi = bands.size() > 1;
    int pre = bad->state == 2 ? 3 : 2;
    int post = bad->state == 2 ? (multi ? 4 : 5) : 4;
    for (int i = 0; i < pre; i++)
        sourceAttempt(*bad);
    if (tp)
        tp->update(0.0);
    for (int i = 0; i < post; i++)
        sourceAttempt(*bad);
    return false;
}
