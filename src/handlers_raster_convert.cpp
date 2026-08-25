#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "gtiff_write.h"
#include "ogr.h"
#include "progress.h"
#include "srs.h"
#include "tiff.h"
#include "util.h"
#include "vrt.h"
#include "vsi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace
{

struct TiffTagInfo
{
    const char *item;
    uint16_t tag;
};

const TiffTagInfo kAsciiTiffTags[] = {
    {"TIFFTAG_DOCUMENTNAME", 269}, {"TIFFTAG_IMAGEDESCRIPTION", 270},
    {"TIFFTAG_SOFTWARE", 305},     {"TIFFTAG_DATETIME", 306},
    {"TIFFTAG_ARTIST", 315},       {"TIFFTAG_HOSTCOMPUTER", 316},
    {"TIFFTAG_COPYRIGHT", 33432},
};

int asciiTiffTag(const std::string &key)
{
    for (const auto &t : kAsciiTiffTags)
        if (strEqualNoCase(key, t.item))
            return t.tag;
    return -1;
}

bool isSpecialTiffTagItem(const std::string &key)
{
    if (asciiTiffTag(key) >= 0)
        return true;
    return strEqualNoCase(key, "TIFFTAG_XRESOLUTION") ||
           strEqualNoCase(key, "TIFFTAG_YRESOLUTION") ||
           strEqualNoCase(key, "TIFFTAG_RESOLUTIONUNIT") ||
           strEqualNoCase(key, "TIFFTAG_MINSAMPLEVALUE") ||
           strEqualNoCase(key, "TIFFTAG_MAXSAMPLEVALUE");
}

// CSLSort ordering: case-insensitive key comparison
void sortItems(MetaDomain &items)
{
    std::stable_sort(items.begin(), items.end(),
                     [](const std::pair<std::string, std::string> &a,
                        const std::pair<std::string, std::string> &b) {
                         return strcasecmp(a.first.c_str(),
                                           b.first.c_str()) < 0;
                     });
}

std::string naturalInterp(int photometric, int bandIdx0,
                          const std::vector<uint16_t> &extras,
                          int colorChannels)
{
    if (photometric == 3)
        return bandIdx0 == 0 ? "Palette" : "Undefined";
    if (photometric == 2)
    {
        if (bandIdx0 == 0)
            return "Red";
        if (bandIdx0 == 1)
            return "Green";
        if (bandIdx0 == 2)
            return "Blue";
    }
    else if (bandIdx0 == 0)
        return "Gray";
    int ei = bandIdx0 - colorChannels;
    if (ei >= 0 && ei < (int)extras.size() &&
        (extras[ei] == 1 || extras[ei] == 2))
        return "Alpha";
    return "Undefined";
}

// round-to-nearest-even, unlike the truncating tailFloatToHalf used by
// value burning
uint16_t halfRoundFromFloat(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t absx = x & 0x7fffffff;
    if (absx >= 0x7f800000)
        return (uint16_t)(sign | 0x7c00 |
                          (absx > 0x7f800000 ? 0x200 : 0));
    int32_t exp = (int32_t)(absx >> 23) - 127 + 15;
    uint32_t mant = absx & 0x7fffff;
    if (exp >= 31)
        return (uint16_t)(sign | 0x7c00);
    if (exp <= 0)
    {
        if (exp < -10)
            return (uint16_t)sign;
        mant |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp);
        uint16_t v = (uint16_t)(mant >> shift);
        uint32_t rem = mant & ((1u << shift) - 1);
        uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (v & 1)))
            v++;
        return (uint16_t)(sign | v);
    }
    uint16_t v = (uint16_t)(((uint32_t)exp << 10) | (mant >> 13));
    uint32_t rem = mant & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (v & 1)))
        v++;  // mantissa carry rolls into the exponent, up to infinity
    return (uint16_t)(sign | v);
}

std::string jsonEscapeCmd(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '"':
                r += "\\\"";
                break;
            case '\\':
                r += "\\\\";
                break;
            case '/':
                r += "\\/";
                break;
            case '\n':
                r += "\\n";
                break;
            case '\r':
                r += "\\r";
                break;
            case '\t':
                r += "\\t";
                break;
            default:
                if ((unsigned char)c < 0x20)
                    r += strPrintf("\\u%04x", c);
                else
                    r += c;
        }
    }
    return r;
}

std::string xmlTextEsc(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        switch (c)
        {
            case '&':
                r += "&amp;";
                break;
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            default:
                r += c;
        }
    }
    return r;
}

std::string xmlAttrEsc(const std::string &s)
{
    std::string r;
    for (char c : s)
    {
        if (c == '"')
            r += "&quot;";
        else
        {
            std::string t = xmlTextEsc(std::string(1, c));
            r += t;
        }
    }
    return r;
}

std::string fmt18g(double d)
{
    if (std::isnan(d))
        return "nan";
    return strPrintf("%.18g", d);
}

std::string fmt16g(double d)
{
    return strPrintf("%.16g", d);
}

std::string baseNameOf(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string dirNameOf(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

// GTiff CreateCopy: photometric / extra samples derivation
void deriveConvertPhotometric(const RasterDatasetBase &ds, bool hasCt,
                              int &photometric,
                              std::vector<uint16_t> &extras)
{
    int bands = (int)ds.bands.size();
    if (hasCt)
        photometric = 3;
    else if (bands >= 3 && !ds.bands.empty() &&
             ds.bands[0].type == DType::Byte)
        photometric = 2;
    else
        photometric = 1;
    int colorChannels = photometric == 2 ? 3 : 1;
    extras.clear();
    for (int i = colorChannels; i < bands; i++)
        extras.push_back(ds.bands[(size_t)i].colorInterp == "Alpha" ||
                                 photometric == 3
                             ? 2
                             : 0);
}

}  // namespace

void printProgress()
{
    fputs("0...10...20...30...40...50...60...70...80...90...100 - done.\n",
          stdout);
    fflush(stdout);
}

std::vector<GmdItem> buildGmdItems(const RasterDatasetBase &ds,
                                   int photometric,
                                   const std::vector<uint16_t> &extras,
                                   bool sortMd, int allDomains)
{
    std::vector<GmdItem> items;
    auto skipDomain = [&](const std::string &d) {
        if (allDomains == 0)
            return !d.empty();
        if (allDomains == 1 && d == "SUBDATASETS")
            return true;
        return d == "IMAGE_STRUCTURE" || d == "DERIVED_SUBDATASETS";
    };
    std::vector<std::string> dsDomains = ds.domainOrder;
    if (std::find(dsDomains.begin(), dsDomains.end(), std::string()) ==
        dsDomains.end())
        dsDomains.push_back("");
    for (const std::string &domain : dsDomains)
    {
        if (skipDomain(domain))
            continue;
        if (domain.compare(0, 4, "xml:") == 0)
        {
            auto x = ds.xmlDomains.find(domain);
            if (x != ds.xmlDomains.end() && !x->second.empty())
                items.push_back({"doc", x->second, -1, "", domain});
            continue;
        }
        auto dsIt = ds.metadata.find(domain);
        if (dsIt == ds.metadata.end())
            continue;
        MetaDomain dsItems;
        for (const auto &kv : dsIt->second)
        {
            if (domain.empty() &&
                (strEqualNoCase(kv.first, "AREA_OR_POINT") ||
                 isSpecialTiffTagItem(kv.first)))
                continue;
            dsItems.push_back(kv);
        }
        if (sortMd)
            sortItems(dsItems);
        for (const auto &kv : dsItems)
            items.push_back({kv.first, kv.second, -1, "", domain});
    }

    int colorChannels = photometric == 2 || photometric == 6 ? 3 : 1;
    bool standardColorInterp = false;
    if (photometric == 3)
        standardColorInterp = !ds.bands.empty() &&
                              ds.bands[0].colorInterp == "Palette";
    else if (photometric == 1)
    {
        standardColorInterp = true;
        for (size_t bi = 0; bi < ds.bands.size(); bi++)
        {
            const std::string &ci = ds.bands[bi].colorInterp;
            if (!(ci == "Gray" || ci == "Undefined" ||
                  (bi > 0 && ci == "Alpha")))
            {
                standardColorInterp = false;
                break;
            }
        }
    }
    else if (photometric == 2 || photometric == 6)
    {
        standardColorInterp = true;
        static const char *rgb[] = {"Red", "Green", "Blue"};
        for (size_t bi = 0; bi < ds.bands.size(); bi++)
        {
            const std::string &ci = ds.bands[bi].colorInterp;
            if (bi < 3 ? ci != rgb[bi]
                       : !(ci == "Undefined" || ci == "Alpha"))
            {
                standardColorInterp = false;
                break;
            }
        }
    }
    for (size_t bi = 0; bi < ds.bands.size(); bi++)
    {
        const Band &b = ds.bands[bi];
        for (const std::string &domain : b.domainOrder)
        {
            if (skipDomain(domain))
                continue;
            if (domain.compare(0, 4, "xml:") == 0)
            {
                auto x = b.xmlDomains.find(domain);
                if (x != b.xmlDomains.end() && !x->second.empty())
                    items.push_back(
                        {"doc", x->second, (int)bi, "", domain});
                continue;
            }
            auto it = b.metadata.find(domain);
            if (it == b.metadata.end())
                continue;
            MetaDomain bItems = it->second;
            if (sortMd)
                sortItems(bItems);
            for (const auto &kv : bItems)
                items.push_back({kv.first, kv.second, (int)bi, "", domain});
        }
        if (b.hasOffset || b.hasScale)
        {
            items.push_back({"OFFSET", strPrintf("%.17g", b.offset),
                             (int)bi, "offset"});
            items.push_back(
                {"SCALE", strPrintf("%.17g", b.scale), (int)bi, "scale"});
        }
        if (!b.unitType.empty() && !b.unitImplicit)
            items.push_back({"UNITTYPE", b.unitType, (int)bi, "unittype"});
        if (!b.description.empty())
            items.push_back(
                {"DESCRIPTION", b.description, (int)bi, "description"});
        if (!standardColorInterp)
            items.push_back(
                {"COLORINTERP", b.colorInterp, (int)bi, "colorinterp"});
    }
    return items;
}

namespace
{

std::string vrtRelativePath(const std::string &input,
                            const std::string &output, int &relative)
{
    std::string outDir = dirNameOf(output);
    if (outDir.empty())
    {
        relative = !input.empty() && input[0] != '/' &&
                           input.find(':') == std::string::npos
                       ? 1
                       : 0;
        return input;
    }
    if (input.size() > outDir.size() + 1 &&
        input.compare(0, outDir.size(), outDir) == 0 &&
        input[outDir.size()] == '/')
    {
        relative = 1;
        return input.substr(outDir.size() + 1);
    }
    relative = 0;
    return input;
}

// VRT real serialization: %g when it round-trips, otherwise %.17g
static std::string fmtVrtReal(double v)
{
    std::string s = strPrintf("%g", v);
    if (strtod(s.c_str(), nullptr) == v)
        return s;
    return strPrintf("%.17g", v);
}

}  // namespace

void emitVrtMetadataEcho(std::string &x,
                         const std::map<std::string, MetaDomain> &md,
                         const std::vector<std::string> &order,
                         const std::string &indent)
{
    auto emitDomain = [&](const std::string &dom)
    {
        auto it = md.find(dom);
        if (it == md.end() || it->second.empty())
            return;
        x += indent + "<Metadata" +
             (dom.empty() ? std::string()
                          : " domain=\"" + xmlAttrEsc(dom) + "\"") +
             ">\n";
        for (const auto &kv : it->second)
            x += indent + "  <MDI key=\"" + xmlAttrEsc(kv.first) + "\">" +
                 xmlTextEsc(kv.second) + "</MDI>\n";
        x += indent + "</Metadata>\n";
    };
    // the default domain always serializes ahead of named domains,
    // whatever position it held in the source XML
    emitDomain("");
    for (const auto &dom : order)
        if (!dom.empty())
            emitDomain(dom);
}

namespace
{

std::string buildVrt(RasterDatasetBase &ds, const std::string &input,
                     const std::string &output, bool openLoneSource = true)
{
    VrtDataset *vd = dynamic_cast<VrtDataset *>(&ds);
    std::string x;
    x += strPrintf("<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\">\n",
                   ds.width, ds.height);
    if (ds.hasSrs && ds.srs.valid())
    {
        std::vector<int> mapv = vd && !vd->axisMapping.empty()
                                    ? vd->axisMapping
                                    : ds.srs.dataAxisToSRSAxisMapping();
        std::string mapping;
        for (int m : mapv)
        {
            if (!mapping.empty())
                mapping += ",";
            mapping += strPrintf("%d", m);
        }
        std::string wkt = ds.srs.wkt1Gdal();
        if (wkt.empty())
            wkt = ds.srs.wkt2SingleLine();
        x += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             xmlTextEsc(wkt) + "</SRS>\n";
    }
    if (ds.hasGT)
    {
        x += "  <GeoTransform>";
        for (int i = 0; i < 6; i++)
        {
            if (i)
                x += ",";
            x += strPrintf("%24.16e", ds.gt[i]);
        }
        x += "</GeoTransform>\n";
    }
    if (vd)
    {
        emitVrtMetadataEcho(x, ds.metadata, ds.domainOrder, "  ");
        x += gcpListXml(ds, "  ");
    }
    else
    {
        {
            auto it = ds.metadata.find("");
            MetaDomain items;
            if (it != ds.metadata.end())
                items = it->second;
            sortItems(items);
            if (!items.empty())
            {
                x += "  <Metadata>\n";
                for (const auto &kv : items)
                    x += "    <MDI key=\"" + xmlAttrEsc(kv.first) + "\">" +
                         xmlTextEsc(kv.second) + "</MDI>\n";
                x += "  </Metadata>\n";
            }
        }
        auto it = ds.metadata.find("IMAGE_STRUCTURE");
        std::string interleave = "BAND", compression;
        if (it != ds.metadata.end())
            for (const auto &kv : it->second)
            {
                if (kv.first == "INTERLEAVE")
                    interleave = kv.second;
                else if (kv.first == "COMPRESSION")
                    compression = kv.second;
            }
        x += "  <Metadata domain=\"IMAGE_STRUCTURE\">\n";
        x += "    <MDI key=\"INTERLEAVE\">" + interleave + "</MDI>\n";
        if (!compression.empty())
            x += "    <MDI key=\"COMPRESSION\">" + compression + "</MDI>\n";
        x += "  </Metadata>\n";
        x += gcpListXml(ds, "  ");
    }
    int srcBlockX = ds.bands.empty() ? ds.width : ds.bands[0].blockX;
    int srcBlockY = ds.bands.empty() ? ds.height : ds.bands[0].blockY;
    int relative = 0;
    std::string srcName = vrtRelativePath(input, output, relative);
    if (!g_infoFilesHide.empty() && input == g_infoFilesHide)
    {
        // the reference serializes its live rasterize temp verbatim,
        // never as a VRT-relative path
        relative = 0;
        srcName = input;
    }
    int bandPos = 0;
    for (const Band &b : ds.bands)
    {
        ++bandPos;
        int defBlockX = std::min(ds.width, 128);
        int defBlockY = std::min(ds.height, 128);
        std::string attrs;
        if (b.blockX != defBlockX)
            attrs += strPrintf(" blockXSize=\"%d\"", b.blockX);
        if (b.blockY != defBlockY)
            attrs += strPrintf(" blockYSize=\"%d\"", b.blockY);
        if (vd)
        {
            bool hasKids = !vd->bandSources[(size_t)bandPos - 1].empty() ||
                           !b.description.empty() || b.hasNodata ||
                           !b.unitType.empty() || b.hasOffset ||
                           b.hasScale || b.colorInterp != "Undefined" ||
                           !b.colorTable.empty() || !b.pamHists.empty();
            for (const auto &dom : b.metadata)
                if (!dom.second.empty())
                    hasKids = true;
            if (!hasKids)
            {
                x += strPrintf(
                    "  <VRTRasterBand dataType=\"%s\" band=\"%d\"%s />\n",
                    dtypeName(b.type), bandPos, attrs.c_str());
                continue;
            }
        }
        x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\"%s>\n",
                       dtypeName(b.type), bandPos, attrs.c_str());
        if (vd)
            emitVrtMetadataEcho(x, b.metadata, b.domainOrder, "    ");
        else
        {
            MetaDomain bItems;
            auto it = b.metadata.find("");
            if (it != b.metadata.end())
                bItems = it->second;
            sortItems(bItems);
            if (!bItems.empty())
            {
                x += "    <Metadata>\n";
                for (const auto &kv : bItems)
                    x += "      <MDI key=\"" + xmlAttrEsc(kv.first) +
                         "\">" + xmlTextEsc(kv.second) + "</MDI>\n";
                x += "    </Metadata>\n";
            }
            const std::string *nb = b.getMd("IMAGE_STRUCTURE", "NBITS");
            if (nb)
            {
                x += "    <Metadata domain=\"IMAGE_STRUCTURE\">\n";
                x += "      <MDI key=\"NBITS\">" + *nb + "</MDI>\n";
                x += "    </Metadata>\n";
            }
        }
        if (!b.description.empty())
            x += "    <Description>" + xmlTextEsc(b.description) +
                 "</Description>\n";
        if (b.hasNodata)
        {
            std::string nd;
            if (b.nodataIsI64)
                nd = strPrintf("%lld", b.nodataI64);
            else if (b.nodataIsU64)
                nd = strPrintf("%llu", b.nodataU64);
            else
                nd = fmt18g(b.nodata);
            x += "    <NoDataValue>" + nd + "</NoDataValue>\n";
        }
        if (!b.unitType.empty())
            x += "    <UnitType>" + xmlTextEsc(b.unitType) +
                 "</UnitType>\n";
        if (b.hasOffset)
            x += "    <Offset>" + fmt16g(b.offset) + "</Offset>\n";
        if (b.hasScale)
            x += "    <Scale>" + fmt16g(b.scale) + "</Scale>\n";
        if (b.colorInterp != "Undefined")
            x += "    <ColorInterp>" + b.colorInterp + "</ColorInterp>\n";
        if (!b.colorTable.empty())
        {
            x += "    <ColorTable>\n";
            for (const ColorEntry &e : b.colorTable)
                x += strPrintf("      <Entry c1=\"%d\" c2=\"%d\" c3=\"%d\" "
                               "c4=\"%d\" />\n",
                               e.c1, e.c2, e.c3, e.c4);
            x += "    </ColorTable>\n";
        }
        if (vd)
        {
            if (!b.pamHists.empty())
                emitHistogramsXml(x, b.pamHists, "    ");
            for (auto &s : vd->bandSources[(size_t)bandPos - 1])
            {
                const char *tag =
                    s.complex ? "ComplexSource" : "SimpleSource";
                RasterDatasetBase *sd = nullptr;
                // SourceProperties appears for sources that got opened:
                // always the lone band-1 source during a copy, plus any
                // source already opened by earlier work (stats, checksum)
                if (s.state == 1)
                    sd = s.ds.get();
                else if (openLoneSource && s.state == 0 && bandPos == 1 &&
                         vd->bandSources[0].size() == 1)
                {
                    cplPushQuietHandler();
                    sd = vd->sourceAttempt(s);
                    cplPopHandler();
                }
                // paths only become VRT-relative when the file exists
                int rel = 0;
                std::string nm = s.resolved;
                if (vsiExists(s.resolved))
                    nm = vrtRelativePath(s.resolved, output, rel);
                x += strPrintf("    <%s>\n", tag);
                if (s.hasNested)
                    xmlSerialize(s.nestedNode, x, 3);
                else
                    x += strPrintf(
                        "      <SourceFilename relativeToVRT=\"%d\">%s"
                        "</SourceFilename>\n",
                        rel, xmlTextEsc(nm).c_str());
                if (s.maskBand)
                    x += strPrintf(
                        "      <SourceBand>mask,%d</SourceBand>\n",
                        s.sourceBand);
                else
                    x += strPrintf("      <SourceBand>%d</SourceBand>\n",
                                   s.sourceBand);
                if (sd)
                {
                    const Band &sb = sd->bands[(size_t)s.sourceBand - 1];
                    x += strPrintf(
                        "      <SourceProperties RasterXSize=\"%d\" "
                        "RasterYSize=\"%d\" DataType=\"%s\" "
                        "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                        sd->width, sd->height, dtypeName(sb.type),
                        sb.blockX, sb.blockY);
                }
                if (s.hasSrcRect)
                    x += strPrintf("      <SrcRect xOff=\"%.15g\" "
                                   "yOff=\"%.15g\" xSize=\"%.15g\" "
                                   "ySize=\"%.15g\" />\n",
                                   s.sx, s.sy, s.sw, s.sh);
                if (s.hasDstRect)
                    x += strPrintf("      <DstRect xOff=\"%.15g\" "
                                   "yOff=\"%.15g\" xSize=\"%.15g\" "
                                   "ySize=\"%.15g\" />\n",
                                   s.dx, s.dy, s.dw, s.dh);
                if (s.complex && (s.hasScaleRatio || s.hasScaleOffset))
                {
                    x += strPrintf("      <ScaleOffset>%.15g"
                                   "</ScaleOffset>\n",
                                   s.scaleOffset);
                    x += strPrintf("      <ScaleRatio>%.15g"
                                   "</ScaleRatio>\n",
                                   s.scaleRatio);
                }
                if (s.complex && s.hasExponent)
                {
                    x += strPrintf("      <Exponent>%.15g</Exponent>\n",
                                   s.exponent);
                    x += strPrintf("      <SrcMin>%.15g</SrcMin>\n",
                                   s.expSrcMin);
                    x += strPrintf("      <SrcMax>%.15g</SrcMax>\n",
                                   s.expSrcMax);
                    x += strPrintf("      <DstMin>%.15g</DstMin>\n",
                                   s.expDstMin);
                    x += strPrintf("      <DstMax>%.15g</DstMax>\n",
                                   s.expDstMax);
                    x += strPrintf("      <Clip>%s</Clip>\n",
                                   s.expClip ? "true" : "false");
                }
                if (s.complex && s.hasLut)
                {
                    x += "      <LUT>";
                    for (size_t li = 0; li < s.lut.size(); ++li)
                    {
                        if (li)
                            x += ",";
                        x += fmtVrtReal(s.lut[li].first) + ":" +
                             fmtVrtReal(s.lut[li].second);
                    }
                    x += "</LUT>\n";
                }
                if (s.complex && s.ctComponent > 0)
                    x += strPrintf("      <ColorTableComponent>%d"
                                   "</ColorTableComponent>\n",
                                   s.ctComponent);
                x += strPrintf("    </%s>\n", tag);
            }
            x += "  </VRTRasterBand>\n";
            continue;
        }
        bool complexTag = false;
        DType srcType = b.type;
        std::string kids;
        ds.vrtWrapperSource(bandPos, complexTag, srcType, kids);
        RasterDatasetBase::WrapRects wr;
        wr.srcW = ds.width;
        wr.srcH = ds.height;
        wr.srcBlockX = srcBlockX;
        wr.srcBlockY = srcBlockY;
        wr.sw = wr.dw = ds.width;
        wr.sh = wr.dh = ds.height;
        ds.vrtWrapperRects(wr);
        const char *tag = complexTag ? "ComplexSource" : "SimpleSource";
        std::string tagAttrs;
        if (!wr.resampling.empty())
            tagAttrs = " resampling=\"" + wr.resampling + "\"";
        x += strPrintf("    <%s%s>\n", tag, tagAttrs.c_str());
        x += strPrintf("      <SourceFilename relativeToVRT=\"%d\">%s"
                       "</SourceFilename>\n",
                       relative, xmlTextEsc(srcName).c_str());
        x += strPrintf("      <SourceBand>%s</SourceBand>\n",
                       ds.vrtWrapperSourceBandText(bandPos).c_str());
        x += strPrintf("      <SourceProperties RasterXSize=\"%d\" "
                       "RasterYSize=\"%d\" DataType=\"%s\" "
                       "BlockXSize=\"%d\" BlockYSize=\"%d\" />\n",
                       wr.srcW, wr.srcH, dtypeName(srcType), wr.srcBlockX,
                       wr.srcBlockY);
        x += strPrintf("      <SrcRect xOff=\"%lld\" yOff=\"%lld\" "
                       "xSize=\"%lld\" ySize=\"%lld\" />\n",
                       wr.sx, wr.sy, wr.sw, wr.sh);
        x += strPrintf("      <DstRect xOff=\"%lld\" yOff=\"%lld\" "
                       "xSize=\"%lld\" ySize=\"%lld\" />\n",
                       wr.dx, wr.dy, wr.dw, wr.dh);
        x += kids;
        x += strPrintf("    </%s>\n", tag);
        x += "  </VRTRasterBand>\n";
    }
    x += "</VRTDataset>\n";
    return x;
}

std::string buildGdalgJson(ParseResult &r, const std::string &input,
                           const std::string &extra = std::string())
{
    std::string cmd = handlerInvokedCli();
    auto ifs = r.list("input-format");
    if (!ifs.empty())
    {
        cmd += " --input-format ";
        for (size_t i = 0; i < ifs.size(); i++)
        {
            if (i)
                cmd += ",";
            cmd += ifs[i];
        }
    }
    for (const auto &o : r.list("open-option"))
        cmd += " --open-option " + o;
    cmd += " --input " + input;
    // stdout serialization runs under forced quiet, and the echo says so
    if (r.flag("quiet") || r.str("output") == "/vsistdout/")
        cmd += " --quiet";
    for (const auto &c : r.list("creation-option"))
        cmd += " --creation-option " + c;
    cmd += extra;
    cmd += " --output-format stream --output streamed_dataset";
    if (!g_pipelineGdalgCli.empty())
        cmd = g_pipelineGdalgCli;
    std::string j = "{\n";
    j += "  \"type\":\"gdal_streamed_alg\",\n";
    j += "  \"command_line\":\"" + jsonEscapeCmd(cmd) + "\",\n";
    j += "  \"gdal_version\":\"3130000\"\n";
    j += "}";
    return j;
}

bool isKnownOutDriver(const std::string &name, std::string &canon)
{
    static const char *kOutDrivers[] = {"GTiff", "COG",   "VRT",
                                        "MEM",   "GDALG", "stream"};
    for (const char *k : kOutDrivers)
        if (strEqualNoCase(name, k))
        {
            canon = k;
            return true;
        }
    if (strEqualNoCase(name, "Memory"))
    {
        canon = "MEM";
        return true;
    }
    return false;
}

}  // namespace

std::string gcpListXml(const RasterDatasetBase &ds,
                       const std::string &indent)
{
    if (ds.gcps.empty())
        return "";
    std::string x = indent + "<GCPList";
    if (ds.hasGcpSrs && ds.gcpSrs.valid())
    {
        std::string wkt = ds.gcpSrs.wkt1Gdal();
        if (wkt.empty())
            wkt = ds.gcpSrs.wkt2SingleLine();
        x += " Projection=\"" + xmlAttrEsc(wkt) + "\"";
        std::vector<int> mapv = !ds.gcpMapping.empty()
                                    ? ds.gcpMapping
                                    : ds.gcpSrs.dataAxisToSRSAxisMapping();
        std::string mapping;
        for (int m : mapv)
        {
            if (!mapping.empty())
                mapping += ",";
            mapping += strPrintf("%d", m);
        }
        x += " dataAxisToSRSAxisMapping=\"" + mapping + "\"";
    }
    x += ">\n";
    for (const GcpEntry &g : ds.gcps)
    {
        x += indent + "  <GCP Id=\"" + xmlAttrEsc(g.id) + "\"";
        x += strPrintf(" Pixel=\"%.4f\" Line=\"%.4f\" X=\"%.12E\" "
                       "Y=\"%.12E\"",
                       g.pixel, g.line, g.x, g.y);
        if (g.z != 0.0)
            x += strPrintf(" Z=\"%.12E\"", g.z);
        if (!g.info.empty())
            x += ">\n" + indent + "    <Info>" + xmlTextEsc(g.info) +
                 "</Info>\n" + indent + "  </GCP>\n";
        else
            x += " />\n";
    }
    x += indent + "</GCPList>\n";
    return x;
}

std::string vrtSerializeXml(RasterDatasetBase &ds, const std::string &input,
                            const std::string &output)
{
    // a PAM-flush rewrite never opens sources by itself; SourceProperties
    // survives only for sources some earlier work already opened
    return buildVrt(ds, input, output, false);
}

std::string rasterOutFormatIssue(const std::string &format,
                                 std::string &canon)
{
    canon.clear();
    if (format.empty())
        return "";
    if (strEqualNoCase(format, "Memory"))
        memoryDriverDeprecationWarnOnce();
    if (isKnownOutDriver(format, canon))
        return "";
    bool ras = false, vec = false;
    if (knownDriverCaps(format, ras, vec) && !ras)
        return "Invalid value for argument 'output-format'. Driver '" +
               format + "' does not expose the required 'DCAP_RASTER' "
                        "capability.";
    return "Invalid value for argument 'output-format'. Driver '" + format +
           "' does not exist.";
}

namespace
{

std::string convertDispatchProbeImpl(const std::string &path)
{
    cplPushQuietHandler();
    std::string err;
    OpenOptions oo;
    auto ds = openRaster(path, err, oo);
    cplPopHandler();
    if (ds)
        return "raster";
    std::string verr;
    cplPushQuietHandler();
    auto vds = openVectorDataset(path, verr, {});
    cplPopHandler();
    if (vds)
        return "vector";
    return "";
}

int convertWriteOutputImpl(
    std::unique_ptr<RasterDatasetBase> &ds, ParseResult &r,
    const std::string &input, const std::string &output, bool quiet,
    bool overwrite, bool append, std::string drv,
    const std::string &gdalgExtra,
    const std::function<int(std::unique_ptr<RasterDatasetBase> &)>
        &materialize = nullptr,
    const PreWriteValidate &preWriteValidate = nullptr);

int rasterConvertHandler(const CmdSpec &spec, ParseResult &r)
{
    bool generic = spec.id == "convert";
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool overwrite = r.flag("overwrite");
    bool append = r.flag("append");
    std::string format = r.str("output-format");
    std::string drv;

    OpenOptions oo;
    oo.allowedDrivers = r.list("input-format");
    for (const auto &kv : r.list("open-option"))
    {
        size_t eq = kv.find('=');
        std::string key = eq == std::string::npos ? kv : kv.substr(0, eq);
        std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
        oo.raw.emplace_back(key, val);
        if (strEqualNoCase(key, "COLOR_TABLE_MULTIPLIER"))
            oo.ctMult = atol(val.c_str());
        else if (strEqualNoCase(key, "GEOREF_SOURCES"))
        {
            oo.georefSet = true;
            oo.georefSources = strSplit(val, ',');
        }
    }

    std::unique_ptr<RasterDatasetBase> ds;
    std::string err;

    if (generic)
    {
        // generic `gdal convert` decides raster/vector by opening first
        struct stat ist;
        if (input.compare(0, 4, "/vsi") != 0 &&
            input.find(':') == std::string::npos &&
            stat(input.c_str(), &ist) != 0)
        {
            handlerPrintUsage();
            return 1;
        }
        bool preSilent = !r.list("input-format").empty() ||
                         (!format.empty() && !isKnownOutDriver(format, drv));
        // driver-style connection strings probe quietly and fail with
        // bare usage, like plain missing paths
        const bool colonName = input.compare(0, 4, "/vsi") != 0 &&
                               input.find(':') != std::string::npos;
        drv.clear();
        if (preSilent || colonName)
            cplPushQuietHandler();
        ds = openRaster(input, err, oo);
        if (preSilent || colonName)
            cplPopHandler();
        if (!ds)
        {
            if (!preSilent && !colonName && err != "reported")
            {
                if (strncmp(input.c_str(), "/vsi", 4) == 0 &&
                    !vsiExists(input))
                {
                    // missing virtual names use the dataset wording;
                    // /vsimem/ stays silent, curl/credential noise is
                    // already fully reported by the VSI layer
                    if (vsiMissingPrelude(input) &&
                        strncasecmp(input.c_str(), "/vsimem/", 8) != 0)
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "`" + input +
                                        "' does not exist in the file "
                                        "system, and is not recognized as "
                                        "a supported dataset name.");
                }
                else
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "`" + input +
                                    "' not recognized as being in a "
                                    "supported file format.");
            }
            handlerPrintUsage();
            return 1;
        }
        handlerAppendUsageSub("raster");
        if (!r.list("input-format").empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "convert: Positional values starting at '" +
                            output + "' are not expected.");
            handlerPrintUsage();
            return 1;
        }
    }

    // ---- output format validation ----
    if (!format.empty() &&
        (!isKnownOutDriver(format, drv) ||
         (drv != "GDALG" && drv != "stream" && gdalSkipHas(drv))))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    convertMsgPrefix() +
                        ": Invalid value for argument "
                        "'output-format'. Driver '" +
                        format + "' does not exist.");
        handlerPrintUsage();
        return 1;
    }
    // GDALG is not eligible for non-terminal pipeline writes
    if (g_convertCaptureWritten && drv == "GDALG")
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "Output driver `GDALG' not recognised.");
        return 1;
    }

    if (!generic)
    {
        // ---- input format validation ----
        static const char *kInDrivers[] = {
            "GTiff",   "COG",        "VRT",      "MEM",
            "GNMFile", "GNMDatabase", "ESRI Shapefile",
            "GeoJSON", "GeoJSONSeq", "ESRIJSON", "TopoJSON"};
        for (const auto &d : r.list("input-format"))
        {
            std::string ferr = inputFormatCapError(false, d);
            if (!ferr.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            convertMsgPrefix() + ": " + ferr);
                handlerPrintUsage();
                return 1;
            }
        }

        // ---- open input ----
        ds = openRaster(input, err, oo);
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
    }


    return convertWriteOutputImpl(ds, r, input, output, quiet, overwrite,
                                  append, drv, std::string());
}

int convertWriteOutputImpl(
    std::unique_ptr<RasterDatasetBase> &ds, ParseResult &r,
    const std::string &input, const std::string &output, bool quiet,
    bool overwrite, bool append, std::string drv,
    const std::string &gdalgExtra,
    const std::function<int(std::unique_ptr<RasterDatasetBase> &)>
        &materialize,
    const PreWriteValidate &preWriteValidate)
{
    if (ds && !g_infoFilesHide.empty() && input == g_infoFilesHide)
    {
        // the live rasterize temp carries no on-disk implicit metadata
        auto strip = [&](const char *dom, const char *key) {
            auto it = ds->metadata.find(dom);
            if (it == ds->metadata.end())
                return;
            for (auto kv = it->second.begin(); kv != it->second.end();
                 ++kv)
                if (kv->first == key)
                {
                    it->second.erase(kv);
                    break;
                }
        };
        strip("", "AREA_OR_POINT");
        strip("IMAGE_STRUCTURE", "LAYOUT");
    }
    // ---- output-exists validation (file-backed drivers) ----
    struct stat st;
    bool exists = stat(output.c_str(), &st) == 0;
    bool preFailed = false;
    if (exists && !overwrite && !append && drv != "MEM" && drv != "stream")
    {
        const char *kind = "File";
        if (S_ISDIR(st.st_mode))
            kind = "Directory";
        else
        {
            cplPushQuietHandler();
            std::string e2;
            auto probe = openRaster(output, e2);
            cplPopHandler();
            if (probe)
                kind = "Dataset";
            else
            {
                std::string head;
                readFileToString(output, head);
                if (head.size() > 4096)
                    head.resize(4096);
                if (head.find("<VRTDataset") != std::string::npos ||
                    head.find("\"gdal_streamed_alg\"") != std::string::npos)
                    kind = "Dataset";
            }
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("%s: %s '%s' already exists. You may "
                              "specify the --overwrite/--append option.",
                              convertMsgPrefix().c_str(), kind,
                              output.c_str()));
        if (!preWriteValidate)
        {
            handlerPrintUsage();
            return 1;
        }
        preFailed = true;
    }

    if (preWriteValidate)
    {
        std::string dg = drv;
        if (dg.empty())
        {
            std::string base = baseNameOf(output);
            std::string lbase = strToLower(base);
            size_t dot = base.find_last_of('.');
            std::string ext =
                dot == std::string::npos ? ""
                                         : strToLower(base.substr(dot + 1));
            if (lbase.size() > 11 &&
                lbase.compare(lbase.size() - 11, 11, ".gdalg.json") == 0)
                dg = "GDALG";
            else if (ext.empty() || ext == "tif" || ext == "tiff")
                dg = "GTiff";
            else if (ext == "vrt")
                dg = "VRT";
        }
        preWriteValidate(dg, preFailed);
        if (preFailed)
        {
            handlerPrintUsage();
            return 1;
        }
    }

    if (exists && overwrite && drv != "MEM" && drv != "stream")
    {
        // GDALTranslate deletes the previous output loudly: the open it
        // performs to collect the fileset surfaces in debug traces
        if (TiffFile::identify(output) && cplDebugEnabled("GDAL"))
        {
            std::string ptr = cplDebugPtr();
            cplDebug("GDAL", "GDALOpen(" + output + ", this=" + ptr +
                                 ") succeeds as GTiff.");
            cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
            cplDebug("GDAL", "GDALClose(" + output + ", this=" + ptr + ")");
        }
        overwriteDeleteFileset(output);
    }

    const std::function<int(std::unique_ptr<RasterDatasetBase> &)> &mat =
        materialize ? materialize : g_pipelineTailMaterialize;

    // GDALTranslate same-dataset guard; wrapped verbs (select/clip/...)
    // interpose a VRT so their source description never matches
    if (!mat && drv != "stream" && !input.empty() && input == output)
    {
        g_pipelineCommitted = true;
        if (g_pipelineMode && !quiet)
            printProgress();
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Source and destination datasets must be different.");
        return 1;
    }

    g_pipelineCommitted = true;

    // ---- driver guess from output extension ----
    if (drv.empty())
    {
        std::string base = baseNameOf(output);
        size_t dot = base.find_last_of('.');
        std::string ext =
            dot == std::string::npos ? "" : strToLower(base.substr(dot + 1));
        std::string lbase = strToLower(base);
        if (lbase.size() > 11 &&
            lbase.compare(lbase.size() - 11, 11, ".gdalg.json") == 0 &&
            !g_convertCaptureWritten)
            drv = "GDALG";
        else if ((ext.empty() || ext == "tif" || ext == "tiff") &&
                 !gdalSkipHas("GTiff"))
            drv = "GTiff";
        else if (ext == "vrt" && !gdalSkipHas("VRT"))
            drv = "VRT";
        else
        {
            if (g_pipelineMode && !quiet)
                printProgress();
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Could not identify an output driver for " +
                            output);
            return 1;
        }
    }

    // ---- creation options ----
    std::vector<std::pair<std::string, std::string>> cos;
    for (const auto &c : r.list("creation-option"))
    {
        size_t eq = c.find('=');
        cos.push_back({c.substr(0, eq),
                       eq == std::string::npos ? "" : c.substr(eq + 1)});
    }

    if (drv == "VRT" && g_pipelineTotalSteps > 3)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "pipeline: VRT output is not supported when there are "
                    "more than 3 steps. Consider using the GDALG driver "
                    "(files with .gdalg.json extension)");
        return 1;
    }

    if (drv == "VRT" && !g_pipelineDemVrtVerb.empty())
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    g_pipelineDemVrtVerb +
                        ": VRT output is not supported. Consider using "
                        "the GDALG driver instead (files with .gdalg.json "
                        "extension)");
        return 1;
    }

    if (drv != "GDALG" && mat)
    {
        ds->replayDeferred();
        int mrc = mat(ds);
        if (mrc)
            return mrc;
    }

    if (drv == "stream")
    {
        if (!quiet && !ds->suppressWriteBar())
            printProgress();
        return 0;
    }

    if (drv == "MEM")
    {
        if (exists && !overwrite)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "File '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
        if (ds->driverShort == "GTiff")
            cplDebug("GTiff", "ScanDirectories()");
        ds->replayDeferred();
        if (ds->driverShort == "GTiff" && ds->overviews.empty() &&
            !ds->isSubdataset)
            ds->debugOverviewScan();
        for (int pass = 0; pass < 2; pass++)
            for (const auto &kv : cos)
                if (!strEqualNoCase(kv.first, "INTERLEAVE"))
                    cplErrorStr(
                        CE_Warning, CPLE_NotSupported,
                        "driver MEM does not support creation option " +
                            kv.first);
        if (cplDebugEnabled("GDAL"))
        {
            cplDebug("GDAL",
                     "Using default GDALDriver::CreateCopy implementation.");
            const std::string *si = ds->getMd("IMAGE_STRUCTURE",
                                              "INTERLEAVE");
            std::string effIleave =
                si && ds->bands.size() > 1 ? *si : "BAND";
            bool coIleave = false;
            for (const auto &kv : cos)
                if (strEqualNoCase(kv.first, "INTERLEAVE"))
                {
                    coIleave = true;
                    effIleave = kv.second;
                }
            bool optPtr = coIleave || strEqualNoCase(effIleave, "PIXEL");
            DType memDt =
                ds->bands.empty() ? DType::Byte : ds->bands[0].type;
            cplDebug("GDAL",
                     strPrintf("GDALDriver::Create(MEM,%s,%d,%d,%d,%s,%s)",
                               output.c_str(), ds->width, ds->height,
                               (int)ds->bands.size(), dtypeName(memDt),
                               optPtr ? cplDebugPtr().c_str() : "(nil)"));
            gdalDebugCacheMaxOnce();
            bool bi = ds->bands.size() > 1 &&
                      strEqualNoCase(effIleave, "PIXEL");
            int bpp = dtypeSizeBytes(memDt);
            long long target = gdalDefaultCacheMax() / 4;
            if (target > 2147483647LL)
                target = 2147483647LL;
            if (target < 10 * 1024 * 1024)
                target = 10 * 1024 * 1024;
            long long pixSize =
                (long long)bpp * (bi ? (long long)ds->bands.size() : 1);
            long long lines = target / (pixSize * ds->width);
            if (lines < 1)
                lines = 1;
            if (lines > ds->height)
                lines = ds->height;
            cplDebug("GDAL",
                     strPrintf("GDALDatasetCopyWholeRaster(): %d*%lld "
                               "swaths, bInterleave=%d",
                               ds->width, lines, bi ? 1 : 0));
        }
        std::vector<std::vector<uint8_t>> px;
        TermProgress tp;
        bool memBar = !quiet && !ds->suppressWriteBar();
        if (!ds->readAllBands(px, memBar ? &tp : nullptr, true))
            return 1;
        if (memBar)
            tp.update(1.0);
        debugCloseDataset(*ds);
        if (cplDebugEnabled("GDAL"))
            cplDebug("GDAL", "GDALClose(" + output + ", this=" +
                                 cplDebugPtr() + ")");
        return 0;
    }

    if (drv == "GDALG")
    {
        if (exists && append)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "File '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
        if (g_pipelineHasMidWrite)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "pipeline: Step write is not natively streaming "
                        "compatible, and may cause significant processing "
                        "time at opening");
        std::string j = buildGdalgJson(r, input, gdalgExtra);
        if (!writeStringToFile(output, j))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "convert: cannot write " + output);
            return 1;
        }
        debugCloseDataset(*ds);
        return 0;
    }

    if (drv == "VRT")
    {
        if (append && exists)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "Subdataset creation not supported for driver VRT");
            return 1;
        }
        cplDebug("GDAL", "Using VRT driver");
        if (ds->driverShort == "GTiff")
            cplDebug("GTiff", "ScanDirectories()");
        ds->replayDeferred();
        if (ds->driverShort == "GTiff" && ds->overviews.empty() &&
            !ds->isSubdataset)
            ds->debugOverviewScan();
        for (const auto &kv : cos)
            if (!strEqualNoCase(kv.first, "BLOCKXSIZE") &&
                !strEqualNoCase(kv.first, "BLOCKYSIZE"))
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "driver VRT does not support creation option " +
                                kv.first);
        if (auto *vd = dynamic_cast<VrtDataset *>(ds.get()))
        {
            // the failure scan must not leave sources marked as opened,
            // or they would gain SourceProperties in the serialized copy
            std::vector<std::pair<VrtSource *, int>> states;
            for (auto &band : vd->bandSources)
                for (auto &s : band)
                    states.push_back({&s, s.state});
            VrtSource *bad = nullptr;
            for (int b = 1; b <= (int)vd->bands.size() && !bad; b++)
                bad = vd->firstFailing(b);
            for (auto &st : states)
                if (st.second == 0 && st.first->state == 1)
                    st.first->state = 0;
            if (bad)
            {
                int pre = bad->state == 2 ? 3 : 2;
                for (int i = 0; i < pre; i++)
                    vd->sourceAttempt(*bad);
            }
        }
        std::string x = ds->customVrtXml(input, output);
        if (x.empty())
            x = buildVrt(*ds, input, output);
        if (!writeStringToFile(output, x))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "convert: cannot write " + output);
            return 1;
        }
        if (!quiet && !ds->suppressWriteBar())
            printProgress();
        if (cplDebugEnabled("GDAL"))
            cplDebug("GDAL", "GDALClose(" + output + ", this=" +
                                 cplDebugPtr() + ")");
        debugCloseDataset(*ds);
        return 0;
    }

    if (drv == "COG")
    {
        // the reference enumerates tiling schemes (tms_*.json under
        // GDAL_DATA) as soon as the COG driver engages
        if (!getenv("GDAL_DATA"))
            cplErrorStr(CE_Warning, CPLE_FileIO,
                        "Cannot find tms_NZTM2000.json (GDAL_DATA is "
                        "not defined)");
        ds->replayDeferred();
        int compression = 5;  // LZW default
        int cogPred = 0;
        int blocksize = 512;
        int zlevel = 6, zstdLevel = 9;
        std::string ovrMethod = "cubic", ovrItem = "CUBIC";
        auto notBuilt = [](const std::string &what)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "convert: COG creation option " + what +
                            " is not built in this reimplementation");
            return 1;
        };
        auto unexpectedSelect = [](const std::string &key,
                                   const std::string &val)
        {
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        strPrintf("'%s' is an unexpected value for %s "
                                  "creation option of type "
                                  "string-select.",
                                  val.c_str(), key.c_str()));
        };
        for (const auto &kv : cos)
        {
            const std::string &key = kv.first;
            const std::string &val = kv.second;
            if (strEqualNoCase(key, "COMPRESS"))
            {
                if (strEqualNoCase(val, "NONE"))
                    compression = 1;
                else if (strEqualNoCase(val, "LZW"))
                    compression = 5;
                else if (strEqualNoCase(val, "DEFLATE"))
                    compression = 8;
                else if (strEqualNoCase(val, "ZSTD"))
                    compression = 50000;
                else if (strEqualNoCase(val, "JPEG") ||
                         strEqualNoCase(val, "WEBP") ||
                         strEqualNoCase(val, "LZMA") ||
                         strEqualNoCase(val, "JXL") ||
                         strEqualNoCase(val, "LERC") ||
                         strEqualNoCase(val, "LERC_DEFLATE") ||
                         strEqualNoCase(val, "LERC_ZSTD"))
                    return notBuilt("COMPRESS=" + val);
                else
                {
                    // outside COG's select list, but a valid GTiff
                    // codec still takes effect after the warning
                    unexpectedSelect("COMPRESS", val);
                    if (strEqualNoCase(val, "PACKBITS"))
                        compression = 32773;
                }
            }
            else if (strEqualNoCase(key, "LEVEL"))
            {
                int v = atoi(val.c_str());
                zlevel = v;
                zstdLevel = v;
            }
            else if (strEqualNoCase(key, "PREDICTOR"))
            {
                if (strEqualNoCase(val, "YES") ||
                    strEqualNoCase(val, "ON") ||
                    strEqualNoCase(val, "TRUE") ||
                    strEqualNoCase(val, "STANDARD") || val == "2")
                    cogPred = 2;
                else if (strEqualNoCase(val, "FLOATING_POINT") ||
                         val == "3")
                    cogPred = 3;
                else if (strEqualNoCase(val, "NO") ||
                         strEqualNoCase(val, "OFF") ||
                         strEqualNoCase(val, "FALSE") || val == "1")
                    cogPred = 0;
                else
                    unexpectedSelect("PREDICTOR", val);
            }
            else if (strEqualNoCase(key, "BLOCKSIZE"))
            {
                int v = atoi(val.c_str());
                if (v > 0)
                    blocksize = v;
            }
            else if (strEqualNoCase(key, "OVERVIEW_RESAMPLING"))
            {
                std::string low;
                for (char c : val)
                    low += (char)tolower((unsigned char)c);
                if (low == "nearest" || low == "average" ||
                    low == "bilinear" || low == "cubic" ||
                    low == "cubicspline" || low == "lanczos" ||
                    low == "rms" || low == "mode")
                {
                    ovrMethod = low;
                    ovrItem.clear();
                    for (char c : val)
                        ovrItem += (char)toupper((unsigned char)c);
                }
                else
                    return notBuilt("OVERVIEW_RESAMPLING=" + val);
            }
            else if (strEqualNoCase(key, "NUM_THREADS"))
            {
                // no effect on the serialized bytes
            }
            else if (strEqualNoCase(key, "BIGTIFF") ||
                     strEqualNoCase(key, "SPARSE_OK") ||
                     strEqualNoCase(key, "STATISTICS"))
            {
                if (strEqualNoCase(val, "NO") ||
                    strEqualNoCase(val, "FALSE") ||
                    strEqualNoCase(val, "OFF") || val == "0" ||
                    (strEqualNoCase(key, "BIGTIFF") &&
                     (strEqualNoCase(val, "IF_NEEDED") ||
                      strEqualNoCase(val, "IF_SAFER"))))
                    continue;
                return notBuilt(key + "=" + val);
            }
            else if (strEqualNoCase(key, "QUALITY") ||
                     strEqualNoCase(key, "OVERVIEW_QUALITY") ||
                     strEqualNoCase(key, "OVERVIEW_COMPRESS") ||
                     strEqualNoCase(key, "OVERVIEW_PREDICTOR") ||
                     strEqualNoCase(key, "OVERVIEW_COUNT") ||
                     strEqualNoCase(key, "OVERVIEWS") ||
                     strEqualNoCase(key, "GEOTIFF_VERSION") ||
                     strEqualNoCase(key, "NBITS") ||
                     strEqualNoCase(key, "MAX_Z_ERROR") ||
                     strEqualNoCase(key, "MAX_Z_ERROR_OVERVIEW") ||
                     strEqualNoCase(key, "JXL_LOSSLESS") ||
                     strEqualNoCase(key, "JXL_EFFORT") ||
                     strEqualNoCase(key, "JXL_DISTANCE") ||
                     strEqualNoCase(key, "JXL_ALPHA_DISTANCE") ||
                     strEqualNoCase(key, "TILING_SCHEME") ||
                     strEqualNoCase(key, "ZOOM_LEVEL") ||
                     strEqualNoCase(key, "ZOOM_LEVEL_STRATEGY") ||
                     strEqualNoCase(key, "TARGET_SRS") ||
                     strEqualNoCase(key, "RES") ||
                     strEqualNoCase(key, "EXTENT") ||
                     strEqualNoCase(key, "ALIGNED_LEVELS") ||
                     strEqualNoCase(key, "ADD_ALPHA") ||
                     strEqualNoCase(key, "RESAMPLING") ||
                     strEqualNoCase(key, "WARP_RESAMPLING"))
                return notBuilt(key + "=" + val);
            else
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "driver COG does not support creation "
                            "option " +
                                key);
        }

        GTiffCreateParams p;
        p.width = ds->width;
        p.height = ds->height;
        p.bands = (int)ds->bands.size();
        p.type = ds->bands.empty() ? DType::Byte : ds->bands[0].type;
        if (p.width <= 0 || p.height <= 0 || p.bands <= 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Attempt to create %dx%d dataset is "
                                  "illegal, sizes must be larger than "
                                  "zero.",
                                  p.width, p.height));
            return 1;
        }
        p.tiled = true;
        p.blockX = blocksize;
        p.blockY = blocksize;
        p.compression = compression;
        p.predictor = cogPred;
        p.zlevel = zlevel;
        p.zstdLevel = zstdLevel;

        if (!ds->bands.empty() && ds->bands[0].hasNodata)
        {
            p.hasNodata = true;
            p.nodata = ds->bands[0].nodata;
            const Band &b0 = ds->bands[0];
            if (b0.nodataIsI64)
                p.nodataText = strPrintf("%lld", b0.nodataI64);
            else if (b0.nodataIsU64)
                p.nodataText = strPrintf("%llu", b0.nodataU64);
        }
        {
            bool haveFirst = p.hasNodata;
            double firstNd = p.nodata;
            int firstBand = 1;
            bool havePrev = haveFirst;
            double prevNd = firstNd;
            for (size_t bi = 1; bi < ds->bands.size(); ++bi)
            {
                const Band &b = ds->bands[bi];
                if (!b.hasNodata)
                    continue;
                if (haveFirst && b.nodata != firstNd &&
                    (!havePrev || b.nodata != prevNd))
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        strPrintf(
                            "%s, band %d: Setting nodata to %.17g on "
                            "band %d, but band %d has nodata at %.17g. "
                            "The TIFFTAG_GDAL_NODATA only support one "
                            "value per dataset. This value of %.17g "
                            "will be used for all bands on re-opening",
                            baseNameOf(output).c_str(), (int)bi + 1,
                            b.nodata, (int)bi + 1, firstBand, firstNd,
                            b.nodata));
                if (!haveFirst)
                {
                    haveFirst = true;
                    firstNd = b.nodata;
                    firstBand = (int)bi + 1;
                }
                havePrev = true;
                prevNd = b.nodata;
                p.hasNodata = true;
                p.nodata = b.nodata;
                if (b.nodataIsI64)
                    p.nodataText = strPrintf("%lld", b.nodataI64);
                else if (b.nodataIsU64)
                    p.nodataText = strPrintf("%llu", b.nodataU64);
                else
                    p.nodataText.clear();
            }
        }

        if (ds->hasGT)
        {
            if (ds->gt[2] != 0 || ds->gt[4] != 0 || ds->gt[5] >= 0)
            {
                p.hasXform = true;
                double *m = p.xform;
                m[0] = ds->gt[1];
                m[1] = ds->gt[2];
                m[2] = 0;
                m[3] = ds->gt[0];
                m[4] = ds->gt[4];
                m[5] = ds->gt[5];
                m[6] = 0;
                m[7] = ds->gt[3];
                m[8] = m[9] = m[10] = m[11] = 0;
                m[12] = m[13] = m[14] = 0;
                m[15] = 1;
            }
            else
            {
                p.hasGT = true;
                memcpy(p.gt, ds->gt, sizeof(p.gt));
            }
        }
        if (ds->hasSrs && ds->srs.valid())
            p.srs = &ds->srs;
        {
            const std::string *aop = ds->getMd("", "AREA_OR_POINT");
            p.pointPixel = aop && strEqualNoCase(*aop, "Point");
            if (p.pointPixel && !p.srs)
                p.forceGeoDir = true;
        }

        bool hasCt =
            !ds->bands.empty() && !ds->bands[0].colorTable.empty();
        if (hasCt)
            return notBuilt("palette sources");
        std::vector<uint16_t> extras;
        int photometric = 1;
        deriveConvertPhotometric(*ds, hasCt, photometric, extras);
        p.photometric = photometric;
        p.extrasSet = true;
        p.extraSamples = extras;
        p.useGmdItems = true;
        p.gmdItems = buildGmdItems(*ds, photometric, extras);

        // overview cascade: halve until both dimensions fit one tile
        std::vector<std::pair<int, int>> dims;
        {
            int w = p.width, h = p.height;
            while (w > blocksize || h > blocksize)
            {
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
                dims.emplace_back(w, h);
            }
        }
        if (!dims.empty())
        {
            GmdItem it;
            it.name = "OVERVIEW_RESAMPLING";
            it.domain = "IMAGE_STRUCTURE";
            it.value = ovrItem;
            p.gmdItems.push_back(it);
        }

        std::vector<std::vector<uint8_t>> pixels;
        TermProgress tp;
        bool tifBar = !quiet && !ds->suppressWriteBar();
        if (!ds->readAllBands(pixels, tifBar ? &tp : nullptr))
        {
            if (!append)
                remove(output.c_str());
            return 1;
        }
        p.pixels = &pixels;

        std::vector<std::vector<std::vector<double>>> ovrGrids;
        std::vector<std::vector<std::vector<uint8_t>>> ovrBytes;
        std::vector<CogOverview> ovrs;
        if (!dims.empty())
        {
            std::vector<std::vector<double>> base(ds->bands.size());
            bool ok = true;
            for (size_t b = 0; b < ds->bands.size() && ok; ++b)
                ok = ds->readBand((int)b + 1, base[b]);
            if (!ok)
            {
                if (!append)
                    remove(output.c_str());
                return 1;
            }
            ovrGrids.resize(dims.size());
            ovrBytes.resize(dims.size());
            const std::vector<std::vector<double>> *src = &base;
            int sw = p.width, sh = p.height;
            for (size_t li = 0; li < dims.size(); ++li)
            {
                cogResampleLevel(*src, sw, sh, ovrGrids[li],
                                 dims[li].first, dims[li].second,
                                 ovrMethod, p.type, p.hasNodata,
                                 p.nodata);
                src = &ovrGrids[li];
                sw = dims[li].first;
                sh = dims[li].second;
                const size_t sz = (size_t)dtypeSizeBytes(p.type);
                ovrBytes[li].assign(ds->bands.size(), {});
                for (size_t b = 0; b < ds->bands.size(); ++b)
                {
                    const std::vector<double> &g = ovrGrids[li][b];
                    std::vector<uint8_t> buf(g.size() * sz, 0);
                    for (size_t i = 0; i < g.size(); ++i)
                        rasterEncodeReal(p.type, buf.data() + i * sz,
                                         g[i], 0);
                    ovrBytes[li][b].swap(buf);
                }
                CogOverview ov;
                ov.w = dims[li].first;
                ov.h = dims[li].second;
                ov.pixels = &ovrBytes[li];
                ovrs.push_back(ov);
            }
        }

        if (overwrite && !append)
            remove((output + ".aux.xml").c_str());
        std::string werr;
        if (!cogWrite(output, p, ovrs, werr))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "convert: " + werr);
            return 1;
        }
        if (tifBar)
            tp.update(1.0);
        debugCloseDataset(*ds);
        if (cplDebugEnabled("GDAL"))
            cplDebug("GDAL", "GDALClose(" + output + ", this=" +
                                 cplDebugPtr() + ")");
        return 0;
    }

    // ---- GTiff ----
    cplDebug("GDAL", "Using GTiff driver");
    bool warpDbg = ds->warpDebugEmit(output);
    if (!warpDbg && ds->driverShort == "GTiff")
    {
        cplDebug("GTiff", "ScanDirectories()");
        ds->replayDeferred();
        if (!ds->isSubdataset && ds->overviews.empty())
            ds->debugOverviewScan();
    }
    ds->replayDeferred();
    if (ds->width <= 0 || ds->height <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("Attempt to create %dx%d dataset is illegal, "
                              "sizes must be larger than zero.",
                              ds->width, ds->height));
        if (g_pipelineMode && !quiet)
            printProgress();
        return 1;
    }
    if (int forcedNbits = ds->pansharpenNbits())
    {
        bool userNbits = false;
        for (const auto &c : cos)
            if (strEqualNoCase(c.first, "NBITS"))
                userNbits = true;
        if (!userNbits)
            cos.push_back({"NBITS", std::to_string(forcedNbits)});
    }
    CreationOptions o = parseCreationOptions(cos, output, "convert");
    if (o.fatal)
        return 1;

    GTiffCreateParams p;
    p.width = ds->width;
    p.height = ds->height;
    p.bands = (int)ds->bands.size();
    p.type = ds->bands.empty() ? DType::Byte : ds->bands[0].type;

    if (!finalizeCreationOptions(o, output, p.bands, p.type, true))
        return 1;
    if (output.compare(0, 4, "/vsi") != 0)
    {
        // the reference creates the tiff before copying pixels, so an
        // uncreatable path fails with the libtiff open error and no
        // progress output
        struct stat pst;
        bool existedBefore = stat(output.c_str(), &pst) == 0;
        FILE *probe = fopen(output.c_str(), "ab");
        if (!probe)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        strPrintf("Attempt to create new tiff file `%s' "
                                  "failed: %s: %s",
                                  output.c_str(), output.c_str(),
                                  strerror(errno)));
            if (g_pipelineMode && !quiet)
                printProgress();
            return 1;
        }
        fclose(probe);
        if (!existedBefore)
            remove(output.c_str());
    }
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
    p.sparse = o.sparse;
    p.profile = o.profile;
    p.bigEndian = o.endianBig;
    p.gtVersion = o.gtVersion;
    {
        const double rawSize = (double)p.width * p.height * p.bands *
                               dtypeSizeBytes(p.type);
        p.bigtiff = o.bigtiffMode == 1 ||
                    (o.bigtiffMode == 2 && o.compression == 1 &&
                     rawSize > 4200000000.0) ||
                    (o.bigtiffMode == 3 && rawSize > 2000000000.0);
    }
    p.append = append && exists;
    if (p.append)
    {
        cplPushQuietHandler();
        std::string aerr;
        auto probe = openRaster(output, aerr);
        cplPopHandler();
        if (!probe)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        outputExistsKind(output) + " '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
    }

    if (o.interleaveSet)
        p.bandInterleave = o.bandInterleave;
    else
    {
        // source BAND layout only survives into uncompressed outputs
        const std::string *il = ds->getMd("IMAGE_STRUCTURE", "INTERLEAVE");
        p.bandInterleave = il && *il == "BAND" && p.bands > 1 &&
                           p.compression == 1;
    }

    if (!ds->bands.empty() && ds->bands[0].hasNodata)
    {
        p.hasNodata = true;
        p.nodata = ds->bands[0].nodata;
        const Band &b0 = ds->bands[0];
        if (b0.nodataIsI64)
            p.nodataText = strPrintf("%lld", b0.nodataI64);
        else if (b0.nodataIsU64)
            p.nodataText = strPrintf("%llu", b0.nodataU64);
    }
    p.nodataLate = ds->warpProduced();

    if (o.hasNbits)
        p.nbits = o.nbitsFinal;
    else
    {
        const std::string *nb =
            ds->bands.empty()
                ? nullptr
                : ds->bands[0].getMd("IMAGE_STRUCTURE", "NBITS");
        if (nb)
            p.nbits = atoi(nb->c_str());
    }

    if (ds->hasGT || ds->demWriteDefaultGt)
    {
        if (ds->gt[2] != 0 || ds->gt[4] != 0 || ds->gt[5] >= 0)
        {
            p.hasXform = true;
            double *m = p.xform;
            m[0] = ds->gt[1];
            m[1] = ds->gt[2];
            m[2] = 0;
            m[3] = ds->gt[0];
            m[4] = ds->gt[4];
            m[5] = ds->gt[5];
            m[6] = 0;
            m[7] = ds->gt[3];
            m[8] = m[9] = m[10] = m[11] = 0;
            m[12] = m[13] = m[14] = 0;
            m[15] = 1;
        }
        else
        {
            p.hasGT = true;
            memcpy(p.gt, ds->gt, sizeof(p.gt));
        }
    }
    if (ds->hasSrs && ds->srs.valid())
        p.srs = &ds->srs;
    if (!ds->gcps.empty())
    {
        if (p.hasGT || p.hasXform)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        output + ": A geotransform previously set is going "
                                 "to be cleared due to the setting of GCPs.");
        else
        {
            p.gcps = &ds->gcps;
            if (ds->hasGcpSrs && ds->gcpSrs.valid())
                p.gcpSrs = &ds->gcpSrs;
        }
    }
    {
        const std::string *aop = ds->getMd("", "AREA_OR_POINT");
        p.pointPixel = aop && strEqualNoCase(*aop, "Point");
        if (p.pointPixel && !p.srs)
            p.forceGeoDir = true;
    }

    bool hasCt = !ds->bands.empty() && !ds->bands[0].colorTable.empty();
    std::vector<uint16_t> extras;
    int photometric = 1;
    int gmdPhot;
    if (o.resolvedPhot >= 0 || o.photOmit)
    {
        photometric = o.resolvedPhot >= 0 ? o.resolvedPhot : 1;
        p.omitPhotometric = o.photOmit;
        p.synthPalette = o.synthPalette && !hasCt;
        extras = o.extras;
        int channels = p.bands - (int)extras.size();
        for (size_t i = 0; i < extras.size(); i++)
            if (ds->bands[(size_t)channels + i].colorInterp == "Alpha")
                extras[i] = 2;
        if (o.photApplied)
            gmdPhot = photometric;
        else if (o.photometricVal == "RGB" && !o.photOmit)
        {
            // an RGB request ignored for band count leaves the interp
            // bookkeeping on the derivation an absent option would give
            std::vector<uint16_t> unused;
            deriveConvertPhotometric(*ds, hasCt, gmdPhot, unused);
        }
        else
            gmdPhot = 1;
    }
    else if (ds->warpProduced())
    {
        // Create-path derivation: the warp write only forces RGB when
        // the first three bands really are Red/Green/Blue (any type);
        // everything else stays MINISBLACK with unspecified extras
        bool rgb = !hasCt && ds->bands.size() >= 3 &&
                   ds->bands[0].colorInterp == "Red" &&
                   ds->bands[1].colorInterp == "Green" &&
                   ds->bands[2].colorInterp == "Blue";
        photometric = hasCt ? 3 : rgb ? 2 : 1;
        int colorChannels = photometric == 2 ? 3 : 1;
        for (int i = colorChannels; i < p.bands; i++)
            extras.push_back(
                ds->bands[(size_t)i].colorInterp == "Alpha" ||
                        photometric == 3
                    ? 2
                    : 0);
        gmdPhot = photometric;
    }
    else if (ds->pansharpenProduced())
    {
        bool byteRgb = !hasCt && !ds->bands.empty() &&
                       ds->bands[0].type == DType::Byte &&
                       (p.bands == 3 || p.bands == 4);
        photometric = byteRgb ? 2 : 1;
        int colorChannels = photometric == 2 ? 3 : 1;
        for (int i = colorChannels; i < p.bands; i++)
            extras.push_back(photometric == 2 ? 2 : 0);
        gmdPhot = photometric;
    }
    else
    {
        deriveConvertPhotometric(*ds, hasCt, photometric, extras);
        gmdPhot = photometric;
    }
    p.photometric = photometric;
    p.extrasSet = true;
    p.extraSamples = extras;
    if (hasCt)
        p.colorTable = &ds->bands[0].colorTable;

    p.useGmdItems = true;
    p.gmdItems = buildGmdItems(*ds, gmdPhot, extras);

    // recognized TIFFTAG_* metadata, in sorted order
    {
        auto it = ds->metadata.find("");
        if (it != ds->metadata.end())
        {
            MetaDomain special;
            for (const auto &kv : it->second)
                if (isSpecialTiffTagItem(kv.first))
                    special.push_back(kv);
            sortItems(special);
            for (const auto &kv : special)
            {
                int tag = asciiTiffTag(kv.first);
                if (tag >= 0)
                    p.asciiTags.push_back({(uint16_t)tag, kv.second});
                else if (strEqualNoCase(kv.first, "TIFFTAG_XRESOLUTION"))
                {
                    p.hasXRes = true;
                    p.xres = (float)atof(kv.second.c_str());
                }
                else if (strEqualNoCase(kv.first, "TIFFTAG_YRESOLUTION"))
                {
                    p.hasYRes = true;
                    p.yres = (float)atof(kv.second.c_str());
                }
                else if (strEqualNoCase(kv.first,
                                        "TIFFTAG_RESOLUTIONUNIT"))
                    p.resUnit = atoi(kv.second.c_str());
                else if (strEqualNoCase(kv.first,
                                        "TIFFTAG_MINSAMPLEVALUE"))
                    p.minSample = atoi(kv.second.c_str());
                else if (strEqualNoCase(kv.first,
                                        "TIFFTAG_MAXSAMPLEVALUE"))
                    p.maxSample = atoi(kv.second.c_str());
            }
        }
    }

    if (strncmp(output.c_str(), "/vsizip/", 8) == 0 ||
        strncmp(output.c_str(), "/vsitar/", 8) == 0)
    {
        // creation fails before any pixels are read or progress shown
        std::string werr;
        gtiffWrite(output, p, werr);
        return 1;
    }

    {
        // the GDAL_NODATA ascii tag is dataset-wide: every band rewrites
        // it, and a band warns (at creation, before pixels move) when
        // its value differs from both the first band's and the previous
        // band's
        bool haveFirst = p.hasNodata;
        double firstNd = p.nodata;
        int firstBand = 1;
        bool havePrev = haveFirst;
        double prevNd = firstNd;
        for (size_t bi = 1; bi < ds->bands.size(); ++bi)
        {
            const Band &b = ds->bands[bi];
            if (!b.hasNodata)
                continue;
            // warp datasets already warned in creation order during
            // warpDebugEmit
            if (!warpDbg && haveFirst && b.nodata != firstNd &&
                (!havePrev || b.nodata != prevNd))
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("%s, band %d: Setting nodata to %.17g on "
                              "band %d, but band %d has nodata at %.17g. "
                              "The TIFFTAG_GDAL_NODATA only support one "
                              "value per dataset. This value of %.17g "
                              "will be used for all bands on re-opening",
                              baseNameOf(output).c_str(), (int)bi + 1,
                              b.nodata, (int)bi + 1, firstBand, firstNd,
                              b.nodata));
            if (!haveFirst)
            {
                haveFirst = true;
                firstNd = b.nodata;
                firstBand = (int)bi + 1;
            }
            havePrev = true;
            prevNd = b.nodata;
            p.hasNodata = true;
            p.nodata = b.nodata;
            if (b.nodataIsI64)
                p.nodataText = strPrintf("%lld", b.nodataI64);
            else if (b.nodataIsU64)
                p.nodataText = strPrintf("%llu", b.nodataU64);
            else
                p.nodataText.clear();
        }
    }

    p.jpegQuality = o.jpegQuality;
    p.jpegTablesMode = o.jpegTablesMode;
    p.webpLevel = o.webpLevel;
    p.webpLossless = o.webpLossless;
    if (p.compression == 7)
    {
        if (photometric == 6 && p.bandInterleave)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        baseNameOf(output) +
                            ": PHOTOMETRIC=YCBCR requires INTERLEAVE=PIXEL");
            return 1;
        }
        if (photometric == 3)
        {
            // the in-memory JPEGTables dataset only sees an explicit
            // PHOTOMETRIC option; a palette synthesized from a
            // MINISWHITE source leaves it on MINISBLACK, so the tables
            // get written and only the driver-level check reports
            const std::string *mw =
                ds->getMd("IMAGE_STRUCTURE", "MINISWHITE");
            bool synthMw = !o.photApplied && mw && *mw == "YES";
            if (!synthMw)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "JPEGSetupEncode:PhotometricInterpretation 3 "
                            "not allowed for JPEG");
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        baseNameOf(output) +
                            ": JPEG compression not supported with "
                            "paletted image");
            p.jpegStub = true;
            p.jpegStubTables = synthMw;
            p.jpegStubNoStrips = true;
            p.gmdItems.clear();
            p.useGmdItems = false;
            std::string werr;
            gtiffWrite(output, p, werr);
            return 1;
        }
    }

    gdalDebugCacheMaxOnce();
    if (!warpDbg && cplDebugEnabled("GDAL"))
    {
        // GDALDatasetCopyWholeRaster swath report, from the destination
        // block geometry (strip height replicating TIFFDefaultStripSize)
        bool bi = !p.bandInterleave && p.bands > 1;
        int bpp = dtypeSizeBytes(p.type);
        // the destination strip geometry sees the packed 16-bit samples,
        // while the copy swath still moves Float32 pixels
        int fileBpp = o.halfFloat ? 2 : bpp;
        int blockY;
        if (p.tiled)
            blockY = p.blockY > 0 ? p.blockY : 256;
        else if (p.blockY > 0)
            blockY = p.blockY;
        else
        {
            long scanline =
                p.nbits > 0
                    ? (long)(((int64_t)p.width *
                                  (p.bandInterleave ? 1 : p.bands) *
                                  p.nbits +
                              7) /
                             8)
                    : (long)p.width * fileBpp *
                          (p.bandInterleave ? 1 : p.bands);
            if (p.compression == 7 && photometric == 6)
                scanline = (long)((p.width + 1) & ~1) * 3 / 2;
            blockY = scanline > 0 ? (int)(8192 / scanline) : 1;
            if (blockY < 1)
                blockY = 1;
            if (p.compression == 7)
                blockY = (blockY + 15) / 16 * 16;
        }
        if (blockY > p.height)
            blockY = p.height;
        long long target = gdalDefaultCacheMax() / 4;
        if (target > 2147483647LL)
            target = 2147483647LL;
        if (target < 10 * 1024 * 1024)
            target = 10 * 1024 * 1024;
        long long pixSize = (long long)bpp * (bi ? p.bands : 1);
        long long lines = target / (pixSize * p.width);
        if (lines < 1)
            lines = 1;
        if (lines > p.height)
            lines = p.height;
        if (blockY > 0 && lines > blockY && lines % blockY != 0)
            lines = lines / blockY * blockY;
        cplDebug("GDAL",
                 strPrintf("GDALDatasetCopyWholeRaster(): %d*%lld swaths, "
                           "bInterleave=%d",
                           p.width, lines, bi ? 1 : 0));
    }

    if (p.compression == 7)
    {
        const int bpp = dtypeSizeBytes(o.halfFloat ? DType::Float16
                                                   : p.type);
        const int bps = o.nbitsFinal > 0 ? o.nbitsFinal : bpp * 8;
        const int mult = photometric == 6 ? 16 : 8;
        int rps;
        if (p.tiled)
            rps = 0;
        else if (p.blockY > 0)
            rps = p.blockY;
        else
        {
            long scanline =
                p.nbits > 0
                    ? (long)(((int64_t)p.width *
                                  (p.bandInterleave ? 1 : p.bands) *
                                  p.nbits +
                              7) /
                             8)
                    : (long)p.width * bpp *
                          (p.bandInterleave ? 1 : p.bands);
            if (photometric == 6)
                scanline = (long)((p.width + 1) & ~1) * 3 / 2;
            rps = scanline > 0 ? (int)(8192 / scanline) : 1;
            if (rps < 1)
                rps = 1;
            rps = (rps + 15) / 16 * 16;
        }
        if (rps > p.height)
            rps = p.height;
        auto bpsError = [&]()
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JPEGSetupEncode:BitsPerSample %d not "
                                  "allowed for JPEG",
                                  bps));
        };
        auto rpsError = [&mult]()
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JPEGSetupEncode:RowsPerStrip must be "
                                  "multiple of %d for JPEG",
                                  mult));
        };
        if (bps != 8)
        {
            if (bps > 16)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("gtiffdataset_jpg_tmp: NBITS=%d is invalid "
                              "for data type UInt16. Using NBITS=16",
                              bps));
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("JPEGSetupEncode:BitsPerSample %d not "
                                  "allowed for JPEG",
                                  bps > 16 ? 16 : bps));
            if (o.nbitsFinal > 0 || o.halfFloat)
            {
                // buffered writes let the copy loop finish; the encode
                // failures surface at flush time and leave the file
                std::vector<std::vector<uint8_t>> px;
                TermProgress tpn;
                if (!ds->readAllBands(px, quiet ? nullptr : &tpn))
                {
                    if (!p.append)
                        remove(output.c_str());
                    return 1;
                }
                if (!quiet)
                    tpn.update(1.0);
                if (o.nbitsFinal > 0)
                {
                    const uint32_t maxv = (1u << o.nbitsFinal) - 1;
                    bool clip = false;
                    for (const auto &buf : px)
                    {
                        if (p.type == DType::Byte ||
                            p.type == DType::Int8)
                        {
                            for (uint8_t v : buf)
                                if (v > maxv)
                                {
                                    clip = true;
                                    break;
                                }
                        }
                        else if (p.type == DType::UInt16 ||
                                 p.type == DType::Int16)
                        {
                            for (size_t i = 0; i + 1 < buf.size();
                                 i += 2)
                            {
                                uint16_t v;
                                memcpy(&v, &buf[i], 2);
                                if (v > maxv)
                                {
                                    clip = true;
                                    break;
                                }
                            }
                        }
                        if (clip)
                            break;
                    }
                    if (clip)
                        cplErrorStr(
                            CE_Warning, CPLE_AppDefined,
                            strPrintf("%s, band 1: One or more pixels "
                                      "clipped to fit %d bit domain.",
                                      baseNameOf(output).c_str(),
                                      o.nbitsFinal));
                }
                bpsError();
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            baseNameOf(output) +
                                ": WriteEncodedTile/Strip() failed.");
                bpsError();
                p.jpegStub = true;
                std::string werr;
                gtiffWrite(output, p, werr);
                return 1;
            }
            if (!quiet)
            {
                TermProgress tpn;
                tpn.update(0.0);
                tpn.update(rps > 0 && p.height % rps == 0 ? 0.5 : 0.25);
            }
            bpsError();
            bpsError();
            return 1;
        }
        if (!p.tiled && rps < p.height && rps % mult)
        {
            if (!quiet)
            {
                TermProgress tpn;
                tpn.update(0.0);
                tpn.update(p.height % rps == 0 ? 0.5 : 0.25);
            }
            rpsError();
            rpsError();
            return 1;
        }
    }

    if (p.compression == 50001)
    {
        const int bpp = dtypeSizeBytes(o.halfFloat ? DType::Float16
                                                   : p.type);
        const int effNbits = o.nbitsFinal > 0 ? o.nbitsFinal : p.nbits;
        const int bps = effNbits > 0 ? effNbits : bpp * 8;
        int rps;
        if (p.tiled)
            rps = p.blockY > 0 ? p.blockY : 256;
        else if (p.blockY > 0)
            rps = p.blockY;
        else
        {
            long scanline = (long)p.width * bpp *
                            (p.bandInterleave ? 1 : p.bands);
            rps = scanline > 0 ? (int)(8192 / scanline) : 1;
            if (rps < 1)
                rps = 1;
        }
        if (rps > p.height)
            rps = p.height;
        if (p.bandInterleave && p.bands > 1)
        {
            // FixupTags rejection fires on both directory re-reads but
            // the write still proceeds with separate planes
            for (int i = 0; i < 2; i++)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "TWebPFixupTags:TIFF WEBP requires data to "
                            "be stored contiguously in RGB e.g. "
                            "RGBRGBRGB or RGBARGBARGBA");
        }
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
            auto setupError = [&setupMsg]()
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined, setupMsg);
            };
            if (effNbits > 0 || o.halfFloat)
            {
                // buffered writes let the copy loop finish; the encode
                // failures surface at flush time and leave the file
                std::vector<std::vector<uint8_t>> px;
                TermProgress tpn;
                if (!ds->readAllBands(px, quiet ? nullptr : &tpn))
                {
                    if (!p.append)
                        remove(output.c_str());
                    return 1;
                }
                if (!quiet)
                    tpn.update(1.0);
                if (o.nbitsFinal > 0)
                {
                    const uint32_t maxv = (1u << o.nbitsFinal) - 1;
                    bool clip = false;
                    for (const auto &buf : px)
                    {
                        if (p.type == DType::Byte ||
                            p.type == DType::Int8)
                        {
                            for (uint8_t v : buf)
                                if (v > maxv)
                                {
                                    clip = true;
                                    break;
                                }
                        }
                        else if (p.type == DType::UInt16 ||
                                 p.type == DType::Int16)
                        {
                            for (size_t i = 0; i + 1 < buf.size();
                                 i += 2)
                            {
                                uint16_t v;
                                memcpy(&v, &buf[i], 2);
                                if (v > maxv)
                                {
                                    clip = true;
                                    break;
                                }
                            }
                        }
                        if (clip)
                            break;
                    }
                    if (clip)
                        cplErrorStr(
                            CE_Warning, CPLE_AppDefined,
                            strPrintf("%s, band 1: One or more pixels "
                                      "clipped to fit %d bit domain.",
                                      baseNameOf(output).c_str(),
                                      o.nbitsFinal));
                }
                setupError();
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            baseNameOf(output) +
                                ": WriteEncodedTile/Strip() failed.");
                setupError();
                p.jpegStub = true;
                std::string werr;
                gtiffWrite(output, p, werr);
                return 1;
            }
            if (!quiet)
            {
                TermProgress tpn;
                tpn.update(0.0);
                tpn.update(rps > 0 && p.height % rps == 0 ? 0.5 : 0.25);
            }
            setupError();
            setupError();
            return 1;
        }
    }

    // pixel data; copy-style progress ticks are emitted while reading
    std::vector<std::vector<uint8_t>> pixels;
    TermProgress tp;
    bool tifBar = !quiet && !ds->suppressWriteBar();
    if (!ds->readAllBands(pixels, tifBar ? &tp : nullptr))
    {
        if (!p.append)
            remove(output.c_str());
        return 1;
    }
    // TIFF cannot mix band types: everything lands as band 1's type,
    // with the reference's CreateCopy warning
    {
        bool mixed = false;
        for (const Band &b : ds->bands)
            if (b.type != p.type)
                mixed = true;
        if (mixed)
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        baseNameOf(output) +
                            ": Unable to export GeoTIFF file with "
                            "different datatypes per different bands. "
                            "All bands should have the same types in "
                            "TIFF.");
            size_t sz = (size_t)dtypeSizeBytes(p.type);
            for (size_t bi = 0; bi < ds->bands.size(); ++bi)
            {
                if (ds->bands[bi].type == p.type)
                    continue;
                std::vector<double> vals;
                if (!ds->readBand((int)bi + 1, vals))
                    continue;
                std::vector<uint8_t> buf(vals.size() * sz, 0);
                for (size_t i = 0; i < vals.size(); ++i)
                    rasterEncodeReal(p.type, buf.data() + i * sz,
                                     rasterFinishReal(vals[i], p.type), 0);
                pixels[bi].swap(buf);
            }
        }
    }
    if (o.halfFloat)
    {
        for (auto &buf : pixels)
        {
            std::vector<uint8_t> half(buf.size() / 2);
            for (size_t i = 0; i * 4 + 3 < buf.size(); i++)
            {
                float f;
                memcpy(&f, &buf[i * 4], 4);
                uint16_t h = halfRoundFromFloat(f);
                memcpy(&half[i * 2], &h, 2);
            }
            buf.swap(half);
        }
        p.type = DType::Float16;
    }
    p.pixels = &pixels;
    p.geoDoubleOrphan = ds->geoDoubleOrphanHint();
    std::vector<uint8_t> maskPx;
    if (ds->selectMaskBand(maskPx))
        p.maskPixels = &maskPx;

    if (overwrite && !p.append)
        remove((output + ".aux.xml").c_str());

    bool clipped = false;
    p.clipNote = &clipped;
    std::string werr;
    if (!gtiffWrite(output, p, werr))
    {
        if (werr != "reported")
            cplErrorStr(CE_Failure, CPLE_AppDefined, "convert: " + werr);
        return 1;
    }
    {
        // interps that are standard for the photometric layout but not
        // representable in the file (gray on an extra sample) land in PAM
        bool anyStd = false;
        for (const auto &it : p.gmdItems)
            if (it.role == "colorinterp")
                anyStd = true;
        if (!anyStd && (photometric == 1 || photometric == 3))
        {
            std::string pam;
            for (size_t bi = 1; bi < ds->bands.size(); ++bi)
            {
                const Band &b = ds->bands[bi];
                if (photometric == 1 && b.colorInterp == "Gray")
                    pam += strPrintf("  <PAMRasterBand band=\"%d\">\n"
                                     "    <ColorInterp>Gray</ColorInterp>"
                                     "\n  </PAMRasterBand>\n",
                                     (int)bi + 1);
                else if (photometric == 3 && b.colorInterp == "Palette")
                {
                    pam += strPrintf("  <PAMRasterBand band=\"%d\">\n"
                                     "    <ColorInterp>Palette"
                                     "</ColorInterp>\n",
                                     (int)bi + 1);
                    pam += "    <ColorTable>\n";
                    for (const auto &ce : b.colorTable)
                        pam += strPrintf("      <Entry c1=\"%d\" "
                                         "c2=\"%d\" c3=\"%d\" "
                                         "c4=\"%d\" />\n",
                                         ce.c1, ce.c2, ce.c3, ce.c4);
                    pam += "    </ColorTable>\n  </PAMRasterBand>\n";
                }
            }
            if (!pam.empty() && gdalPamEnabled())
                writeStringToFile(output + ".aux.xml",
                                  "<PAMDataset>\n" + pam +
                                      "</PAMDataset>\n");
        }
    }
    if (tifBar)
        tp.update(1.0);
    debugCloseDataset(*ds);
    if (cplDebugEnabled("GDAL"))
        cplDebug("GDAL",
                 "GDALClose(" + output + ", this=" + cplDebugPtr() + ")");
    return clipped ? 1 : 0;
}

}  // namespace

int rasterConvertHandlerEntry(const CmdSpec &spec, ParseResult &r)
{
    return rasterConvertHandler(spec, r);
}

int rasterConvertWriteOutput(
    std::unique_ptr<RasterDatasetBase> &ds, ParseResult &r,
    const std::string &input, const std::string &output, bool quiet,
    bool overwrite, bool append, const std::string &drv,
    const std::string &gdalgExtra,
    const std::function<int(std::unique_ptr<RasterDatasetBase> &)>
        &materialize,
    const PreWriteValidate &preWriteValidate)
{
    return convertWriteOutputImpl(ds, r, input, output, quiet, overwrite,
                                  append, drv, gdalgExtra, materialize,
                                  preWriteValidate);
}

namespace
{
// GDALAlgorithm validates --of/--if at parse time, before opening inputs
int rasterConvertPreValidator(const CmdSpec &, ParseResult &r)
{
    std::string format = r.str("output-format");
    std::string drv;
    std::string issue = rasterOutFormatIssue(format, drv);
    if (!issue.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    convertMsgPrefix() + ": " + issue);
        handlerPrintUsage();
        return 1;
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(false, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        convertMsgPrefix() + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}
}  // namespace

void registerRasterConvertHandler()
{
    registerHandler("raster_convert", rasterConvertHandler);
    registerHandler("convert", rasterConvertHandler);
    registerPreValidator("raster_convert", rasterConvertPreValidator);
}

std::string convertDispatchProbe(const std::string &path)
{
    return convertDispatchProbeImpl(path);
}
