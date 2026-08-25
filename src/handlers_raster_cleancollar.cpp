#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "gtiff_write.h"
#include "util.h"

#include <sys/stat.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

std::vector<uint8_t> gtiffDeflateBlock(const std::vector<uint8_t> &in,
                                       int level);

namespace
{

uint8_t ccClampByte(double v)
{
    if (std::isnan(v))
        return 0;
    v = std::floor(v + 0.5);
    if (v <= 0)
        return 0;
    if (v >= 255)
        return 255;
    return (uint8_t)v;
}

// "black" / "white" / comma tuple of integers (empty fields dropped);
// anything else is a parse error
bool ccParseColor(const std::string &s, std::vector<int> *out, int nbands)
{
    if (s == "black")
    {
        if (out)
            out->assign((size_t)(nbands > 0 ? nbands : 1), 0);
        return true;
    }
    if (s == "white")
    {
        if (out)
            out->assign((size_t)(nbands > 0 ? nbands : 1), 255);
        return true;
    }
    if (out)
        out->clear();
    size_t pos = 0;
    while (pos <= s.size())
    {
        size_t comma = s.find(',', pos);
        std::string tok = s.substr(
            pos, comma == std::string::npos ? std::string::npos
                                            : comma - pos);
        if (!tok.empty())
        {
            size_t i = 0;
            if (tok[0] == '+' || tok[0] == '-')
                i = 1;
            if (i >= tok.size())
                return false;
            for (; i < tok.size(); i++)
                if (tok[i] < '0' || tok[i] > '9')
                    return false;
            if (out)
                out->push_back(atoi(tok.c_str()));
        }
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return true;
}

// ---- the two-pass automaton (model v8) + border floodfill ----
void ccRunAlgorithm(std::vector<std::vector<uint8_t>> &bands, int w, int h,
                    const std::vector<std::vector<int>> &colors, int thr,
                    int nmax, bool flood, uint8_t rep,
                    std::vector<uint8_t> &eaten,
                    std::vector<uint8_t> *floodRegion)
{
    const int nb = (int)bands.size();
    eaten.assign((size_t)w * h, 0);

    auto rawNear = [&](const uint8_t *vals, int stride)
    {
        for (const auto &c : colors)
        {
            bool ok = true;
            for (int b = 0; b < nb && ok; b++)
                if (std::abs((int)vals[(size_t)b * stride] - c[b]) > thr)
                    ok = false;
            if (ok)
                return true;
        }
        return false;
    };
    std::vector<uint8_t> repVec(nb, rep);
    const bool repNear = rawNear(repVec.data(), 1);

    // scans test the values a row had when its processing started in the
    // current pass; pixels replaced earlier in the same row stay near
    // only when the replacement value itself is near a color
    std::vector<uint8_t> snap((size_t)nb * w);
    auto near = [&](size_t row, int x)
    {
        if (rawNear(&snap[x], w))
            return true;
        return eaten[row + x] && repNear;
    };
    auto liveNear = [&](size_t idx)
    {
        for (const auto &c : colors)
        {
            bool ok = true;
            for (int b = 0; b < nb && ok; b++)
                if (std::abs((int)bands[b][idx] - c[b]) > thr)
                    ok = false;
            if (ok)
                return true;
        }
        return false;
    };
    auto eat = [&](size_t idx)
    {
        for (int b = 0; b < nb; b++)
            bands[b][idx] = rep;
        eaten[idx] = 1;
    };

    // two passes (top-down then bottom-up), per row: vertical step over
    // all columns, then a left and a right horizontal scan; per-column
    // state (sv activated by a leading near run, cv counting non-near
    // pixels) persists through a pass, horizontal state is per scan
    auto doPass = [&](bool topDown, bool pass2)
    {
        std::vector<int> cv(w, 0);
        std::vector<uint8_t> sv(w, 0);
        auto hscan = [&](size_t row, int start, int end, int step)
        {
            int ch = 0;
            bool sh = false, dead = false;
            for (int i = start; i != end; i += step)
            {
                if (near(row, i))
                {
                    if (!sh && sv[i] && cv[i] == 0)
                    {
                        sh = true;
                        dead = false;
                        ch = 0;
                        eat(row + i);
                    }
                    else if (sh)
                        eat(row + i);
                    else if (!dead && ch == 0)
                    {
                        sh = true;
                        eat(row + i);
                    }
                }
                else
                {
                    ch++;
                    if (sh)
                    {
                        if (pass2)
                            sh = false;
                        else if (ch > nmax)
                        {
                            sh = false;
                            dead = true;
                        }
                        else
                            eat(row + i);
                    }
                }
            }
        };
        for (int yi = 0; yi < h; yi++)
        {
            int y = topDown ? yi : h - 1 - yi;
            size_t row = (size_t)y * w;
            for (int b = 0; b < nb; b++)
                for (int x = 0; x < w; x++)
                    snap[(size_t)b * w + x] = bands[b][row + x];
            for (int x = 0; x < w; x++)
            {
                if (near(row, x))
                {
                    if (sv[x] && cv[x] <= nmax)
                        eat(row + x);
                    else if (!sv[x] && cv[x] == 0)
                    {
                        sv[x] = 1;
                        eat(row + x);
                    }
                }
                else
                {
                    cv[x]++;
                    if (sv[x] && cv[x] <= nmax)
                        eat(row + x);
                }
            }
            if (w < 2)
                continue;
            hscan(row, 0, w - 1, 1);
            hscan(row, w - 1, 0, -1);
        }
    };
    doPass(true, false);
    doPass(false, true);

    if (flood)
    {
        std::vector<uint8_t> seen((size_t)w * h, 0);
        std::deque<size_t> q;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                if (y != 0 && y != h - 1 && x != 0 && x != w - 1)
                    continue;
                size_t idx = (size_t)y * w + x;
                if (liveNear(idx))
                {
                    q.push_back(idx);
                    seen[idx] = 1;
                }
            }
        while (!q.empty())
        {
            size_t idx = q.front();
            q.pop_front();
            eaten[idx] = 1;
            int y = (int)(idx / w), x = (int)(idx % w);
            const int dy[4] = {1, -1, 0, 0};
            const int dx[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++)
            {
                int ny = y + dy[k], nx = x + dx[k];
                if (ny < 0 || ny >= h || nx < 0 || nx >= w)
                    continue;
                size_t ni = (size_t)ny * w + nx;
                if (!seen[ni] && liveNear(ni))
                {
                    seen[ni] = 1;
                    q.push_back(ni);
                }
            }
        }
        // only the flood region is zeroed; scan replacements outside it
        // keep the replacement value
        for (size_t i = 0; i < seen.size(); i++)
            if (seen[i])
                for (int b = 0; b < nb; b++)
                    bands[b][i] = 0;
        if (floodRegion)
            *floodRegion = std::move(seen);
    }
}

// ---- minimal classic-LE TIFF pixel patcher for --update ----
struct CcTiffInfo
{
    int width = 0, height = 0, spp = 1, rps = 0;
    std::vector<int> bps;
    std::vector<int> sfmt;
    int compression = 1, planar = 1;
    std::vector<uint32_t> stripOff, stripCnt;
    size_t ifd0Off = 0;
    size_t nextPtrPos = 0;
    bool ok = false;
};

CcTiffInfo ccParseTiff(const std::string &d)
{
    CcTiffInfo t;
    if (d.size() < 8 || d[0] != 'I' || d[1] != 'I' || (uint8_t)d[2] != 42)
        return t;
    auto rd16 = [&](size_t o)
    { return (uint32_t)(uint8_t)d[o] | ((uint32_t)(uint8_t)d[o + 1] << 8); };
    auto rd32 = [&](size_t o)
    {
        return (uint32_t)(uint8_t)d[o] | ((uint32_t)(uint8_t)d[o + 1] << 8) |
               ((uint32_t)(uint8_t)d[o + 2] << 16) |
               ((uint32_t)(uint8_t)d[o + 3] << 24);
    };
    size_t off = rd32(4);
    if (off + 2 > d.size())
        return t;
    t.ifd0Off = off;
    uint32_t n = rd16(off);
    if (off + 2 + n * 12 + 4 > d.size())
        return t;
    t.nextPtrPos = off + 2 + (size_t)n * 12;
    for (uint32_t i = 0; i < n; i++)
    {
        size_t e = off + 2 + (size_t)i * 12;
        uint32_t tag = rd16(e), ty = rd16(e + 2), cnt = rd32(e + 4);
        uint32_t val = rd32(e + 8);
        auto arr = [&](std::vector<uint32_t> &out)
        {
            out.clear();
            int es = ty == 3 ? 2 : 4;
            size_t src = cnt * es <= 4 ? e + 8 : val;
            for (uint32_t k = 0; k < cnt; k++)
                out.push_back(ty == 3 ? rd16(src + k * 2)
                                      : rd32(src + k * 4));
        };
        std::vector<uint32_t> tmp;
        switch (tag)
        {
            case 256: t.width = (int)val; break;
            case 257: t.height = (int)val; break;
            case 258:
                arr(tmp);
                t.bps.assign(tmp.begin(), tmp.end());
                break;
            case 259: t.compression = (int)val; break;
            case 273: arr(t.stripOff); break;
            case 277: t.spp = (int)val; break;
            case 278: t.rps = (int)val; break;
            case 279: arr(t.stripCnt); break;
            case 284: t.planar = (int)val; break;
            case 339:
                arr(tmp);
                t.sfmt.assign(tmp.begin(), tmp.end());
                break;
            default: break;
        }
    }
    if (t.rps == 0)
        t.rps = t.height;
    if (t.bps.empty())
        t.bps.assign((size_t)t.spp, 1);
    while ((int)t.bps.size() < t.spp)
        t.bps.push_back(t.bps[0]);
    while ((int)t.sfmt.size() < t.spp)
        t.sfmt.push_back(t.sfmt.empty() ? 1 : t.sfmt[0]);
    t.ok = t.width > 0 && t.height > 0 && t.compression == 1 &&
           t.planar == 1 && !t.stripOff.empty();
    return t;
}

void ccStoreSample(std::string &d, size_t off, int bps, int sfmt,
                   uint8_t v)
{
    switch (bps)
    {
        case 8:
            d[off] = (char)v;
            break;
        case 16:
        {
            uint16_t x = v;
            memcpy(&d[off], &x, 2);
            break;
        }
        case 32:
            if (sfmt == 3)
            {
                float f = v;
                memcpy(&d[off], &f, 4);
            }
            else
            {
                uint32_t x = v;
                memcpy(&d[off], &x, 4);
            }
            break;
        case 64:
            if (sfmt == 3)
            {
                double f = v;
                memcpy(&d[off], &f, 8);
            }
            else
            {
                uint64_t x = v;
                memcpy(&d[off], &x, 8);
            }
            break;
        default:
            break;
    }
}

// deflate 1-bit mask sub-IFD appended at EOF for --update --add-mask
void ccAppendMaskIfd(std::string &d, const CcTiffInfo &t,
                     const std::vector<uint8_t> &maskBits)
{
    if (d.size() & 1)
        d += '\0';
    uint32_t base = (uint32_t)d.size();
    const int w = t.width, h = t.height;
    // the mask block height follows the default strip rule applied to
    // the main band scanline, clamped to the image height
    int mainRowBytes = 0;
    for (int b = 0; b < t.spp; b++)
        mainRowBytes += t.bps[b] / 8;
    mainRowBytes *= w;
    int rps = mainRowBytes > 0 ? 8192 / mainRowBytes : h;
    if (rps < 1)
        rps = 1;
    if (rps > h)
        rps = h;
    const int rowBytes = (w + 7) / 8;
    const int nStrips = (h + rps - 1) / rps;
    std::vector<std::vector<uint8_t>> strips;
    uint32_t maxCnt = 0;
    for (int si = 0; si < nStrips; si++)
    {
        int y0 = si * rps;
        int rows = y0 + rps > h ? h - y0 : rps;
        std::vector<uint8_t> raw((size_t)rows * rowBytes, 0);
        for (int r = 0; r < rows; r++)
            for (int x = 0; x < w; x++)
                if (maskBits[(size_t)(y0 + r) * w + x])
                    raw[(size_t)r * rowBytes + x / 8] |=
                        (uint8_t)(0x80 >> (x & 7));
        strips.push_back(gtiffDeflateBlock(raw, 6));
        if (strips.back().size() > maxCnt)
            maxCnt = (uint32_t)strips.back().size();
    }
    const size_t fullRaw = (size_t)rowBytes * rps;
    const bool cntShort = nStrips > 1 && fullRaw + fullRaw / 4 + 2 <= 8192;

    struct Ent
    {
        uint16_t tag, type;
        uint32_t count;
        std::vector<uint8_t> data;
    };
    auto put16v = [](std::vector<uint8_t> &v, uint16_t x)
    {
        v.push_back(x & 0xff);
        v.push_back(x >> 8);
    };
    auto put32v = [](std::vector<uint8_t> &v, uint32_t x)
    {
        for (int i = 0; i < 4; i++)
            v.push_back((x >> (8 * i)) & 0xff);
    };
    auto mkS = [&](uint16_t tag, uint16_t x)
    {
        Ent e{tag, 3, 1, {}};
        put16v(e.data, x);
        return e;
    };
    auto mkSL = [&](uint16_t tag, uint32_t x)
    {
        if (x <= 0xffff)
            return mkS(tag, (uint16_t)x);
        Ent e{tag, 4, 1, {}};
        put32v(e.data, x);
        return e;
    };
    auto mkL = [&](uint16_t tag, const std::vector<uint32_t> &vals)
    {
        Ent e{tag, 4, (uint32_t)vals.size(), {}};
        for (uint32_t v : vals)
            put32v(e.data, v);
        return e;
    };
    std::vector<Ent> ents;
    ents.push_back(mkL(254, {4}));
    ents.push_back(mkSL(256, (uint32_t)w));
    ents.push_back(mkSL(257, (uint32_t)h));
    ents.push_back(mkS(258, 1));
    ents.push_back(mkS(259, 8));
    ents.push_back(mkS(262, 4));
    ents.push_back(mkS(277, 1));
    ents.push_back(mkSL(278, (uint32_t)rps));
    if (cntShort)
    {
        Ent e{279, 3, (uint32_t)nStrips, {}};
        for (const auto &s : strips)
            put16v(e.data, (uint16_t)s.size());
        ents.push_back(std::move(e));
    }
    else
    {
        std::vector<uint32_t> cnts;
        for (const auto &s : strips)
            cnts.push_back((uint32_t)s.size());
        ents.push_back(mkL(279, cnts));
    }
    size_t offIdx = ents.size();
    ents.push_back(mkL(273, std::vector<uint32_t>(nStrips, 0)));
    ents.push_back(mkS(284, 1));
    ents.push_back(mkS(317, 1));
    ents.push_back(mkS(339, 1));

    // layout: entries, payloads (>4 bytes), strip data
    uint32_t cur = base + 2 + (uint32_t)ents.size() * 12 + 4;
    std::vector<uint32_t> dataOff(ents.size(), 0);
    for (size_t i = 0; i < ents.size(); i++)
        if (ents[i].data.size() > 4)
        {
            if (cur & 1)
                ++cur;
            dataOff[i] = cur;
            cur += (uint32_t)ents[i].data.size();
        }
    {
        Ent &e = ents[offIdx];
        e.data.clear();
        for (const auto &s : strips)
        {
            put32v(e.data, cur);
            cur += (uint32_t)s.size();
        }
    }
    std::vector<size_t> order(ents.size());
    for (size_t i = 0; i < ents.size(); i++)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b)
                     { return ents[a].tag < ents[b].tag; });
    std::vector<uint8_t> blob;
    put16v(blob, (uint16_t)ents.size());
    for (size_t oi : order)
    {
        const Ent &e = ents[oi];
        put16v(blob, e.tag);
        put16v(blob, e.type);
        put32v(blob, e.count);
        if (e.data.size() > 4)
            put32v(blob, dataOff[oi]);
        else
        {
            std::vector<uint8_t> v(e.data);
            v.resize(4, 0);
            blob.insert(blob.end(), v.begin(), v.end());
        }
    }
    put32v(blob, 0);
    for (size_t i = 0; i < ents.size(); i++)
        if (ents[i].data.size() > 4)
        {
            if ((base + blob.size()) & 1)
                blob.push_back(0);
            blob.insert(blob.end(), ents[i].data.begin(),
                        ents[i].data.end());
        }
    for (const auto &s : strips)
        blob.insert(blob.end(), s.begin(), s.end());
    d.append((const char *)blob.data(), blob.size());
    // patch IFD0's next pointer
    uint32_t b = base;
    memcpy(&d[t.nextPtrPos], &b, 4);
}

std::string ccVrtSkeleton(const RasterDatasetBase &ds, int outBands,
                          bool alphaOut)
{
    std::string s = strPrintf(
        "<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\">\n", ds.width,
        ds.height);
    if (ds.hasSrs && ds.srs.valid())
    {
        std::string mapping;
        for (int m : ds.srs.dataAxisToSRSAxisMapping())
        {
            if (!mapping.empty())
                mapping += ",";
            mapping += strPrintf("%d", m);
        }
        s += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             ds.srs.wkt1Gdal() + "</SRS>\n";
    }
    if (ds.hasGT)
    {
        s += "  <GeoTransform>";
        for (int i = 0; i < 6; i++)
        {
            if (i)
                s += ",";
            s += strPrintf("%24.16e", ds.gt[i]);
        }
        s += "</GeoTransform>\n";
    }
    for (int b = 1; b <= outBands; b++)
    {
        if (alphaOut && b == outBands)
            s += strPrintf("  <VRTRasterBand dataType=\"Byte\" "
                           "band=\"%d\">\n    "
                           "<ColorInterp>Alpha</ColorInterp>\n  "
                           "</VRTRasterBand>\n",
                           b);
        else
            s += strPrintf(
                "  <VRTRasterBand dataType=\"Byte\" band=\"%d\" />\n", b);
    }
    s += "</VRTDataset>\n";
    return s;
}

int rasterCleanCollarHandler(const CmdSpec &, ParseResult &r)
{
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    bool update = r.flag("update");
    bool addAlpha = r.flag("add-alpha");
    bool addMask = r.flag("add-mask");
    std::string format = r.str("output-format");
    int thr = atoi(r.str("color-threshold", "15").c_str());
    int nmax = atoi(r.str("pixel-distance", "2").c_str());
    bool flood = !strEqualNoCase(r.str("algorithm", "floodfill"),
                                 "twopasses");
    std::vector<std::string> colorArgs = r.list("color");
    if (colorArgs.empty())
        colorArgs.push_back("black");

    // resolve the output driver
    std::string drv;
    if (!format.empty())
    {
        std::string issue = rasterOutFormatIssue(format, drv);
        if (!issue.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clean-collar: " + issue);
            handlerPrintUsage();
            return 1;
        }
    }
    else if (!output.empty())
    {
        std::string lower = strToLower(output);
        size_t dot = lower.find_last_of("./");
        std::string ext =
            dot != std::string::npos && lower[dot] == '.'
                ? lower.substr(dot + 1)
                : "";
        if (ext == "tif" || ext == "tiff" || ext.empty())
            drv = "GTiff";
        else if (ext == "vrt")
            drv = "VRT";
        else
            return 1;  // no driver guess: silent failure
    }

    bool isMem = strEqualNoCase(drv, "MEM");
    if (output.empty() && !update)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "clean-collar: Output dataset is not specified. If "
                    "you intend to update the input dataset, set the "
                    "'update' option");
        return 1;
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
    if (update)
    {
        // in-place rewrites never consume the CRS, so its lazy decode
        // diagnostics stay unflushed
        if (!r.list("creation-option").empty())
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Warning: creation options are ignored when "
                        "writing to an existing file.");
    }
    else
        ds->replayDeferred();
    const int w = ds->width, h = ds->height;
    const int nAll = (int)ds->bands.size();
    const bool hasAlpha =
        nAll > 1 && ds->bands[nAll - 1].colorInterp == "Alpha";
    const int nColor = hasAlpha ? nAll - 1 : nAll;

    if (update && addAlpha && !hasAlpha)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Last band is not an alpha band.");
        return 1;
    }

    // add-mask replaces a source alpha band by the mask; otherwise a
    // source alpha (or --add-alpha) puts an alpha band last
    const bool alphaOut = !update && addMask ? false
                                             : (addAlpha || hasAlpha);
    const int outBands = !update && addMask
                             ? nColor
                             : nAll + (addAlpha && !hasAlpha ? 1 : 0);

    // creation options (GTiff outputs only)
    std::vector<std::pair<std::string, std::string>> cos;
    for (const auto &c : r.list("creation-option"))
    {
        size_t eq = c.find('=');
        cos.push_back({c.substr(0, eq),
                       eq == std::string::npos ? "" : c.substr(eq + 1)});
    }

    // update target checks
    std::unique_ptr<RasterDatasetBase> tgt;
    std::string tgtPath;
    if (update)
    {
        tgtPath = output.empty() ? input : output;
        if (!output.empty())
        {
            std::string terr;
            cplPushQuietHandler();
            tgt = openRaster(output, terr);
            cplPopHandler();
            if (!tgt)
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + output +
                                "' not recognized as being in a "
                                "supported file format.");
                return 1;
            }
            if (tgt->width != w || tgt->height != h)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "The dimensions of the output dataset don't "
                            "match the dimensions of the input dataset.");
                return 1;
            }
            if ((int)tgt->bands.size() != nAll)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Inconsistent number of source and "
                            "destination bands.");
                return 1;
            }
        }
    }

    // GTiff/VRT output shell parameters (also used for the zero-filled
    // leftover the color-count failure leaves behind)
    GTiffCreateParams p;
    p.width = w;
    p.height = h;
    p.bands = outBands;
    p.type = DType::Byte;
    if (ds->hasGT)
    {
        p.hasGT = true;
        memcpy(p.gt, ds->gt, sizeof(p.gt));
    }
    if (ds->hasSrs && ds->srs.valid())
        p.srs = &ds->srs;
    if (alphaOut && outBands != 4)
    {
        // Create-default interps plus alpha on the last band; the
        // three-band case (Red,Green,Alpha) is not natively
        // representable and dumps COLORINTERP items
        p.photometric = 1;
        p.extrasSet = true;
        p.extraSamples.assign((size_t)(outBands - 1), 0);
        p.extraSamples.back() = 2;
        if (outBands == 3)
        {
            p.useGmdItems = true;
            const char *names[3] = {"Red", "Green", "Alpha"};
            for (int i = 0; i < 3; i++)
            {
                GmdItem it;
                it.name = "COLORINTERP";
                it.value = names[i];
                it.sample = i;
                it.role = "colorinterp";
                p.gmdItems.push_back(it);
            }
        }
    }
    bool coFatal = false;
    if (!update && drv == "GTiff")
    {
        CreationOptions o = parseCreationOptions(cos, output,
                                                 "clean-collar");
        if (o.fatal)
            coFatal = true;
        else if (!finalizeCreationOptions(o, output, p.bands, p.type))
            coFatal = true;
        else
        {
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
            p.bigtiff = o.bigtiffMode == 1;
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
        }
        if (coFatal)
            return 1;
    }

    // create the output shell before validating colors: a count
    // mismatch leaves a zero-filled dataset behind
    bool vrtWritten = false;
    if (!update && drv == "VRT")
    {
        writeStringToFile(output, ccVrtSkeleton(*ds, outBands, alphaOut));
        vrtWritten = true;
    }

    // parse colors against the non-alpha band count
    std::vector<std::vector<int>> colors;
    bool countBad = false;
    for (const auto &ca : colorArgs)
    {
        std::vector<int> tup;
        ccParseColor(ca, &tup, nColor);
        if ((int)tup.size() != nColor)
            countBad = true;
        colors.push_back(std::move(tup));
    }
    if (countBad)
    {
        if (!update && drv == "GTiff")
        {
            std::vector<std::vector<uint8_t>> zeros(
                (size_t)outBands,
                std::vector<uint8_t>((size_t)w * h, 0));
            p.pixels = &zeros;
            std::string werr;
            gtiffWrite(output, p, werr);
        }
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "-color args must have the same number of values as "
                    "the non alpha input band count.");
        return 1;
    }
    (void)vrtWritten;

    // non-Byte warnings, then processing
    for (int b = 0; b < nAll; b++)
        if (ds->bands[b].type != DType::Byte)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        strPrintf("Band %d is not of type GDT_UInt8. It "
                                  "can lead to unexpected results.",
                                  b + 1));

    std::vector<std::vector<uint8_t>> planes((size_t)nAll);
    for (int b = 0; b < nAll; b++)
    {
        std::vector<double> vals;
        if (!ds->readBand(b + 1, vals))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("clean-collar: cannot read band %d",
                                  b + 1));
            return 1;
        }
        planes[b].resize((size_t)w * h);
        for (size_t i = 0; i < vals.size(); i++)
            planes[b][i] = ccClampByte(vals[i]);
    }

    uint8_t rep = 0;
    if (colors.size() == 1 && !colors[0].empty() && colors[0][0] == 255)
        rep = 255;

    std::vector<std::vector<uint8_t>> work(planes.begin(),
                                           planes.begin() + nColor);
    std::vector<uint8_t> eaten, floodRegion;
    ccRunAlgorithm(work, w, h, colors, thr, nmax, flood, rep, eaten,
                   &floodRegion);

    std::vector<uint8_t> alphaPlane;
    if (alphaOut)
    {
        alphaPlane.resize((size_t)w * h);
        for (size_t i = 0; i < eaten.size(); i++)
            alphaPlane[i] = eaten[i] ? 0 : 255;
    }
    std::vector<uint8_t> maskBits;
    if (addMask)
    {
        maskBits.resize((size_t)w * h);
        for (size_t i = 0; i < eaten.size(); i++)
            maskBits[i] = eaten[i] ? 0 : 1;
    }

    if (update)
    {
        std::string data;
        if (!readFileToString(tgtPath, data))
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "clean-collar: cannot read " + tgtPath);
            return 1;
        }
        CcTiffInfo t = ccParseTiff(data);
        if (!t.ok || t.spp != nAll || t.width != w || t.height != h)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "clean-collar: updating this dataset is not "
                        "supported");
            return 1;
        }
        // the floodfill run with a zero pixel-distance skips the scan
        // phase writeback: only rows the flood region touches are stored
        std::vector<uint8_t> rowStore(h, 1);
        if (flood && nmax == 0)
        {
            rowStore.assign(h, 0);
            if (!floodRegion.empty())
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        if (floodRegion[(size_t)y * w + x])
                        {
                            rowStore[y] = 1;
                            break;
                        }
        }
        for (int si = 0; si < (int)t.stripOff.size(); si++)
        {
            int y0 = si * t.rps;
            int rows = y0 + t.rps > h ? h - y0 : t.rps;
            size_t off = t.stripOff[si];
            const size_t rowBytesN = [&]
            {
                size_t s = 0;
                for (int b = 0; b < nAll; b++)
                    s += t.bps[b] / 8;
                return s * w;
            }();
            for (int rrow = 0; rrow < rows; rrow++)
            {
                if (!rowStore[y0 + rrow])
                {
                    off += rowBytesN;
                    continue;
                }
                for (int x = 0; x < w; x++)
                {
                    size_t idx = (size_t)(y0 + rrow) * w + x;
                    for (int b = 0; b < nAll; b++)
                    {
                        uint8_t v;
                        if (alphaOut && b == nAll - 1)
                            v = alphaPlane[idx];
                        else if (b < nColor)
                            v = work[b][idx];
                        else
                            v = planes[b][idx];
                        ccStoreSample(data, off, t.bps[b], t.sfmt[b], v);
                        off += t.bps[b] / 8;
                    }
                }
            }
        }
        if (addMask)
            ccAppendMaskIfd(data, t, maskBits);
        if (!writeStringToFile(tgtPath, data))
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "clean-collar: cannot write " + tgtPath);
            return 1;
        }
        if (!quiet)
            printProgress();
        return 0;
    }

    if (isMem)
    {
        if (!quiet)
            printProgress();
        return 0;
    }

    if (drv == "VRT")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Writing through VRTSourcedRasterBand is not "
                    "supported.");
        return 1;
    }

    // GTiff
    std::vector<std::vector<uint8_t>> pixels;
    for (int b = 0; b < nColor; b++)
        pixels.push_back(std::move(work[b]));
    if (alphaOut)
        pixels.push_back(std::move(alphaPlane));
    p.pixels = &pixels;
    if (addMask)
        p.maskPixels = &maskBits;
    std::string werr;
    if (!gtiffWrite(output, p, werr))
    {
        if (werr != "reported")
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "clean-collar: " + werr);
        return 1;
    }
    if (!quiet)
        printProgress();
    return 0;
}

}  // namespace

void registerRasterCleanCollarHandler()
{
    registerHandler("raster_clean-collar", rasterCleanCollarHandler);
    registerArgValueCheck(
        "raster_clean-collar",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName != "color")
                return "";
            if (!ccParseColor(value, nullptr, 0))
                return "\x05Value for 'color' should be tuple of integer "
                       "(like 'r,g,b'), 'black' or 'white'";
            return "";
        });
    registerPreValidator(
        "raster_clean-collar",
        [](const CmdSpec &, ParseResult &r) -> int
        {
            std::string format = r.str("output-format");
            if (format.empty())
                return 0;
            if (strEqualNoCase(format, "GDALG"))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "clean-collar: GDALG output is not "
                            "supported.");
                handlerPrintUsage();
                return 1;
            }
            std::string drv;
            std::string issue = rasterOutFormatIssue(format, drv);
            if (!issue.empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "clean-collar: " + issue);
                handlerPrintUsage();
                return 1;
            }
            return 0;
        });
    registerPostValidator(
        "raster_clean-collar",
        [](const CmdSpec &, ParseResult &r, bool) -> bool
        {
            std::string of = r.str("output-format");
            std::string out = r.str("output");
            if (of.empty() &&
                strEndsWith(strToLower(out), ".gdalg.json"))
            {
                cplErrorStr(CE_Failure, CPLE_NotSupported,
                            "clean-collar: GDALG output is not supported");
                return true;
            }
            return false;
        });
}
