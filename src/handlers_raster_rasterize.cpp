#include "engine.h"
#include "cpl.h"
#include "dataset.h"
#include "gtiff_write.h"
#include "ogr.h"
#include "ogrsql.h"
#include "progress.h"
#include "srs.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C"
{
    int uncompress(unsigned char *dest, unsigned long *destLen,
                   const unsigned char *source, unsigned long sourceLen);
    int compress2(unsigned char *dest, unsigned long *destLen,
                  const unsigned char *source, unsigned long sourceLen,
                  int level);
    unsigned long compressBound(unsigned long sourceLen);
    struct libdeflate_compressor;
    struct libdeflate_compressor *libdeflate_alloc_compressor(int level);
    size_t libdeflate_zlib_compress_bound(struct libdeflate_compressor *,
                                          size_t in_nbytes);
    void libdeflate_free_compressor(struct libdeflate_compressor *);
}

// deflate codec shared with the GTiff writer
std::vector<uint8_t> gtiffDeflateBlock(const std::vector<uint8_t> &in,
                                       int level);

bool g_rasterizeLastSrsSet = false;
Srs g_rasterizeLastSrs;

namespace
{

bool rzFileExists(const std::string &p)
{
    struct stat sb;
    return stat(p.c_str(), &sb) == 0;
}

bool rzIsDir(const std::string &p)
{
    struct stat sb;
    return stat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}

// CPLAtofM semantics: no nan/inf tokens (they parse to 0)
double rzAtof(const std::string &s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    size_t j = i;
    if (j < s.size() && (s[j] == '+' || s[j] == '-'))
        ++j;
    if (j >= s.size() ||
        !((s[j] >= '0' && s[j] <= '9') || s[j] == '.'))
        return 0.0;
    return strtod(s.c_str() + i, nullptr);
}

// ------------------------------------------------------------------
// pixel burn target (double buffers; the encode to the native dtype
// happens at write time)
// ------------------------------------------------------------------

struct RzTarget
{
    int w = 0, h = 0;
    int bands = 1;
    bool add = false;
    bool dedup = false;  // add-mode same-shape duplicate suppression
    std::vector<std::vector<double>> buf;
    std::vector<double> burnVals;  // per band, for the current shape
    bool burn3d = false;           // value comes from the variant
    std::set<int64_t> done;
    void beginShape() { done.clear(); }
    void burnPx(int x, int y, double variant)
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        if (dedup && !done.insert((int64_t)y * w + x).second)
            return;
        for (int b = 0; b < bands; ++b)
        {
            const double v = burn3d ? variant : burnVals[(size_t)b];
            double &c = buf[(size_t)b][(size_t)y * w + x];
            c = add ? c + v : v;
        }
    }
    void scanline(int y, int x0, int x1, double variant)
    {
        if (y < 0 || y >= h)
            return;
        if (x0 < 0)
            x0 = 0;
        if (x1 >= w)
            x1 = w - 1;
        for (int x = x0; x <= x1; ++x)
            burnPx(x, y, variant);
    }
};

// ------------------------------------------------------------------
// rasterization primitives (GDALdllImage* semantics: pixel-center
// even-odd fill, integer Bresenham lines, grid-walking all-touched)
// ------------------------------------------------------------------

struct RzShape
{
    // flattened rings/lines in pixel space
    std::vector<int> partSize;
    std::vector<double> xs, ys, vs;
};

void rzFilledPolygon(RzTarget &t, const RzShape &s, bool useVariant)
{
    const int n = (int)s.xs.size();
    if (!n || s.partSize.empty())
        return;
    const double variant0 = useVariant && !s.vs.empty() ? s.vs[0] : 0.0;

    double dminy = s.ys[0], dmaxy = s.ys[0];
    for (int i = 1; i < n; ++i)
    {
        dminy = std::min(dminy, s.ys[i]);
        dmaxy = std::max(dmaxy, s.ys[i]);
    }
    int miny = (int)dminy, maxy = (int)dmaxy;
    if (miny < 0)
        miny = 0;
    if (maxy >= t.h)
        maxy = t.h - 1;

    std::vector<int> ints;
    ints.reserve((size_t)n);
    for (int y = miny; y <= maxy; ++y)
    {
        const double dy = y + 0.5;
        int part = 0, partoffset = 0;
        ints.clear();
        for (int i = 0; i < n; ++i)
        {
            if (i == partoffset + s.partSize[part])
            {
                partoffset += s.partSize[part];
                ++part;
            }
            int ind1, ind2 = i;
            if (i == partoffset)
                ind1 = partoffset + s.partSize[part] - 1;
            else
                ind1 = i - 1;

            double dy1 = s.ys[ind1], dy2 = s.ys[ind2];
            if ((dy1 < dy && dy2 < dy) || (dy1 > dy && dy2 > dy))
                continue;
            double dx1, dx2;
            if (dy1 < dy2)
            {
                dx1 = s.xs[ind1];
                dx2 = s.xs[ind2];
            }
            else if (dy1 > dy2)
            {
                std::swap(dy1, dy2);
                dx2 = s.xs[ind1];
                dx1 = s.xs[ind2];
            }
            else  // horizontal edge on the scan center: only bottom
                  // segments (left-to-right) are filled here, top ones
                  // are covered by the regular crossings
            {
                if (s.xs[ind1] < s.xs[ind2])
                {
                    const int hx1 =
                        (int)std::floor(s.xs[ind1] + 0.5);
                    const int hx2 =
                        (int)std::floor(s.xs[ind2] + 0.5);
                    if (hx1 > t.w || hx2 <= 0)
                        continue;
                    t.scanline(y, hx1, hx2 - 1, variant0);
                }
                continue;
            }
            if (dy < dy2 && dy >= dy1)
            {
                const double intersect =
                    (dy - dy1) * (dx2 - dx1) / (dy2 - dy1) + dx1;
                ints.push_back((int)std::floor(intersect + 0.5));
            }
        }
        std::sort(ints.begin(), ints.end());
        for (size_t i = 0; i + 1 < ints.size(); i += 2)
        {
            if (ints[i] <= t.w && ints[i + 1] > 0)
                t.scanline(y, ints[i], ints[i + 1] - 1, variant0);
        }
    }
}

void rzImagePoint(RzTarget &t, double x, double y, double v)
{
    const int iX = (int)std::floor(x);
    const int iY = (int)std::floor(y);
    if (iX < 0 || iX >= t.w || iY < 0 || iY >= t.h)
        return;
    t.burnPx(iX, iY, v);
}

void rzImageLine(RzTarget &t, const RzShape &s)
{
    int n = 0;
    for (size_t p = 0; p < s.partSize.size();
         n += s.partSize[p++])
    {
        // segments run last-to-first, each walked from its later
        // vertex back to the earlier one; a plain overwrite lets the
        // earliest segment win shared cells, while in add mode each
        // walk (except the part's first segment) stops one point
        // short: the ending vertex cell belongs to the next walk,
        // which starts there with the pristine vertex variant
        for (int j = s.partSize[p] - 1; j >= 1; --j)
        {
            const int lim = t.add && j > 1 ? 1 : 0;
            int iX = (int)std::floor(s.xs[n + j]);
            int iY = (int)std::floor(s.ys[n + j]);
            const int iX1 = (int)std::floor(s.xs[n + j - 1]);
            const int iY1 = (int)std::floor(s.ys[n + j - 1]);
            double variant = s.vs[n + j];
            const double variant1 = s.vs[n + j - 1];

            int dX = std::abs(iX1 - iX), dY = std::abs(iY1 - iY);
            const int stepX = iX > iX1 ? -1 : 1;
            const int stepY = iY > iY1 ? -1 : 1;

            if (dX >= dY)
            {
                const int xErr = dY << 1;
                const int yErr = xErr - (dX << 1);
                int err = xErr - dX;
                const double dVar =
                    dX == 0 ? 0.0 : (variant1 - variant) / dX;
                while (dX-- >= lim)
                {
                    if (iX >= 0 && iX < t.w && iY >= 0 && iY < t.h)
                        t.burnPx(iX, iY, variant);
                    variant += dVar;
                    iX += stepX;
                    if (err > 0)
                    {
                        iY += stepY;
                        err += yErr;
                    }
                    else
                        err += xErr;
                }
            }
            else
            {
                const int xErr = dX << 1;
                const int yErr = xErr - (dY << 1);
                int err = xErr - dY;
                const double dVar =
                    dY == 0 ? 0.0 : (variant1 - variant) / dY;
                while (dY-- >= lim)
                {
                    if (iX >= 0 && iX < t.w && iY >= 0 && iY < t.h)
                        t.burnPx(iX, iY, variant);
                    variant += dVar;
                    iY += stepY;
                    if (err > 0)
                    {
                        iX += stepX;
                        err += yErr;
                    }
                    else
                        err += xErr;
                }
            }
        }
    }
}

void rzImageLineAllTouched(RzTarget &t, const RzShape &s,
                           bool intersectOnly)
{
    int n = 0;
    for (size_t p = 0; p < s.partSize.size();
         n += s.partSize[p++])
    {
        // segments walk from the last to the first: a plain burn lets
        // the earliest segment overwrite the junction cells, an add
        // dedup keeps the later segment's pristine vertex variant
        for (int j = s.partSize[p] - 1; j >= 1; --j)
        {
            double dfX = s.xs[n + j - 1], dfY = s.ys[n + j - 1];
            double dfXEnd = s.xs[n + j], dfYEnd = s.ys[n + j];
            double variant = s.vs[n + j - 1];
            double variantEnd = s.vs[n + j];

            // left to right
            if (dfXEnd < dfX)
            {
                std::swap(dfX, dfXEnd);
                std::swap(dfY, dfYEnd);
                std::swap(variant, variantEnd);
            }

            // zero-length segment: one burn of the end variant, none
            // when it sits exactly on a row boundary
            if (dfX == dfXEnd && dfY == dfYEnd)
            {
                if (intersectOnly && dfX == std::floor(dfX))
                    continue;
                const int iX = (int)std::floor(dfX);
                const int iY = (int)std::floor(dfY);
                if (iX >= 0 && iX < t.w && iY >= 0 && iY < t.h &&
                    iY <= (int)std::floor(dfY - 0.000000001))
                    t.burnPx(iX, iY, variantEnd);
                continue;
            }

            // vertical special case
            if (std::fabs(dfX - dfXEnd) < 0.01)
            {
                // a segment lying exactly on a cell boundary has a
                // zero-area intersection: polygon rings skip it
                if (intersectOnly && dfX == dfXEnd &&
                    dfX == std::floor(dfX))
                    continue;
                if (dfYEnd < dfY)
                {
                    std::swap(dfY, dfYEnd);
                    std::swap(variant, variantEnd);
                }
                const int iX = (int)std::floor(dfXEnd);
                if (iX < 0 || iX >= t.w)
                    continue;
                const double dVar =
                    dfYEnd == dfY
                        ? 0.0
                        : (variantEnd - variant) / (dfYEnd - dfY);
                if (dfY < 0.0)
                {
                    variant += (0.0 - dfY) * dVar;
                    dfY = 0.0;
                }
                int iY = (int)std::floor(dfY);
                const int iYEnd =
                    (int)std::floor(dfYEnd - 0.000000001);
                double v = variant + dVar * (iY - dfY);
                for (; iY <= iYEnd; ++iY, v += dVar)
                    if (iY >= 0 && iY < t.h)
                        t.burnPx(iX, iY, v);
                continue;
            }

            // horizontal special case
            if (std::fabs(dfY - dfYEnd) < 0.01)
            {
                if (intersectOnly && dfY == dfYEnd &&
                    dfY == std::floor(dfY))
                    continue;
                const int iY = (int)std::floor(dfY);
                if (iY < 0 || iY >= t.h)
                    continue;
                const double dVar =
                    dfXEnd == dfX
                        ? 0.0
                        : (variantEnd - variant) / (dfXEnd - dfX);
                if (dfX < 0.0)
                {
                    variant += (0.0 - dfX) * dVar;
                    dfX = 0.0;
                }
                int iX = (int)std::floor(dfX);
                const int iXEnd =
                    (int)std::floor(dfXEnd - 0.000000001);
                double v = variant + dVar * (iX - dfX);
                for (; iX <= iXEnd; ++iX, v += dVar)
                    if (iX >= 0 && iX < t.w)
                        t.burnPx(iX, iY, v);
                continue;
            }

            // general case: walk from cell to cell, burning every cell
            // visited; tiny epsilon steps push exactly-on-boundary
            // positions into the next row
            if ((dfY < 0.0 && dfYEnd < 0.0) ||
                (dfY > (double)t.h && dfYEnd > (double)t.h))
                continue;
            const double dfM = (dfYEnd - dfY) / (dfXEnd - dfX);
            const double dfVarSlope =
                (variantEnd - variant) / (dfXEnd - dfX);
            // clip the walk against the raster: left and right in x
            // (the right clamp slides dfYEnd along the segment, which
            // fixes the exact bit pattern of the y clips below), then
            // the start is advanced to its top/bottom edge crossing
            // with the variant in tow, and the end is pulled back to
            // its own crossing, which decides the exact loop bound
            // and with it whether the edge-landing burn still fires
            if (dfX < 0.0)
            {
                variant += (0.0 - dfX) * dfVarSlope;
                dfY += (0.0 - dfX) * dfM;
                dfX = 0.0;
            }
            if (dfXEnd > (double)t.w)
            {
                dfYEnd += ((double)t.w - dfXEnd) * dfM;
                dfXEnd = (double)t.w;
            }
            if (dfY < 0.0)
            {
                const double dfStep = (0.0 - dfY) / dfM;
                dfX += dfStep;
                variant += dfStep * dfVarSlope;
                dfY = 0.0;
            }
            else if (dfY > (double)t.h)
            {
                const double dfStep = ((double)t.h - dfY) / dfM;
                dfX += dfStep;
                variant += dfStep * dfVarSlope;
                dfY = (double)t.h;
            }
            if (dfYEnd < 0.0)
            {
                const double dfXCut =
                    dfXEnd + (0.0 - dfYEnd) / dfM;
                if (dfXCut < dfXEnd)
                    dfXEnd = dfXCut;
            }
            else if (dfYEnd > (double)t.h)
            {
                const double dfXCut =
                    dfXEnd + ((double)t.h - dfYEnd) / dfM;
                if (dfXCut < dfXEnd)
                    dfXEnd = dfXCut;
            }
            while (dfX < dfXEnd)
            {
                const int iX = (int)std::floor(dfX);
                const int iY = (int)std::floor(dfY);
                if (iX >= 0 && iX < t.w && iY >= 0 && iY < t.h)
                    t.burnPx(iX, iY, variant);

                double dfStepX = std::floor(dfX + 1.0) - dfX;
                double dfStepY = dfStepX * dfM;
                if ((int)std::floor(dfY + dfStepY) == iY)
                {
                }
                else if (dfM < 0)
                {
                    dfStepY = iY - dfY;
                    if (dfStepY > -0.000000001)
                        dfStepY = -0.000000001;
                    dfStepX = dfStepY / dfM;
                }
                else
                {
                    dfStepY = iY + 1 - dfY;
                    if (dfStepY < 0.000000001)
                        dfStepY = 0.000000001;
                    dfStepX = dfStepY / dfM;
                }
                dfX += dfStepX;
                dfY += dfStepY;
                variant += dfStepX * dfVarSlope;
            }
        }
    }
}

// ------------------------------------------------------------------
// geometry flattening (world -> pixel space)
// ------------------------------------------------------------------

struct RzXform
{
    // the reference applies a precomputed inverted geotransform; the
    // axis-aligned fast path keeps whole pixel coordinates exact where
    // an on-the-fly 2x2 solve drifts an ulp and flips floor(x + 0.5)
    double gt[6] = {0, 1, 0, 0, 0, 1};
    void apply(double x, double y, double &px, double &py) const
    {
        double inv[6];
        if (gt[2] == 0.0 && gt[4] == 0.0 && gt[1] != 0.0 &&
            gt[5] != 0.0)
        {
            inv[1] = 1.0 / gt[1];
            inv[2] = 0.0;
            inv[0] = -gt[0] / gt[1];
            inv[4] = 0.0;
            inv[5] = 1.0 / gt[5];
            inv[3] = -gt[3] / gt[5];
        }
        else
        {
            const double det = gt[1] * gt[5] - gt[2] * gt[4];
            const double idet = 1.0 / det;
            inv[1] = gt[5] * idet;
            inv[4] = -gt[4] * idet;
            inv[2] = -gt[2] * idet;
            inv[5] = gt[1] * idet;
            inv[0] = (gt[2] * gt[3] - gt[0] * gt[5]) * idet;
            inv[3] = (-gt[1] * gt[3] + gt[0] * gt[4]) * idet;
        }
        px = inv[0] + x * inv[1] + y * inv[2];
        py = inv[3] + x * inv[4] + y * inv[5];
    }
};

void rzAppendRing(const OgrGeometry &ring, const RzXform &xf,
                  RzShape &s)
{
    const size_t nv = ring.coords.size() / 3;
    if (!nv)
        return;
    s.partSize.push_back((int)nv);
    for (size_t i = 0; i < nv; ++i)
    {
        double px, py;
        xf.apply(ring.coords[i * 3], ring.coords[i * 3 + 1], px, py);
        s.xs.push_back(px);
        s.ys.push_back(py);
        s.vs.push_back(ring.hasZ ? ring.coords[i * 3 + 2] : 0.0);
    }
}

bool rzGeomIsEmpty(const OgrGeometry &g)
{
    if (g.empty)
        return true;
    if (g.type == 1 || g.type == 2)
        return g.coords.empty();
    if (g.type == 3 || g.type >= 4)
    {
        for (const auto &p : g.parts)
            if (!rzGeomIsEmpty(p))
                return false;
        return g.coords.empty();
    }
    return g.coords.empty() && g.parts.empty();
}

struct RzBurnCtx
{
    RzTarget *t = nullptr;
    RzXform xf;
    bool allTouched = false;
    bool use3d = false;
};

void rzBurnGeometry(RzBurnCtx &c, const OgrGeometry &g)
{
    switch (g.type)
    {
        case 1:  // point
        {
            if (g.coords.size() >= 2)
            {
                double px, py;
                c.xf.apply(g.coords[0], g.coords[1], px, py);
                rzImagePoint(*c.t, px, py,
                             g.hasZ && g.coords.size() >= 3
                                 ? g.coords[2]
                                 : 0.0);
            }
            break;
        }
        case 2:  // linestring
        {
            RzShape s;
            rzAppendRing(g, c.xf, s);
            if (c.allTouched)
                rzImageLineAllTouched(*c.t, s, false);
            else
                rzImageLine(*c.t, s);
            break;
        }
        case 3:  // polygon
        {
            RzShape s;
            for (const auto &ring : g.parts)
                rzAppendRing(ring, c.xf, s);
            rzFilledPolygon(*c.t, s, c.use3d);
            if (c.allTouched)
            {
                // ring outlines burn the same constant value as the
                // fill (first vertex Z in 3d mode)
                RzShape rs = s;
                if (!rs.vs.empty())
                    std::fill(rs.vs.begin(), rs.vs.end(), rs.vs[0]);
                rzImageLineAllTouched(*c.t, rs, true);
            }
            break;
        }
        case 4:  // multipoint
        case 5:  // multilinestring
        case 6:  // multipolygon
        case 7:  // geometrycollection
        {
            for (const auto &part : g.parts)
                rzBurnGeometry(c, part);
            break;
        }
        default:
            break;
    }
}

// polygon rings of a geometry, for the invert collection
void rzCollectRings(const OgrGeometry &g, const RzXform &xf,
                    RzShape &s, bool &sawNonPolygon)
{
    switch (g.type)
    {
        case 3:
            for (const auto &ring : g.parts)
                rzAppendRing(ring, xf, s);
            break;
        case 4:
        case 5:
        case 6:
        case 7:
            if (g.type == 4 || g.type == 5)
            {
                sawNonPolygon = true;
                break;
            }
            for (const auto &part : g.parts)
                rzCollectRings(part, xf, s, sawNonPolygon);
            break;
        case 1:
        case 2:
            sawNonPolygon = true;
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------
// attribute value extraction (numeric coercion)
// ------------------------------------------------------------------

bool rzValueExactAs(double v, DType t)
{
    auto intIn = [&](double lo, double hi)
    { return v >= lo && v <= hi && v == std::floor(v); };
    switch (t)
    {
        case DType::Byte:
            return intIn(0, 255);
        case DType::Int8:
            return intIn(-128, 127);
        case DType::UInt16:
            return intIn(0, 65535);
        case DType::Int16:
            return intIn(-32768, 32767);
        case DType::UInt32:
            return intIn(0, 4294967295.0);
        case DType::Int32:
            return intIn(-2147483648.0, 2147483647.0);
        case DType::UInt64:
            return v >= 0 && v < 18446744073709551616.0 &&
                   v == std::floor(v);
        case DType::Int64:
            return v >= -9223372036854775808.0 &&
                   v < 9223372036854775808.0 && v == std::floor(v);
        case DType::Float32:
            return std::isnan(v) || (double)(float)v == v;
        default:
            return true;
    }
}

double rzFieldAsDouble(const OgrFieldValue &fv, long long fid,
                       bool &warned, DType bt, bool &exactWarned)
{
    if (!fv.set)
        return 0.0;
    double v = 0.0;
    std::string txt;
    switch (fv.v.type)
    {
        case JVal::INT:
            v = (double)fv.v.i;
            txt = std::to_string(fv.v.i);
            break;
        case JVal::DOUBLE:
            v = fv.v.d;
            txt = strPrintf("%.15g", fv.v.d);
            break;
        case JVal::STRING:
        {
            const char *s = fv.v.s.c_str();
            char *end = nullptr;
            v = strtod(s, &end);
            if (end == s || *end != '\0')
            {
                if (!warned)
                {
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        "Failed to parse attribute value " + fv.v.s +
                            " of feature " + std::to_string(fid) +
                            " as a number. A value of zero will be "
                            "burned for this feature. Further "
                            "messages of this type will be "
                            "suppressed.");
                    warned = true;
                }
                return 0.0;
            }
            txt = fv.v.s;
            break;
        }
        case JVal::BOOL:
            v = fv.v.b ? 1.0 : 0.0;
            txt = fv.v.b ? "1" : "0";
            break;
        default:
            return 0.0;
    }
    if (!exactWarned && !rzValueExactAs(v, bt))
    {
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "Attribute value " + txt + " of feature " +
                        std::to_string(fid) +
                        " cannot be exactly burned to an output band"
                        " of type " +
                        dtypeName(bt) +
                        ". Further messages of this type will be "
                        "suppressed.");
        exactWarned = true;
    }
    return v;
}

// ------------------------------------------------------------------
// in-place classic-TIFF pixel patching for --update / --add
// ------------------------------------------------------------------

uint16_t rzRd16(const std::vector<uint8_t> &d, size_t o)
{
    return (uint16_t)(d[o] | (d[o + 1] << 8));
}

uint32_t rzRd32(const std::vector<uint8_t> &d, size_t o)
{
    return (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) |
                      ((uint32_t)d[o + 3] << 24));
}

int rzTiffTypeSize(int t)
{
    switch (t)
    {
        case 3:
        case 8:
            return 2;
        case 4:
        case 9:
        case 11:
        case 13:
            return 4;
        case 5:
        case 10:
        case 12:
        case 16:
        case 17:
            return 8;
        default:
            return 1;
    }
}

struct RzTag
{
    uint16_t id = 0, type = 0;
    uint32_t count = 0, val = 0;
    size_t entryOff = 0;
};

struct RzIfd
{
    std::vector<RzTag> tags;
    const RzTag *find(uint16_t id) const
    {
        for (const auto &e : tags)
            if (e.id == id)
                return &e;
        return nullptr;
    }
};

bool rzParseIfd(const std::vector<uint8_t> &d, RzIfd &out)
{
    if (d.size() < 8 || d[0] != 'I' || d[1] != 'I' ||
        rzRd16(d, 2) != 42)
        return false;
    uint32_t off = rzRd32(d, 4);
    if (off + 2 > d.size())
        return false;
    uint16_t n = rzRd16(d, off);
    if (off + 2 + (size_t)n * 12 + 4 > d.size())
        return false;
    for (int i = 0; i < n; ++i)
    {
        size_t e = off + 2 + (size_t)i * 12;
        RzTag t;
        t.id = rzRd16(d, e);
        t.type = rzRd16(d, e + 2);
        t.count = rzRd32(d, e + 4);
        t.val = rzRd32(d, e + 8);
        t.entryOff = e;
        out.tags.push_back(t);
    }
    return true;
}

std::vector<uint64_t> rzTagInts(const std::vector<uint8_t> &d,
                                const RzTag &e)
{
    std::vector<uint64_t> out;
    const int ts = rzTiffTypeSize(e.type);
    const size_t total = (size_t)ts * e.count;
    uint8_t inl[4];
    const uint8_t *p;
    if (total <= 4)
    {
        inl[0] = (uint8_t)(e.val & 0xff);
        inl[1] = (uint8_t)((e.val >> 8) & 0xff);
        inl[2] = (uint8_t)((e.val >> 16) & 0xff);
        inl[3] = (uint8_t)((e.val >> 24) & 0xff);
        p = inl;
    }
    else
    {
        if ((size_t)e.val + total > d.size())
            return out;
        p = d.data() + e.val;
    }
    for (uint32_t i = 0; i < e.count; ++i)
    {
        if (ts == 2)
            out.push_back((uint64_t)(p[i * 2] | (p[i * 2 + 1] << 8)));
        else
            out.push_back((uint64_t)(p[i * 4] | (p[i * 4 + 1] << 8) |
                                     (p[i * 4 + 2] << 16) |
                                     ((uint32_t)p[i * 4 + 3] << 24)));
    }
    return out;
}

// strip/tile sample placement; supports both planar configurations
struct RzGrid
{
    int w = 0, h = 0, spp = 1;
    std::vector<int> bpe;  // per sample
    int planar = 1;
    bool tiled = false;
    int tw = 0, th = 0, rps = 0;
    int compression = 1;
    int predictor = 1;
    std::vector<uint64_t> offs;
    std::vector<uint64_t> counts;
    const RzTag *offTag = nullptr;
    const RzTag *cntTag = nullptr;

    bool blockLoc(int x, int y, int band, size_t &blk,
                  size_t &inOff) const
    {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return false;
        const int bsz = bpe[(size_t)band];
        if (tiled)
        {
            const int across = (w + tw - 1) / tw;
            const int down = (h + th - 1) / th;
            int ti = (y / th) * across + (x / tw);
            if (planar == 2)
                ti += band * across * down;
            if (ti >= (int)offs.size())
                return false;
            size_t inTile = (size_t)(y % th) * tw + (x % tw);
            if (planar == 1)
                inTile = inTile * spp + band;
            blk = (size_t)ti;
            inOff = inTile * bsz;
            return true;
        }
        const int perBand = (h + rps - 1) / rps;
        int si = y / rps;
        if (planar == 2)
            si += band * perBand;
        if (si >= (int)offs.size())
            return false;
        size_t inStrip = (size_t)(y % rps) * w + x;
        if (planar == 1)
            inStrip = inStrip * spp + band;
        blk = (size_t)si;
        inOff = inStrip * bsz;
        return true;
    }

    bool sampleOffset(int x, int y, int band, size_t &fileOff) const
    {
        size_t blk, inOff;
        if (!blockLoc(x, y, band, blk, inOff))
            return false;
        fileOff = (size_t)offs[blk] + inOff;
        return true;
    }

    size_t blockRawSize(size_t blk) const
    {
        const int bsz = bpe[0];
        const int perRow = planar == 1 ? spp : 1;
        if (tiled)
            return (size_t)tw * th * perRow * bsz;
        const int perBand = (h + rps - 1) / rps;
        const int si = (int)blk % std::max(1, perBand);
        const int rows = std::min(rps, h - si * rps);
        return (size_t)rows * w * perRow * bsz;
    }
};

// rewritten partial tiles come back from the block cache with their
// padding zeroed (the cache only carries the valid window)
void rzZeroTilePad(const RzGrid &g, size_t blk, uint8_t *raw)
{
    const int across = (g.w + g.tw - 1) / g.tw;
    const int down = (g.h + g.th - 1) / g.th;
    const size_t bandBlk = (size_t)across * down;
    const int ti = g.planar == 2 ? (int)(blk % bandBlk) : (int)blk;
    const int vw = std::min(g.tw, g.w - (ti % across) * g.tw);
    const int vh = std::min(g.th, g.h - (ti / across) * g.th);
    const int px = (g.planar == 1 ? g.spp : 1) * g.bpe[0];
    const size_t rowB = (size_t)g.tw * px;
    if (vw < g.tw && vw > 0)
        for (int ry = 0; ry < vh; ++ry)
            memset(raw + (size_t)ry * rowB + (size_t)vw * px, 0,
                   (size_t)(g.tw - vw) * px);
    if (vh < g.th && vh > 0)
        memset(raw + (size_t)vh * rowB, 0,
               (size_t)(g.th - vh) * rowB);
}

bool rzGridFromIfd(const std::vector<uint8_t> &d, const RzIfd &ifd,
                   RzGrid &g)
{
    const RzTag *ew = ifd.find(256), *eh = ifd.find(257);
    if (!ew || !eh)
        return false;
    g.w = (int)ew->val;
    g.h = (int)eh->val;
    const RzTag *comp = ifd.find(259);
    g.compression = comp ? (int)comp->val : 1;
    if (g.compression != 1 && g.compression != 8)
        return false;
    const RzTag *pred = ifd.find(317);
    g.predictor = pred ? (int)pred->val : 1;
    const RzTag *pc = ifd.find(284);
    g.planar = pc ? (int)pc->val : 1;
    const RzTag *spp = ifd.find(277);
    g.spp = spp ? (int)spp->val : 1;
    const RzTag *bps = ifd.find(258);
    g.bpe.assign((size_t)g.spp, 1);
    if (bps)
    {
        std::vector<uint64_t> v = rzTagInts(d, *bps);
        for (int i = 0; i < g.spp; ++i)
        {
            const uint64_t bits =
                v.empty() ? 8
                          : v[(size_t)std::min<size_t>(i, v.size() - 1)];
            if (bits % 8)
                return false;
            g.bpe[(size_t)i] = (int)(bits / 8);
        }
    }
    if (ifd.find(324))
    {
        g.tiled = true;
        const RzTag *tw = ifd.find(322), *th = ifd.find(323);
        if (!tw || !th)
            return false;
        g.tw = (int)tw->val;
        g.th = (int)th->val;
        g.offTag = ifd.find(324);
        g.cntTag = ifd.find(325);
        g.offs = rzTagInts(d, *g.offTag);
        if (g.cntTag)
            g.counts = rzTagInts(d, *g.cntTag);
        return !g.offs.empty() &&
               (g.compression == 1 || g.counts.size() == g.offs.size());
    }
    const RzTag *so = ifd.find(273);
    if (!so)
        return false;
    const RzTag *rps = ifd.find(278);
    g.rps = rps ? (int)rps->val : g.h;
    if (g.rps <= 0)
        g.rps = g.h;
    g.offTag = so;
    g.cntTag = ifd.find(279);
    g.offs = rzTagInts(d, *so);
    if (g.cntTag)
        g.counts = rzTagInts(d, *g.cntTag);
    return !g.offs.empty() &&
           (g.compression == 1 || g.counts.size() == g.offs.size());
}

void rzWriteTagInt(std::vector<uint8_t> &d, const RzTag &e, size_t idx,
                   uint64_t v)
{
    const int ts = rzTiffTypeSize(e.type);
    const size_t total = (size_t)ts * e.count;
    const size_t base = total <= 4 ? e.entryOff + 8 : (size_t)e.val;
    const size_t p = base + idx * (size_t)ts;
    if (ts != 2 && ts != 4)
        return;
    if (p + (size_t)ts > d.size())
        return;
    d[p] = (uint8_t)(v & 0xff);
    d[p + 1] = (uint8_t)((v >> 8) & 0xff);
    if (ts == 4)
    {
        d[p + 2] = (uint8_t)((v >> 16) & 0xff);
        d[p + 3] = (uint8_t)((v >> 24) & 0xff);
    }
}

// ------------------------------------------------------------------
// geotransform tags (scale+tiepoint only for north-up gt)
// ------------------------------------------------------------------

void rzApplyGtTags(GTiffCreateParams &p, const double gt[6])
{
    memcpy(p.gt, gt, 6 * sizeof(double));
    if (!(gt[5] < 0))
    {
        p.hasXform = true;
        const double xf[16] = {gt[1], 0, 0, gt[0], 0, gt[5], 0, gt[3],
                               0,     0, 0, 0,     0, 0,     0, 1};
        memcpy(p.xform, xf, sizeof(xf));
    }
    else
        p.hasXform = false;
}

int rzErr(int cls, const std::string &msg)
{
    cplErrorStr(CE_Failure, cls, msg);
    return 1;
}

// ------------------------------------------------------------------
// the handler
// ------------------------------------------------------------------

int rasterizeHandler(const CmdSpec &, ParseResult &r)
{
    const bool quiet = r.flag("quiet");
    const std::string input =
        r.list("input").empty() ? "" : r.list("input")[0];
    const std::string output = r.str("output");
    const std::string of = r.str("output-format");
    const bool overwrite = r.flag("overwrite");
    const bool fUpdate = r.flag("update");
    const bool fAdd = r.flag("add");
    const bool invert = r.flag("invert");
    const bool allTouched = r.flag("all-touched");
    const bool use3d = r.flag("3d");
    const std::string attrName = r.str("attribute-name");
    const bool attrSet = r.get("attribute-name") != nullptr;
    const bool burnSet = r.get("burn") != nullptr;
    const bool sizeSet = r.get("size") != nullptr;
    const bool resSet = r.get("resolution") != nullptr;
    const bool extentSet = r.get("extent") != nullptr;
    const bool tap = r.flag("target-aligned-pixels");

    // update-family flag refusals, in the reference's fixed order
    if (fUpdate || fAdd)
    {
        static const struct
        {
            const char *arg;
            const char *cli;
        } kForbid[] = {{"nodata", "--nodata"},
                       {"crs", "--crs"},
                       {"size", "--size"},
                       {"resolution", "--resolution"},
                       {"output-data-type", "--output-data-type"}};
        for (const auto &f : kForbid)
            if (r.get(f.arg))
                return rzErr(CPLE_AppDefined,
                             std::string("rasterize: Cannot specify ") +
                                 f.cli +
                                 " when updating an existing raster.");
    }
    const bool updateMode = (fUpdate || fAdd) && !overwrite;

    if (!updateMode && !sizeSet && !resSet)
        return rzErr(
            CPLE_AppDefined,
            "rasterize: Must specify output resolution (--resolution) "
            "or size (--size) when writing rasterized features to a "
            "new dataset.");

    // classic option-parse conflicts and value checks
    if (attrSet && burnSet)
        return rzErr(CPLE_AppDefined,
                     "Argument '-a <attribute_name>' not allowed with "
                     "'-burn <value>'");
    if (use3d && burnSet)
        return rzErr(CPLE_AppDefined,
                     "Argument '-3d' not allowed with '-burn <value>'");
    if (use3d && attrSet)
        return rzErr(
            CPLE_AppDefined,
            "Argument '-3d' not allowed with '-a <attribute_name>'");

    double resX = 0, resY = 0;
    if (resSet)
    {
        const ArgValue *v = r.get("resolution");
        resX = rzAtof(v->values[0]);
        resY = rzAtof(v->values[1]);
        if (resX <= 0 || resY <= 0)
            return rzErr(CPLE_AppDefined,
                         "Wrong value for -tr parameter.");
    }
    int reqW = 0, reqH = 0;
    if (sizeSet)
    {
        const ArgValue *v = r.get("size");
        reqW = atoi(v->values[0].c_str());
        reqH = atoi(v->values[1].c_str());
        if (reqW <= 0 && reqH <= 0)
            return rzErr(CPLE_AppDefined,
                         "Wrong value for -ts parameter: at least one "
                         "of the arguments must be greater than zero.");
    }

    // classic post-parse validation (bCreateOutput implied by any
    // create-flavoured option even in update mode)
    const bool createImplied =
        !updateMode || r.get("output-format") ||
        r.get("creation-option") || r.get("init") || extentSet || tap;
    if (createImplied)
    {
        if (!sizeSet && !resSet)
            return rzErr(CPLE_NotSupported,
                         "'-tr xres yres' or '-ts xsize ysize' is "
                         "required.");
        if (tap && !resSet)
            return rzErr(CPLE_NotSupported,
                         "-tap option cannot be used without using "
                         "-tr.");
        if (r.get("band"))
            return rzErr(CPLE_NotSupported,
                         "-b option cannot be used when creating a "
                         "GDAL dataset.");
    }

    // output driver resolution (extension guesses fail silently)
    bool isMem = false;
    if (!updateMode)
    {
        if (!of.empty())
        {
            if (strEqualNoCase(of, "MEM") ||
                strEqualNoCase(of, "Memory"))
                isMem = true;
            else if (!strEqualNoCase(of, "GTiff") &&
                     !strEqualNoCase(of, "COG"))
                return 1;  // refused during validation
        }
        else
        {
            const std::string lo = strToLower(output);
            if (!strEndsWith(lo, ".tif") && !strEndsWith(lo, ".tiff"))
                return 1;
        }
    }

    // open the input
    std::string err;
    std::unique_ptr<OgrDataset> ds;
    if (g_convertSourceOverride)
        ds = std::move(g_convertSourceOverride);
    else
        ds = openVectorDataset(input, err, r.list("input-format"),
                               r.list("open-option"));
    if (!ds)
        return 1;

    // layer resolution
    std::string sql = r.str("sql");
    const std::string dialect = r.str("dialect");
    const std::string layerName = r.str("input-layer");
    OgrLayer *layer = nullptr;
    std::unique_ptr<OgrLayer> sqlLayer;
    bool sqlFailed = false;
    if (r.get("sql"))
    {
        const bool sqlite = strEqualNoCase(dialect, "SQLITE");
        if (!dialect.empty() && !sqlite &&
            !strEqualNoCase(dialect, "OGRSQL"))
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "Dialect '" + dialect +
                            "' is unsupported. Only supported dialects "
                            "are 'OGRSQL'. Defaulting to OGRSQL");
        if (sqlite)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "The SQLite driver needs to be compiled to "
                        "support the SQLite SQL dialect");
            sqlFailed = true;
        }
        else
        {
            sqlLayer = ogrExecuteSql(*ds, sql);
            if (sqlLayer)
                layer = sqlLayer.get();
            else
                sqlFailed = true;
        }
    }
    else if (r.get("input-layer"))
    {
        for (auto &l : ds->layers)
            if (l.name == layerName)
            {
                layer = &l;
                break;
            }
        if (!layer)
            for (auto &l : ds->layers)
                if (strEqualNoCase(l.name, layerName))
                {
                    layer = &l;
                    break;
                }
        if (!layer)
            return rzErr(CPLE_AppDefined, "Unable to find layer \"" +
                                              layerName + "\".");
    }
    else
    {
        if (ds->layers.size() != 1)
            return rzErr(CPLE_NotSupported,
                         "Neither -sql nor -l are specified, but the "
                         "source dataset has not one single layer.");
        layer = &ds->layers[0];
    }

    // burn plan
    std::vector<double> burnList;
    for (const auto &b : r.list("burn"))
        burnList.push_back(rzAtof(b));
    std::vector<double> initList;
    for (const auto &b : r.list("init"))
        initList.push_back(rzAtof(b));
    const std::string whereClause = r.str("where");
    const bool whereSet = r.get("where") != nullptr;

    // -------------------------------------------------------------
    // update / add path: burn into the existing raster in place
    // -------------------------------------------------------------
    if (updateMode)
    {
        std::string oerr;
        auto tgt = openRaster(output, oerr);
        if (!tgt)
            return 1;  // reported during validation

        if (layer)
        {
            if (layer->hasSrs && !tgt->hasSrs)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "The input vector layer has a SRS, but "
                            "the output raster dataset SRS is "
                            "unknown.\nEnsure output raster dataset "
                            "has the same SRS, otherwise results "
                            "might be incorrect.");
            else if (!layer->hasSrs && tgt->hasSrs)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "The output raster dataset has a SRS, but "
                            "the input vector layer SRS is "
                            "unknown.\nEnsure input vector has the "
                            "same SRS, otherwise results might be "
                            "incorrect.");
        }

        if (whereSet && !sqlFailed && layer)
            if (!ogrApplyAttributeFilter(*layer, whereClause, false))
                return 1;

        int attrIdx = -1;
        if (attrSet && layer)
        {
            for (size_t f = 0; f < layer->fields.size(); ++f)
                if (strEqualNoCase(layer->fields[f].name, attrName))
                {
                    attrIdx = (int)f;
                    break;
                }
            if (attrIdx < 0)
                return rzErr(CPLE_AppDefined,
                             "Failed to find field " + attrName +
                                 " on layer " + layer->name + ".");
        }

        // with no geometries at all the reference never reaches the
        // band checks or the write-back: bare progress only (invert
        // always burns its synthetic outer ring)
        bool anyGeom = invert;
        if (layer && !anyGeom)
            for (const auto &f : layer->features)
                if (f.hasGeom)
                {
                    anyGeom = true;
                    break;
                }
        if (!anyGeom)
        {
            if (!quiet)
                printProgress();
            return 0;
        }

        std::vector<int> bandList;
        if (const ArgValue *v = r.get("band"))
            for (const auto &sv : v->values)
                bandList.push_back(atoi(sv.c_str()));
        if (bandList.empty())
            bandList.push_back(1);
        bool anyBadBand = false;
        for (int b : bandList)
            if (b > (int)tgt->bands.size())
            {
                anyBadBand = true;
                cplErrorStr(
                    CE_Failure, CPLE_IllegalArg,
                    output +
                        strPrintf(": GDALDataset::GetRasterBand(%d)"
                                  " - Illegal band #\n",
                                  b));
                cplErrorStr(CE_Failure, CPLE_ObjectNull,
                            "Pointer 'hBand' is NULL in "
                            "'GDALGetRasterDataType'.\n");
            }
        if (anyBadBand)
        {
            if (bandList[0] > (int)tgt->bands.size())
            {
                cplErrorStr(
                    CE_Failure, CPLE_IllegalArg,
                    output + strPrintf(": GDALDataset::GetRasterBand"
                                       "(%d) - Illegal band #\n",
                                       bandList[0]));
                return 1;
            }
            if (!quiet)
            {
                fputs("0", stdout);
                fflush(stdout);
            }
            for (size_t i = 0; i < bandList.size(); ++i)
                if (bandList[i] > (int)tgt->bands.size())
                {
                    cplErrorStr(
                        CE_Failure, CPLE_IllegalArg,
                        output +
                            strPrintf(": RasterIO(): panBandMap[%d]"
                                      " = %d, this band does not "
                                      "exist on dataset.",
                                      (int)i, bandList[i]));
                    break;
                }
            return 1;
        }

        RzTarget t;
        t.w = tgt->width;
        t.h = tgt->height;
        t.bands = (int)bandList.size();
        t.add = fAdd;
        t.dedup = fAdd;
        t.buf.resize((size_t)t.bands);
        for (int i = 0; i < t.bands; ++i)
            if (!tgt->readBand(bandList[(size_t)i], t.buf[(size_t)i]))
                return 1;
        std::vector<std::vector<double>> pristine = t.buf;

        RzBurnCtx c;
        c.t = &t;
        c.allTouched = allTouched;
        c.use3d = use3d;
        if (tgt->hasGT)
            memcpy(c.xf.gt, tgt->gt, sizeof(c.xf.gt));

        // differing known SRSs: geometries are transformed into the
        // raster's CRS before burning
        void *crsOp = nullptr;
        if (layer && layer->hasSrs && tgt->hasSrs &&
            !layer->srs.isEquivalentTo(tgt->srs))
            crsOp = vectorCrsOpCreate(layer->srs, tgt->srs);

        if (layer)
        {
            bool attrWarned = false;
            bool exactWarned = false;
            const DType wbt =
                tgt->bands[(size_t)(bandList[0] - 1)].type;
            std::vector<double> baseBurn((size_t)t.bands, 255.0);
            for (int i = 0; i < t.bands; ++i)
                if (!burnList.empty())
                    baseBurn[(size_t)i] = burnList[std::min(
                        (size_t)i, burnList.size() - 1)];

            if (invert)
            {
                RzShape rings;
                rings.partSize.push_back(4);
                const double fx[4] = {-2.0, t.w + 2.0, t.w + 2.0,
                                      -2.0};
                const double fy[4] = {-2.0, -2.0, t.h + 2.0,
                                      t.h + 2.0};
                for (int i = 0; i < 4; ++i)
                {
                    rings.xs.push_back(fx[i]);
                    rings.ys.push_back(fy[i]);
                    rings.vs.push_back(0.0);
                }
                bool sawNonPolygon = false;
                bool haveAttr = false;
                for (const auto &f : layer->features)
                {
                    if (attrIdx >= 0 && !haveAttr &&
                        (size_t)attrIdx < f.values.size())
                    {
                        const double v = rzFieldAsDouble(
                            f.values[(size_t)attrIdx], f.fid,
                            attrWarned, wbt, exactWarned);
                        for (auto &bv : baseBurn)
                            bv = v;
                        haveAttr = true;
                    }
                    if (!f.hasGeom || rzGeomIsEmpty(f.geom))
                        continue;
                    if (crsOp)
                    {
                        OgrGeometry g = f.geom;
                        if (vectorCrsOpApply(crsOp, g))
                            rzCollectRings(g, c.xf, rings,
                                           sawNonPolygon);
                    }
                    else
                        rzCollectRings(f.geom, c.xf, rings,
                                       sawNonPolygon);
                }
                if (sawNonPolygon)
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "Ignoring non-polygon geometries in "
                                "-i mode");
                t.burnVals = baseBurn;
                t.burn3d = use3d;
                t.beginShape();
                rzFilledPolygon(t, rings, use3d);
            }
            else
            {
                for (const auto &f : layer->features)
                {
                    if (!f.hasGeom || rzGeomIsEmpty(f.geom))
                        continue;
                    t.burnVals = baseBurn;
                    if (attrIdx >= 0)
                    {
                        const double v =
                            (size_t)attrIdx < f.values.size()
                                ? rzFieldAsDouble(
                                      f.values[(size_t)attrIdx],
                                      f.fid, attrWarned, wbt,
                                      exactWarned)
                                : 0.0;
                        for (auto &bv : t.burnVals)
                            bv = v;
                    }
                    t.burn3d = use3d;
                    t.beginShape();
                    if (crsOp)
                    {
                        OgrGeometry g = f.geom;
                        if (vectorCrsOpApply(crsOp, g))
                            rzBurnGeometry(c, g);
                    }
                    else
                        rzBurnGeometry(c, f.geom);
                }
            }
        }
        if (crsOp)
            vectorCrsOpFree(crsOp);

        // patch the touched samples in place
        std::vector<uint8_t> bytes;
        {
            FILE *f = fopen(output.c_str(), "rb");
            if (!f)
                return 1;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            bytes.resize((size_t)sz);
            size_t rd =
                sz ? fread(bytes.data(), 1, (size_t)sz, f) : 0;
            fclose(f);
            if (rd != (size_t)sz)
                return 1;
        }
        RzIfd ifd;
        RzGrid grid;
        if (rzParseIfd(bytes, ifd) && rzGridFromIfd(bytes, ifd, grid) &&
            (grid.compression == 1 ||
             (grid.compression == 8 && grid.predictor == 1)))
        {
            bool dirty = false;
            // per-block list of (offset in raw block, encoded cell)
            std::map<size_t,
                     std::vector<std::pair<size_t,
                                           std::array<uint8_t, 16>>>>
                patches;
            std::map<size_t, int> patchSize;
            for (int i = 0; i < t.bands; ++i)
            {
                const DType bt =
                    tgt->bands[(size_t)(bandList[(size_t)i] - 1)]
                        .type;
                for (int y = 0; y < t.h; ++y)
                    for (int x = 0; x < t.w; ++x)
                    {
                        const size_t idx = (size_t)y * t.w + x;
                        if (t.buf[(size_t)i][idx] ==
                            pristine[(size_t)i][idx])
                            continue;
                        size_t blk, inOff;
                        if (!grid.blockLoc(x, y,
                                           bandList[(size_t)i] - 1,
                                           blk, inOff))
                            continue;
                        uint8_t cell[16];
                        rasterEncodeReal(
                            bt, cell,
                            rasterFinishReal(t.buf[(size_t)i][idx],
                                             bt),
                            0.0);
                        const int bsz = dtypeSizeBytes(bt);
                        if (grid.compression == 1)
                        {
                            const size_t off =
                                (size_t)grid.offs[blk] + inOff;
                            if (off + (size_t)bsz <= bytes.size())
                            {
                                memcpy(&bytes[off], cell,
                                       (size_t)bsz);
                                dirty = true;
                            }
                            continue;
                        }
                        std::array<uint8_t, 16> c{};
                        memcpy(c.data(), cell, sizeof(cell));
                        patches[blk].push_back({inOff, c});
                        patchSize[blk] = bsz;
                    }
            }
            if (grid.compression == 1 && grid.tiled && dirty)
            {
                // partial tiles are written back from the block
                // cache, which zero-fills their padding
                for (size_t blk = 0; blk < grid.offs.size(); ++blk)
                    if ((size_t)grid.offs[blk] +
                            grid.blockRawSize(blk) <=
                        bytes.size())
                        rzZeroTilePad(grid, blk,
                                      bytes.data() + grid.offs[blk]);
            }
            if (grid.compression == 8)
            {
                // the reference rewrites every block on update: the
                // whole raster is written back through libtiff, which
                // re-encodes with libdeflate only when the worst-case
                // bound fits the read buffer (largest original
                // compressed block, rounded up to 1KB), and falls
                // back to classic zlib level 6 otherwise
                uint64_t maxCnt = 0;
                for (uint64_t c : grid.counts)
                    maxCnt = std::max(maxCnt, c);
                const size_t rawBuf =
                    (size_t)((maxCnt + 1023) / 1024) * 1024;
                libdeflate_compressor *lc =
                    libdeflate_alloc_compressor(6);
                for (size_t blk = 0; blk < grid.offs.size(); ++blk)
                {
                    const size_t need = grid.blockRawSize(blk);
                    if ((size_t)grid.offs[blk] +
                            (size_t)grid.counts[blk] >
                        bytes.size())
                        continue;
                    std::vector<uint8_t> raw(need);
                    unsigned long dl = (unsigned long)need;
                    if (uncompress(raw.data(), &dl,
                                   bytes.data() + grid.offs[blk],
                                   (unsigned long)grid.counts[blk]) !=
                            0 ||
                        dl != need)
                        continue;
                    auto pit = patches.find(blk);
                    if (pit != patches.end())
                    {
                        const int bsz = patchSize[blk];
                        for (const auto &pc : pit->second)
                            if (pc.first + (size_t)bsz <= raw.size())
                                memcpy(&raw[pc.first],
                                       pc.second.data(),
                                       (size_t)bsz);
                    }
                    if (grid.tiled)
                        rzZeroTilePad(grid, blk, raw.data());
                    const size_t bound =
                        lc ? libdeflate_zlib_compress_bound(
                                 lc, raw.size())
                           : (size_t)-1;
                    std::vector<uint8_t> comp;
                    if (bound <= rawBuf)
                        comp = gtiffDeflateBlock(raw, 6);
                    else
                    {
                        unsigned long cl = compressBound(
                            (unsigned long)raw.size());
                        comp.resize(cl);
                        if (compress2(comp.data(), &cl, raw.data(),
                                      (unsigned long)raw.size(),
                                      6) != 0)
                            continue;
                        comp.resize(cl);
                    }
                    // libtiff keeps a block in place when it does
                    // not grow, else appends at EOF
                    const bool changed =
                        comp.size() != (size_t)grid.counts[blk] ||
                        memcmp(comp.data(),
                               bytes.data() + grid.offs[blk],
                               comp.size()) != 0;
                    if (comp.size() <= (size_t)grid.counts[blk])
                        memcpy(&bytes[(size_t)grid.offs[blk]],
                               comp.data(), comp.size());
                    else
                    {
                        grid.offs[blk] = bytes.size();
                        bytes.insert(bytes.end(), comp.begin(),
                                     comp.end());
                        if (grid.offTag)
                            rzWriteTagInt(bytes, *grid.offTag, blk,
                                          grid.offs[blk]);
                    }
                    if ((size_t)grid.counts[blk] != comp.size())
                    {
                        grid.counts[blk] = comp.size();
                        if (grid.cntTag)
                            rzWriteTagInt(bytes, *grid.cntTag, blk,
                                          grid.counts[blk]);
                    }
                    if (changed)
                        dirty = true;
                }
                if (lc)
                    libdeflate_free_compressor(lc);
            }
            if (dirty)
            {
                FILE *f = fopen(output.c_str(), "wb");
                if (f)
                {
                    fwrite(bytes.data(), 1, bytes.size(), f);
                    fclose(f);
                }
            }
        }
        if (!quiet)
            printProgress();
        return 0;
    }

    // -------------------------------------------------------------
    // create path
    // -------------------------------------------------------------

    // bounds
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool haveBounds = false;
    if (extentSet)
    {
        const ArgValue *v = r.get("extent");
        minX = rzAtof(v->values[0]);
        minY = rzAtof(v->values[1]);
        maxX = rzAtof(v->values[2]);
        maxY = rzAtof(v->values[3]);
        haveBounds = true;
    }
    else if (layer)
    {
        if (layer->hasExtent)
        {
            minX = layer->extent[0];
            minY = layer->extent[1];
            maxX = layer->extent[2];
            maxY = layer->extent[3];
            haveBounds = true;
        }
        else
        {
            vectorLayerRecomputeExtent(*layer);
            if (layer->hasExtent)
            {
                minX = layer->extent[0];
                minY = layer->extent[1];
                maxX = layer->extent[2];
                maxY = layer->extent[3];
                haveBounds = true;
            }
            else
                return rzErr(CPLE_AppDefined,
                             "Cannot get layer extent");
        }
        // an unset envelope (only empty geometries) keeps inverted
        // sentinels: rejected in both sizing modes
        if (minX > maxX || minY > maxY)
            return rzErr(CPLE_AppDefined,
                         "Could not determine bounds");
        // implicit bounds get the half-pixel buffer in resolution mode
        if (resSet && !tap)
        {
            minX -= resX / 2;
            maxX += resX / 2;
            minY -= resY / 2;
            maxY += resY / 2;
        }
    }
    if (tap)
    {
        minX = std::floor(minX / resX) * resX;
        maxX = std::ceil(maxX / resX) * resX;
        minY = std::floor(minY / resY) * resY;
        maxY = std::ceil(maxY / resY) * resY;
    }

    int width = 0, height = 0;
    double xres = resX, yres = resY;
    if (!resSet)
    {
        if (minX == maxX || minY == maxY)
            return rzErr(CPLE_AppDefined,
                         "Could not determine bounds");
        width = reqW;
        height = reqH;
        if (width == 0)
            width = height;
        if (height == 0)
            height = width;
        xres = (maxX - minX) / width;
        yres = (maxY - minY) / height;
    }
    (void)haveBounds;
    width = (int)(0.5 + (maxX - minX) / xres);
    height = (int)(0.5 + (maxY - minY) / yres);

    // output srs
    Srs outSrs;
    bool haveSrs = false;
    if (const ArgValue *v = r.get("crs"))
    {
        bool ok = false;
        outSrs = Srs::fromCliInput(v->str(), ok, true);
        haveSrs = ok && outSrs.valid();
    }
    else if (layer && layer->hasSrs)
    {
        outSrs = layer->srs;
        haveSrs = true;
    }
    g_rasterizeLastSrs = outSrs;
    g_rasterizeLastSrsSet = haveSrs;

    DType type = DType::Float64;
    if (const ArgValue *v = r.get("output-data-type"))
    {
        std::string s = v->str();
        type = s == "UInt8" ? DType::Byte : dtypeFromName(s);
    }

    const int bands =
        (int)std::max<size_t>(1, std::max(burnList.size(),
                                          initList.size()));

    GTiffCreateParams p;
    p.width = width;
    p.height = height;
    p.bands = bands;
    p.type = type;

    std::vector<std::pair<std::string, std::string>> cos;
    for (const auto &c : r.list("creation-option"))
    {
        size_t eq = c.find('=');
        cos.push_back({c.substr(0, eq),
                       eq == std::string::npos ? "" : c.substr(eq + 1)});
    }
    CreationOptions o = parseCreationOptions(cos, output, "rasterize");
    if (o.fatal)
        return 1;
    if (!finalizeCreationOptions(o, output, p.bands, p.type))
        return rzErr(CPLE_AppDefined, "Cannot create " + output);
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
    p.bandInterleave = o.interleaveSet && o.bandInterleave;
    p.sparse = o.sparse;
    p.profile = o.profile;
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

    if (width <= 0 || height <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("Attempt to create %dx%d dataset is "
                              "illegal, sizes must be larger than "
                              "zero.",
                              width, height));
        return rzErr(CPLE_AppDefined, "Cannot create " + output);
    }

    RzTarget t;
    t.w = width;
    t.h = height;
    t.bands = bands;
    t.add = fAdd;
    t.dedup = fAdd;
    t.buf.assign((size_t)bands,
                 std::vector<double>((size_t)width * height, 0.0));
    for (int b = 0; b < bands; ++b)
    {
        // a fresh dataset without --init starts at the nodata value
        double iv = r.get("nodata") ? rzAtof(r.str("nodata")) : 0.0;
        if (!initList.empty())
        {
            iv = 0.0;
            if (initList.size() == 1)
                iv = initList[0];
            else if ((size_t)b < initList.size())
                iv = initList[(size_t)b];
        }
        if (iv != 0.0)
            std::fill(t.buf[(size_t)b].begin(),
                      t.buf[(size_t)b].end(), iv);
    }

    const double gt[6] = {minX, xres, 0, maxY, 0, -yres};
    rzApplyGtTags(p, gt);
    p.hasGT = true;
    if (haveSrs)
        p.srs = &outSrs;
    p.hasNodata = true;
    p.nodata = r.get("nodata") ? rzAtof(r.str("nodata")) : 0.0;
    p.nodataLate = true;
    if (type == DType::Int64)
    {
        // the reference stores Int64 nodata by re-parsing the double's
        // text as an integer prefix ("-9.22e+18" becomes -9)
        char b[64];
        snprintf(b, sizeof(b), "%.17g", p.nodata);
        p.nodataText = std::to_string(strtoll(b, nullptr, 10));
    }

    auto writeOut = [&](int rc) -> int {
        std::vector<std::vector<uint8_t>> pixels(
            (size_t)bands,
            std::vector<uint8_t>((size_t)width * height *
                                 dtypeSizeBytes(type)));
        const int pxSize = dtypeSizeBytes(type);
        for (int b = 0; b < bands; ++b)
        {
            uint8_t *q = pixels[(size_t)b].data();
            const double *src = t.buf[(size_t)b].data();
            for (size_t i = 0; i < (size_t)width * height; ++i)
                rasterEncodeReal(type, q + i * pxSize,
                                 rasterFinishReal(src[i], type), 0.0);
        }
        p.pixels = &pixels;
        std::string werr;
        if (!isMem && !gtiffWrite(output, p, werr))
        {
            if (werr != "reported")
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "rasterize: " + werr);
            return 1;
        }
        return rc;
    };

    RzBurnCtx c;
    c.t = &t;
    c.allTouched = allTouched;
    c.use3d = use3d;
    memcpy(c.xf.gt, gt, sizeof(c.xf.gt));

    if (whereSet && layer)
        if (!ogrApplyAttributeFilter(*layer, whereClause, false))
            return writeOut(1);

    int attrIdx = -1;
    if (attrSet && layer)
    {
        for (size_t f = 0; f < layer->fields.size(); ++f)
            if (strEqualNoCase(layer->fields[f].name, attrName))
            {
                attrIdx = (int)f;
                break;
            }
        if (attrIdx < 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Failed to find field " + attrName +
                            " on layer " + layer->name + ".");
            return writeOut(1);
        }
    }

    if (layer)
    {
        bool attrWarned = false;
        bool exactWarned = false;
        std::vector<double> baseBurn((size_t)bands, 255.0);
        for (int i = 0; i < bands; ++i)
            if (!burnList.empty())
                baseBurn[(size_t)i] =
                    burnList[std::min((size_t)i, burnList.size() - 1)];

        if (invert)
        {
            RzShape rings;
            rings.partSize.push_back(4);
            const double fx[4] = {-2.0, width + 2.0, width + 2.0,
                                  -2.0};
            const double fy[4] = {-2.0, -2.0, height + 2.0,
                                  height + 2.0};
            for (int i = 0; i < 4; ++i)
            {
                rings.xs.push_back(fx[i]);
                rings.ys.push_back(fy[i]);
                rings.vs.push_back(0.0);
            }
            bool sawNonPolygon = false;
            bool haveAttr = false;
            for (const auto &f : layer->features)
            {
                if (attrIdx >= 0 && !haveAttr &&
                    (size_t)attrIdx < f.values.size())
                {
                    const double v = rzFieldAsDouble(
                        f.values[(size_t)attrIdx], f.fid, attrWarned,
                        type, exactWarned);
                    for (auto &bv : baseBurn)
                        bv = v;
                    haveAttr = true;
                }
                if (!f.hasGeom || rzGeomIsEmpty(f.geom))
                    continue;
                rzCollectRings(f.geom, c.xf, rings, sawNonPolygon);
            }
            if (sawNonPolygon)
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Ignoring non-polygon geometries in -i "
                            "mode");
            t.burnVals = baseBurn;
            t.burn3d = use3d;
            t.beginShape();
            rzFilledPolygon(t, rings, use3d);
        }
        else
        {
            for (const auto &f : layer->features)
            {
                if (!f.hasGeom || rzGeomIsEmpty(f.geom))
                    continue;
                t.burnVals = baseBurn;
                if (attrIdx >= 0)
                {
                    const double v =
                        (size_t)attrIdx < f.values.size()
                            ? rzFieldAsDouble(
                                  f.values[(size_t)attrIdx],
                                  f.fid, attrWarned, type,
                                  exactWarned)
                            : 0.0;
                    for (auto &bv : t.burnVals)
                        bv = v;
                }
                t.burn3d = use3d;
                t.beginShape();
                rzBurnGeometry(c, f.geom);
            }
        }
    }

    if (!quiet)
        printProgress();
    return writeOut(0);
}

// ------------------------------------------------------------------
// validation hooks
// ------------------------------------------------------------------

std::string rasterizeArgValueCheck(const std::string &argName,
                                   const std::string &value)
{
    if (argName == "crs")
    {
        bool ok = false;
        Srs s = Srs::fromCliInput(value, ok, true);
        if (!ok || !s.valid())
            return "Invalid value for 'crs' argument";
    }
    if (argName == "output-format")
    {
        if (strEqualNoCase(value, "VRT"))
            return "\x07VRT output is not supported.";
        if (strEqualNoCase(value, "GDALG"))
            return "\x07GDALG output is not supported.";
        std::string canon;
        std::string issue = rasterOutFormatIssue(value, canon);
        if (!issue.empty())
            return issue;
    }
    return "";
}

int rasterizeArgCheck(const std::string &argName, ParseResult &r)
{
    if (argName == "band")
    {
        const ArgValue *v = r.get("band");
        if (v)
            for (const auto &sv : v->values)
                if (atoi(sv.c_str()) < 1)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Value of 'band' should greater or "
                                "equal to 1.");
                    handlerPrintUsage();
                    return 1;
                }
    }
    return 0;
}

bool rasterizePostValidator(const CmdSpec &, ParseResult &r, bool)
{
    const std::string of = r.str("output-format");
    const std::string out = r.str("output");
    if (strEqualNoCase(of, "MEM") || strEqualNoCase(of, "Memory") ||
        strEqualNoCase(of, "stream"))
        return false;
    if (of.empty() && strEndsWith(strToLower(out), ".gdalg.json"))
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "rasterize: GDALG output is not supported");
        return true;
    }
    if (out.empty())
        return false;
    const bool ow = r.flag("overwrite");
    const bool fam = r.flag("update") || r.flag("add");
    const bool exists = rzFileExists(out);
    if (fam && !ow)
    {
        // the update-family output opens during validation
        if (!exists)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        out + ": No such file or directory");
            return true;
        }
        if (rzIsDir(out))
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        out + ": Is a directory");
            return true;
        }
        std::string oerr;
        cplPushQuietHandler();
        auto probe = openRaster(out, oerr);
        cplPopHandler();
        if (!probe)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + out +
                            "' not recognized as being in a supported "
                            "file format.");
            return true;
        }
        return false;
    }
    if (!exists)
        return false;
    if (!ow && !fam)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "rasterize: " + outputExistsKind(out) + " '" + out +
                        "' already exists. You may specify the "
                        "--overwrite/--update option.");
        return true;
    }
    // --overwrite: QuietDelete semantics
    const std::string kind = outputExistsKind(out);
    if (kind == "Directory")
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "rasterize: Directory '" + out +
                        "' already exists, but is not recognized as a "
                        "valid GDAL dataset. Please manually delete it "
                        "before retrying");
        return true;
    }
    if (kind == "Dataset")
        overwriteDeleteFileset(out);
    return false;
}

}  // namespace

void registerRasterRasterizeHandler()
{
    registerHandler("vector_rasterize", rasterizeHandler);
    registerArgValueCheck("vector_rasterize", rasterizeArgValueCheck);
    registerArgCheck("vector_rasterize", rasterizeArgCheck);
    registerPostValidator("vector_rasterize", rasterizePostValidator);
}
