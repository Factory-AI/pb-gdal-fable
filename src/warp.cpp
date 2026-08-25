// raster reproject: warped dataset core, output-grid prediction and
// VRTWarpedDataset serialization
#include "warp.h"

#include "cpl.h"
#include "engine.h"
#include "proj_min.h"
#include "spec.h"
#include "util.h"
#include "vsi.h"
#include "xml_min.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>

namespace
{

std::string fmtG(double d)
{
    return strPrintf("%g", d);
}

std::string fmt17(double d)
{
    return strPrintf("%.17g", d);
}

std::string fmt16(double d)
{
    return strPrintf("%.16g", d);
}

std::string join17(const double *v, int n)
{
    std::string r;
    for (int i = 0; i < n; ++i)
    {
        if (i)
            r += ",";
        r += fmt17(v[i]);
    }
    return r;
}

std::string wxmlTextEsc(const std::string &s)
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

std::string wDirNameOf(const std::string &p)
{
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "" : p.substr(0, slash);
}

std::string wVrtRelativePath(const std::string &input,
                             const std::string &output, int &relative)
{
    std::string outDir = wDirNameOf(output);
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

// GDALInvGeoTransform: simplified path for axis-aligned transforms
bool invGeoTransform(const double *gt, double *inv)
{
    if (gt[2] == 0.0 && gt[4] == 0.0 && gt[1] != 0.0 && gt[5] != 0.0)
    {
        inv[0] = -gt[0] / gt[1];
        inv[1] = 1.0 / gt[1];
        inv[2] = 0.0;
        inv[3] = -gt[3] / gt[5];
        inv[4] = 0.0;
        inv[5] = 1.0 / gt[5];
        return true;
    }
    double det = gt[1] * gt[5] - gt[2] * gt[4];
    if (std::fabs(det) < 1e-15)
        return false;
    double idet = 1.0 / det;
    inv[1] = gt[5] * idet;
    inv[2] = -gt[2] * idet;
    inv[4] = -gt[4] * idet;
    inv[5] = gt[1] * idet;
    inv[0] = (gt[2] * gt[3] - gt[0] * gt[5]) * idet;
    inv[3] = (-gt[1] * gt[3] + gt[0] * gt[4]) * idet;
    return true;
}

// normalized (GIS axis order) coordinate operation; writes outputs even
// on failure so callers replicate PROJ's HUGE_VAL propagation
bool pjXform(PJ *op, PJ_DIRECTION dir, double &x, double &y)
{
    if (!op)
        return true;
    proj_errno_reset(op);
    PJ_COORD c = proj_coord(x, y, 0, HUGE_VAL);
    PJ_COORD o = proj_trans(op, dir, c);
    x = o.v[0];
    y = o.v[1];
    return proj_errno(op) == 0 && std::isfinite(x) && std::isfinite(y);
}

PJ *makeOp(PJ *srcPj, PJ *dstPj)
{
    PJ *raw = proj_create_crs_to_crs_from_pj(projCtx(), srcPj, dstPj,
                                             nullptr, nullptr);
    if (!raw)
        return nullptr;
    PJ *norm = proj_normalize_for_visualization(projCtx(), raw);
    if (norm)
    {
        proj_destroy(raw);
        return norm;
    }
    return raw;
}

// vertical shift applies when either side is compound or a projected/
// geographic CRS with a third axis
static bool warpCrsHasVertical(PJ *crs)
{
    if (!crs)
        return false;
    PJ_TYPE t = proj_get_type(crs);
    if (t == PJ_TYPE_COMPOUND_CRS)
        return true;
    if (t != PJ_TYPE_PROJECTED_CRS && t != PJ_TYPE_GEOGRAPHIC_CRS &&
        t != PJ_TYPE_GEOGRAPHIC_2D_CRS && t != PJ_TYPE_GEOGRAPHIC_3D_CRS &&
        t != PJ_TYPE_GEODETIC_CRS)
        return false;
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), crs);
    if (!cs)
        return false;
    int n = proj_cs_get_axis_count(projCtx(), cs);
    proj_destroy(cs);
    return n == 3;
}

static PJ *makeOpZ(PJ *srcPj, PJ *dstPj)
{
    PJ *s3 = proj_crs_promote_to_3D(projCtx(), nullptr, srcPj);
    PJ *d3 = proj_crs_promote_to_3D(projCtx(), nullptr, dstPj);
    PJ *op = makeOp(s3 ? s3 : srcPj, d3 ? d3 : dstPj);
    if (s3)
        proj_destroy(s3);
    if (d3)
        proj_destroy(d3);
    return op;
}

struct SuggestOut
{
    double minx = HUGE_VAL, miny = HUGE_VAL;
    double maxx = -HUGE_VAL, maxy = -HUGE_VAL;
    double psx = 0, psy = 0;
    bool any = false;
    // unrounded corner-sampled extent: a same-horizontal warp with a
    // vertical member keeps the exact grid for the suggested output but
    // an explicit --resolution reads the raw sampled bounds instead
    double rminx = HUGE_VAL, rminy = HUGE_VAL;
    double rmaxx = -HUGE_VAL, rmaxy = -HUGE_VAL;
    bool haveRaw = false;
};

SuggestOut suggestedOutput(const double *sgt, int sw, int sh, PJ *op,
                           bool sameCrs, bool vertOp)
{
    SuggestOut s;
    // with no reprojection step the reference short-circuits to the
    // source geotransform verbatim (anisotropic pixel sizes preserved)
    if (sameCrs && sgt[2] == 0 && sgt[4] == 0 && sgt[1] > 0 && sgt[5] < 0)
    {
        s.minx = sgt[0];
        s.maxy = sgt[3];
        s.maxx = sgt[0] + sw * sgt[1];
        s.miny = sgt[3] + sh * sgt[5];
        s.psx = sgt[1];
        s.psy = -sgt[5];
        s.any = true;
        if (vertOp && op)
        {
            for (int j = 0; j <= 20; ++j)
            {
                for (int i = 0; i <= 20; ++i)
                {
                    double px = sw * (double)i / 20.0;
                    double py = sh * (double)j / 20.0;
                    double gx = sgt[0] + px * sgt[1] + py * sgt[2];
                    double gy = sgt[3] + px * sgt[4] + py * sgt[5];
                    if (!pjXform(op, PJ_FWD, gx, gy))
                        continue;
                    s.haveRaw = true;
                    s.rminx = std::min(s.rminx, gx);
                    s.rmaxx = std::max(s.rmaxx, gx);
                    s.rminy = std::min(s.rminy, gy);
                    s.rmaxy = std::max(s.rmaxy, gy);
                }
            }
        }
        return s;
    }
    double c0x = 0, c0y = 0, c1x = 0, c1y = 0;
    for (int j = 0; j <= 20; ++j)
    {
        for (int i = 0; i <= 20; ++i)
        {
            double px = sw * (double)i / 20.0;
            double py = sh * (double)j / 20.0;
            double gx = sgt[0] + px * sgt[1] + py * sgt[2];
            double gy = sgt[3] + px * sgt[4] + py * sgt[5];
            bool ok = pjXform(op, PJ_FWD, gx, gy);
            if (i == 0 && j == 0)
            {
                c0x = gx;
                c0y = gy;
            }
            if (i == 20 && j == 20)
            {
                c1x = gx;
                c1y = gy;
            }
            if (!ok)
                continue;
            s.any = true;
            s.minx = std::min(s.minx, gx);
            s.maxx = std::max(s.maxx, gx);
            s.miny = std::min(s.miny, gy);
            s.maxy = std::max(s.maxy, gy);
        }
    }
    double dx = c1x - c0x, dy = c1y - c0y;
    s.psx = std::sqrt(dx * dx + dy * dy) /
            std::sqrt((double)sw * sw + (double)sh * sh);
    s.psy = s.psx;
    return s;
}

// bbox-only pixel size: min over a 21x21 grid of the pointwise mean of
// the per-axis dst->src-pixel scales (finite differences at 1e-7 of the
// extent)
double bboxPixelSize(const double *sinv, PJ *op, const double *bb)
{
    double w = bb[2] - bb[0], h = bb[3] - bb[1];
    double ex = w * 1e-7, ey = h * 1e-7;
    double ps = 1e30;
    auto toSrcPx = [&](double gx, double gy, double &px, double &py)
    {
        if (!pjXform(op, PJ_INV, gx, gy))
            return false;
        px = sinv[0] + gx * sinv[1] + gy * sinv[2];
        py = sinv[3] + gx * sinv[4] + gy * sinv[5];
        return true;
    };
    for (int j = 0; j <= 20; ++j)
    {
        for (int i = 0; i <= 20; ++i)
        {
            double gx = bb[0] + w * (double)i / 20.0;
            double gy = bb[1] + h * (double)j / 20.0;
            double p0x, p0y, p1x, p1y, p2x, p2y;
            if (!toSrcPx(gx, gy, p0x, p0y) ||
                !toSrcPx(gx + ex, gy, p1x, p1y) ||
                !toSrcPx(gx, gy + ey, p2x, p2y))
                continue;
            double sx = ex / std::hypot(p1x - p0x, p1y - p0y);
            double sy = ey / std::hypot(p2x - p0x, p2y - p0y);
            ps = std::min(ps, (sx + sy) / 2.0);
        }
    }
    return ps;
}

struct GridResult
{
    long long W = 0, H = 0;
    double gt[6] = {0, 1, 0, 0, 0, -1};
};

GridResult predictGrid(const WarpParams &p, const double *bboxDst,
                       const double *sgt, const double *sinv, int sw,
                       int sh, PJ *op, bool sameCrs, bool vertOp)
{
    SuggestOut s = suggestedOutput(sgt, sw, sh, op, sameCrs, vertOp);
    double minx = s.minx, miny = s.miny, maxx = s.maxx, maxy = s.maxy;
    bool haveBbox = bboxDst != nullptr;
    if (haveBbox)
    {
        minx = bboxDst[0];
        miny = bboxDst[1];
        maxx = bboxDst[2];
        maxy = bboxDst[3];
    }
    GridResult g;
    auto setGt = [&](double ox, double rx, double oy, double ry)
    {
        g.gt[0] = ox;
        g.gt[1] = rx;
        g.gt[2] = 0;
        g.gt[3] = oy;
        g.gt[4] = 0;
        g.gt[5] = -ry;
    };
    if (p.hasRes)
    {
        if (!haveBbox && s.haveRaw)
        {
            minx = s.rminx;
            miny = s.rminy;
            maxx = s.rmaxx;
            maxy = s.rmaxy;
        }
        double rx = p.resX, ry = p.resY;
        if (p.tap)
        {
            if (haveBbox)
            {
                minx = std::floor(minx / rx) * rx;
                maxx = std::ceil(maxx / rx) * rx;
                miny = std::floor(miny / ry) * ry;
                maxy = std::ceil(maxy / ry) * ry;
            }
            else
            {
                minx = std::floor(minx / rx + 0.5) * rx;
                maxx = std::floor(maxx / rx + 0.5) * rx;
                miny = std::floor(miny / ry + 0.5) * ry;
                maxy = std::floor(maxy / ry + 0.5) * ry;
            }
            g.W = std::max(1LL, (long long)((maxx - minx) / rx + 0.5));
            g.H = std::max(1LL, (long long)((maxy - miny) / ry + 0.5));
            setGt(minx, rx, maxy, ry);
            return g;
        }
        double wext, hext;
        if (haveBbox || s.haveRaw)
        {
            wext = maxx - minx;
            hext = maxy - miny;
        }
        else
        {
            // the suggested size snaps the extent before the round
            long long w0 = (long long)((maxx - minx) / s.psx + 0.5);
            long long h0 = (long long)((maxy - miny) / s.psy + 0.5);
            wext = std::min(maxx - minx, (double)w0 * s.psx);
            hext = std::min(maxy - miny, (double)h0 * s.psy);
        }
        g.W = std::max(1LL, (long long)(wext / rx + 0.5));
        g.H = std::max(1LL, (long long)(hext / ry + 0.5));
        setGt(minx, rx, maxy, ry);
        return g;
    }
    if (p.hasSize && (p.sizeW || p.sizeH))
    {
        if (!haveBbox && s.haveRaw)
        {
            minx = s.rminx;
            miny = s.rminy;
            maxx = s.rmaxx;
            maxy = s.rmaxy;
        }
        else if (!haveBbox)
        {
            long long w0 = (long long)((maxx - minx) / s.psx + 0.5);
            long long h0 = (long long)((maxy - miny) / s.psy + 0.5);
            maxx = minx + (double)w0 * s.psx;
            miny = maxy - (double)h0 * s.psy;
        }
        if (p.sizeW && p.sizeH)
        {
            g.W = p.sizeW;
            g.H = p.sizeH;
            setGt(minx, (maxx - minx) / (double)g.W, maxy,
                  (maxy - miny) / (double)g.H);
            return g;
        }
        if (p.sizeW)
        {
            g.W = p.sizeW;
            double rx = (maxx - minx) / (double)g.W;
            g.H = (long long)((maxy - miny) / rx + 0.5);
            setGt(minx, rx, maxy, rx);
            return g;
        }
        g.H = p.sizeH;
        double ry = (maxy - miny) / (double)g.H;
        g.W = (long long)((maxx - minx) / ry + 0.5);
        setGt(minx, ry, maxy, ry);
        return g;
    }
    if (haveBbox)
    {
        double ps2 = bboxPixelSize(sinv, op, bboxDst);
        g.W = std::max(1LL, (long long)((maxx - minx) / ps2 + 0.5));
        g.H = std::max(1LL, (long long)((maxy - miny) / ps2 + 0.5));
        setGt(minx, (maxx - minx) / (double)g.W, maxy,
              (maxy - miny) / (double)g.H);
        return g;
    }
    g.W = (long long)((maxx - minx) / s.psx + 0.5);
    g.H = (long long)((maxy - miny) / s.psy + 0.5);
    setGt(minx, s.psx, maxy, s.psy);
    return g;
}

// 21 points per edge, matching OCTTransformBounds densification
bool transformBboxDensified(PJ *op, const double *in, double *out)
{
    double minx = HUGE_VAL, miny = HUGE_VAL;
    double maxx = -HUGE_VAL, maxy = -HUGE_VAL;
    bool any = false;
    auto probe = [&](double x, double y)
    {
        if (!pjXform(op, PJ_FWD, x, y))
            return;
        any = true;
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
    };
    for (int i = 0; i <= 20; ++i)
    {
        double fx = in[0] + (in[2] - in[0]) * (double)i / 20.0;
        double fy = in[1] + (in[3] - in[1]) * (double)i / 20.0;
        probe(fx, in[1]);
        probe(fx, in[3]);
        probe(in[0], fy);
        probe(in[2], fy);
    }
    if (!any)
        return false;
    out[0] = minx;
    out[1] = miny;
    out[2] = maxx;
    out[3] = maxy;
    return true;
}

double readNativeAsDouble(DType t, const uint8_t *p)
{
    switch (t)
    {
        case DType::Byte:
            return *p;
        case DType::Int8:
            return *(const int8_t *)p;
        case DType::UInt16:
            return *(const uint16_t *)p;
        case DType::Int16:
            return *(const int16_t *)p;
        case DType::UInt32:
            return *(const uint32_t *)p;
        case DType::Int32:
            return *(const int32_t *)p;
        case DType::UInt64:
            return (double)*(const uint64_t *)p;
        case DType::Int64:
            return (double)*(const int64_t *)p;
        case DType::Float32:
            return *(const float *)p;
        case DType::Float64:
            return *(const double *)p;
        case DType::CInt16:
            return *(const int16_t *)p;
        case DType::CInt32:
            return *(const int32_t *)p;
        case DType::CFloat32:
            return *(const float *)p;
        case DType::CFloat64:
            return *(const double *)p;
        default:
            return 0;
    }
}

void writeNativeFromDouble(DType t, double v, uint8_t *p)
{
    auto clampll = [&](double lo, double hi) -> long long
    {
        double r = std::floor(v + 0.5);
        if (r < lo)
            r = lo;
        if (r > hi)
            r = hi;
        return (long long)r;
    };
    switch (t)
    {
        case DType::Byte:
            *p = (uint8_t)clampll(0, 255);
            break;
        case DType::Int8:
            *(int8_t *)p = (int8_t)clampll(-128, 127);
            break;
        case DType::UInt16:
            *(uint16_t *)p = (uint16_t)clampll(0, 65535);
            break;
        case DType::Int16:
        case DType::CInt16:
            *(int16_t *)p = (int16_t)clampll(-32768, 32767);
            break;
        case DType::UInt32:
            *(uint32_t *)p = (uint32_t)clampll(0, 4294967295.0);
            break;
        case DType::Int32:
        case DType::CInt32:
            *(int32_t *)p =
                (int32_t)clampll(-2147483648.0, 2147483647.0);
            break;
        case DType::UInt64:
        {
            double r = std::floor(v + 0.5);
            if (r < 0)
                r = 0;
            if (r > 1.8446744073709552e19)
                r = 1.8446744073709552e19;
            *(uint64_t *)p = (uint64_t)r;
            break;
        }
        case DType::Int64:
        {
            double r = std::floor(v + 0.5);
            if (r < -9.2233720368547758e18)
                r = -9.2233720368547758e18;
            if (r > 9.2233720368547758e18)
                r = 9.2233720368547758e18;
            *(int64_t *)p = (int64_t)r;
            break;
        }
        case DType::Float32:
        case DType::CFloat32:
            *(float *)p = (float)v;
            break;
        case DType::Float64:
        case DType::CFloat64:
            *(double *)p = v;
            break;
        default:
            break;
    }
}

const char *resampleAlgName(const std::string &r)
{
    if (r == "nearest")
        return "NearestNeighbour";
    if (r == "bilinear")
        return "Bilinear";
    if (r == "cubic")
        return "Cubic";
    if (r == "cubicspline")
        return "CubicSpline";
    if (r == "lanczos")
        return "Lanczos";
    if (r == "average")
        return "Average";
    if (r == "rms")
        return "RMS";
    if (r == "mode")
        return "Mode";
    if (r == "min")
        return "Minimum";
    if (r == "max")
        return "Maximum";
    if (r == "med")
        return "Median";
    if (r == "q1")
        return "Quartile1";
    if (r == "q3")
        return "Quartile3";
    if (r == "sum")
        return "Sum";
    return "NearestNeighbour";
}

int hardwareCpuCount()
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

struct BandNd
{
    bool has = false;
    double v = 0;
};

class WarpedDataset final : public RasterDatasetBase
{
  public:
    bool warpProduced() const override { return true; }
    std::unique_ptr<RasterDatasetBase> src;
    PJ *op = nullptr;  // owned
    double srcGt[6], srcInv[6], dstInv[6];
    double maxErr = 0.125;
    std::string resampling = "nearest";
    std::vector<BandNd> srcNd, dstNd;
    bool unifiedSrcNodata = false;
    bool unifiedDbg_ = false;
    bool initDestNdErr_ = false;
    bool alphaDbg_ = true;
    bool hasAlphaMaxOv_ = false;
    double alphaMaxOv_ = 0;
    int maskAllValidCache_ = -1;
    bool alpha = false;
    std::vector<std::string> woUser, toUser;
    bool etExplicit = false;
    double etValue = 0;
    std::string numThreadsXml;
    Srs srcSrsOverride;
    bool haveSrcSrsOverride = false;
    bool srcSrsIsDst = false;  // src srs moved into this->srs (same CRS)

    std::vector<int64_t> map_;
    std::vector<double> mapX_, mapY_;
    std::vector<char> masked_;
    // per-destination-pixel additive vertical shift (compound/3D CRS)
    PJ *opZ = nullptr;  // owned
    std::vector<double> zAdd_;
    std::string vertUnitName_;
    bool vertWarp_ = false;
    bool explicitExtent_ = false;
    bool mapped_ = false;
    bool kernScaleDone_ = false;
    bool scaledKernel_ = false;
    bool sameCrsWarp_ = false;
    double xKernScale_ = 1.0, yKernScale_ = 1.0;
    double dbgScaleX_ = 1.0, dbgScaleY_ = 1.0;
    bool dbgAoiSet_ = false;
    double dbgAoi_[4] = {0, 0, 0, 0};
    bool dbgWrap_ = false;
    bool dstNdCli_ = false;
    size_t dstNdCliCount_ = 0;
    bool footDone_ = false, footAny_ = false;
    int footWin_[4] = {0, 0, 0, 0};
    int identAligned_ = -1;
    std::vector<std::vector<uint8_t>> srcRaw_;
    std::vector<std::vector<double>> bilOut_;
    std::vector<std::vector<double>> cubOut_;
    bool shiftAllowed_ = true;
    bool shiftWarned_ = false;
    bool blockChunked_ = false;
    // windowed-read emulation: destination sample coords come from a
    // global pixel grid rather than this warp's own geotransform
    bool pixGrid_ = false;
    double pgOX_ = 0, pgOY_ = 0;
    long long pgIX_ = 0, pgIY_ = 0;

    bool geoDoubleOrphanHint() override
    {
        // the orphan slot appears only when an explicit target extent
        // (--bbox or --like) meets an empty source window; a grid whose
        // extent came from the suggested output never carries it
        if (!explicitExtent_)
            return false;
        computeMapping();
        for (int64_t m : map_)
            if (m >= 0)
                return false;
        return true;
    }

    static double shiftedNodata(DType t, double nd)
    {
        switch (t)
        {
            case DType::Float32:
            case DType::CFloat32:
                return (double)std::nextafter((float)nd, HUGE_VALF);
            case DType::Float64:
            case DType::CFloat64:
                return std::nextafter(nd, HUGE_VAL);
            case DType::Byte:
            case DType::UInt16:
            case DType::UInt32:
            case DType::UInt64:
                return nd - 1 < 0 ? nd + 1 : nd - 1;
            case DType::Int8:
                return nd - 1 < -128 ? nd + 1 : nd - 1;
            case DType::Int16:
            case DType::CInt16:
                return nd - 1 < -32768 ? nd + 1 : nd - 1;
            case DType::Int32:
            case DType::CInt32:
                return nd - 1 < -2147483648.0 ? nd + 1 : nd - 1;
            case DType::Int64:
                return nd - 1 < -9.2233720368547758e18 ? nd + 1 : nd - 1;
            default:
                return nd - 1;
        }
    }

    ~WarpedDataset() override
    {
        if (op)
            proj_destroy(op);
        if (opZ)
            proj_destroy(opZ);
    }

    WarpedDataset(std::unique_ptr<RasterDatasetBase> s, const GridResult &g)
        : src(std::move(s))
    {
        path = src->path;
        debugPtr = src->debugPtr;
        driverShort = src->driverShort;
        driverLong = src->driverLong;
        width = (int)g.W;
        height = (int)g.H;
        hasGT = true;
        memcpy(gt, g.gt, sizeof gt);
        memcpy(srcGt, src->gt, sizeof srcGt);
        invGeoTransform(srcGt, srcInv);
        invGeoTransform(gt, dstInv);
        metadata = src->metadata;
        domainOrder = src->domainOrder;
        sortedDomains = src->sortedDomains;
        xmlDomains = src->xmlDomains;
        files = src->files;
        deferredWarnings = src->deferredWarnings;
        src->deferredWarnings.clear();
        pamPath = src->pamPath;
        pamExists = src->pamExists;
        pamSuppressItems = true;
        auto dropDomain = [&](const char *d)
        {
            metadata.erase(d);
            domainOrder.erase(
                std::remove(domainOrder.begin(), domainOrder.end(), d),
                domainOrder.end());
            sortedDomains.erase(
                std::remove(sortedDomains.begin(), sortedDomains.end(), d),
                sortedDomains.end());
        };
        dropDomain("IMAGE_STRUCTURE");
    }

    void finishBands()
    {
        int nb = (int)src->bands.size() + (alpha ? 1 : 0);
        for (int i = 0; i < nb; ++i)
        {
            bool isAlpha = alpha && i == nb - 1;
            const Band &sb = src->bands[isAlpha ? 0 : (size_t)i];
            Band b;
            b.index = i + 1;
            b.type = sb.type;
            b.blockX = std::min(width, 512);
            b.blockY = std::min(height, 128);
            if (isAlpha)
                b.colorInterp = "Alpha";
            else
            {
                b.colorInterp = sb.colorInterp;
                b.colorTable = sb.colorTable;
                if (!vertUnitName_.empty())
                    b.unitType = vertUnitName_;
                if (dstNd[(size_t)i].has)
                {
                    b.hasNodata = true;
                    b.nodata = dstNd[(size_t)i].v;
                }
            }
            bands.push_back(std::move(b));
        }
        if ((int)bands.size() > 1)
        {
            metadata["IMAGE_STRUCTURE"] = {{"INTERLEAVE", "PIXEL"}};
            domainOrder.push_back("IMAGE_STRUCTURE");
            sortedDomains.push_back("IMAGE_STRUCTURE");
            std::sort(sortedDomains.begin(), sortedDomains.end());
        }
    }

    double alphaMax() const
    {
        if (hasAlphaMaxOv_)
            return alphaMaxOv_;
        DType t = bands.back().type;
        if (t == DType::Int16)
            return 32767;
        if (t == DType::UInt16)
            return 65535;
        return 255;
    }

    // recursive scanline subdivision replicating GDALApproxTransformer;
    // the vertical-shift channel is linearly interpolated wherever the
    // x/y approximation is accepted, exact elsewhere
    void approxRow(const std::vector<double> &ex,
                   const std::vector<double> &ey,
                   const std::vector<char> &okv, std::vector<double> &ax,
                   std::vector<double> &ay,
                   const std::vector<double> *ez = nullptr,
                   std::vector<double> *az = nullptr)
    {
        int W = (int)ex.size();
        auto copyExact = [&](int i0, int i1)
        {
            for (int i = i0; i <= i1; ++i)
            {
                ax[i] = ex[i];
                ay[i] = ey[i];
                if (ez)
                    (*az)[i] = (*ez)[i];
            }
        };
        // GDALApproxTransform computes every point exactly when the
        // whole line carries five or fewer points
        if (W <= 5 || maxErr <= 0)
        {
            copyExact(0, W - 1);
            return;
        }
        std::function<void(int, int, double, double, bool, double, double,
                           bool, double, double, bool, int)>
            rec = [&](int i0, int i1, double p0x, double p0y, bool ok0,
                      double pmx, double pmy, bool okm, double p1x,
                      double p1y, bool ok1, int mid)
        {
            if (!ok0 || !okm || !ok1)
            {
                copyExact(i0, i1);
                return;
            }
            double dx = (double)(i1 - i0);
            double dX = (p1x - p0x) / dx;
            double dY = (p1y - p0y) / dx;
            double dm = (double)(mid - i0);
            if (std::max(std::fabs(p0x + dX * dm - pmx),
                         std::fabs(p0y + dY * dm - pmy)) > maxErr)
            {
                int l1 = mid - 1;
                int nl = l1 - i0 + 1;
                if (nl <= 2)
                    copyExact(i0, l1);
                else
                {
                    int ml = i0 + (nl - 1) / 2;
                    rec(i0, l1, p0x, p0y, ok0, ex[ml], ey[ml], okv[ml],
                        ex[l1], ey[l1], okv[l1], ml);
                }
                int nr = i1 - mid + 1;
                if (nr <= 2)
                    copyExact(mid, i1);
                else
                {
                    int mr = mid + (nr - 1) / 2;
                    rec(mid, i1, pmx, pmy, okm, ex[mr], ey[mr], okv[mr],
                        p1x, p1y, ok1, mr);
                }
                return;
            }
            ax[i0] = p0x;
            ay[i0] = p0y;
            ax[i1] = p1x;
            ay[i1] = p1y;
            for (int i = i0 + 1; i < i1; ++i)
            {
                double d = (double)(i - i0);
                ax[i] = p0x + dX * d;
                ay[i] = p0y + dY * d;
            }
            if (ez)
            {
                double z0 = (*ez)[i0], z1 = (*ez)[i1];
                (*az)[i0] = z0;
                (*az)[i1] = z1;
                for (int i = i0 + 1; i < i1; ++i)
                {
                    double f = (double)(i - i0) / dx;
                    (*az)[i] = z0 + (z1 - z0) * f;
                }
            }
        };
        int m = (W - 1) / 2;
        rec(0, W - 1, ex[0], ey[0], okv[0], ex[m], ey[m], okv[m], ex[W - 1],
            ey[W - 1], okv[W - 1], m);
    }

    void computeMapping()
    {
        if (mapped_)
            return;
        mapped_ = true;
        int W = width, H = height;
        int sw = src->width, sh = src->height;
        map_.assign((size_t)W * H, -1);
        bool keepXY = resampling != "nearest";
        if (keepXY)
        {
            mapX_.assign((size_t)W * H, 0.0);
            mapY_.assign((size_t)W * H, 0.0);
        }
        std::vector<double> ex(W), ey(W), ax(W), ay(W);
        std::vector<char> okv(W);
        std::vector<double> ez, az;
        if (opZ)
        {
            zAdd_.assign((size_t)W * H, 0.0);
            ez.assign(W, 0.0);
            az.assign(W, 0.0);
        }
        for (int j = 0; j < H; ++j)
        {
            double dpy = j + 0.5;
            for (int i = 0; i < W; ++i)
            {
                double gx = gt[0] + (i + 0.5) * gt[1] + dpy * gt[2];
                double gy = gt[3] + (i + 0.5) * gt[4] + dpy * gt[5];
                if (pixGrid_)
                {
                    gx = pgOX_ +
                         ((double)(pgIX_ + i) + 0.5) * gt[1];
                    gy = pgOY_ +
                         ((double)(pgIY_ + j) + 0.5) * gt[5];
                }
                bool ok;
                if (opZ)
                {
                    proj_errno_reset(opZ);
                    PJ_COORD c = proj_coord(gx, gy, 0, HUGE_VAL);
                    PJ_COORD o = proj_trans(opZ, PJ_INV, c);
                    gx = o.v[0];
                    gy = o.v[1];
                    ez[i] = std::isfinite(o.v[2]) ? -o.v[2] : 0.0;
                    ok = proj_errno(opZ) == 0 && std::isfinite(gx) &&
                         std::isfinite(gy);
                }
                else
                    ok = pjXform(op, PJ_INV, gx, gy);
                ex[i] = srcInv[0] + gx * srcInv[1] + gy * srcInv[2];
                ey[i] = srcInv[3] + gx * srcInv[4] + gy * srcInv[5];
                okv[i] = ok ? 1 : 0;
            }
            // a vertical-shift transform is never routed through the
            // scanline approximator: x, y and z stay exact per pixel
            if (opZ)
            {
                ax = ex;
                ay = ey;
            }
            else
                approxRow(ex, ey, okv, ax, ay);
            for (int i = 0; i < W; ++i)
            {
                if (!okv[i])
                    continue;
                double x = ax[i], y = ay[i];
                // approximate results falling outside the source get a
                // second, exact evaluation
                if (!(x >= 0 && x < sw && y >= 0 && y < sh))
                {
                    x = ex[i];
                    y = ey[i];
                }
                // the vertical shift is always the exact per-pixel
                // value, never the scanline approximation
                if (opZ)
                    zAdd_[(size_t)j * W + i] = ez[i];
                // nearest source index replicates the reference's
                // truncation with a +1e-10 guard against inverse-chain
                // FP dust landing just below an exact integer (probed:
                // identity --size 5,4 samples src row 7 from
                // 6.9999999999999943)
                if (x < 0 || y < 0)
                    continue;
                int fx = (int)(x + 1e-10), fy = (int)(y + 1e-10);
                if (fx < sw && fy < sh)
                {
                    map_[(size_t)j * W + i] =
                        (int64_t)fy * sw + (int64_t)fx;
                    if (keepXY)
                    {
                        mapX_[(size_t)j * W + i] = x;
                        mapY_[(size_t)j * W + i] = y;
                    }
                }
            }
        }
        // per-pixel unified nodata mask over the source
        bool anySrcNd = false;
        for (const auto &nd : srcNd)
            if (nd.has)
                anySrcNd = true;
        if (anySrcNd)
        {
            size_t n = (size_t)sw * sh;
            masked_.assign(n, 1);
            for (size_t bi = 0; bi < src->bands.size(); ++bi)
            {
                loadSrcRaw((int)bi + 1);
                const Band &sb = src->bands[bi];
                int esz = dtypeSizeBytes(sb.type);
                const uint8_t *raw = srcRaw_[bi].data();
                if (!srcNd[bi].has)
                {
                    if (unifiedSrcNodata)
                        continue;
                    std::fill(masked_.begin(), masked_.end(), 0);
                    break;
                }
                for (size_t k = 0; k < n; ++k)
                {
                    double v = readNativeAsDouble(sb.type, raw + k * esz);
                    bool m = v == srcNd[bi].v;
                    if (unifiedSrcNodata)
                    {
                        if (!m)
                            masked_[k] = 0;
                    }
                }
                if (!unifiedSrcNodata)
                    break;
            }
        }
    }

    bool loadSrcRaw(int band)
    {
        if (srcRaw_.size() < src->bands.size())
            srcRaw_.resize(src->bands.size());
        auto &buf = srcRaw_[(size_t)band - 1];
        if (!buf.empty())
            return true;
        return src->readBandRaw(band, buf);
    }

    bool bandMasked(size_t bi, int64_t srcIdx)
    {
        if (srcIdx < 0)
            return false;
        if (unifiedSrcNodata)
            return !masked_.empty() && masked_[(size_t)srcIdx];
        if (!srcNd[bi].has)
            return false;
        const Band &sb = src->bands[bi];
        int esz = dtypeSizeBytes(sb.type);
        double v = readNativeAsDouble(
            sb.type, srcRaw_[bi].data() + (size_t)srcIdx * esz);
        return v == srcNd[bi].v;
    }

    bool pixelInvalid(size_t bi, int64_t srcIdx)
    {
        if (srcIdx < 0)
            return true;
        if (unifiedSrcNodata)
            return !masked_.empty() && masked_[(size_t)srcIdx];
        return bandMasked(bi, srcIdx);
    }

    // reference 4x4 cubic convolution: Catmull-Rom weights evaluated in
    // the kernel's exact Horner form
    static double cubicConvolution(double d1, double d2, double d3,
                                   double f0, double f1, double f2,
                                   double f3)
    {
        return f1 + 0.5 * (d1 * (f2 - f0) +
                           d2 * (2.0 * f0 - 5.0 * f1 + 4.0 * f2 - f3) +
                           d3 * (3.0 * (f1 - f2) + f3 - f0));
    }

    static double gwkCubicWeight(double dfX)
    {
        double a = std::fabs(dfX);
        if (a <= 1.0)
        {
            double x2 = dfX * dfX;
            return x2 * (1.5 * a - 2.5) + 1.0;
        }
        if (a <= 2.0)
        {
            double x2 = dfX * dfX;
            return x2 * (-0.5 * a + 2.5) - 4.0 * a + 2.0;
        }
        return 0.0;
    }

    static double finalizeKernelValue(DType t, double v)
    {
        switch (t)
        {
            case DType::Byte:
                return std::min(std::max(std::floor(v + 0.5), 0.0), 255.0);
            case DType::Int8:
                return std::min(std::max(std::floor(v + 0.5), -128.0),
                                127.0);
            case DType::UInt16:
                return std::min(std::max(std::floor(v + 0.5), 0.0),
                                65535.0);
            case DType::Int16:
                return std::min(std::max(std::floor(v + 0.5), -32768.0),
                                32767.0);
            case DType::UInt32:
                return std::min(std::max(std::floor(v + 0.5), 0.0),
                                4294967295.0);
            case DType::Int32:
                return std::min(std::max(std::floor(v + 0.5),
                                         -2147483648.0),
                                2147483647.0);
            case DType::UInt64:
                return std::min(std::max(std::floor(v + 0.5), 0.0),
                                1.8446744073709552e19);
            case DType::Int64:
                return std::min(std::max(std::floor(v + 0.5),
                                         -9.2233720368547758e18),
                                9.2233720368547758e18);
            case DType::Float32:
                return (double)(float)v;
            default:
                return v;
        }
    }

    // Integer source window for the kernel debug line. The reference
    // window comes from the raw (unclipped) min/max of the destination
    // border inverse-transformed to source pixel space, sampled densely
    // (per destination pixel; measured: floor(min + 1e-6) transitions
    // exactly at the continuum edge minimum), then clamped to the
    // raster and grown by the kernel radius.
    void computeFootprint()
    {
        if (footDone_)
            return;
        footDone_ = true;
        footWin_[0] = 0;
        footWin_[1] = 0;
        footWin_[2] = src->width;
        footWin_[3] = src->height;
        double minX = HUGE_VAL, minY = HUGE_VAL;
        double maxX = -HUGE_VAL, maxY = -HUGE_VAL;
        int stepsX = std::min(std::max(width, 21), 4096);
        int stepsY = std::min(std::max(height, 21), 4096);
        for (int i = 0; i <= stepsX; ++i)
        {
            double r = (double)i / stepsX;
            double pts[2][2] = {{r * width, 0.0},
                                {r * width, (double)height}};
            for (auto &pt : pts)
            {
                double sx, sy;
                if (!invToSrcPx(pt[0], pt[1], sx, sy))
                    continue;
                footAny_ = true;
                minX = std::min(minX, sx);
                maxX = std::max(maxX, sx);
                minY = std::min(minY, sy);
                maxY = std::max(maxY, sy);
            }
        }
        for (int j = 0; j <= stepsY; ++j)
        {
            double r = (double)j / stepsY;
            double pts[2][2] = {{0.0, r * height},
                                {(double)width, r * height}};
            for (auto &pt : pts)
            {
                double sx, sy;
                if (!invToSrcPx(pt[0], pt[1], sx, sy))
                    continue;
                footAny_ = true;
                minX = std::min(minX, sx);
                maxX = std::max(maxX, sx);
                minY = std::min(minY, sy);
                maxY = std::max(maxY, sy);
            }
        }
        if (!footAny_)
            return;
        int rad = 0;
        if (resampling == "bilinear")
            rad = 1;
        else if (resampling == "cubic" || resampling == "cubicspline")
            rad = 2;
        else if (resampling == "lanczos")
            rad = 3;
        int x0 = (int)std::floor(minX + 1e-6) - rad;
        int y0 = (int)std::floor(minY + 1e-6) - rad;
        int x1 = (int)std::ceil(maxX - 1e-6) + rad;
        int y1 = (int)std::ceil(maxY - 1e-6) + rad;
        x0 = std::max(0, std::min(x0, src->width));
        y0 = std::max(0, std::min(y0, src->height));
        x1 = std::max(x0, std::min(x1, src->width));
        y1 = std::max(y0, std::min(y1, src->height));
        footWin_[0] = x0;
        footWin_[1] = y0;
        footWin_[2] = x1 - x0;
        footWin_[3] = y1 - y0;
    }

    bool invToSrcPx(double px, double py, double &sx, double &sy)
    {
        double gx = gt[0] + px * gt[1] + py * gt[2];
        double gy = gt[3] + px * gt[4] + py * gt[5];
        if (!pjXform(op, PJ_INV, gx, gy))
            return false;
        sx = srcInv[0] + gx * srcInv[1] + gy * srcInv[2];
        sy = srcInv[3] + gx * srcInv[4] + gy * srcInv[5];
        return true;
    }

    // Anti-aliasing kernel scales. The reference engages a scaled
    // (triangle) kernel when either axis is downsampled by 2x or more,
    // and then both axes use their raw scale clamped to 1. The raw scale
    // denominator is approximated by the mean source-pixel span of the
    // destination rows/columns (exact for affine transforms; within
    // ~0.1% of the reference for curved reprojections, whose exact
    // window rule resisted identification - see NOTES).
    // KNOWN RESIDUAL: the reference derives these from the source-window
    // bbox of its edge sampling through the approx transformer; the
    // per-row averages below land within ~0.1% for curved transforms
    // (exact for affine), close but not bit-equal in the scaled kernel
    void rawKernelScales(double &sxRaw, double &syRaw)
    {
        double sum = 0;
        int cnt = 0;
        for (int k = 0; k < height; ++k)
        {
            double x0, y0, x1, y1;
            if (invToSrcPx(0.0, k + 0.5, x0, y0) &&
                invToSrcPx(width, k + 0.5, x1, y1))
            {
                sum += std::fabs(x1 - x0);
                ++cnt;
            }
        }
        sxRaw = cnt ? width / (sum / cnt) : 1.0;
        sum = 0;
        cnt = 0;
        for (int k = 0; k < width; ++k)
        {
            double x0, y0, x1, y1;
            if (invToSrcPx(k + 0.5, 0.0, x0, y0) &&
                invToSrcPx(k + 0.5, height, x1, y1))
            {
                sum += std::fabs(y1 - y0);
                ++cnt;
            }
        }
        syRaw = cnt ? height / (sum / cnt) : 1.0;
    }

    void computeKernelScales()
    {
        if (kernScaleDone_)
            return;
        kernScaleDone_ = true;
        double sxRaw, syRaw;
        rawKernelScales(sxRaw, syRaw);
        finishKernelScales(sxRaw, syRaw);
    }

    // XSCALE=/YSCALE= warp options replace the computed scales verbatim
    // and participate in the kernel-choice gate: the reference selects
    // the scaled (triangle) resampler when either axis scale ends up
    // <= 0.5, and inside it each axis with scale < 1 shrinks the kernel
    // argument
    void finishKernelScales(double sxRaw, double syRaw)
    {
        // near-1:1 axes snap to exactly 1.0: the reference treats the
        // axis as unscaled when the source window is at least as large
        // as the destination while the sampled span rounds down to it
        computeFootprint();
        if (sxRaw > 0 && footWin_[2] >= width &&
            (int)std::floor(width / sxRaw + 0.5) <= width)
            sxRaw = 1.0;
        if (syRaw > 0 && footWin_[3] >= height &&
            (int)std::floor(height / syRaw + 0.5) <= height)
            syRaw = 1.0;
        for (const std::string &o : woUser)
        {
            if (o.compare(0, 7, "XSCALE=") == 0)
                sxRaw = strtod(o.c_str() + 7, nullptr);
            else if (o.compare(0, 7, "YSCALE=") == 0)
                syRaw = strtod(o.c_str() + 7, nullptr);
        }
        scaledKernel_ = sxRaw <= 0.5 || syRaw <= 0.5;
        xKernScale_ = std::min(1.0, sxRaw);
        yKernScale_ = std::min(1.0, syRaw);
        dbgScaleX_ = sxRaw;
        dbgScaleY_ = syRaw;
    }

    bool avgFamily() const
    {
        return resampling == "average" || resampling == "rms" ||
               resampling == "mode" || resampling == "min" ||
               resampling == "max" || resampling == "med" ||
               resampling == "q1" || resampling == "q3";
    }

    // per-band all-valid source masks are dropped before the kernel
    // dispatch: a mask survives only if some band actually contains its
    // own nodata value
    bool srcMaskAllValid()
    {
        if (maskAllValidCache_ >= 0)
            return maskAllValidCache_ != 0;
        bool allValid = true;
        for (size_t bi = 0; bi < srcNd.size() && allValid; ++bi)
        {
            if (!srcNd[bi].has)
                continue;
            loadSrcRaw((int)bi + 1);
            const Band &sb = src->bands[bi];
            int esz = dtypeSizeBytes(sb.type);
            const uint8_t *raw = srcRaw_[bi].data();
            size_t n = (size_t)src->width * src->height;
            for (size_t k = 0; k < n; ++k)
                if (readNativeAsDouble(sb.type, raw + k * esz) ==
                    srcNd[bi].v)
                {
                    allValid = false;
                    break;
                }
        }
        maskAllValidCache_ = allValid ? 1 : 0;
        return allValid;
    }

    // the reference swaps in the nearest kernel when both axis scales
    // are exactly 1.0 and every destination centre lands exactly on a
    // source centre (integer-pixel translations, identity warps)
    bool identityAligned()
    {
        if (identAligned_ >= 0)
            return identAligned_ != 0;
        bool aligned = false;
        if (resampling == "bilinear" || resampling == "cubic" ||
            resampling == "cubicspline" || resampling == "lanczos")
        {
            computeKernelScales();
            if (dbgScaleX_ == 1.0 && dbgScaleY_ == 1.0)
            {
                computeMapping();
                aligned = true;
                size_t n = (size_t)width * height;
                for (size_t k = 0; k < n && aligned; ++k)
                {
                    if (map_[k] < 0)
                        continue;
                    double fx = mapX_[k] - 0.5;
                    double fy = mapY_[k] - 0.5;
                    if (fx != std::floor(fx) || fy != std::floor(fy))
                        aligned = false;
                }
            }
        }
        identAligned_ = aligned ? 1 : 0;
        return aligned;
    }

    std::string kernelDebugName()
    {
        if (avgFamily())
            return "GWKAverageOrMode";
        if (resampling == "sum")
            return "GWKSumPreserving";
        DType t = src->bands.empty() ? DType::Byte : src->bands[0].type;
        bool masks = false;
        bool srcHas = false;
        for (const auto &nd : srcNd)
            if (nd.has)
                srcHas = true;
        for (const auto &nd : dstNd)
            if (nd.has)
                masks = true;
        if (!masks && srcHas && !srcMaskAllValid())
            masks = true;
        if (resampling == "nearest" || identityAligned())
        {
            if (t == DType::Int8)
                return "GWKNearestInt8";
            if (masks)
                switch (t)
                {
                    case DType::Byte:
                        return "GWKNearestByte";
                    case DType::Int16:
                        return "GWKNearestShort";
                    case DType::UInt16:
                        return "GWKNearestUnsignedShort";
                    case DType::Float32:
                        return "GWKNearestFloat";
                    default:
                        return "GWKRealCase";
                }
            switch (t)
            {
                case DType::Byte:
                    return "GWKNearestNoMasksOrDstDensityOnlyByte";
                case DType::Int16:
                case DType::UInt16:
                    return "GWKNearestNoMasksOrDstDensityOnlyShort";
                case DType::Float32:
                    return "GWKNearestNoMasksOrDstDensityOnlyFloat";
                default:
                    return "GWKRealCase";
            }
        }
        std::string base;
        if (resampling == "bilinear")
            base = "GWKBilinear";
        else if (resampling == "cubic")
            base = "GWKCubic";
        else if (resampling == "cubicspline")
            base = "GWKCubicSpline";
        if (base.empty() || masks)
            return "GWKRealCase";
        switch (t)
        {
            case DType::Byte:
                return base + "NoMasksOrDstDensityOnlyByte";
            case DType::Int16:
                return base + "NoMasksOrDstDensityOnlyShort";
            case DType::UInt16:
                return base + "NoMasksOrDstDensityOnlyUShort";
            case DType::Float32:
                // cubicspline has no float specialization
                return resampling == "cubicspline"
                           ? "GWKRealCase"
                           : base + "NoMasksOrDstDensityOnlyFloat";
            default:
                return "GWKRealCase";
        }
    }

    bool warpDebugEmit(const std::string &outPath) override
    {
        if (dbgAoiSet_)
        {
            cplDebug("GDAL", strPrintf("Computing area of interest: "
                                       "%g, %g, %g, %g",
                                       dbgAoi_[0], dbgAoi_[1], dbgAoi_[2],
                                       dbgAoi_[3]));
            if (dbgWrap_)
                cplDebug("OGRCT",
                         strPrintf("Wrap source at %g.",
                                   (dbgAoi_[0] + dbgAoi_[2]) / 2.0));
        }
        cplDebug("GDAL",
                 strPrintf("GDALDriver::Create(GTiff,%s,%d,%d,%d,%s,%s)",
                           outPath.c_str(), width, height,
                           (int)bands.size(),
                           dtypeName(bands.empty() ? DType::Byte
                                                   : bands[0].type),
                           cplDebugPtr().c_str()));
        cplDebug("WARP", "Copying metadata from first source to "
                         "destination dataset");
        cplDebug("GTiff", "ScanDirectories()");
        cplDebug("GDAL", "GDALDefaultOverviews::OverviewScan()");
        if (unifiedDbg_)
            cplDebug("WARP", "Set UNIFIED_SRC_NODATA=YES");
        size_t nb = std::min(srcNd.size(), dstNd.size());
        if (dstNdCli_)
        {
            // the GDAL_NODATA tag is dataset-wide: a band setting a
            // value that differs from both the first and the previous
            // band's warns mid-sequence (fires with or without
            // CPL_DEBUG)
            bool haveFirst = false, havePrev = false;
            double firstNd = 0, prevNd = 0;
            int firstBand = 1;
            size_t slash = outPath.find_last_of('/');
            std::string base = slash == std::string::npos
                                   ? outPath
                                   : outPath.substr(slash + 1);
            for (size_t i = 0; i < nb; ++i)
            {
                if (!dstNd[i].has)
                    continue;
                if (i < dstNdCliCount_)
                    cplDebug("WARP",
                             strPrintf("dstnodata of band %d set to %f",
                                       (int)i, dstNd[i].v));
                else
                    cplDebug("WARP", strPrintf("dstnodata of band %d set "
                                               "from previous band",
                                               (int)i));
                if (haveFirst && dstNd[i].v != firstNd &&
                    (!havePrev || dstNd[i].v != prevNd))
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        strPrintf("%s, band %d: Setting nodata to %.17g "
                                  "on band %d, but band %d has nodata "
                                  "at %.17g. The TIFFTAG_GDAL_NODATA "
                                  "only support one value per dataset. "
                                  "This value of %.17g will be used for "
                                  "all bands on re-opening",
                                  base.c_str(), (int)i + 1, dstNd[i].v,
                                  (int)i + 1, firstBand, firstNd,
                                  dstNd[i].v));
                if (!haveFirst)
                {
                    haveFirst = true;
                    firstNd = dstNd[i].v;
                    firstBand = (int)i + 1;
                }
                havePrev = true;
                prevNd = dstNd[i].v;
            }
        }
        else
        {
            bool haveFirst = false, havePrev = false;
            double firstNd = 0, prevNd = 0;
            int firstBand = 1;
            size_t slash = outPath.find_last_of('/');
            std::string base = slash == std::string::npos
                                   ? outPath
                                   : outPath.substr(slash + 1);
            for (size_t i = 0; i < nb; ++i)
            {
                if (!srcNd[i].has || !dstNd[i].has)
                    continue;
                cplDebug("WARP",
                         strPrintf("srcNoData=%f dstNoData=%f",
                                   srcNd[i].v, dstNd[i].v));
                cplDebug("WARP", strPrintf("calling GDALSetRasterNoData"
                                           "Value() for band#%d",
                                           (int)i));
                if (haveFirst && dstNd[i].v != firstNd &&
                    (!havePrev || dstNd[i].v != prevNd))
                    cplErrorStr(
                        CE_Warning, CPLE_AppDefined,
                        strPrintf("%s, band %d: Setting nodata to %.17g "
                                  "on band %d, but band %d has nodata "
                                  "at %.17g. The TIFFTAG_GDAL_NODATA "
                                  "only support one value per dataset. "
                                  "This value of %.17g will be used for "
                                  "all bands on re-opening",
                                  base.c_str(), (int)i + 1, dstNd[i].v,
                                  (int)i + 1, firstBand, firstNd,
                                  dstNd[i].v));
                if (!haveFirst)
                {
                    haveFirst = true;
                    firstNd = dstNd[i].v;
                    firstBand = (int)i + 1;
                }
                havePrev = true;
                prevNd = dstNd[i].v;
            }
        }
        cplDebug("GDALWARP", "Defining SKIP_NOSOURCE=YES");
        if (initDestNdErr_)
            for (size_t i = 0; i < srcNd.size(); ++i)
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "INIT_DEST was set to NO_DATA, but a NoData "
                            "value was not defined.");
        // Int16/UInt16 alpha gets DST_ALPHA_MAX set automatically, and
        // an explicit warp option also silences the not-set debug
        if (alpha && alphaDbg_)
            cplDebug("WARP", "SetAlphaMax: AlphaMax not set.");
        gdalDebugCacheMaxOnce();
        computeFootprint();
        computeKernelScales();
        double sx = dbgScaleX_, sy = dbgScaleY_;
        cplDebug("WARP",
                 strPrintf("dfXScale = %f, dfYScale = %f", sx, sy));
        if ((resampling == "bilinear" || resampling == "cubic") &&
            (sx <= 0.5 || sy <= 0.5))
            cplDebug("WARP",
                     strPrintf("Not using 4-sample bilinear/bicubic "
                               "formula because XSCALE(=%f) and/or "
                               "YSCALE(=%f) <= 0.5. Further messages of "
                               "this type will be suppressed.",
                               sx, sy));
        cplDebug("GDAL",
                 strPrintf("GDALWarpKernel()::%s() Src=%d,%d,%dx%d "
                           "Dst=0,0,%dx%d",
                           kernelDebugName().c_str(), footWin_[0],
                           footWin_[1], footWin_[2], footWin_[3], width,
                           height));
        int req = atoi(numThreadsXml.c_str());
        if (req < 1)
            req = 1;
        long long capped =
            std::max<long long>(1, (long long)width * height / 65536);
        int nT = (int)std::min<long long>(req, capped);
        if (req > 1)
            cplDebug("WARP", strPrintf("Using %d threads", nT));
        bool avg = avgFamily();
        if (avg)
            cplDebug("GDAL", "GDALWarpKernel():GWKAverageOrModeThread()");
        for (int i = 1; i < nT; ++i)
        {
            if (dbgWrap_)
                cplDebug("OGRCT",
                         strPrintf("Wrap source at %g.",
                                   (dbgAoi_[0] + dbgAoi_[2]) / 2.0));
            if (avg)
                cplDebug("GDAL",
                         "GDALWarpKernel():GWKAverageOrModeThread()");
        }
        if (alpha)
            cplDebug("GDAL", "Flushing dirty blocks: 0...10...20...30..."
                             "40...50...60...70...80...90...100 - done.");
        return true;
    }

    // final (rounded/clamped) bilinear values; NaN marks invalid pixels.
    // NoMasks lerp specialization for Byte/Int16/UInt16/Float32; the
    // general path accumulates the four products, skips masked samples
    // with renormalization, and gates on the nearest sample's unified
    // mask (single-band nodata behaves unified)
    const std::vector<double> &bilComputed(size_t bi)
    {
        if (bilOut_.size() != src->bands.size())
            bilOut_.assign(src->bands.size(), {});
        auto &res = bilOut_[bi];
        if (!res.empty())
            return res;
        computeMapping();
        loadSrcRaw((int)bi + 1);
        const Band &sb = src->bands[bi];
        int esz = dtypeSizeBytes(sb.type);
        const uint8_t *raw = srcRaw_[bi].data();
        int sw = src->width, sh = src->height;
        bool anyNd = unifiedSrcNodata;
        for (const auto &nd : srcNd)
            if (nd.has)
                anyNd = true;
        // dst nodata alone also disables the NoMasks specialization
        // (padfDstNoDataReal is part of the reference's gate), shifting
        // knife-edge pixels through the accumulate-divide arithmetic;
        // src nodata only counts while its all-valid mask survives
        bool anyDstNd = false;
        for (const auto &nd : dstNd)
            if (nd.has)
                anyDstNd = true;
        bool maskEff = anyNd && !srcMaskAllValid();
        bool spec = !maskEff && !anyDstNd &&
                    (sb.type == DType::Byte || sb.type == DType::Int16 ||
                     sb.type == DType::UInt16 ||
                     sb.type == DType::Float32);
        bool gate = anyNd &&
                    (unifiedSrcNodata || src->bands.size() == 1);
        size_t n = (size_t)width * height;
        res.assign(n, std::numeric_limits<double>::quiet_NaN());
        auto sample = [&](int xx, int yy)
        {
            return readNativeAsDouble(
                sb.type, raw + ((size_t)yy * sw + xx) * esz);
        };
        computeKernelScales();
        bool cub = resampling == "cubic";
        if (scaledKernel_)
        {
            double fR = cub ? 2.0 : 1.0;
            int rx = (int)std::ceil(fR / xKernScale_);
            int ry = (int)std::ceil(fR / yKernScale_);
            std::vector<double> wx((size_t)2 * rx), wy((size_t)2 * ry);
            for (size_t k = 0; k < n; ++k)
            {
                int64_t nidx = map_[k];
                if (nidx < 0)
                    continue;
                if (gate && pixelInvalid(bi, nidx))
                    continue;
                double x = mapX_[k], y = mapY_[k];
                int iX = (int)std::floor(x - 0.5);
                int iY = (int)std::floor(y - 0.5);
                double dX = x - 0.5 - iX;
                double dY = y - 0.5 - iY;
                int ix0 = std::max(1 - rx, -iX);
                int ix1 = std::min(rx, sw - 1 - iX);
                int iy0 = std::max(1 - ry, -iY);
                int iy1 = std::min(ry, sh - 1 - iY);
                if (ix0 > ix1 || iy0 > iy1)
                    continue;
                double sumWx = 0, sumWy = 0;
                for (int i = ix0; i <= ix1; ++i)
                {
                    double w;
                    if (cub)
                        w = gwkCubicWeight((i - dX) * xKernScale_);
                    else
                    {
                        w = 1.0 - std::fabs((i - dX) * xKernScale_);
                        if (w < 0)
                            w = 0;
                    }
                    wx[(size_t)(i - ix0)] = w;
                    sumWx += w;
                }
                for (int j = iy0; j <= iy1; ++j)
                {
                    double w;
                    if (cub)
                        w = gwkCubicWeight((j - dY) * yKernScale_);
                    else
                    {
                        w = 1.0 - std::fabs((j - dY) * yKernScale_);
                        if (w < 0)
                            w = 0;
                    }
                    wy[(size_t)(j - iy0)] = w;
                    sumWy += w;
                }
                double v;
                if (spec)
                {
                    if (sumWx <= 0 || sumWy <= 0)
                        continue;
                    double acc = 0;
                    for (int j = iy0; j <= iy1; ++j)
                    {
                        double rowAcc = 0;
                        for (int i = ix0; i <= ix1; ++i)
                            rowAcc += sample(iX + i, iY + j) *
                                      wx[(size_t)(i - ix0)];
                        acc += rowAcc * wy[(size_t)(j - iy0)];
                    }
                    v = acc / (sumWx * sumWy);
                }
                else
                {
                    double acc = 0, div = 0;
                    for (int j = iy0; j <= iy1; ++j)
                    {
                        double wyj = wy[(size_t)(j - iy0)];
                        for (int i = ix0; i <= ix1; ++i)
                        {
                            if (anyNd &&
                                bandMasked(bi, (int64_t)(iY + j) * sw +
                                                   (iX + i)))
                                continue;
                            double w = wyj * wx[(size_t)(i - ix0)];
                            acc += sample(iX + i, iY + j) * w;
                            div += w;
                        }
                    }
                    if (div < 0.00001)
                        continue;
                    v = acc / div;
                }
                // specialized kernels round to the data type first and
                // shift after; the masked path shifts the raw resample
                if (!zAdd_.empty())
                    v = (spec ? finalizeKernelValue(sb.type, v) : v) +
                        zAdd_[k];
                res[k] = finalizeKernelValue(sb.type, v);
            }
            return res;
        }
        for (size_t k = 0; k < n; ++k)
        {
            int64_t nidx = map_[k];
            if (nidx < 0)
                continue;
            if (gate && pixelInvalid(bi, nidx))
                continue;
            double x = mapX_[k], y = mapY_[k];
            int iX = (int)std::floor(x - 0.5);
            int iY = (int)std::floor(y - 0.5);
            double rX = 1.5 - (x - iX);
            double rY = 1.5 - (y - iY);
            double v;
            // the 4x4 cubic formula needs its whole window inside the
            // source and (when masked) fully valid; anything else drops
            // to the bilinear resampler like the reference kernel
            bool cubDone = false;
            if (cub && iX - 1 >= 0 && iY - 1 >= 0 && iX + 2 < sw &&
                iY + 2 < sh)
            {
                bool all16 = true;
                if (!spec && anyNd)
                    for (int j = -1; all16 && j < 3; ++j)
                        for (int i = -1; i < 3; ++i)
                            if (bandMasked(bi, (int64_t)(iY + j) * sw +
                                                   (iX + i)))
                            {
                                all16 = false;
                                break;
                            }
                if (all16)
                {
                    double dX = x - 0.5 - iX, dY = y - 0.5 - iY;
                    double dX2 = dX * dX, dX3 = dX2 * dX;
                    double dY2 = dY * dY, dY3 = dY2 * dY;
                    double rows[4];
                    for (int j = -1; j < 3; ++j)
                        rows[j + 1] = cubicConvolution(
                            dX, dX2, dX3, sample(iX - 1, iY + j),
                            sample(iX, iY + j), sample(iX + 1, iY + j),
                            sample(iX + 2, iY + j));
                    v = cubicConvolution(dY, dY2, dY3, rows[0], rows[1],
                                         rows[2], rows[3]);
                    cubDone = true;
                }
            }
            if (cubDone)
            {
            }
            else if (spec)
            {
                // full windows lerp directly; clipped windows accumulate
                // the in-image corners and renormalize (the reference
                // never clamps here, so knife-edge halves keep the dust
                // of the guarded divide)
                if (iX >= 0 && iX + 1 < sw && iY >= 0 && iY + 1 < sh)
                {
                    double v00 = sample(iX, iY), v10 = sample(iX + 1, iY);
                    double v01 = sample(iX, iY + 1),
                           v11 = sample(iX + 1, iY + 1);
                    v = (v00 * rX + v10 * (1.0 - rX)) * rY +
                        (v01 * rX + v11 * (1.0 - rX)) * (1.0 - rY);
                }
                else
                {
                    double acc = 0.0, div = 0.0;
                    if (iX >= 0 && iX < sw && iY >= 0 && iY < sh)
                    {
                        double w = rX * rY;
                        acc += sample(iX, iY) * w;
                        div += w;
                    }
                    if (iX + 1 >= 0 && iX + 1 < sw && iY >= 0 && iY < sh)
                    {
                        double w = (1.0 - rX) * rY;
                        acc += sample(iX + 1, iY) * w;
                        div += w;
                    }
                    if (iX >= 0 && iX < sw && iY + 1 >= 0 && iY + 1 < sh)
                    {
                        double w = rX * (1.0 - rY);
                        acc += sample(iX, iY + 1) * w;
                        div += w;
                    }
                    if (iX + 1 >= 0 && iX + 1 < sw && iY + 1 >= 0 &&
                        iY + 1 < sh)
                    {
                        double w = (1.0 - rX) * (1.0 - rY);
                        acc += sample(iX + 1, iY + 1) * w;
                        div += w;
                    }
                    if (div < 0.00001)
                        continue;
                    v = div == 1.0 ? acc : acc / div;
                }
            }
            else
            {
                // masked-kernel semantics: only exact -1 hits clamp;
                // out-of-range samples are skipped and the weight sum
                // renormalizes
                if (iX == -1)
                {
                    iX = 0;
                    rX = 1.0;
                }
                if (iY == -1)
                {
                    iY = 0;
                    rY = 1.0;
                }
                double acc = 0.0, div = 0.0;
                const int xs[2] = {iX, iX + 1};
                const int ys[2] = {iY, iY + 1};
                const double wxs[2] = {rX, 1.0 - rX};
                const double wys[2] = {rY, 1.0 - rY};
                for (int a = 0; a < 2; ++a)
                    for (int c = 0; c < 2; ++c)
                    {
                        if (xs[c] < 0 || xs[c] >= sw || ys[a] < 0 ||
                            ys[a] >= sh)
                            continue;
                        int64_t sidx = (int64_t)ys[a] * sw + xs[c];
                        if (anyNd && bandMasked(bi, sidx))
                            continue;
                        double w = wxs[c] * wys[a];
                        acc += sample(xs[c], ys[a]) * w;
                        div += w;
                    }
                if (div < 0.00001)
                    continue;
                v = div == 1.0 ? acc : acc / div;
            }
            if (!zAdd_.empty())
                v = (spec ? finalizeKernelValue(sb.type, v) : v) +
                    zAdd_[k];
            res[k] = finalizeKernelValue(sb.type, v);
        }
        return res;
    }

    // 4-sample cubic weight vector in the factored Horner arrangement
    // (matches the reference last-ulp for doubles; symbolic-equivalent
    // rewrites land one ulp away on ~30% of pixels)
    static void cubicWeights4(double x, double w[4])
    {
        w[0] = 0.5 * (x * ((2.0 - x) * x - 1.0));
        w[1] = 0.5 * (x * x * (3.0 * x - 5.0) + 2.0);
        w[2] = 0.5 * (x * ((4.0 - 3.0 * x) * x + 1.0));
        w[3] = 0.5 * (x * x * (x - 1.0));
    }

    // Catmull-Rom convolution form used by the integer/float NoMasks
    // specializations (double intermediates; only the last-ulp grouping
    // differs from cubicWeights4 + dot)
    static double cubicConvolution(double d, double f0, double f1,
                                   double f2, double f3)
    {
        double d2 = d * d;
        double d3 = d2 * d;
        return f1 +
               0.5 * (d * (f2 - f0) +
                      d2 * (2.0 * f0 - 5.0 * f1 + 4.0 * f2 - f3) +
                      d3 * (3.0 * (f1 - f2) + f3 - f0));
    }

    // piecewise cubic (a=-0.5) for the scaled kernel path
    static double cubicKernel(double x)
    {
        double ax = std::fabs(x);
        if (ax <= 1.0)
            return x * x * (1.5 * ax - 2.5) + 1.0;
        if (ax <= 2.0)
            return x * x * (-0.5 * ax + 2.5) - 4.0 * ax + 2.0;
        return 0.0;
    }

    // unnormalized B-spline; the branch nesting mirrors the reference's
    // conditional-sum ordering (innermost tail terms added first)
    static double bsplineKernel(double x)
    {
        double xp2 = x + 2.0;
        double xp1 = x + 1.0;
        double xm1 = x - 1.0;
        double xp2c = xp2 * xp2 * xp2;
        if (xp2 <= 0.0)
            return 0.0;
        if (xp1 <= 0.0)
            return xp2c;
        if (x <= 0.0)
            return -4.0 * xp1 * xp1 * xp1 + xp2c;
        if (xm1 <= 0.0)
            return (6.0 * x * x * x + -4.0 * xp1 * xp1 * xp1) + xp2c;
        return ((-4.0 * xm1 * xm1 * xm1 + 6.0 * x * x * x) +
                -4.0 * xp1 * xp1 * xp1) +
               xp2c;
    }

    // final (rounded/clamped) cubic/cubicspline values; NaN marks
    // invalid pixels. Layout mirrors bilComputed: NoMasks convolution
    // specialization for Byte/Int16/UInt16/Float32 (integer-only for
    // cubicspline), a general path on the factored weights with a
    // masked-bilinear fallback whenever the 4x4 window clips the border
    // or contains an invalid sample, and a weighted scaled kernel once
    // either axis shrinks to half size (cubicspline always resolves
    // through the weighted path)
    const std::vector<double> &cubComputed(size_t bi)
    {
        if (cubOut_.size() != src->bands.size())
            cubOut_.assign(src->bands.size(), {});
        auto &res = cubOut_[bi];
        if (!res.empty())
            return res;
        computeMapping();
        loadSrcRaw((int)bi + 1);
        const Band &sb = src->bands[bi];
        int esz = dtypeSizeBytes(sb.type);
        const uint8_t *raw = srcRaw_[bi].data();
        int sw = src->width, sh = src->height;
        bool spline = resampling == "cubicspline";
        bool anyNd = unifiedSrcNodata;
        for (const auto &nd : srcNd)
            if (nd.has)
                anyNd = true;
        bool anyDstNd = false;
        for (const auto &nd : dstNd)
            if (nd.has)
                anyDstNd = true;
        bool maskEff = anyNd && !srcMaskAllValid();
        bool spec = !maskEff && !anyDstNd &&
                    (sb.type == DType::Byte || sb.type == DType::Int16 ||
                     sb.type == DType::UInt16 ||
                     (sb.type == DType::Float32 && !spline));
        bool gate = anyNd &&
                    (unifiedSrcNodata || src->bands.size() == 1);
        size_t n = (size_t)width * height;
        res.assign(n, std::numeric_limits<double>::quiet_NaN());
        auto sample = [&](int xx, int yy)
        {
            return readNativeAsDouble(
                sb.type, raw + ((size_t)yy * sw + xx) * esz);
        };
        // NoMasks four-sample bilinear, the fallback of the specialized
        // 4x4 path near borders and against tiny sources; clipped
        // windows accumulate the in-image corners and renormalize
        auto bilSpec = [&](double x, double y, double &v)
        {
            int iX = (int)std::floor(x - 0.5);
            int iY = (int)std::floor(y - 0.5);
            double rX = 1.5 - (x - iX);
            double rY = 1.5 - (y - iY);
            if (iX >= 0 && iX + 1 < sw && iY >= 0 && iY + 1 < sh)
            {
                double v00 = sample(iX, iY), v10 = sample(iX + 1, iY);
                double v01 = sample(iX, iY + 1),
                       v11 = sample(iX + 1, iY + 1);
                v = (v00 * rX + v10 * (1.0 - rX)) * rY +
                    (v01 * rX + v11 * (1.0 - rX)) * (1.0 - rY);
                return true;
            }
            double acc = 0.0, div = 0.0;
            if (iX >= 0 && iX < sw && iY >= 0 && iY < sh)
            {
                double w = rX * rY;
                acc += sample(iX, iY) * w;
                div += w;
            }
            if (iX + 1 >= 0 && iX + 1 < sw && iY >= 0 && iY < sh)
            {
                double w = (1.0 - rX) * rY;
                acc += sample(iX + 1, iY) * w;
                div += w;
            }
            if (iX >= 0 && iX < sw && iY + 1 >= 0 && iY + 1 < sh)
            {
                double w = rX * (1.0 - rY);
                acc += sample(iX, iY + 1) * w;
                div += w;
            }
            if (iX + 1 >= 0 && iX + 1 < sw && iY + 1 >= 0 &&
                iY + 1 < sh)
            {
                double w = (1.0 - rX) * (1.0 - rY);
                acc += sample(iX + 1, iY + 1) * w;
                div += w;
            }
            if (div < 0.00001)
                return false;
            v = div == 1.0 ? acc : acc / div;
            return true;
        };
        // masked four-sample bilinear; also the border/invalid fallback
        // of the general 4x4 path (weights renormalize over the samples
        // that survive)
        auto bilMasked = [&](double x, double y, double &v)
        {
            int iX = (int)std::floor(x - 0.5);
            int iY = (int)std::floor(y - 0.5);
            double rX = 1.5 - (x - iX);
            double rY = 1.5 - (y - iY);
            if (iX == -1)
            {
                iX = 0;
                rX = 1.0;
            }
            if (iY == -1)
            {
                iY = 0;
                rY = 1.0;
            }
            double acc = 0.0, div = 0.0;
            const int xs[2] = {iX, iX + 1};
            const int ys[2] = {iY, iY + 1};
            const double wxs[2] = {rX, 1.0 - rX};
            const double wys[2] = {rY, 1.0 - rY};
            for (int a = 0; a < 2; ++a)
                for (int c = 0; c < 2; ++c)
                {
                    if (xs[c] < 0 || xs[c] >= sw || ys[a] < 0 ||
                        ys[a] >= sh)
                        continue;
                    int64_t sidx = (int64_t)ys[a] * sw + xs[c];
                    if (anyNd && bandMasked(bi, sidx))
                        continue;
                    double w = wxs[c] * wys[a];
                    acc += sample(xs[c], ys[a]) * w;
                    div += w;
                }
            if (div < 0.00001)
                return false;
            v = div == 1.0 ? acc : acc / div;
            return true;
        };
        computeKernelScales();
        if (scaledKernel_ || spline)
        {
            const double xsc = xKernScale_, ysc = yKernScale_;
            int rx = xsc < 1.0 ? (int)std::ceil(2.0 / xsc) : 2;
            int ry = ysc < 1.0 ? (int)std::ceil(2.0 / ysc) : 2;
            std::vector<double> wx((size_t)2 * rx + 1),
                wy((size_t)2 * ry + 1);
            for (size_t k = 0; k < n; ++k)
            {
                int64_t nidx = map_[k];
                if (nidx < 0)
                    continue;
                if (gate && pixelInvalid(bi, nidx))
                    continue;
                double x = mapX_[k], y = mapY_[k];
                int iX = (int)std::floor(x - 0.5);
                int iY = (int)std::floor(y - 0.5);
                double dX = x - 0.5 - iX;
                double dY = y - 0.5 - iY;
                int ix0 = std::max(1 - rx, -iX);
                int ix1 = std::min(rx, sw - 1 - iX);
                int iy0 = std::max(1 - ry, -iY);
                int iy1 = std::min(ry, sh - 1 - iY);
                if (ix0 > ix1 || iy0 > iy1)
                    continue;
                for (int i = ix0; i <= ix1; ++i)
                    wx[(size_t)(i - ix0)] =
                        spline ? bsplineKernel((i - dX) * xsc)
                               : cubicKernel((i - dX) * xsc);
                for (int j = iy0; j <= iy1; ++j)
                    wy[(size_t)(j - iy0)] =
                        spline ? bsplineKernel((j - dY) * ysc)
                               : cubicKernel((j - dY) * ysc);
                double v;
                if (spec)
                {
                    double sumWx = 0, sumWy = 0;
                    for (int i = ix0; i <= ix1; ++i)
                        sumWx += wx[(size_t)(i - ix0)];
                    for (int j = iy0; j <= iy1; ++j)
                        sumWy += wy[(size_t)(j - iy0)];
                    double acc = 0;
                    for (int j = iy0; j <= iy1; ++j)
                    {
                        double rowAcc = 0;
                        for (int i = ix0; i <= ix1; ++i)
                            rowAcc += sample(iX + i, iY + j) *
                                      wx[(size_t)(i - ix0)];
                        acc += rowAcc * wy[(size_t)(j - iy0)];
                    }
                    v = acc / (sumWx * sumWy);
                }
                else
                {
                    double acc = 0, div = 0;
                    for (int j = iy0; j <= iy1; ++j)
                    {
                        double wyj = wy[(size_t)(j - iy0)];
                        for (int i = ix0; i <= ix1; ++i)
                        {
                            if (anyNd &&
                                bandMasked(bi, (int64_t)(iY + j) * sw +
                                                   (iX + i)))
                                continue;
                            double w = wyj * wx[(size_t)(i - ix0)];
                            acc += sample(iX + i, iY + j) * w;
                            div += w;
                        }
                    }
                    if (div < 0.000001)
                        continue;
                    v = (div > 0.99999 && div < 1.00001) ? acc
                                                         : acc / div;
                }
                if (!zAdd_.empty())
                    v = (spec ? finalizeKernelValue(sb.type, v) : v) +
                        zAdd_[k];
                res[k] = finalizeKernelValue(sb.type, v);
            }
            return res;
        }
        for (size_t k = 0; k < n; ++k)
        {
            int64_t nidx = map_[k];
            if (nidx < 0)
                continue;
            if (gate && pixelInvalid(bi, nidx))
                continue;
            double x = mapX_[k], y = mapY_[k];
            int iX = (int)(x - 0.5);
            int iY = (int)(y - 0.5);
            double dX = x - 0.5 - iX;
            double dY = y - 0.5 - iY;
            bool inside = iX - 1 >= 0 && iX + 2 < sw && iY - 1 >= 0 &&
                          iY + 2 < sh;
            double v;
            if (spec)
            {
                // the specialized 4x4 window clips near borders and
                // against tiny sources, where the reference drops to
                // its guarded bilinear
                if (!inside)
                {
                    if (!bilSpec(x, y, v))
                        continue;
                }
                else
                {
                    double rows[4];
                    for (int j = -1; j <= 2; ++j)
                        rows[j + 1] = cubicConvolution(
                            dX, sample(iX - 1, iY + j),
                            sample(iX, iY + j), sample(iX + 1, iY + j),
                            sample(iX + 2, iY + j));
                    v = cubicConvolution(dY, rows[0], rows[1], rows[2],
                                         rows[3]);
                }
            }
            else
            {
                bool clean = inside;
                if (clean && anyNd)
                    for (int j = -1; j <= 2 && clean; ++j)
                        for (int i = -1; i <= 2 && clean; ++i)
                            if (bandMasked(bi,
                                           (int64_t)(iY + j) * sw +
                                               (iX + i)))
                                clean = false;
                if (clean)
                {
                    double wxv[4], wyv[4];
                    cubicWeights4(dX, wxv);
                    cubicWeights4(dY, wyv);
                    double rows[4];
                    for (int j = -1; j <= 2; ++j)
                    {
                        rows[j + 1] =
                            ((sample(iX - 1, iY + j) * wxv[0] +
                              sample(iX, iY + j) * wxv[1]) +
                             sample(iX + 1, iY + j) * wxv[2]) +
                            sample(iX + 2, iY + j) * wxv[3];
                    }
                    v = ((rows[0] * wyv[0] + rows[1] * wyv[1]) +
                         rows[2] * wyv[2]) +
                        rows[3] * wyv[3];
                }
                else if (!bilMasked(x, y, v))
                    continue;
            }
            if (!zAdd_.empty())
                v = (spec ? finalizeKernelValue(sb.type, v) : v) +
                    zAdd_[k];
            res[k] = finalizeKernelValue(sb.type, v);
        }
        return res;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        if (resampling != "nearest" && resampling != "bilinear" &&
            resampling != "cubic" && resampling != "cubicspline")
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "reproject: resampling '" + resampling +
                            "' is not implemented in this build");
            return false;
        }
        computeMapping();
        const Band &b = bands[(size_t)band - 1];
        int esz = dtypeSizeBytes(b.type);
        size_t n = (size_t)width * height;
        out.assign(n * esz, 0);
        bool isAlpha = alpha && band == (int)bands.size();
        if (isAlpha)
        {
            double amax = alphaMax();
            std::vector<uint8_t> on(esz), off(esz);
            writeNativeFromDouble(b.type, amax, on.data());
            writeNativeFromDouble(b.type, 0, off.data());
            bool anySrcNd = false;
            for (const auto &nd : srcNd)
                if (nd.has)
                    anySrcNd = true;
            for (size_t k = 0; k < n; ++k)
            {
                int64_t idx = map_[k];
                bool valid = idx >= 0;
                if (valid && anySrcNd)
                    valid = !pixelInvalid(0, idx);
                memcpy(out.data() + k * esz,
                       valid ? on.data() : off.data(), esz);
            }
            return true;
        }
        size_t bi = (size_t)band - 1;
        if (!loadSrcRaw(band))
            return false;
        const uint8_t *raw = srcRaw_[bi].data();
        std::vector<uint8_t> fill(esz);
        writeNativeFromDouble(
            b.type, dstNd[bi].has ? dstNd[bi].v : 0.0, fill.data());
        bool haveNd = srcNd[bi].has || unifiedSrcNodata;
        // valid source values equal to the destination nodata get nudged
        // so they are not read back as holes
        std::vector<uint8_t> shifted(esz);
        bool doShift = shiftAllowed_ && dstNd[bi].has;
        if (doShift)
            writeNativeFromDouble(b.type, shiftedNodata(b.type, dstNd[bi].v),
                                  shifted.data());
        if ((resampling == "bilinear" || resampling == "cubic" ||
             resampling == "cubicspline") &&
            !identityAligned())
        {
            const std::vector<double> &cv = resampling == "bilinear"
                                                ? bilComputed(bi)
                                                : cubComputed(bi);
            for (size_t k = 0; k < n; ++k)
            {
                double v = cv[k];
                if (std::isnan(v))
                {
                    memcpy(out.data() + k * esz, fill.data(), esz);
                    continue;
                }
                if (doShift && v == dstNd[bi].v)
                {
                    emitShiftWarning();
                    memcpy(out.data() + k * esz, shifted.data(), esz);
                    continue;
                }
                writeNativeFromDouble(b.type, v, out.data() + k * esz);
            }
            return true;
        }
        for (size_t k = 0; k < n; ++k)
        {
            int64_t idx = map_[k];
            if (idx < 0 || (haveNd && pixelInvalid(bi, idx)))
            {
                memcpy(out.data() + k * esz, fill.data(), esz);
                continue;
            }
            const uint8_t *srcPix = raw + (size_t)idx * esz;
            if (!zAdd_.empty())
            {
                double v = finalizeKernelValue(
                    b.type, readNativeAsDouble(b.type, srcPix) + zAdd_[k]);
                if (doShift && v == dstNd[bi].v)
                {
                    emitShiftWarning();
                    memcpy(out.data() + k * esz, shifted.data(), esz);
                    continue;
                }
                writeNativeFromDouble(b.type, v, out.data() + k * esz);
                continue;
            }
            if (doShift &&
                readNativeAsDouble(b.type, srcPix) == dstNd[bi].v)
            {
                emitShiftWarning();
                memcpy(out.data() + k * esz, shifted.data(), esz);
                continue;
            }
            memcpy(out.data() + k * esz, srcPix, esz);
        }
        return true;
    }

    // the warp kernel scans pixels with bands innermost, so the warned
    // value is the first collision in that order, once per warp region:
    // the whole raster for the leaf path, each cached block when the
    // dataset was opened from a warped VRT
    void emitShiftWarning()
    {
        if (shiftWarned_)
            return;
        shiftWarned_ = true;
        size_t nb = src->bands.size();
        for (size_t b = 0; b < nb; ++b)
            if (dstNd[b].has)
                loadSrcRaw((int)b + 1);
        bool ident = identityAligned();
        bool bil = resampling == "bilinear" && !ident;
        bool cub = (resampling == "cubic" ||
                    resampling == "cubicspline") &&
                   !ident;
        auto firstCollision = [&](int x0, int y0, int x1, int y1)
        {
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                {
                    size_t k = (size_t)y * width + x;
                    int64_t idx = map_[k];
                    if (idx < 0)
                        continue;
                    for (size_t b = 0; b < nb; ++b)
                    {
                        if (!dstNd[b].has)
                            continue;
                        DType t = src->bands[b].type;
                        double v;
                        if (bil || cub)
                        {
                            v = bil ? bilComputed(b)[k]
                                    : cubComputed(b)[k];
                            if (std::isnan(v))
                                continue;
                        }
                        else
                        {
                            if ((srcNd[b].has || unifiedSrcNodata) &&
                                pixelInvalid(b, idx))
                                continue;
                            int esz = dtypeSizeBytes(t);
                            v = readNativeAsDouble(
                                t, srcRaw_[b].data() + (size_t)idx * esz);
                            if (!zAdd_.empty())
                                v = finalizeKernelValue(t, v + zAdd_[k]);
                        }
                        if (v != dstNd[b].v)
                            continue;
                        cplErrorStr(
                            CE_Warning, CPLE_AppDefined,
                            strPrintf(
                                "Value %g in the source dataset has been "
                                "changed to %g in the destination dataset "
                                "to avoid being treated as NoData. To "
                                "avoid this, select a different NoData "
                                "value for the destination dataset.",
                                dstNd[b].v, shiftedNodata(t, dstNd[b].v)));
                        return;
                    }
                }
        };
        if (!blockChunked_ || bands.empty())
        {
            firstCollision(0, 0, width, height);
            return;
        }
        int bw = bands[0].blockX > 0 ? bands[0].blockX : width;
        int bh = bands[0].blockY > 0 ? bands[0].blockY : height;
        for (int y0 = 0; y0 < height; y0 += bh)
            for (int x0 = 0; x0 < width; x0 += bw)
                firstCollision(x0, y0, std::min(x0 + bw, width),
                               std::min(y0 + bh, height));
    }

    bool readBand(int band, std::vector<double> &out) override
    {
        std::vector<uint8_t> raw;
        if (!readBandRaw(band, raw))
            return false;
        const Band &b = bands[(size_t)band - 1];
        int esz = dtypeSizeBytes(b.type);
        size_t n = (size_t)width * height;
        out.resize(n);
        for (size_t k = 0; k < n; ++k)
            out[k] = readNativeAsDouble(b.type, raw.data() + k * esz);
        return true;
    }

    std::string customVrtXml(const std::string &input,
                             const std::string &output) override;
};

std::string WarpedDataset::customVrtXml(const std::string &input,
                                        const std::string &output)
{
    std::string x;
    x += strPrintf("<VRTDataset rasterXSize=\"%d\" rasterYSize=\"%d\" "
                   "subClass=\"VRTWarpedDataset\">\n",
                   width, height);
    if (hasSrs && srs.valid())
    {
        std::string mapping;
        for (int m : srs.dataAxisToSRSAxisMapping())
        {
            if (!mapping.empty())
                mapping += ",";
            mapping += strPrintf("%d", m);
        }
        std::string wkt = srs.wkt1Gdal();
        if (wkt.empty())
            wkt = srs.wkt2SingleLine();
        x += "  <SRS dataAxisToSRSAxisMapping=\"" + mapping + "\">" +
             wxmlTextEsc(wkt) + "</SRS>\n";
    }
    x += "  <GeoTransform>";
    for (int i = 0; i < 6; i++)
    {
        if (i)
            x += ",";
        x += strPrintf("%24.16e", gt[i]);
    }
    x += "</GeoTransform>\n";
    {
        auto it = metadata.find("");
        MetaDomain items;
        if (it != metadata.end())
            items = it->second;
        std::stable_sort(items.begin(), items.end(),
                         [](const std::pair<std::string, std::string> &a,
                            const std::pair<std::string, std::string> &b) {
                             return strcasecmp(a.first.c_str(),
                                               b.first.c_str()) < 0;
                         });
        if (!items.empty())
        {
            x += "  <Metadata>\n";
            for (const auto &kv : items)
                x += "    <MDI key=\"" + kv.first + "\">" +
                     wxmlTextEsc(kv.second) + "</MDI>\n";
            x += "  </Metadata>\n";
        }
    }
    if (bands.size() > 1)
    {
        x += "  <Metadata domain=\"IMAGE_STRUCTURE\">\n";
        x += "    <MDI key=\"INTERLEAVE\">PIXEL</MDI>\n";
        x += "  </Metadata>\n";
    }
    for (size_t i = 0; i < bands.size(); ++i)
    {
        const Band &b = bands[i];
        if (!b.hasNodata && b.colorInterp == "Undefined")
        {
            x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\" "
                           "subClass=\"VRTWarpedRasterBand\" />\n",
                           dtypeName(b.type), (int)i + 1);
            continue;
        }
        x += strPrintf("  <VRTRasterBand dataType=\"%s\" band=\"%d\" "
                       "subClass=\"VRTWarpedRasterBand\">\n",
                       dtypeName(b.type), (int)i + 1);
        if (b.hasNodata)
        {
            std::string nd = std::isnan(b.nodata)
                                 ? "nan"
                                 : strPrintf("%.18g", b.nodata);
            x += "    <NoDataValue>" + nd + "</NoDataValue>\n";
        }
        if (b.colorInterp != "Undefined")
            x += "    <ColorInterp>" + b.colorInterp + "</ColorInterp>\n";
        x += "  </VRTRasterBand>\n";
    }
    x += strPrintf("  <BlockXSize>%d</BlockXSize>\n", std::min(width, 512));
    x += strPrintf("  <BlockYSize>%d</BlockYSize>\n",
                   std::min(height, 128));
    x += "  <GDALWarpOptions>\n";
    x += "    <WarpMemoryLimit>6.71089e+07</WarpMemoryLimit>\n";
    x += strPrintf("    <ResampleAlg>%s</ResampleAlg>\n",
                   resampleAlgName(resampling));
    x += strPrintf("    <WorkingDataType>%s</WorkingDataType>\n",
                   dtypeName(bands.empty() ? DType::Byte : bands[0].type));
    x += "    <Option name=\"NUM_THREADS\">" + numThreadsXml +
         "</Option>\n";
    if (etExplicit)
        x += "    <Option name=\"ERROR_THRESHOLD\">" + fmtG(etValue) +
             "</Option>\n";
    bool anyDstNd = false;
    for (const auto &nd : dstNd)
        if (nd.has)
            anyDstNd = true;
    x += std::string("    <Option name=\"INIT_DEST\">") +
         (anyDstNd ? "NO_DATA" : "0") + "</Option>\n";
    if (unifiedSrcNodata && src->bands.size() >= 2)
        x += "    <Option name=\"UNIFIED_SRC_NODATA\">YES</Option>\n";
    for (const auto &o : woUser)
    {
        size_t eq = o.find('=');
        if (eq == std::string::npos)
            continue;
        x += "    <Option name=\"" + o.substr(0, eq) + "\">" +
             wxmlTextEsc(o.substr(eq + 1)) + "</Option>\n";
    }
    x += "    <Option name=\"ERROR_OUT_IF_EMPTY_SOURCE_WINDOW\">FALSE"
         "</Option>\n";
    if (alpha)
    {
        DType t = bands.back().type;
        if (t == DType::Int16)
            x += "    <Option name=\"DST_ALPHA_MAX\">32767</Option>\n";
        else if (t == DType::UInt16)
            x += "    <Option name=\"DST_ALPHA_MAX\">65535</Option>\n";
    }
    int relative = 0;
    (void)input;
    std::string srcName = wVrtRelativePath(src->path, output, relative);
    x += strPrintf("    <SourceDataset relativeToVRT=\"%d\">%s"
                   "</SourceDataset>\n",
                   relative, wxmlTextEsc(srcName).c_str());
    x += "    <Transformer>\n";
    std::string ind = "      ";
    bool approx = maxErr > 0;
    if (approx)
    {
        x += ind + "<ApproxTransformer>\n";
        x += ind + "  <MaxError>" + fmtG(maxErr) + "</MaxError>\n";
        x += ind + "  <BaseTransformer>\n";
        ind += "    ";
    }
    x += ind + "<GenImgProjTransformer>\n";
    x += ind + "  <SrcGeoTransform>" + join17(srcGt, 6) +
         "</SrcGeoTransform>\n";
    x += ind + "  <SrcInvGeoTransform>" + join17(srcInv, 6) +
         "</SrcInvGeoTransform>\n";
    x += ind + "  <DstGeoTransform>" + join17(gt, 6) +
         "</DstGeoTransform>\n";
    x += ind + "  <DstInvGeoTransform>" + join17(dstInv, 6) +
         "</DstInvGeoTransform>\n";
    if (op)
    {
        const Srs &ssrs = haveSrcSrsOverride
                              ? srcSrsOverride
                              : (srcSrsIsDst ? srs : src->srs);
        x += ind + "  <ReprojectTransformer>\n";
        x += ind + "    <ReprojectionTransformer>\n";
        x += ind + "      <SourceSRS>" + wxmlTextEsc(ssrs.wkt1Gdal()) +
             "</SourceSRS>\n";
        x += ind + "      <TargetSRS>" + wxmlTextEsc(srs.wkt1Gdal()) +
             "</TargetSRS>\n";
        if (ssrs.isGeographic())
        {
            double xs[4], ys[4];
            double sw = src->width, sh = src->height;
            xs[0] = srcGt[0];
            ys[0] = srcGt[3];
            xs[1] = srcGt[0] + sw * srcGt[1];
            ys[1] = srcGt[3] + sw * srcGt[4];
            xs[2] = srcGt[0] + sh * srcGt[2];
            ys[2] = srcGt[3] + sh * srcGt[5];
            xs[3] = srcGt[0] + sw * srcGt[1] + sh * srcGt[2];
            ys[3] = srcGt[3] + sw * srcGt[4] + sh * srcGt[5];
            double mnx = xs[0], mxx = xs[0], mny = ys[0], mxy = ys[0];
            for (int i = 1; i < 4; ++i)
            {
                mnx = std::min(mnx, xs[i]);
                mxx = std::max(mxx, xs[i]);
                mny = std::min(mny, ys[i]);
                mxy = std::max(mxy, ys[i]);
            }
            x += ind + "      <Options>\n";
            x += ind + "        <Option key=\"CENTER_LONG\">" +
                 fmtG((mnx + mxx) / 2.0) + "</Option>\n";
            x += ind + "        <Option key=\"AREA_OF_INTEREST\">" +
                 fmt16(mnx) + "," + fmt16(mny) + "," + fmt16(mxx) + "," +
                 fmt16(mxy) + "</Option>\n";
            x += ind + "      </Options>\n";
        }
        x += ind + "    </ReprojectionTransformer>\n";
        x += ind + "  </ReprojectTransformer>\n";
    }
    x += ind + "</GenImgProjTransformer>\n";
    if (approx)
    {
        x += "        </BaseTransformer>\n";
        x += "      </ApproxTransformer>\n";
    }
    x += "    </Transformer>\n";
    x += "    <BandList>\n";
    for (size_t i = 0; i < src->bands.size(); ++i)
    {
        bool kids = srcNd[i].has || dstNd[i].has;
        if (!kids)
        {
            x += strPrintf("      <BandMapping src=\"%d\" dst=\"%d\" />\n",
                           (int)i + 1, (int)i + 1);
            continue;
        }
        x += strPrintf("      <BandMapping src=\"%d\" dst=\"%d\">\n",
                       (int)i + 1, (int)i + 1);
        if (srcNd[i].has)
        {
            x += "        <SrcNoDataReal>" + fmt16(srcNd[i].v) +
                 "</SrcNoDataReal>\n";
            x += "        <SrcNoDataImag>0</SrcNoDataImag>\n";
        }
        if (dstNd[i].has)
        {
            x += "        <DstNoDataReal>" + fmt16(dstNd[i].v) +
                 "</DstNoDataReal>\n";
            x += "        <DstNoDataImag>0</DstNoDataImag>\n";
        }
        x += "      </BandMapping>\n";
    }
    x += "    </BandList>\n";
    if (alpha)
        x += strPrintf("    <DstAlphaBand>%d</DstAlphaBand>\n",
                       (int)bands.size());
    x += "  </GDALWarpOptions>\n";
    x += "</VRTDataset>\n";
    return x;
}

}  // namespace

WarpParams warpFillParams(const WarpGetter &get)
{
    WarpParams p;
    const CmdSpec *cs = Spec::instance().findById("raster_reproject");
    auto first = [&](const std::string &n) -> const std::string *
    {
        const auto *v = get(n);
        return v && !v->empty() ? &(*v)[0] : nullptr;
    };
    // pipeline steps keep packed lists as one comma-joined token while
    // the leaf parser splits them
    auto flat = [&](const std::string &n) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        if (const auto *v = get(n))
            for (const auto &s : *v)
                for (const auto &part : strSplit(s, ','))
                    out.push_back(part);
        return out;
    };
    if (const std::string *v = first("src-crs"))
        p.srcCrs = *v;
    if (const std::string *v = first("dst-crs"))
        p.dstCrs = *v;
    if (const std::string *v = first("bbox-crs"))
        p.bboxCrs = *v;
    if (const std::string *v = first("like"))
        p.like = *v;
    if (const std::string *v = first("resampling"))
    {
        p.resamplingSet = true;
        p.resampling = *v;
        if (cs)
            for (const auto &a : cs->args)
                if (a.name == "resampling")
                    for (const auto &c : a.choices)
                        if (strEqualNoCase(c, *v))
                            p.resampling = c;
    }
    if (auto v = flat("resolution"); v.size() == 2)
    {
        p.hasRes = true;
        p.resX = strtod(v[0].c_str(), nullptr);
        p.resY = strtod(v[1].c_str(), nullptr);
    }
    if (auto v = flat("size"); v.size() == 2)
    {
        p.hasSize = true;
        p.sizeW = strtoll(v[0].c_str(), nullptr, 10);
        p.sizeH = strtoll(v[1].c_str(), nullptr, 10);
    }
    if (auto v = flat("bbox"); v.size() == 4)
    {
        p.hasBbox = true;
        for (int i = 0; i < 4; ++i)
            p.bbox[i] = strtod(v[i].c_str(), nullptr);
    }
    if (const auto *v = get("target-aligned-pixels"); v && !v->empty())
        p.tap = (*v)[0] == "true";
    if (auto v = flat("src-nodata"); !v.empty())
    {
        p.hasSrcNodata = true;
        p.srcNodata = v;
    }
    if (auto v = flat("dst-nodata"); !v.empty())
    {
        p.hasDstNodata = true;
        p.dstNodata = v;
    }
    if (const auto *v = get("add-alpha"); v && !v->empty())
        p.addAlpha = (*v)[0] == "true";
    if (const auto *v = get("warp-option"))
        p.wo = *v;
    if (const auto *v = get("transform-option"))
        p.to = *v;
    if (const std::string *v = first("error-threshold"))
    {
        p.hasEt = true;
        p.et = strtod(v->c_str(), nullptr);
    }
    if (const std::string *v = first("num-threads"))
    {
        p.hasNumThreads = true;
        p.numThreads = *v;
    }
    return p;
}

bool warpNumThreadsValid(const std::string &v)
{
    if (strEqualNoCase(v, "ALL_CPUS"))
        return true;
    if (v.empty())
        return false;
    char *endp = nullptr;
    strtoll(v.c_str(), &endp, 10);
    return *endp == '\0';
}

std::string warpArgsEcho(const WarpParams &p)
{
    std::string e;
    if (!p.srcCrs.empty())
        e += " --src-crs " + p.srcCrs;
    if (!p.like.empty())
        e += " --like " + p.like;
    if (!p.dstCrs.empty())
        e += " --dst-crs " + p.dstCrs;
    if (p.resamplingSet)
        e += " --resampling " + p.resampling;
    if (p.hasRes)
        e += " --resolution " + fmt17(p.resX) + "," + fmt17(p.resY);
    if (p.hasSize)
        e += strPrintf(" --size %lld,%lld", p.sizeW, p.sizeH);
    if (p.hasBbox)
        e += " --bbox " + fmt17(p.bbox[0]) + "," + fmt17(p.bbox[1]) + "," +
             fmt17(p.bbox[2]) + "," + fmt17(p.bbox[3]);
    if (!p.bboxCrs.empty())
        e += " --bbox-crs " + p.bboxCrs;
    if (p.tap)
        e += " --target-aligned-pixels";
    auto joinList = [](const std::vector<std::string> &v)
    {
        std::string r;
        for (const auto &s : v)
        {
            if (!r.empty())
                r += ",";
            r += s;
        }
        return r;
    };
    if (p.hasSrcNodata)
        e += " --src-nodata " + joinList(p.srcNodata);
    if (p.hasDstNodata)
        e += " --dst-nodata " + joinList(p.dstNodata);
    if (p.addAlpha)
        e += " --add-alpha";
    for (const auto &o : p.wo)
        e += " --warp-option " + o;
    for (const auto &o : p.to)
        e += " --transform-option " + o;
    if (p.hasEt)
        e += " --error-threshold " + fmt17(p.et);
    if (p.hasNumThreads)
        e += " --num-threads " + p.numThreads;
    return e;
}

int warpWrap(const WarpParams &p0, std::unique_ptr<RasterDatasetBase> &ds,
             bool leafUsage)
{
    WarpParams p = p0;
    if (!p.like.empty() && (p.hasRes || p.hasSize || p.hasBbox))
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "-tr and -ts options cannot be used at the same "
                    "time.");
        return 1;
    }
    if (p.tap && !p.hasRes)
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "-tap option cannot be used without using -tr.");
        if (leafUsage)
            return 1;
        g_pipelineDeferredFail = true;
        p.tap = false;
    }
    RasterDatasetBase *src = ds.get();
    bool noGtBypass = false;
    for (const auto &o : p.to)
        if (strEqualNoCase(o, "SRC_METHOD=NO_GEOTRANSFORM"))
            noGtBypass = true;
    if (!src->hasGT && !noGtBypass)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Unable to compute a transformation between "
                    "pixel/line and georeferenced coordinates for " +
                        src->path +
                        ". There is no affine transformation and no "
                        "GCPs. Specify transformation option "
                        "SRC_METHOD=NO_GEOTRANSFORM to bypass this "
                        "check.");
        return 1;
    }

    bool ok = false;
    Srs srcSrsOverride;
    bool haveSrcOverride = false;
    if (!p.srcCrs.empty())
    {
        srcSrsOverride = Srs::fromCliInput(p.srcCrs, ok);
        haveSrcOverride = ok;
    }
    PJ *srcPj = haveSrcOverride
                    ? srcSrsOverride.pj()
                    : (src->hasSrs && src->srs.valid() ? src->srs.pj()
                                                       : nullptr);
    bool haveSrcSrs = srcPj != nullptr;

    Srs dstSrsOwn;
    bool haveDstOwn = false;
    std::unique_ptr<RasterDatasetBase> likeDs;
    if (!p.like.empty())
    {
        std::string err;
        likeDs = openRaster(p.like, err);
        if (!likeDs)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + p.like +
                            "' not recognized as being in a supported "
                            "file format.");
            return 1;
        }
        likeDs->replaySrsDecodeWarnings();
        if (likeDs->hasSrs && likeDs->srs.valid())
        {
            dstSrsOwn = std::move(likeDs->srs);
            haveDstOwn = true;
        }
    }
    else if (!p.dstCrs.empty())
    {
        dstSrsOwn = Srs::fromCliInput(p.dstCrs, ok);
        haveDstOwn = ok;
    }

    // the reprojection transformer is created even when source and
    // destination CRS are identical (identity op); a vertical component
    // on either side promotes both to 3D (PROMOTE_TO_3D)
    PJ *op = nullptr;
    bool srcVert = false, dstVert = false;
    if (haveSrcSrs)
    {
        srcVert = warpCrsHasVertical(srcPj);
        dstVert = haveDstOwn && warpCrsHasVertical(dstSrsOwn.pj());
        PJ *dstP = haveDstOwn ? dstSrsOwn.pj() : srcPj;
        op = haveDstOwn && (srcVert || dstVert) ? makeOpZ(srcPj, dstP)
                                                : makeOp(srcPj, dstP);
    }

    double srcGtLocal[6] = {0, 1, 0, 0, 0, 1};
    if (src->hasGT)
        memcpy(srcGtLocal, src->gt, sizeof srcGtLocal);
    double sinv[6];
    invGeoTransform(srcGtLocal, sinv);

    // the grid passthrough compares horizontal components only: a
    // vertical member on either side leaves the pixel grid untouched
    auto sameHorizontal = [&](PJ *a, PJ *b)
    {
        PJ *ha = proj_get_type(a) == PJ_TYPE_COMPOUND_CRS
                     ? proj_crs_get_sub_crs(projCtx(), a, 0)
                     : nullptr;
        PJ *hb = proj_get_type(b) == PJ_TYPE_COMPOUND_CRS
                     ? proj_crs_get_sub_crs(projCtx(), b, 0)
                     : nullptr;
        bool r = proj_is_equivalent_to_with_ctx(projCtx(), ha ? ha : a,
                                                hb ? hb : b, 2);
        if (ha)
            proj_destroy(ha);
        if (hb)
            proj_destroy(hb);
        return r;
    };
    bool sameCrs = !haveDstOwn ||
                   (haveSrcSrs && sameHorizontal(srcPj, dstSrsOwn.pj()));
    GridResult g;
    if (likeDs)
    {
        g.W = likeDs->width;
        g.H = likeDs->height;
        memcpy(g.gt, likeDs->gt, sizeof g.gt);
    }
    else
    {
        double bboxDst[4];
        const double *bb = nullptr;
        if (p.hasBbox)
        {
            memcpy(bboxDst, p.bbox, sizeof bboxDst);
            if (!p.bboxCrs.empty())
            {
                bool bok = false;
                Srs bboxSrs = Srs::fromCliInput(p.bboxCrs, bok);
                if (bok && haveDstOwn &&
                    bboxSrs.wkt1Gdal() !=
                        (haveDstOwn ? dstSrsOwn.wkt1Gdal()
                                    : std::string()))
                {
                    PJ *op2 = makeOp(bboxSrs.pj(), dstSrsOwn.pj());
                    if (op2)
                    {
                        transformBboxDensified(op2, p.bbox, bboxDst);
                        proj_destroy(op2);
                    }
                }
            }
            bb = bboxDst;
        }
        g = predictGrid(p, bb, srcGtLocal, sinv, src->width, src->height,
                        op, sameCrs, srcVert || dstVert);
    }

    auto w = std::make_unique<WarpedDataset>(std::move(ds), g);
    w->op = op;
    w->sameCrsWarp_ = sameCrs;
    // band values pass through the vertical component of the operation:
    // single-band sources only, either side compound or 3-axis
    w->vertWarp_ = op && haveDstOwn && (srcVert || dstVert);
    w->explicitExtent_ = p.hasBbox || likeDs != nullptr;
    if (w->vertWarp_ && w->src->bands.size() == 1)
        w->opZ = makeOpZ(srcPj, dstSrsOwn.pj());
    // a vertical CRS dropped by the target keeps the unit the source
    // bands carried from their file; an override CRS contributes none
    if (srcVert && !dstVert && haveDstOwn && !haveSrcOverride &&
        !w->src->bands.empty())
        w->vertUnitName_ = w->src->bands[0].unitType;
    // "near" is a hidden alias kept verbatim in echoes but warped as nearest
    w->resampling = strEqualNoCase(p.resampling, "near") ? "nearest"
                                                         : p.resampling;
    w->alpha = p.addAlpha;
    w->woUser = p.wo;
    w->toUser = p.to;
    w->pixGrid_ = p.pixGrid;
    w->pgOX_ = p.pgOX;
    w->pgOY_ = p.pgOY;
    w->pgIX_ = p.pgIX;
    w->pgIY_ = p.pgIY;
    if (op && haveSrcSrs)
    {
        double cs[4][2] = {{0, 0},
                           {(double)src->width, 0},
                           {0, (double)src->height},
                           {(double)src->width, (double)src->height}};
        double mnx = HUGE_VAL, mny = HUGE_VAL;
        double mxx = -HUGE_VAL, mxy = -HUGE_VAL;
        for (auto &c : cs)
        {
            double gx = srcGtLocal[0] + c[0] * srcGtLocal[1] +
                        c[1] * srcGtLocal[2];
            double gy = srcGtLocal[3] + c[0] * srcGtLocal[4] +
                        c[1] * srcGtLocal[5];
            mnx = std::min(mnx, gx);
            mxx = std::max(mxx, gx);
            mny = std::min(mny, gy);
            mxy = std::max(mxy, gy);
        }
        const Srs &sref =
            haveSrcOverride ? srcSrsOverride : w->src->srs;
        bool geo = sref.valid() && sref.isGeographic();
        double aoi[4] = {mnx, mny, mxx, mxy};
        bool okAoi = geo;
        if (!geo)
        {
            okAoi = false;
            PJ *geod = proj_crs_get_geodetic_crs(projCtx(), srcPj);
            if (geod)
            {
                PJ *op2 = makeOp(srcPj, geod);
                if (op2)
                {
                    double in[4] = {mnx, mny, mxx, mxy};
                    okAoi = transformBboxDensified(op2, in, aoi);
                    proj_destroy(op2);
                }
                proj_destroy(geod);
            }
        }
        if (okAoi)
        {
            w->dbgAoiSet_ = true;
            memcpy(w->dbgAoi_, aoi, sizeof aoi);
            w->dbgWrap_ = geo;
        }
    }
    if (p.hasEt)
    {
        w->etExplicit = true;
        w->etValue = p.et;
        w->maxErr = p.et;
    }
    if (p.hasNumThreads && !strEqualNoCase(p.numThreads, "ALL_CPUS"))
        w->numThreadsXml = p.numThreads;
    else
        w->numThreadsXml = strPrintf("%d", hardwareCpuCount());
    if (haveSrcOverride)
    {
        w->srcSrsOverride = std::move(srcSrsOverride);
        w->haveSrcSrsOverride = true;
    }
    if (haveDstOwn)
    {
        w->srs = std::move(dstSrsOwn);
        w->hasSrs = true;
    }
    else if (w->src->hasSrs && w->src->srs.valid())
    {
        w->srs = std::move(w->src->srs);
        w->hasSrs = true;
        w->srcSrsIsDst = true;
    }

    // nodata resolution: CLI overrides propagate to the output; the
    // source's intrinsic nodata is inherited on both sides otherwise
    size_t nb = w->src->bands.size();
    w->srcNd.resize(nb);
    w->dstNd.resize(nb);
    for (size_t i = 0; i < nb; ++i)
    {
        if (w->src->bands[i].hasNodata)
        {
            w->srcNd[i].has = true;
            w->srcNd[i].v = w->src->bands[i].nodata;
        }
    }
    auto parseNdList = [&](const std::vector<std::string> &vals,
                           std::vector<BandNd> &out) -> bool
    {
        // "None" clears
        if (vals.size() == 1 && strEqualNoCase(vals[0], "None"))
        {
            for (auto &nd : out)
                nd.has = false;
            return true;
        }
        std::vector<double> parsed;
        for (const auto &v : vals)
            parsed.push_back(strtod(v.c_str(), nullptr));
        for (size_t i = 0; i < nb; ++i)
        {
            out[i].has = true;
            out[i].v = parsed[parsed.size() == 1 ? 0
                                                 : std::min(i, parsed.size() -
                                                                   1)];
        }
        return true;
    };
    if (p.hasSrcNodata)
        parseNdList(p.srcNodata, w->srcNd);
    for (size_t i = 0; i < nb; ++i)
        w->dstNd[i] = w->srcNd[i];
    if (p.hasDstNodata)
    {
        parseNdList(p.dstNodata, w->dstNd);
        // the warning is emitted per provided list element, not per
        // broadcast band
        bool none = p.dstNodata.size() == 1 &&
                    strEqualNoCase(p.dstNodata[0], "None");
        w->dstNdCli_ = !none;
        w->dstNdCliCount_ = none ? 0 : p.dstNodata.size();
        size_t nWarn = none ? 0 : std::min(nb, p.dstNodata.size());
        for (size_t i = 0; i < nb; ++i)
        {
            DType t = w->src->bands[i].type;
            bool isInt =
                t != DType::Float32 && t != DType::Float64 &&
                t != DType::CFloat32 && t != DType::CFloat64;
            if (!isInt || !w->dstNd[i].has ||
                w->dstNd[i].v == std::floor(w->dstNd[i].v))
                continue;
            double r = std::round(w->dstNd[i].v);
            if (i < nWarn)
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf("for band %d, destination nodata value "
                              "has been rounded to %g, %s being an "
                              "integer datatype.",
                              (int)i + 1, r, dtypeName(t)));
            w->dstNd[i].v = r;
        }
    }
    else if (p.addAlpha)
    {
        // the alpha channel replaces inherited nodata semantics
        for (auto &nd : w->dstNd)
            nd.has = false;
    }
    w->unifiedSrcNodata = p.hasSrcNodata &&
                          !(p.srcNodata.size() == 1 &&
                            strEqualNoCase(p.srcNodata[0], "None"));
    // an explicit UNIFIED_SRC_NODATA warp option wins and silences the
    // "Set UNIFIED_SRC_NODATA=YES" debug that the implicit path prints
    bool unifiedExplicit = false;
    for (const auto &o : p.wo)
        if (o.compare(0, 19, "UNIFIED_SRC_NODATA=") == 0)
        {
            unifiedExplicit = true;
            w->unifiedSrcNodata =
                strEqualNoCase(o.substr(19), "YES");
        }
    w->unifiedDbg_ = w->unifiedSrcNodata && !unifiedExplicit;
    // INIT_DEST resolves to NO_DATA whenever --dst-nodata was given
    // (including None); without any defined value the warp core errors
    // once per band and falls back to zero-init
    std::string initDest = p.hasDstNodata ? "NO_DATA" : "";
    for (const auto &o : p.wo)
        if (o.compare(0, 10, "INIT_DEST=") == 0)
            initDest = o.substr(10);
    bool anyDstHas = false;
    for (const auto &nd : w->dstNd)
        if (nd.has)
            anyDstHas = true;
    w->initDestNdErr_ =
        strEqualNoCase(initDest, "NO_DATA") && !anyDstHas;
    if (p.addAlpha && !w->src->bands.empty())
    {
        DType at = w->src->bands.back().type;
        w->alphaDbg_ = at != DType::Int16 && at != DType::UInt16;
        for (const auto &o : p.wo)
            if (o.compare(0, 14, "DST_ALPHA_MAX=") == 0)
            {
                w->alphaDbg_ = false;
                w->hasAlphaMaxOv_ = true;
                w->alphaMaxOv_ = strtod(o.c_str() + 14, nullptr);
            }
    }
    // explicit source nodata on a multiband warp routes through a kernel
    // that skips the collision nudge
    w->shiftAllowed_ = !(w->unifiedSrcNodata && nb >= 2);
    w->finishBands();
    ds = std::move(w);
    return 0;
}

std::unique_ptr<RasterDatasetBase> openWarpedVrt(const XmlNode &root,
                                                 const std::string &path,
                                                 std::string &err)
{
    err = "reported";
    const XmlNode *wopt = root.child("GDALWarpOptions");
    if (!wopt)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Count not find required GDALWarpOptions in XML.");
        return nullptr;
    }
    int W = atoi(root.attr("rasterXSize", "0").c_str());
    int H = atoi(root.attr("rasterYSize", "0").c_str());
    if (W <= 0 || H <= 0)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Missing one of rasterXSize, rasterYSize or bands on"
                    " VRTDataset.");
        return nullptr;
    }
    const XmlNode *sd = wopt->child("SourceDataset");
    if (!sd || sd->text.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Count not find required SourceDataset in XML.");
        return nullptr;
    }
    std::string resolved = sd->text;
    bool rel = atoi(sd->attr("relativeToVRT", "0").c_str()) == 1;
    if (rel && !resolved.empty() && resolved[0] != '/')
    {
        size_t slash = path.rfind('/');
        if (slash != std::string::npos)
            resolved = path.substr(0, slash) + "/" + resolved;
    }
    if (!vsiExists(resolved))
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    resolved + ": No such file or directory");
        return nullptr;
    }
    std::string oerr;
    auto srcds = openRaster(resolved, oerr);
    if (!srcds)
    {
        cplErrorStr(CE_Failure, CPLE_OpenFailed,
                    "`" + resolved +
                        "' not recognized as being in a supported file "
                        "format.");
        return nullptr;
    }
    size_t nb = srcds->bands.size();

    GridResult g;
    g.W = W;
    g.H = H;
    if (const XmlNode *gtn = root.child("GeoTransform"))
    {
        std::vector<std::string> parts = strSplit(gtn->text, ',');
        for (size_t i = 0; i < 6 && i < parts.size(); ++i)
            g.gt[i] = strtod(parts[i].c_str(), nullptr);
    }

    auto w = std::make_unique<WarpedDataset>(std::move(srcds), g);
    w->path = path;
    w->driverShort = "VRT";
    w->driverLong = "Virtual Raster";
    w->files.clear();
    w->files.push_back(path);
    w->files.push_back(resolved);
    w->metadata.clear();
    w->domainOrder.clear();
    w->sortedDomains.clear();
    w->xmlDomains.clear();
    w->pamPath = path + ".aux.xml";
    w->pamExists = vsiExists(w->pamPath);
    for (const auto &c : root.children)
    {
        if (c.name != "Metadata")
            continue;
        std::string dom = c.attr("domain");
        MetaDomain items;
        for (const auto &m : c.children)
            if (m.name == "MDI")
                items.push_back({m.attr("key"), m.text});
        if (items.empty())
            continue;
        w->metadata[dom] = std::move(items);
        w->domainOrder.push_back(dom);
        w->sortedDomains.push_back(dom);
    }
    std::sort(w->sortedDomains.begin(), w->sortedDomains.end());

    if (const XmlNode *sn = root.child("SRS"))
    {
        bool ok = false;
        Srs s = Srs::fromUserInput(sn->text, ok);
        if (ok)
        {
            w->srs = std::move(s);
            w->hasSrs = true;
        }
    }

    w->srcNd.assign(nb, BandNd());
    w->dstNd.assign(nb, BandNd());
    if (const XmlNode *bl = wopt->child("BandList"))
        for (const auto &bm : bl->children)
        {
            if (bm.name != "BandMapping")
                continue;
            int sb = atoi(bm.attr("src", "0").c_str());
            if (sb < 1 || (size_t)sb > nb)
                continue;
            if (const XmlNode *v = bm.child("SrcNoDataReal"))
            {
                w->srcNd[(size_t)sb - 1].has = true;
                w->srcNd[(size_t)sb - 1].v =
                    strtod(v->text.c_str(), nullptr);
            }
            if (const XmlNode *v = bm.child("DstNoDataReal"))
            {
                w->dstNd[(size_t)sb - 1].has = true;
                w->dstNd[(size_t)sb - 1].v =
                    strtod(v->text.c_str(), nullptr);
            }
        }
    if (const XmlNode *ab = wopt->child("DstAlphaBand"))
        w->alpha = atoi(ab->text.c_str()) > 0;

    if (const XmlNode *ra = wopt->child("ResampleAlg"))
    {
        static const std::pair<const char *, const char *> kAlgs[] = {
            {"NearestNeighbour", "nearest"}, {"Bilinear", "bilinear"},
            {"Cubic", "cubic"},              {"CubicSpline", "cubicspline"},
            {"Lanczos", "lanczos"},          {"Average", "average"},
            {"RMS", "rms"},                  {"Mode", "mode"},
            {"Minimum", "min"},              {"Maximum", "max"},
            {"Median", "med"},               {"Q1", "q1"},
            {"Q3", "q3"},                    {"Sum", "sum"}};
        for (const auto &kv : kAlgs)
            if (ra->text == kv.first)
                w->resampling = kv.second;
    }

    for (const auto &o : wopt->children)
    {
        if (o.name != "Option")
            continue;
        std::string key = o.attr("name");
        if (key == "NUM_THREADS")
            w->numThreadsXml = o.text;
        else if (key == "ERROR_THRESHOLD")
        {
            w->etExplicit = true;
            w->etValue = strtod(o.text.c_str(), nullptr);
        }
        else if (key == "UNIFIED_SRC_NODATA")
            w->unifiedSrcNodata = strEqualNoCase(o.text, "YES");
        else if (key != "INIT_DEST" &&
                 key != "ERROR_OUT_IF_EMPTY_SOURCE_WINDOW" &&
                 key != "DST_ALPHA_MAX")
            w->woUser.push_back(key + "=" + o.text);
    }
    if (w->numThreadsXml.empty())
        w->numThreadsXml = "1";

    const XmlNode *gen = nullptr;
    w->maxErr = 0;
    if (const XmlNode *tr = wopt->child("Transformer"))
    {
        if (const XmlNode *ap = tr->child("ApproxTransformer"))
        {
            if (const XmlNode *me = ap->child("MaxError"))
                w->maxErr = strtod(me->text.c_str(), nullptr);
            if (const XmlNode *bt = ap->child("BaseTransformer"))
                gen = bt->child("GenImgProjTransformer");
        }
        else
            gen = tr->child("GenImgProjTransformer");
    }
    if (gen)
    {
        auto parse6 = [&](const char *n, double *out)
        {
            const XmlNode *e = gen->child(n);
            if (!e)
                return false;
            std::vector<std::string> parts = strSplit(e->text, ',');
            if (parts.size() < 6)
                return false;
            for (int i = 0; i < 6; ++i)
                out[i] = strtod(parts[i].c_str(), nullptr);
            return true;
        };
        double tmp[6];
        if (parse6("SrcGeoTransform", tmp))
        {
            memcpy(w->srcGt, tmp, sizeof tmp);
            invGeoTransform(w->srcGt, w->srcInv);
        }
        if (parse6("SrcInvGeoTransform", tmp))
            memcpy(w->srcInv, tmp, sizeof tmp);
        const XmlNode *rp = gen->child("ReprojectTransformer");
        const XmlNode *rj = rp ? rp->child("ReprojectionTransformer")
                               : nullptr;
        if (rj)
        {
            const XmlNode *ss = rj->child("SourceSRS");
            const XmlNode *ts = rj->child("TargetSRS");
            bool ok1 = false, ok2 = false;
            Srs s1, s2;
            if (ss)
                s1 = Srs::fromUserInput(ss->text, ok1);
            if (ts)
                s2 = Srs::fromUserInput(ts->text, ok2);
            if (ok1 && ok2)
            {
                w->op = makeOp(s1.pj(), s2.pj());
                w->srcSrsOverride = std::move(s1);
                w->haveSrcSrsOverride = true;
            }
        }
    }

    w->shiftAllowed_ = !(w->unifiedSrcNodata && nb >= 2);
    w->blockChunked_ = true;
    w->finishBands();
    int bx = 0, by = 0;
    if (const XmlNode *n = root.child("BlockXSize"))
        bx = atoi(n->text.c_str());
    if (const XmlNode *n = root.child("BlockYSize"))
        by = atoi(n->text.c_str());
    size_t bi = 0;
    for (const auto &c : root.children)
    {
        if (c.name != "VRTRasterBand")
            continue;
        if (bi >= w->bands.size())
            break;
        Band &b = w->bands[bi];
        std::string dt = c.attr("dataType");
        if (!dt.empty() && dtypeFromName(dt) != DType::Unknown)
            b.type = dtypeFromName(dt);
        if (const XmlNode *nd = c.child("NoDataValue"))
        {
            b.hasNodata = true;
            b.nodata = strtod(nd->text.c_str(), nullptr);
        }
        if (const XmlNode *ci = c.child("ColorInterp"))
            b.colorInterp = ci->text;
        ++bi;
    }
    for (Band &b : w->bands)
    {
        if (bx > 0)
            b.blockX = bx;
        if (by > 0)
            b.blockY = by;
    }
    err.clear();
    return w;
}
