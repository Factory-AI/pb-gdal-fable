#include "cpl.h"
#include "dataset.h"
#include "embedded.h"
#include "engine.h"
#include "gtiff_write.h"
#include "progress.h"
#include "proj_min.h"
#include "spec.h"
#include "srs.h"
#include "util.h"
#include "warp.h"

#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

extern char **environ;

namespace
{

double kernelWeightTl(const std::string &m, double t)
{
    double a = fabs(t);
    if (m == "bilinear")
        return a < 1.0 ? 1.0 - a : 0.0;
    if (m == "cubic")
    {
        if (a <= 1.0)
            return (1.5 * a - 2.5) * a * a + 1.0;
        if (a < 2.0)
            return -0.5 * (((a - 5.0) * a + 8.0) * a - 4.0);
        return 0.0;
    }
    if (m == "cubicspline")
    {
        double xp2 = t + 2.0, xp1 = t + 1.0, xm1 = t - 1.0;
        double r = xp2 > 0.0 ? xp2 * xp2 * xp2 : 0.0;
        if (xp1 > 0.0)
            r -= 4.0 * xp1 * xp1 * xp1;
        if (t > 0.0)
            r += 6.0 * t * t * t;
        if (xm1 > 0.0)
            r -= 4.0 * xm1 * xm1 * xm1;
        return r;
    }
    if (m == "lanczos")
    {
        if (a >= 3.0)
            return 0.0;
        if (a == 0.0)
            return 1.0;
        double px = M_PI * a;
        double pxr = px / 3.0;
        return sin(px) * sin(pxr) / (px * pxr);
    }
    return 0.0;
}

double kernelRadiusTl(const std::string &m)
{
    if (m == "bilinear")
        return 1.0;
    if (m == "lanczos")
        return 3.0;
    return 2.0;
}

double roundStoreTl(double v, DType dt)
{
    double lo = 0, hi = 0;
    bool isInt = true;
    switch (dt)
    {
        case DType::Byte:
            lo = 0;
            hi = 255;
            break;
        case DType::Int8:
            lo = -128;
            hi = 127;
            break;
        case DType::UInt16:
            lo = 0;
            hi = 65535;
            break;
        case DType::Int16:
            lo = -32768;
            hi = 32767;
            break;
        case DType::UInt32:
            lo = 0;
            hi = 4294967295.0;
            break;
        case DType::Int32:
            lo = -2147483648.0;
            hi = 2147483647.0;
            break;
        default:
            isInt = false;
            break;
    }
    if (!isInt)
        return v;
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v < 0 ? -floor(-v + 0.5) : floor(v + 0.5);
}

// tile overview resampler (RasterIO flavor): output pixels map into the
// source with an exact power-of-two ratio; the mask plane (the finer
// level's alpha) marks valid pixels, invalid ones are excluded and the
// kernel renormalized
void tileResample(const std::vector<double> &src, int sw, int sh,
                  const std::vector<double> *mask, std::vector<double> &dst,
                  int dw, int dh, double rx, double ry,
                  const std::string &method, DType dt,
                  const std::vector<double> *exVals = nullptr,
                  double exPct = 50.0)
{
    auto valid = [&](int sx, int sy) {
        return !mask || (*mask)[(size_t)sy * sw + sx] > 0.0;
    };
    dst.assign((size_t)dw * dh, 0.0);
    if (method == "nearest")
    {
        for (int y = 0; y < dh; ++y)
        {
            int sy = (int)floor((y + 0.5) * ry);
            if (sy >= sh)
                sy = sh - 1;
            for (int x = 0; x < dw; ++x)
            {
                int sx = (int)floor((x + 0.5) * rx);
                if (sx >= sw)
                    sx = sw - 1;
                double v = src[(size_t)sy * sw + sx];
                if (!valid(sx, sy))
                    v = 0.0;
                dst[(size_t)y * dw + x] = v;
            }
        }
        return;
    }
    if (method == "average" || method == "rms" || method == "mode")
    {
        bool sq = method == "rms";
        bool md = method == "mode";
        std::vector<std::pair<double, int>> counts;
        for (int y = 0; y < dh; ++y)
        {
            int y0 = (int)(y * ry), y1 = (int)((y + 1) * ry);
            if (y1 <= y0)
                y1 = y0 + 1;
            if (y1 > sh)
                y1 = sh;
            for (int x = 0; x < dw; ++x)
            {
                int x0 = (int)(x * rx), x1 = (int)((x + 1) * rx);
                if (x1 <= x0)
                    x1 = x0 + 1;
                // mode zeroes any output pixel whose unclamped window
                // holds an invalid or out-of-canvas pixel
                if (md && mask)
                {
                    bool bad = x1 > sw || (int)((y + 1) * ry) > sh;
                    for (int sy = y0; sy < y1 && !bad; ++sy)
                        for (int sx = x0; sx < x1; ++sx)
                            if (!valid(sx, sy))
                            {
                                bad = true;
                                break;
                            }
                    if (bad)
                    {
                        dst[(size_t)y * dw + x] = 0.0;
                        continue;
                    }
                }
                if (x1 > sw)
                    x1 = sw;
                double tot = 0.0;
                int cnt = 0, exCnt = 0, considered = 0;
                double exHit = 0.0;
                counts.clear();
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                    {
                        if (!valid(sx, sy))
                            continue;
                        double v = src[(size_t)sy * sw + sx];
                        if (exVals && !exVals->empty())
                        {
                            ++considered;
                            bool ex = false;
                            for (double e : *exVals)
                                if (v == e)
                                {
                                    ex = true;
                                    exHit = e;
                                    break;
                                }
                            if (ex)
                            {
                                ++exCnt;
                                continue;
                            }
                        }
                        if (md)
                        {
                            bool found = false;
                            for (auto &c : counts)
                                if (c.first == v)
                                {
                                    c.second++;
                                    found = true;
                                    break;
                                }
                            if (!found)
                                counts.emplace_back(v, 1);
                        }
                        else
                        {
                            tot += sq ? v * v : v;
                            cnt++;
                        }
                    }
                double out = 0.0;
                if (md)
                {
                    int bestN = 0;
                    for (const auto &c : counts)
                        if (c.second > bestN)
                        {
                            bestN = c.second;
                            out = c.first;
                        }
                }
                else if (exVals && !exVals->empty() && considered > 0 &&
                         exCnt > 0 &&
                         100.0 * exCnt / considered >= exPct)
                    out = exHit;
                else if (cnt > 0)
                {
                    double r = tot / cnt;
                    out = roundStoreTl(sq ? sqrt(r) : r, dt);
                }
                dst[(size_t)y * dw + x] = out;
            }
        }
        return;
    }
    // separable convolution kernels; kernels with negative lobes zero
    // any output pixel whose mapped center pixel is invalid
    double radius = kernelRadiusTl(method);
    bool hasNeg = method == "cubic" || method == "lanczos";
    double sxs = 1.0 / rx, sys = 1.0 / ry;
    double rxk = radius * rx, ryk = radius * ry;
    std::vector<double> xw, yw;
    for (int y = 0; y < dh; ++y)
    {
        double cy = (y + 0.5) * ry - 0.5;
        int iy0 = (int)ceil(cy - ryk), iy1 = (int)floor(cy + ryk);
        if (iy0 < 0)
            iy0 = 0;
        if (iy1 >= sh)
            iy1 = sh - 1;
        yw.clear();
        {
            double a = sys * ((double)iy0 - ((y + 0.5) * ry) + 0.5);
            for (int iy = iy0; iy <= iy1; ++iy, a += sys)
                yw.push_back(kernelWeightTl(method, a));
        }
        for (int x = 0; x < dw; ++x)
        {
            double cx = (x + 0.5) * rx - 0.5;
            int ix0 = (int)ceil(cx - rxk), ix1 = (int)floor(cx + rxk);
            if (ix0 < 0)
                ix0 = 0;
            if (ix1 >= sw)
                ix1 = sw - 1;
            xw.clear();
            {
                double a = sxs * ((double)ix0 - ((x + 0.5) * rx) + 0.5);
                for (int ix = ix0; ix <= ix1; ++ix, a += sxs)
                    xw.push_back(kernelWeightTl(method, a));
            }
            double out = 0.0;
            if (mask)
            {
                // two asymmetric passes: horizontally each pixel is
                // weighted by its mask fraction and renormalized per
                // row, vertically rows with any weight count binarily;
                // negative-lobe kernels drop any row where valid
                // pixels are a minority of its window, and zero the
                // output when kept rows are a minority of the vertical
                // window (GDAL's majority-invalid guard)
                double num = 0, den = 0;
                int rowsKept = 0;
                for (int iy = iy0; iy <= iy1; ++iy)
                {
                    double tot = 0, ws = 0;
                    int nValid = 0;
                    for (int ix = ix0; ix <= ix1; ++ix)
                    {
                        double a = (*mask)[(size_t)iy * sw + ix];
                        if (a <= 0.0)
                            continue;
                        // the mask fraction lives in a float32 chunk
                        double w =
                            xw[ix - ix0] * (double)(float)(a / 255.0);
                        tot += w * src[(size_t)iy * sw + ix];
                        ws += w;
                        ++nValid;
                    }
                    if (hasNeg && 2 * nValid < ix1 - ix0 + 1)
                        continue;
                    if (ws > 0)
                    {
                        num += yw[iy - iy0] * (tot / ws);
                        den += yw[iy - iy0];
                        ++rowsKept;
                    }
                }
                if (hasNeg && 2 * rowsKept < iy1 - iy0 + 1)
                    den = 0;
                if (den != 0)
                    out = roundStoreTl(num / den, dt);
            }
            else
            {
                double tot = 0, wsum = 0;
                for (int iy = iy0; iy <= iy1; ++iy)
                {
                    double wy = yw[iy - iy0];
                    for (int ix = ix0; ix <= ix1; ++ix)
                    {
                        double w = xw[ix - ix0] * wy;
                        tot += w * src[(size_t)iy * sw + ix];
                        wsum += w;
                    }
                }
                if (wsum != 0)
                    out = roundStoreTl(tot / wsum, dt);
            }
            dst[(size_t)y * dw + x] = out;
        }
    }
}

std::string baseNameTl(const std::string &p)
{
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// CPLSPrintf("%.17g") flavor used by the viewer templates
std::string fmtG(double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

// json-c real formatting: %.17g with a ".0" suffix for plain integers
std::string fmtJ(double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') &&
        !strchr(buf, 'i'))
        strcat(buf, ".0");
    return buf;
}

std::string jsonEscTl(const std::string &s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += c;
        }
        else if (c == '\n')
            o += "\\n";
        else
            o += c;
    }
    return o;
}

const char *stacTypeName(DType t)
{
    switch (t)
    {
        case DType::Byte:
            return "uint8";
        case DType::Int8:
            return "int8";
        case DType::UInt16:
            return "uint16";
        case DType::Int16:
            return "int16";
        case DType::UInt32:
            return "uint32";
        case DType::Int32:
            return "int32";
        case DType::Float64:
            return "float64";
        default:
            return "float32";
    }
}

struct CrsOpTl
{
    PJ *op = nullptr;
    ~CrsOpTl()
    {
        if (op)
            proj_destroy(op);
    }
    bool init(const Srs &src, const Srs &dst)
    {
        PJ *raw = proj_create_crs_to_crs_from_pj(projCtx(), src.pj(),
                                                 dst.pj(), nullptr, nullptr);
        if (!raw)
            return false;
        PJ *norm = proj_normalize_for_visualization(projCtx(), raw);
        if (norm)
        {
            proj_destroy(raw);
            op = norm;
        }
        else
            op = raw;
        return true;
    }
    void transform(double &x, double &y) const
    {
        if (!op)
            return;
        PJ_COORD c = proj_coord(x, y, 0, HUGE_VAL);
        PJ_COORD o = proj_trans(op, PJ_FWD, c);
        if (isfinite(o.v[0]) && isfinite(o.v[1]))
        {
            x = o.v[0];
            y = o.v[1];
        }
    }
};

struct ZoomLevel
{
    int z = 0;
    int tx = 0, ty = 0;          // tile grid dims
    int minX = 0, maxX = 0;      // written tile range
    int minY = 0, maxY = 0;
    int mMinX = 0, mMaxX = 0;    // overview mosaic / limits tile range
    int mMinY = 0, mMaxY = 0;
    std::vector<std::vector<double>> canvas;  // per band, padded dims
    std::vector<double> mask;    // validity plane when no alpha is kept
    int pw = 0, ph = 0;          // padded canvas dims
    int iw = 0, ih = 0;          // image (data) dims at this zoom
    int cbx = 0, cby = 0;        // canvas origin in tile columns/rows
};

int rasterTilePreValidator(const CmdSpec &cmd, ParseResult &r)
{
    std::string format = r.str("output-format");
    if (!format.empty())
    {
        if (strEqualNoCase(format, "Memory"))
            memoryDriverDeprecationWarnOnce();
        if (strEqualNoCase(format, "GDALG"))
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        cmd.name + ": GDALG output is not supported.");
            handlerPrintUsage();
            return 1;
        }
        if (strEqualNoCase(format, "VRT"))
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        cmd.name + ": VRT output is not supported.");
            handlerPrintUsage();
            return 1;
        }
        if (strEqualNoCase(format, "MEM") ||
            strEqualNoCase(format, "Memory"))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name +
                            ": Invalid value for argument "
                            "'output-format'. Driver '" +
                            format +
                            "' does not advertise any file format "
                            "extension.");
            handlerPrintUsage();
            return 1;
        }
        if (!strEqualNoCase(format, "GTiff") &&
            !strEqualNoCase(format, "COG"))
        {
            bool ras = false, vec = false;
            std::string msg;
            if (knownDriverCaps(format, ras, vec) && !ras)
                msg = "Invalid value for argument 'output-format'. "
                      "Driver '" +
                      format +
                      "' does not expose the required 'DCAP_RASTER' "
                      "capability.";
            else
                msg = "Invalid value for argument 'output-format'. "
                      "Driver '" +
                      format + "' does not exist.";
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + msg);
            handlerPrintUsage();
            return 1;
        }
    }
    for (const auto &d : r.list("input-format"))
    {
        std::string ferr = inputFormatCapError(false, d);
        if (!ferr.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        cmd.name + ": " + ferr);
            handlerPrintUsage();
            return 1;
        }
    }
    return 0;
}

// zoom ordering error joins the validation-phase reports (after dataset
// existence checks, before the shared usage block)
bool rasterTilePostValidator(const CmdSpec &, ParseResult &r, bool)
{
    const ArgValue *mn = r.get("min-zoom");
    const ArgValue *mx = r.get("max-zoom");
    if (mn && mx && atoi(mn->str().c_str()) > atoi(mx->str().c_str()))
    {
        cplErrorStr(CE_Failure, CPLE_IllegalArg,
                    "tile: 'min-zoom' must be lesser or equal to "
                    "'max-zoom'");
        return true;
    }
    if (r.str("resampling", "cubic") != "average")
    {
        for (const char *n :
             {"excluded-values", "excluded-values-pct-threshold",
              "nodata-values-pct-threshold"})
            if (r.get(n))
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            std::string("tile: '") + n +
                                "' can only be specified if "
                                "'resampling' is set to 'average'");
                return true;
            }
    }
    return false;
}

bool mkdirIfNeeded(const std::string &p)
{
    struct stat st;
    if (stat(p.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(p.c_str(), 0755) == 0;
}

// --parallel-method spawn sanity-checks a `gdal` binary from PATH by
// running `gdal --version` at run entry, before any other run-time
// work (even before the default-PNG driver resolution); the actual
// tiling then still happens in-process here, which is byte-identical
// to the reference's worker spawning
int tileSpawnGdalCheck()
{
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    char arg0[] = "gdal", arg1[] = "--version";
    char *cargv[] = {arg0, arg1, nullptr};
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "gdal", &fa, nullptr, cargv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        cplErrorStr(CE_Failure, CPLE_AppDefined, "posix_spawnp() failed");
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Could not find 'gdal' binary. Make sure it is in "
                    "the PATH environment variable.");
        return 1;
    }
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0)
        out.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    std::string expected = embGet("misc/version.out");
    while (!expected.empty() &&
           (expected.back() == '\n' || expected.back() == '\r'))
        expected.pop_back();
    if (out.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Could not find 'gdal' binary. Make sure it is in "
                    "the PATH environment variable.");
        return 1;
    }
    if (out != expected)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "'gdal --version' returned '" + out + "', whereas '" +
                        expected +
                        "' expected. Make sure the gdal binary "
                        "corresponding to the version of the libgdal of "
                        "the current process is in the PATH environment "
                        "variable");
        return 1;
    }
    return 0;
}

int rasterTileHandler(const CmdSpec &cmd, ParseResult &r)
{
    if (strEqualNoCase(r.str("parallel-method"), "spawn") &&
        tileSpawnGdalCheck())
        return 1;
    std::string of = r.str("output-format");
    if (of.empty())
    {
        // the default PNG driver is absent from the trimmed build; this
        // late check reports without a trailing period or usage block
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    cmd.name +
                        ": Invalid value for argument 'output-format'. "
                        "Driver 'PNG' does not exist");
        return 1;
    }
    std::string input = r.str("input");
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");
    std::string scheme = r.str("tiling-scheme", "WebMercatorQuad");
    std::string err;
    cplPushQuietHandler();
    auto ds = openRaster(input, err);
    cplPopHandler();
    if (!ds)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    cmd.name + ": cannot open '" + input + "'");
        return 1;
    }
    bool wmq = strEqualNoCase(scheme, "WebMercatorQuad");
    if ((!ds->hasGT || (wmq && !ds->hasSrs)) &&
        !strEqualNoCase(scheme, "raster"))
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    cmd.name +
                        ": Ungeoreferenced datasets are not supported, "
                        "unless 'tiling-scheme' is set to 'raster'");
        return 1;
    }
    bool kml = r.flag("kml");
    if (kml && !ds->hasSrs)
    {
        // the output directory is already created by this point
        mkdirIfNeeded(output);
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    cmd.name +
                        ": Tiling scheme not compatible with KML output");
        return 1;
    }
    if (!strEqualNoCase(scheme, "raster") && !wmq)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    cmd.name +
                        ": this command is not implemented in this build");
        return 1;
    }

    int ts = 256;
    if (const ArgValue *v = r.get("tile-size"))
        ts = atoi(v->str().c_str());
    int W = ds->width, H = ds->height;
    int nb = (int)ds->bands.size();
    DType dt = nb ? ds->bands[0].type : DType::Byte;
    bool srcHasAlpha = nb > 1 && ds->bands[nb - 1].colorInterp == "Alpha";
    bool hasDstNodata = false;
    double dstNodata = 0;
    std::string dstNodataTxt;
    if (const ArgValue *v = r.get("dst-nodata"))
    {
        hasDstNodata = true;
        dstNodataTxt = v->str();
        dstNodata = atof(dstNodataTxt.c_str());
    }
    bool noAlpha = r.flag("no-alpha") || hasDstNodata;
    bool addAlpha = !noAlpha && !srcHasAlpha;
    int nAll = nb + (addAlpha ? 1 : 0);
    int alphaIdx = (addAlpha || srcHasAlpha) ? nAll - 1 : -1;

    bool hasGT = ds->hasGT;
    double gt[6] = {0, 1, 0, 0, 0, -1};
    if (hasGT)
        memcpy(gt, ds->gt, sizeof(gt));
    bool hasSrs = ds->hasSrs;

    // WebMercatorQuad grid: tile positions step by the zoom-0 cell size
    // literal halved per level; index products stay in pixel space
    const double kWmOrig = 20037508.342789244;
    const double kWmRes0 = 156543.03392804097;
    // the reference transforms tile CRS coordinates through a classic
    // spherical Mercator pipeline (half-pi form, not atan(sinh));
    // both axes multiply by the reciprocal radius and the rad-to-deg
    // constant - dividing by R instead lands +2 ulp on some latitudes
    auto wmInv = [](double &X, double &Y)
    {
        const double R = 6378137.0;
        X = X * (1.0 / R) * 57.29577951308232;
        Y = (M_PI / 2 - 2.0 * atan(exp(-(Y * (1.0 / R))))) *
            57.29577951308232;
    };
    auto wmInvKml = wmInv;
    auto wmRes = [&](int z) { return kWmRes0 / (double)(1 << z); };
    auto wmPosX = [&](int z, int64_t i)
    { return -kWmOrig + (double)(i * 256) * wmRes(z); };
    auto wmPosY = [&](int z, int64_t i)
    { return kWmOrig - (double)(i * 256) * wmRes(z); };
    double sug[4] = {0, 0, 0, 0};  // suggested warp xmin ymin xmax ymax
    double warpResEff = 0;         // base tile pixel resolution

    int baseZ = 0;
    if (wmq)
    {
        std::string oerr;
        cplPushQuietHandler();
        auto d1 = openRaster(input, oerr);
        cplPopHandler();
        WarpParams sp;
        sp.dstCrs = "EPSG:3857";
        if (!d1 || warpWrap(sp, d1, false) != 0)
            return 1;
        sug[0] = d1->gt[0];
        sug[3] = d1->gt[3];
        sug[2] = d1->gt[0] + (double)d1->width * d1->gt[1];
        sug[1] = d1->gt[3] + (double)d1->height * d1->gt[5];
        // the native zoom is the WebMercator level closest to the
        // suggested resolution in log2 space
        baseZ = (int)lround(log2(kWmRes0 / d1->gt[1]));
        if (baseZ < 0)
            baseZ = 0;
        if (baseZ > 30)
            baseZ = 30;
    }
    else
    {
        // native max zoom: ceil(log2(max(W/ts, H/ts))) with integer
        // division
        int q = W / ts > H / ts ? W / ts : H / ts;
        if (q < 1)
            q = 1;
        while ((1 << baseZ) < q)
            baseZ++;
    }
    int maxZ = baseZ, minZ = -1;
    if (const ArgValue *v = r.get("max-zoom"))
        maxZ = atoi(v->str().c_str());
    if (const ArgValue *v = r.get("min-zoom"))
        minZ = atoi(v->str().c_str());
    if (minZ < 0)
        minZ = maxZ;

    std::string resampling = r.str("resampling", "cubic");
    std::string ovrResampling = r.str("overview-resampling");
    if (ovrResampling.empty())
        ovrResampling = resampling;
    if (strEqualNoCase(ovrResampling, "near"))
        ovrResampling = "nearest";
    std::vector<double> exVals;
    double exPct = 50.0;
    if (const ArgValue *v = r.get("excluded-values"))
    {
        std::string s = v->str();
        std::string cur;
        for (char c : s + ",")
        {
            if (c == '(' || c == ')' || c == ' ')
                continue;
            if (c == ',')
            {
                if (!cur.empty())
                    exVals.push_back(atof(cur.c_str()));
                cur.clear();
            }
            else
                cur += c;
        }
    }
    if (const ArgValue *v = r.get("excluded-values-pct-threshold"))
        exPct = atof(v->str().c_str());

    // creation options
    std::vector<std::pair<std::string, std::string>> cos;
    for (const auto &c : r.list("creation-option"))
    {
        size_t eq = c.find('=');
        cos.push_back({c.substr(0, eq),
                       eq == std::string::npos ? "" : c.substr(eq + 1)});
    }
    CreationOptions co = parseCreationOptions(cos, output, cmd.name);
    if (co.fatal)
        return 1;
    bool coHasCompress = false;
    for (const auto &kv : cos)
        if (strToUpper(kv.first) == "COMPRESS")
            coHasCompress = true;
    int compression = coHasCompress ? co.compression : 5;

    // ---- level grid ----
    std::vector<ZoomLevel> levels(maxZ - minZ + 1);
    if (wmq)
    {
        for (int z = maxZ; z >= minZ; --z)
        {
            ZoomLevel &L = levels[z - minZ];
            L.z = z;
            L.tx = L.ty = 1 << z;
            double span = 256.0 * wmRes(z);
            // an epsilon absorbs the last-bit crumbs of the range math
            auto idx = [&](double v)
            {
                int64_t i = (int64_t)floor(v / span + 1e-3);
                if (i < 0)
                    i = 0;
                if (i > L.tx - 1)
                    i = L.tx - 1;
                return (int)i;
            };
            L.minX = idx(sug[0] + kWmOrig);
            L.maxX = idx(sug[2] + kWmOrig);
            L.minY = idx(kWmOrig - sug[3]);
            L.maxY = idx(kWmOrig - sug[1]);
            if (z == maxZ)
            {
                // x/y filters clamp the written base range; the mosaic
                // and stacta limits extend one tile past the written
                // range, capped by the data-derived range
                int dMaxX = L.maxX, dMaxY = L.maxY;
                const ArgValue *v;
                if ((v = r.get("min-x")))
                    L.minX = std::max(L.minX, atoi(v->str().c_str()));
                if ((v = r.get("max-x")))
                    L.maxX = std::min(L.maxX, atoi(v->str().c_str()));
                if ((v = r.get("min-y")))
                    L.minY = std::max(L.minY, atoi(v->str().c_str()));
                if ((v = r.get("max-y")))
                    L.maxY = std::min(L.maxY, atoi(v->str().c_str()));
                L.mMinX = L.minX;
                L.mMinY = L.minY;
                L.mMaxX = std::min(L.maxX + 1, dMaxX);
                L.mMaxY = std::min(L.maxY + 1, dMaxY);
            }
            else
            {
                // overview ranges clip to the halved finer mosaic
                const ZoomLevel &P = levels[z + 1 - minZ];
                L.minX = std::max(L.minX, P.mMinX / 2);
                L.maxX = std::min(L.maxX, P.mMaxX / 2);
                L.minY = std::max(L.minY, P.mMinY / 2);
                L.maxY = std::min(L.maxY, P.mMaxY / 2);
                L.mMinX = L.minX;
                L.mMaxX = L.maxX;
                L.mMinY = L.minY;
                L.mMaxY = L.maxY;
            }
            if (getenv("TILE_DBG_RANGE"))
                fprintf(stderr, "z%d min %d..%d x %d..%d y mM %d..%d x %d..%d y\n",
                        z, L.minX, L.maxX, L.minY, L.maxY, L.mMinX, L.mMaxX,
                        L.mMinY, L.mMaxY);
            L.cbx = L.mMinX;
            L.cby = L.mMinY;
            L.pw = (L.mMaxX + 1 - L.mMinX) * ts;
            L.ph = (L.mMaxY + 1 - L.mMinY) * ts;
            L.iw = L.pw;
            L.ih = L.ph;
        }
    }
    else
    for (int z = maxZ; z >= minZ; --z)
    {
        ZoomLevel &L = levels[z - minZ];
        L.z = z;
        if (z == maxZ)
        {
            L.tx = (W + ts - 1) / ts;
            L.ty = (H + ts - 1) / ts;
            L.iw = W;
            L.ih = H;
        }
        else
        {
            const ZoomLevel &P = levels[z + 1 - minZ];
            L.tx = (P.tx + 1) / 2;
            L.ty = (P.ty + 1) / 2;
            L.iw = P.iw / 2 > 0 ? P.iw / 2 : 1;
            L.ih = P.ih / 2 > 0 ? P.ih / 2 : 1;
        }
        L.pw = L.tx * ts;
        L.ph = L.ty * ts;
        L.minX = 0;
        L.maxX = L.tx - 1;
        L.minY = 0;
        L.maxY = L.ty - 1;
    }
    // x/y filters restrict the written base grid; the mosaic (and the
    // stacta limits) extend one extra tile past the max side at max zoom
    // (upstream quirk), and overview ranges halve the mosaic range
    if (!wmq)
    {
        const ArgValue *v;
        ZoomLevel &B = levels[maxZ - minZ];
        if ((v = r.get("min-x")))
            B.minX = std::max(B.minX, atoi(v->str().c_str()));
        if ((v = r.get("max-x")))
            B.maxX = std::min(B.maxX, atoi(v->str().c_str()));
        if ((v = r.get("min-y")))
            B.minY = std::max(B.minY, atoi(v->str().c_str()));
        if ((v = r.get("max-y")))
            B.maxY = std::min(B.maxY, atoi(v->str().c_str()));
        B.mMinX = B.minX;
        B.mMaxX = std::min(B.maxX + 1, B.tx - 1);
        B.mMinY = B.minY;
        B.mMaxY = std::min(B.maxY + 1, B.ty - 1);
        for (int z = maxZ - 1; z >= minZ; --z)
        {
            ZoomLevel &L = levels[z - minZ];
            const ZoomLevel &P = levels[z + 1 - minZ];
            L.minX = L.mMinX = P.mMinX / 2;
            L.maxX = L.mMaxX = P.mMaxX / 2;
            L.minY = L.mMinY = P.mMinY / 2;
            L.maxY = L.mMaxY = P.mMaxY / 2;
        }
    }

    // ---- base canvas ----
    if (wmq)
    {
        // the base canvas is warped to EPSG:3857 one suggested-zoom
        // tile at a time (the approximate transformer state is per
        // warp chunk, so any other granularity samples slightly
        // different source coords); when the base zoom is not the
        // suggested one each warp covers 2^k x 2^k base tiles (or a
        // fraction of one)
        ZoomLevel &B = levels[maxZ - minZ];
        warpResEff = wmRes(maxZ) * (256.0 / (double)ts);
        B.canvas.assign(nAll,
                        std::vector<double>((size_t)B.pw * B.ph,
                                            hasDstNodata ? dstNodata
                                                         : 0.0));
        std::vector<double> vals;
        int64_t sc0 = B.minX, sc1 = B.maxX;
        int64_t sr0 = B.minY, sr1 = B.maxY;
        int64_t pxSug = ts;
        for (int64_t srow = sr0; srow <= sr1; ++srow)
            for (int64_t scol = sc0; scol <= sc1; ++scol)
            {
                WarpParams p;
                p.dstCrs = "EPSG:3857";
                p.resampling = resampling;
                p.resamplingSet = true;
                p.hasRes = true;
                p.resX = p.resY = warpResEff;
                p.hasBbox = true;
                p.bbox[0] = wmPosX(maxZ, scol);
                p.bbox[1] = wmPosY(maxZ, srow + 1);
                p.bbox[2] = wmPosX(maxZ, scol + 1);
                p.bbox[3] = wmPosY(maxZ, srow);
                // sample as a windowed read on a mosaic-anchored grid
                p.pixGrid = true;
                p.pgOX = wmPosX(maxZ, B.minX);
                p.pgOY = wmPosY(maxZ, B.minY);
                p.pgIX = (scol - B.minX) * ts;
                p.pgIY = (srow - B.minY) * ts;
                p.addAlpha = addAlpha;
                if (hasDstNodata)
                {
                    p.hasDstNodata = true;
                    p.dstNodata = {dstNodataTxt};
                }
                std::string oerr;
                cplPushQuietHandler();
                auto d2 = openRaster(input, oerr);
                cplPopHandler();
                if (!d2 || warpWrap(p, d2, false) != 0)
                    return 1;
                // canvas placement in base-canvas pixels, clipped to
                // the written tile range (mosaic-extension regions
                // beyond the written tiles stay zero)
                int64_t ox = scol * pxSug - (int64_t)B.cbx * ts;
                int64_t oy = srow * pxSug - (int64_t)B.cby * ts;
                int64_t wx0 = (int64_t)(B.minX - B.cbx) * ts;
                int64_t wx1 = (int64_t)(B.maxX + 1 - B.cbx) * ts;
                int64_t wy0 = (int64_t)(B.minY - B.cby) * ts;
                int64_t wy1 = (int64_t)(B.maxY + 1 - B.cby) * ts;
                int nRead = std::min(nAll, (int)d2->bands.size());
                for (int b = 0; b < nRead; ++b)
                {
                    if (!d2->readBand(b + 1, vals))
                        continue;
                    for (int64_t y = 0; y < d2->height; ++y)
                    {
                        int64_t cy = oy + y;
                        if (cy < wy0 || cy >= wy1 || cy >= B.ph ||
                            cy < 0)
                            continue;
                        int64_t x0 = std::max<int64_t>(wx0 - ox,
                                                       (int64_t)0);
                        int64_t x1 = std::min<int64_t>(
                            d2->width,
                            std::min<int64_t>(wx1, (int64_t)B.pw) -
                                ox);
                        if (x0 >= x1)
                            continue;
                        memcpy(&B.canvas[b]
                                   [(size_t)cy * B.pw + ox + x0],
                               &vals[(size_t)y * d2->width + x0],
                               sizeof(double) * (size_t)(x1 - x0));
                    }
                }
            }
    }
    else
    {
        ZoomLevel &B = levels[maxZ - minZ];
        B.canvas.assign(nAll,
                        std::vector<double>((size_t)B.pw * B.ph, 0.0));
        std::vector<double> vals;
        for (int b = 0; b < nb; ++b)
        {
            if (!ds->readBand(b + 1, vals))
                vals.assign((size_t)W * H, 0.0);
            for (int y = 0; y < H; ++y)
                memcpy(&B.canvas[b][(size_t)y * B.pw], &vals[(size_t)y * W],
                       sizeof(double) * W);
        }
        if (addAlpha)
        {
            for (int y = 0; y < H; ++y)
            {
                double *row = &B.canvas[nAll - 1][(size_t)y * B.pw];
                for (int x = 0; x < W; ++x)
                    row[x] = 255.0;
            }
        }
    }
    // ---- overview canvases: each level halves the next finer level's
    // padded canvas (already quantized), so rounding compounds per level.
    // Data bands weight against the finer level's unquantized mask plane
    // (kernels with negative lobes zero any output pixel whose center
    // pixel is transparent); the alpha plane itself resamples unmasked so
    // its ramp bleeds, except rms which masks alpha by itself
    for (int z = maxZ - 1; z >= minZ; --z)
    {
        ZoomLevel &L = levels[z - minZ];
        const ZoomLevel &P = levels[z + 1 - minZ];
        // regions the resample never touches read back as dst-nodata
        L.canvas.assign(nAll,
                        std::vector<double>((size_t)L.pw * L.ph,
                                            hasDstNodata ? dstNodata
                                                         : 0.0));
        std::vector<double> out;
        // the overview source is the mosaic of the tiles selected by the
        // x/y filters at the finer level; its edges truncate resampling
        // windows just like image edges do
        // the mosaic covers the extended tile range; anything past the
        // written tiles reads back as zero
        int sx0, sy0, rw, rh, wrw, wrh, dx0, dy0;
        if (wmq)
        {
            // clip the doubled parent extent to the finer canvas; both
            // canvases carry their own tile-range origin
            int fx0 = (2 * L.minX - P.cbx) * ts;
            int fy0 = (2 * L.minY - P.cby) * ts;
            int fx1 = std::min((2 * (L.maxX + 1) - P.cbx) * ts, P.pw);
            int fy1 = std::min((2 * (L.maxY + 1) - P.cby) * ts, P.ph);
            sx0 = std::max(fx0, 0);
            sy0 = std::max(fy0, 0);
            rw = fx1 - sx0;
            rh = fy1 - sy0;
            wrw = rw;
            wrh = rh;
            dx0 = (sx0 - fx0) / 2;
            dy0 = (sy0 - fy0) / 2;
        }
        else
        {
            sx0 = P.mMinX * ts;
            sy0 = P.mMinY * ts;
            rw = (P.mMaxX + 1 - P.mMinX) * ts;
            rh = (P.mMaxY + 1 - P.mMinY) * ts;
            wrw = (P.maxX + 1 - P.mMinX) * ts;
            wrh = (P.maxY + 1 - P.mMinY) * ts;
            dx0 = sx0 / 2;
            dy0 = sy0 / 2;
        }
        int dwr = (rw + 1) / 2, dhr = (rh + 1) / 2;
        int ew = std::min(dwr, L.pw - dx0), eh = std::min(dhr, L.ph - dy0);
        auto subrect = [&](const std::vector<double> &band,
                           std::vector<double> &dst)
        {
            dst.assign((size_t)rw * rh, 0.0);
            for (int y = 0; y < wrh; ++y)
                memcpy(&dst[(size_t)y * rw],
                       &band[(size_t)(sy0 + y) * P.pw + sx0],
                       sizeof(double) * std::min(wrw, rw));
        };
        std::vector<double> sb, sa;
        const std::vector<double> *pMask = nullptr;
        if (alphaIdx >= 0)
        {
            subrect(P.canvas[alphaIdx], sa);
            pMask = &sa;
        }
        int nData = nAll - (alphaIdx >= 0 ? 1 : 0);
        for (int b = 0; b < nData; ++b)
        {
            subrect(P.canvas[b], sb);
            tileResample(sb, rw, rh, pMask, out, dwr, dhr, 2.0, 2.0,
                         ovrResampling, dt,
                         exVals.empty() ? nullptr : &exVals, exPct);
            for (int y = 0; y < eh; ++y)
                memcpy(&L.canvas[b][(size_t)(dy0 + y) * L.pw + dx0],
                       &out[(size_t)y * dwr], sizeof(double) * ew);
        }
        bool aMask = strEqualNoCase(ovrResampling, "rms");
        if (alphaIdx >= 0)
        {
            tileResample(sa, rw, rh, aMask ? pMask : nullptr, out, dwr,
                         dhr, 2.0, 2.0, ovrResampling, dt);
            for (int y = 0; y < eh; ++y)
                memcpy(&L.canvas[alphaIdx][(size_t)(dy0 + y) * L.pw + dx0],
                       &out[(size_t)y * dwr], sizeof(double) * ew);
            // negative-lobe kernels carry no data on fully transparent
            // output pixels
            if (strEqualNoCase(ovrResampling, "cubic") ||
                strEqualNoCase(ovrResampling, "lanczos"))
                for (size_t i = 0; i < L.canvas[alphaIdx].size(); ++i)
                    if (L.canvas[alphaIdx][i] == 0.0)
                        for (int b = 0; b < nData; ++b)
                            L.canvas[b][i] = 0.0;
        }
    }

    bool tms = strEqualNoCase(r.str("convention", "xyz"), "tms");
    bool resume = r.flag("resume");
    bool copySrcMd = r.flag("copy-src-metadata");
    // combined metadata dict: source items (when requested) then user
    // items; tiles skip AREA_OR_POINT, stacta keeps it
    std::vector<std::pair<std::string, std::string>> allMd, tileMd;
    if (copySrcMd)
    {
        auto it = ds->metadata.find("");
        if (it != ds->metadata.end())
            for (const auto &kv : it->second)
                allMd.push_back(kv);
    }
    for (const auto &m : r.list("metadata"))
    {
        size_t eq = m.find('=');
        allMd.push_back({m.substr(0, eq),
                         eq == std::string::npos ? "" : m.substr(eq + 1)});
    }
    for (const auto &kv : allMd)
        if (kv.first != "AREA_OR_POINT")
            tileMd.push_back(kv);
    std::sort(tileMd.begin(), tileMd.end(),
              [](const std::pair<std::string, std::string> &a,
                 const std::pair<std::string, std::string> &b)
              { return a.first < b.first; });

    // WebMercatorQuad per-tile pixel scales: base tiles inherit the warp
    // resolution; an overview tile whose four children were all written
    // uses the grid resolution literal, others recompute the scale from
    // the tile span so the fp crumbs match the bounds-based path
    bool okWm = false;
    Srs srsWm = wmq ? Srs::fromEpsg(3857, okWm) : Srs();
    // the overview method picks the scale flavor: nearest is grid
    // literal everywhere, the windowed statistics methods recompute
    // from the tile span everywhere, the kernel methods use the
    // covering-children test
    bool ovrLiteralAll = strEqualNoCase(ovrResampling, "nearest");
    bool ovrSpanAll =
        strEqualNoCase(ovrResampling, "rms") ||
        strEqualNoCase(ovrResampling, "min") ||
        strEqualNoCase(ovrResampling, "max") ||
        strEqualNoCase(ovrResampling, "med") ||
        strEqualNoCase(ovrResampling, "q1") ||
        strEqualNoCase(ovrResampling, "q3") ||
        strEqualNoCase(ovrResampling, "sum");
    std::vector<std::vector<double>> wmScaleX(levels.size()),
        wmScaleY(levels.size());
    if (wmq)
        for (int z = maxZ; z >= minZ; --z)
        {
            const ZoomLevel &L = levels[z - minZ];
            int w = L.maxX - L.minX + 1, h = L.maxY - L.minY + 1;
            auto &SX = wmScaleX[z - minZ];
            auto &SY = wmScaleY[z - minZ];
            SX.assign((size_t)w * h, 0.0);
            SY.assign((size_t)w * h, 0.0);
            double span = 256.0 * wmRes(z);
            for (int y = L.minY; y <= L.maxY; ++y)
                for (int x = L.minX; x <= L.maxX; ++x)
                {
                    size_t si = (size_t)(y - L.minY) * w + (x - L.minX);
                    if (z == maxZ)
                    {
                        SX[si] = SY[si] = warpResEff;
                        continue;
                    }
                    const ZoomLevel &P = levels[z + 1 - minZ];
                    // beyond the suggested zoom the covering-children
                    // test uses the suggested-zoom range successively
                    // doubled (both ends), not the level's own range
                    int cMinX = P.mMinX, cMinY = P.mMinY;
                    int cMaxX = P.mMaxX, cMaxY = P.mMaxY;
                    if (z + 1 > baseZ)
                    {
                        int k = z + 1 - baseZ;
                        double bspan = 256.0 * wmRes(baseZ);
                        auto bidx = [&](double v)
                        {
                            int64_t i =
                                (int64_t)floor(v / bspan + 1e-3);
                            int64_t t = ((int64_t)1 << baseZ) - 1;
                            if (i < 0)
                                i = 0;
                            if (i > t)
                                i = t;
                            return i;
                        };
                        cMinX = std::max(
                            cMinX, (int)(bidx(sug[0] + kWmOrig) << k));
                        cMaxX = std::min(
                            cMaxX, (int)(bidx(sug[2] + kWmOrig) << k));
                        cMinY = std::max(
                            cMinY, (int)(bidx(kWmOrig - sug[3]) << k));
                        cMaxY = std::min(
                            cMaxY, (int)(bidx(kWmOrig - sug[1]) << k));
                    }
                    if (ovrLiteralAll ||
                        (!ovrSpanAll && 2 * x >= cMinX &&
                         2 * x + 1 <= cMaxX && 2 * y >= cMinY &&
                         2 * y + 1 <= cMaxY))
                    {
                        SX[si] = SY[si] = wmRes(z) * (256.0 / (double)ts);
                    }
                    else
                    {
                        double xt0 = wmPosX(z, x);
                        double yt0 = wmPosY(z, y);
                        SX[si] = ((xt0 + span) - xt0) / (double)ts;
                        SY[si] = (yt0 - (yt0 - span)) / (double)ts;
                    }
                }
        }

    // ---- write tiles ----
    if (!mkdirIfNeeded(output))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    cmd.name + ": Cannot create output directory " +
                        output);
        return 1;
    }
    int total = 0;
    for (const ZoomLevel &L : levels)
        total += (L.maxX - L.minX + 1) * (L.maxY - L.minY + 1);
    if (total < 1)
        total = 1;
    TermProgress tp;
    int written = 0;
    size_t sz = (size_t)dtypeSizeBytes(dt);
    for (const ZoomLevel &L : levels)
    {
        int k = maxZ - L.z;
        double resx = gt[1] * (double)(1 << k);
        double resy = gt[5] * (double)(1 << k);
        std::string zdir = output + "/" + std::to_string(L.z);
        if (L.maxX >= L.minX && L.maxY >= L.minY)
            mkdirIfNeeded(zdir);
        for (int x = L.minX; x <= L.maxX; ++x)
        {
            std::string xdir = zdir + "/" + std::to_string(x);
            mkdirIfNeeded(xdir);
            for (int y = L.minY; y <= L.maxY; ++y)
            {
                int fy = tms ? L.ty - 1 - y : y;
                std::string path =
                    xdir + "/" + std::to_string(fy) + ".tif";
                if (resume)
                {
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0)
                    {
                        ++written;
                        if (!quiet)
                            tp.update((double)written / total);
                        continue;
                    }
                }
                // fully-opaque tiles drop the alpha band, unless the
                // user explicitly asked for it
                bool tileAlpha = alphaIdx >= 0;
                if (tileAlpha && !r.flag("add-alpha"))
                {
                    bool allOpaque = true;
                    for (int ry2 = 0; allOpaque && ry2 < ts; ++ry2)
                    {
                        const double *arow =
                            &L.canvas[alphaIdx]
                                     [(size_t)((y - L.cby) * ts + ry2) *
                                          L.pw +
                                      (x - L.cbx) * ts];
                        for (int rx2 = 0; rx2 < ts; ++rx2)
                            if (arow[rx2] != 255.0)
                            {
                                allOpaque = false;
                                break;
                            }
                    }
                    if (allOpaque)
                        tileAlpha = false;
                }
                int nOut = tileAlpha ? nAll : nAll - (alphaIdx >= 0);
                GTiffCreateParams p;
                p.width = ts;
                p.height = ts;
                p.bands = nOut;
                p.type = dt;
                p.compression = compression;
                p.predictor = co.predictor;
                p.zlevel = co.zlevel;
                p.zstdLevel = co.zstdLevel;
                // uncompressed multiband tiles default to band
                // interleaving; an explicit INTERLEAVE=BAND marks even
                // single-band tiles as separate
                p.bandInterleave =
                    co.interleaveSet ? co.bandInterleave
                                     : (compression == 1 && nOut > 1);
                p.blockY = ts;
                if (hasDstNodata)
                {
                    p.hasNodata = true;
                    p.nodata = dstNodata;
                    p.nodataText = dstNodataTxt;
                }
                p.metadata = tileMd;
                p.hasGT = true;
                if (wmq)
                {
                    size_t si = (size_t)(y - L.minY) *
                                    (L.maxX - L.minX + 1) +
                                (x - L.minX);
                    p.gt[0] = wmPosX(L.z, x);
                    p.gt[1] = wmScaleX[L.z - minZ][si];
                    p.gt[2] = 0;
                    p.gt[3] = wmPosY(L.z, y);
                    p.gt[4] = 0;
                    p.gt[5] = -wmScaleY[L.z - minZ][si];
                    p.srs = &srsWm;
                }
                else
                {
                    p.gt[0] = gt[0] + (double)x * ts * resx;
                    p.gt[1] = resx;
                    p.gt[2] = 0;
                    p.gt[3] = gt[3] + (double)y * ts * resy;
                    p.gt[4] = 0;
                    p.gt[5] = resy;
                    if (hasSrs)
                        p.srs = &ds->srs;
                }
                if (tileAlpha)
                {
                    p.extrasSet = true;
                    p.extraSamples = {2};
                }
                p.photometric = nAll - (alphaIdx >= 0 ? 1 : 0) >= 3 ? 2 : 1;
                // band interps that do not match the photometric layout
                // dump COLORINTERP items for every sample
                {
                    std::vector<std::string> interps, expect(
                                                          (size_t)nOut,
                                                          "Undefined");
                    int nData = tileAlpha ? nOut - 1 : nOut;
                    for (int b = 0; b < nData; ++b)
                        interps.push_back(b < nb ? ds->bands[b].colorInterp
                                                 : "Undefined");
                    if (tileAlpha)
                        interps.push_back("Alpha");
                    if (p.photometric == 2 && nOut >= 3)
                    {
                        expect[0] = "Red";
                        expect[1] = "Green";
                        expect[2] = "Blue";
                    }
                    else
                        expect[0] = "Gray";
                    if (tileAlpha)
                        expect[nOut - 1] = "Alpha";
                    bool mismatch = false;
                    for (int b = 0; b < nOut; ++b)
                        if (interps[b] != expect[b])
                            mismatch = true;
                    if (mismatch)
                    {
                        p.useGmdItems = true;
                        for (int b = 0; b < nOut; ++b)
                        {
                            GmdItem it;
                            it.name = "COLORINTERP";
                            it.value = interps[b];
                            it.sample = b;
                            it.role = "colorinterp";
                            p.gmdItems.push_back(it);
                        }
                    }
                }
                std::vector<std::vector<uint8_t>> pixels(nOut);
                for (int b = 0; b < nOut; ++b)
                {
                    std::vector<uint8_t> buf((size_t)ts * ts * sz);
                    for (int ry2 = 0; ry2 < ts; ++ry2)
                    {
                        const double *srow =
                            &L.canvas[b]
                                     [(size_t)((y - L.cby) * ts + ry2) *
                                          L.pw +
                                      (x - L.cbx) * ts];
                        for (int rx2 = 0; rx2 < ts; ++rx2)
                            rasterEncodeReal(
                                dt, buf.data() + ((size_t)ry2 * ts + rx2) * sz,
                                rasterFinishReal(srow[rx2], dt), 0);
                    }
                    pixels[b].swap(buf);
                }
                p.pixels = &pixels;
                std::string werr;
                if (!gtiffWrite(path, p, werr))
                {
                    if (werr != "reported")
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    cmd.name + ": " + werr);
                    return 1;
                }
                ++written;
                if (!quiet)
                    tp.update((double)written / total);
            }
        }
    }
    if (!quiet)
        tp.update(1.0);

    // ---- KML tree ----
    if (kml)
    {
        bool ok84 = false;
        Srs wgs84 = Srs::fromEpsg(4326, ok84);
        CrsOpTl kop;
        bool opOk = ok84 && !wmq && kop.init(ds->srs, wgs84);
        auto f14 = [](double v)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.14f", v);
            return std::string(buf);
        };
        struct KQuad
        {
            double lon[4], lat[4];
            double w, e, s, n;
        };
        auto quadOf = [&](int z, int x, int y)
        {
            KQuad q;
            double x0, x1, y0, y1;
            if (wmq)
            {
                x0 = wmPosX(z, x);
                x1 = wmPosX(z, x + 1);
                y0 = wmPosY(z, y);
                y1 = wmPosY(z, y + 1);
            }
            else
            {
                int k = maxZ - z;
                double rx = gt[1] * (double)(1 << k);
                double ry = gt[5] * (double)(1 << k);
                x0 = gt[0] + (double)x * ts * rx;
                x1 = x0 + ts * rx;
                y0 = gt[3] + (double)y * ts * ry;
                y1 = y0 + ts * ry;
            }
            const double cx[4] = {x0, x1, x1, x0};
            const double cy[4] = {y1, y1, y0, y0};
            q.w = q.s = HUGE_VAL;
            q.e = q.n = -HUGE_VAL;
            for (int i = 0; i < 4; ++i)
            {
                double X = cx[i], Y = cy[i];
                if (wmq)
                    wmInvKml(X, Y);
                else if (opOk)
                    kop.transform(X, Y);
                q.lon[i] = X;
                q.lat[i] = Y;
                q.w = std::min(q.w, X);
                q.e = std::max(q.e, X);
                q.s = std::min(q.s, Y);
                q.n = std::max(q.n, Y);
            }
            return q;
        };
        auto tileName = [&](const ZoomLevel &L, int x, int y)
        {
            int fy = tms ? L.ty - 1 - y : y;
            return std::to_string(L.z) + "/" + std::to_string(x) + "/" +
                   std::to_string(fy);
        };
        auto nlBlock = [&](const std::string &name, const KQuad &q,
                           const std::string &href)
        {
            std::string t;
            t += "    <NetworkLink>\n";
            t += "      <name>" + name + ".tif</name>\n";
            t += "      <Region>\n";
            t += "        <LatLonAltBox>\n";
            t += "          <north>" + f14(q.n) + "</north>\n";
            t += "          <south>" + f14(q.s) + "</south>\n";
            t += "          <east>" + f14(q.e) + "</east>\n";
            t += "          <west>" + f14(q.w) + "</west>\n";
            t += "        </LatLonAltBox>\n";
            t += "        <Lod>\n";
            t += "          <minLodPixels>128</minLodPixels>\n";
            t += "          <maxLodPixels>-1</maxLodPixels>\n";
            t += "        </Lod>\n";
            t += "      </Region>\n";
            t += "      <Link>\n";
            t += "        <href>" + href + "</href>\n";
            t += "        <viewRefreshMode>onRegion</viewRefreshMode>\n";
            t += "        <viewFormat/>\n";
            t += "      </Link>\n";
            t += "    </NetworkLink>\n";
            return t;
        };
        const std::string styleBlock =
            "    <Style>\n"
            "      <ListStyle id=\"hideChildren\">\n"
            "        <listItemType>checkHideChildren</listItemType>\n"
            "      </ListStyle>\n"
            "    </Style>\n";
        auto writeText = [](const std::string &path, const std::string &t)
        {
            FILE *f = fopen(path.c_str(), "wb");
            if (f)
            {
                fwrite(t.data(), 1, t.size(), f);
                fclose(f);
            }
        };
        for (const ZoomLevel &L : levels)
        {
            for (int x = L.minX; x <= L.maxX; ++x)
            {
                for (int y = L.minY; y <= L.maxY; ++y)
                {
                    int fy = tms ? L.ty - 1 - y : y;
                    std::string nm = tileName(L, x, y);
                    KQuad q = quadOf(L.z, x, y);
                    // an axis-aligned quad collapses into the LatLonBox
                    bool hasQuad =
                        !(q.lon[0] == q.lon[3] && q.lon[1] == q.lon[2] &&
                          q.lat[0] == q.lat[1] && q.lat[2] == q.lat[3]);
                    std::string t;
                    t += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
                    t += hasQuad
                             ? "<kml xmlns=\"http://www.opengis.net/kml/"
                               "2.2\" xmlns:gx=\"http://www.google.com/"
                               "kml/ext/2.2\">\n"
                             : "<kml xmlns=\"http://www.opengis.net/kml/"
                               "2.2\">\n";
                    t += "  <Document>\n";
                    t += "    <name>" + nm + ".kml</name>\n";
                    t += "    <description></description>\n";
                    t += styleBlock;
                    t += "    <Region>\n";
                    t += "      <LatLonAltBox>\n";
                    t += "        <north>" + f14(q.n) + "</north>\n";
                    t += "        <south>" + f14(q.s) + "</south>\n";
                    t += "        <east>" + f14(q.e) + "</east>\n";
                    t += "        <west>" + f14(q.w) + "</west>\n";
                    t += "      </LatLonAltBox>\n";
                    t += "      <Lod>\n";
                    t += "        <minLodPixels>128</minLodPixels>\n";
                    t += std::string("        <maxLodPixels>") +
                         (L.z == maxZ ? "-1" : "2048") +
                         "</maxLodPixels>\n";
                    t += "      </Lod>\n";
                    t += "    </Region>\n";
                    t += "    <GroundOverlay>\n";
                    t += "      <drawOrder>" +
                         std::to_string(2 * L.z + (x == 0 ? 1 : 0)) +
                         "</drawOrder>\n";
                    t += "      <Icon>\n";
                    t += "        <href>" + std::to_string(fy) +
                         ".tif</href>\n";
                    t += "      </Icon>\n";
                    t += "      <LatLonBox>\n";
                    t += "        <north>" + f14(q.n) + "</north>\n";
                    t += "        <south>" + f14(q.s) + "</south>\n";
                    t += "        <east>" + f14(q.e) + "</east>\n";
                    t += "        <west>" + f14(q.w) + "</west>\n";
                    t += "      </LatLonBox>\n";
                    if (hasQuad)
                    {
                        t += "      <gx:LatLonQuad><coordinates>";
                        for (int i = 0; i < 4; ++i)
                        {
                            if (i)
                                t += " ";
                            t += f14(q.lon[i]) + "," + f14(q.lat[i]);
                        }
                        t += "</coordinates></gx:LatLonQuad>\n";
                    }
                    t += "    </GroundOverlay>\n";
                    if (L.z < maxZ)
                    {
                        const ZoomLevel &C = levels[L.z + 1 - minZ];
                        for (int cy2 = 2 * y; cy2 <= 2 * y + 1; ++cy2)
                            for (int cx2 = 2 * x; cx2 <= 2 * x + 1; ++cx2)
                            {
                                if (cx2 < C.minX || cx2 > C.maxX ||
                                    cy2 < C.minY || cy2 > C.maxY)
                                    continue;
                                std::string cn = tileName(C, cx2, cy2);
                                t += nlBlock(cn, quadOf(C.z, cx2, cy2),
                                             "../../" + cn + ".kml");
                            }
                    }
                    t += "</Document>\n";
                    t += "</kml>";
                    writeText(output + "/" + std::to_string(L.z) + "/" +
                                  std::to_string(x) + "/" +
                                  std::to_string(fy) + ".kml",
                              t);
                }
            }
        }
        const ZoomLevel &M = levels[0];
        std::string t;
        t += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        t += "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n";
        t += "  <Document>\n";
        t += "    <name>" + baseNameTl(input) + "</name>\n";
        t += "    <description></description>\n";
        t += styleBlock;
        for (int y = M.minY; y <= M.maxY; ++y)
            for (int x = M.minX; x <= M.maxX; ++x)
            {
                std::string nm = tileName(M, x, y);
                t += nlBlock(nm, quadOf(M.z, x, y), nm + ".kml");
            }
        t += "</Document>\n";
        t += "</kml>";
        writeText(output + "/doc.kml", t);
    }

    // ---- web viewers ----
    std::vector<std::string> wv = r.list("webviewer");
    if (wv.empty())
        wv.push_back("all");
    bool wvNone = false, wvAll = false, wvOl = false, wvStac = false;
    bool wvLeaf = false, wvMapml = false;
    for (const auto &s : wv)
    {
        if (s == "none")
            wvNone = true;
        else if (s == "all")
            wvAll = true;
        else if (s == "openlayers")
            wvOl = true;
        else if (s == "stac")
            wvStac = true;
        else if (s == "leaflet")
            wvLeaf = true;
        else if (s == "mapml")
            wvMapml = true;
    }
    if (wvNone)
        wvOl = wvStac = wvLeaf = wvMapml = false;
    else if (wvAll)
        wvOl = wvStac = wvLeaf = wvMapml = true;
    // the mapml generator bails out on the tms convention before it
    // ever looks for its template
    if (tms)
        wvMapml = false;

    // leaflet/mapml templates ship as GDAL data files; without them the
    // generators only leave a warning (non-raster schemes only)
    if (wmq && (wvLeaf || wvMapml))
    {
        auto dataFileMissing = [](const std::string &bn)
        {
            std::string p = configIsSet("GDAL_DATA")
                                ? configGet("GDAL_DATA") + "/" + bn
                                : "./" + bn;
            FILE *f = fopen(p.c_str(), "rb");
            if (f)
                fclose(f);
            // a GDAL_DATA that lacks the file stays silent, like the
            // tms diagnostic
            return !f && !configIsSet("GDAL_DATA");
        };
        if (wvLeaf && dataFileMissing("leaflet_template.html"))
            cplErrorStr(CE_Warning, CPLE_FileIO,
                        "Cannot find leaflet_template.html (GDAL_DATA is "
                        "not defined)");
        if (wvMapml && dataFileMissing("template_tiles.mapml"))
            cplErrorStr(CE_Warning, CPLE_FileIO,
                        "Cannot find template_tiles.mapml (GDAL_DATA is "
                        "not defined)");
    }

    const ZoomLevel &B = levels[maxZ - minZ];
    double xmin = gt[0];
    double ymax = gt[3];
    double xmax = gt[0] + (double)W * gt[1];
    double ymin = gt[3] + (double)H * gt[5];
    if (wmq)
    {
        xmin = sug[0];
        ymin = sug[1];
        xmax = sug[2];
        ymax = sug[3];
    }
    // the layer extent is the image extent clipped to the tiles selected
    // by the x/y filters at max zoom
    double exmin, exmax, eymin, eymax;
    if (wmq)
    {
        exmin = std::max(xmin, wmPosX(maxZ, B.minX));
        exmax = std::min(xmax, wmPosX(maxZ, B.maxX + 1));
        eymax = std::min(ymax, wmPosY(maxZ, B.minY));
        eymin = std::max(ymin, wmPosY(maxZ, B.maxY + 1));
    }
    else
    {
        exmin = std::max(xmin, gt[0] + (double)B.minX * ts * gt[1]);
        exmax = std::min(xmax, gt[0] + (double)(B.maxX + 1) * ts * gt[1]);
        eymax = std::min(ymax, gt[3] + (double)B.minY * ts * gt[5]);
        eymin = std::max(ymin, gt[3] + (double)(B.maxY + 1) * ts * gt[5]);
    }
    // the viewer tile grid spans the zoom-0 grid scaled back up, i.e. a
    // ceil-power-of-two tile count per axis
    std::vector<int> gtx(maxZ + 1), gty(maxZ + 1);
    gtx[maxZ] = B.tx;
    gty[maxZ] = B.ty;
    for (int z = maxZ - 1; z >= 0; --z)
    {
        gtx[z] = (gtx[z + 1] + 1) / 2;
        gty[z] = (gty[z + 1] + 1) / 2;
    }
    double cxmax =
        gt[0] + (double)((int64_t)gtx[0] << maxZ) * ts * gt[1];
    double cymin =
        gt[3] + (double)((int64_t)gty[0] << maxZ) * ts * gt[5];
    std::string title = r.str("title");
    if (title.empty())
        title = baseNameTl(input);
    std::string copyrightTxt = r.str("copyright");
    std::string url = r.str("url");

    if (wvOl && wmq)
    {
        std::string t;
        t += "<!DOCTYPE html>\n";
        t += "<html>\n";
        t += "<head>\n";
        t += "    <title>" + title + "</title>\n";
        t += "    <meta http-equiv=\"content-type\" content=\"text/html; "
             "charset=utf-8\"/>\n";
        t += "    <meta http-equiv='imagetoolbar' content='no'/>\n";
        t += "    <style type=\"text/css\"> v\\:* {behavior:url(#default#"
             "VML);}\n";
        t += "        html, body { overflow: hidden; padding: 0; height: "
             "100%; width: 100%; font-family: 'Lucida Grande',Geneva,"
             "Arial,Verdana,sans-serif; }\n";
        t += "        body { margin: 10px; background: #fff; }\n";
        t += "        h1 { margin: 0; padding: 6px; border:0; font-size: "
             "20pt; }\n";
        t += "        #header { height: 43px; padding: 0; background-color:"
             " #eee; border: 1px solid #888; }\n";
        t += "        #subheader { height: 12px; text-align: right; "
             "font-size: 10px; color: #555;}\n";
        t += "        #map { height: 90%; border: 1px solid #888; }\n";
        t += "    </style>\n";
        t += "    <link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/"
             "gh/openlayers/openlayers.github.io@main/dist/en/v7.0.0/"
             "legacy/ol.css\" type=\"text/css\">\n";
        t += "    <script src=\"https://cdn.jsdelivr.net/gh/openlayers/"
             "openlayers.github.io@main/dist/en/v7.0.0/legacy/ol.js\">"
             "</script>\n";
        t += "    <script src=\"https://unpkg.com/ol-layerswitcher@4.1.1\">"
             "</script>\n";
        t += "    <link rel=\"stylesheet\" href=\"https://unpkg.com/"
             "ol-layerswitcher@4.1.1/src/ol-layerswitcher.css\" />\n";
        t += "</head>\n";
        t += "<body>\n";
        t += "    <div id=\"header\"><h1>" + title + "</h1></div>\n";
        t += "    <div id=\"subheader\">Generated by <a href=\"https://"
             "gdal.org/programs/gdal_raster_tile.html\">gdal raster tile"
             "</a>&nbsp;&nbsp;&nbsp;&nbsp;</div>\n";
        t += "    <div id=\"map\" class=\"map\"></div>\n";
        t += "    <div id=\"mouse-position\"></div>\n";
        t += "    <script type=\"text/javascript\">\n";
        t += "        var mousePositionControl = new ol.control."
             "MousePosition({\n";
        t += "            className: 'custom-mouse-position',\n";
        t += "            target: document.getElementById('mouse-position'"
             "),\n";
        t += "            undefinedHTML: '&nbsp;'\n";
        t += "        });\n";
        t += "        var map = new ol.Map({\n";
        t += "            controls: ol.control.defaults.defaults().extend("
             "[mousePositionControl]),\n";
        t += "            target: 'map',\n";
        t += "            layers: [\n";
        t += "                new ol.layer.Group({\n";
        t += "                        title: 'Base maps',\n";
        t += "                        layers: [\n";
        t += "                            new ol.layer.Tile({\n";
        t += "                                title: 'OpenStreetMap',\n";
        t += "                                type: 'base',\n";
        t += "                                visible: true,\n";
        t += "                                source: new ol.source.OSM()\n";
        t += "                            }),\n";
        t += "                        ]\n";
        t += "                }),new ol.layer.Group({\n";
        t += "                    title: 'Overlay',\n";
        t += "                    layers: [\n";
        t += "                        new ol.layer.Tile({\n";
        t += "                            title: 'Overlay',\n";
        t += "                            // opacity: 0.7,\n";
        t += "                            extent: [" + fmtG(exmin) + ", " +
             fmtG(eymin) + "," + fmtG(exmax) + ", " + fmtG(eymax) + "],\n";
        t += "                            source: new ol.source.XYZ({\n";
        t += "                                attributions: '" +
             copyrightTxt + "',\n";
        t += "                                minZoom: " +
             std::to_string(minZ) + ",\n";
        t += "                                maxZoom: " +
             std::to_string(maxZ) + ",\n";
        t += std::string("                                url: './{z}/{x}/") +
             (tms ? "{-y}" : "{y}") + ".tif',\n";
        t += "                                tileSize: [" +
             std::to_string(ts) + ", " + std::to_string(ts) + "]\n";
        t += "                            })\n";
        t += "                        }),\n";
        t += "                    ]\n";
        t += "                }),\n";
        t += "            ],\n";
        t += "            view: new ol.View({\n";
        t += "                center: [" + fmtG((exmin + exmax) / 2) +
             ", " + fmtG((eymin + eymax) / 2) + "],\n";
        t += "                zoom: " + std::to_string(minZ) + ",\n";
        t += "            })\n";
        t += "        });\n";
        t += "        map.addControl(new ol.control.LayerSwitcher());\n";
        t += "    </script>\n";
        t += "</body>\n";
        t += "</html>";
        FILE *f = fopen((output + "/openlayers.html").c_str(), "wb");
        if (f)
        {
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
    }
    else if (wvOl)
    {
        std::string t;
        t += "<!DOCTYPE html>\n";
        t += "<html>\n";
        t += "<head>\n";
        t += "    <title>" + title + "</title>\n";
        t += "    <meta http-equiv=\"content-type\" content=\"text/html; "
             "charset=utf-8\"/>\n";
        t += "    <meta http-equiv='imagetoolbar' content='no'/>\n";
        t += "    <style type=\"text/css\"> v\\:* {behavior:url(#default#"
             "VML);}\n";
        t += "        html, body { overflow: hidden; padding: 0; height: "
             "100%; width: 100%; font-family: 'Lucida Grande',Geneva,"
             "Arial,Verdana,sans-serif; }\n";
        t += "        body { margin: 10px; background: #fff; }\n";
        t += "        h1 { margin: 0; padding: 6px; border:0; font-size: "
             "20pt; }\n";
        t += "        #header { height: 43px; padding: 0; background-color:"
             " #eee; border: 1px solid #888; }\n";
        t += "        #subheader { height: 12px; text-align: right; "
             "font-size: 10px; color: #555;}\n";
        t += "        #map { height: 90%; border: 1px solid #888; }\n";
        t += "    </style>\n";
        t += "    <link rel=\"stylesheet\" href=\"https://cdn.jsdelivr.net/"
             "gh/openlayers/openlayers.github.io@main/dist/en/v7.0.0/"
             "legacy/ol.css\" type=\"text/css\">\n";
        t += "    <script src=\"https://cdn.jsdelivr.net/gh/openlayers/"
             "openlayers.github.io@main/dist/en/v7.0.0/legacy/ol.js\">"
             "</script>\n";
        t += "    <script src=\"https://unpkg.com/ol-layerswitcher@4.1.1\">"
             "</script>\n";
        t += "    <link rel=\"stylesheet\" href=\"https://unpkg.com/"
             "ol-layerswitcher@4.1.1/src/ol-layerswitcher.css\" />\n";
        t += "</head>\n";
        t += "<body>\n";
        t += "    <div id=\"header\"><h1>" + title + "</h1></div>\n";
        t += "    <div id=\"subheader\">Generated by <a href=\"https://"
             "gdal.org/programs/gdal_raster_tile.html\">gdal raster tile"
             "</a>&nbsp;&nbsp;&nbsp;&nbsp;</div>\n";
        t += "    <div id=\"map\" class=\"map\"></div>\n";
        t += "    <div id=\"mouse-position\"></div>\n";
        t += "    <script type=\"text/javascript\">\n";
        t += "        var mousePositionControl = new ol.control."
             "MousePosition({\n";
        t += "            className: 'custom-mouse-position',\n";
        t += "            target: document.getElementById('mouse-position'"
             "),\n";
        t += "            undefinedHTML: '&nbsp;'\n";
        t += "        });\n";
        t += "        var map = new ol.Map({\n";
        t += "            controls: ol.control.defaults.defaults().extend("
             "[mousePositionControl]),\n";
        t += "            target: 'map',\n";
        t += "            layers: [\n";
        t += "                new ol.layer.Group({\n";
        t += "                    title: 'Overlay',\n";
        t += "                    layers: [\n";
        t += "                        new ol.layer.Tile({\n";
        t += "                            title: 'Overlay',\n";
        t += "                            // opacity: 0.7,\n";
        t += "                            extent: [" + fmtG(exmin) + ", " +
             fmtG(eymin) + "," + fmtG(exmax) + ", " + fmtG(eymax) + "],\n";
        t += "                            source: new ol.source.TileImage"
             "({\n";
        t += "                                attributions: '" +
             copyrightTxt + "',\n";
        t += "                                minZoom: " +
             std::to_string(minZ) + ",\n";
        t += "                                maxZoom: " +
             std::to_string(maxZ) + ",\n";
        t += "                                tileGrid: new ol.tilegrid."
             "TileGrid({\n";
        t += "                                    extent: [" + fmtG(xmin) +
             "," + fmtG(cymin) + "," + fmtG(cxmax) + "," + fmtG(ymax) +
             "],\n";
        if (tms)
            t += "                                    origin: [" +
                 fmtG(xmin) + "," + fmtG(cymin) + "],\n";
        else
            t += "                                    origin: [" +
                 fmtG(xmin) + "," + fmtG(ymax) + "],\n";
        // the grid arrays always span zoom 0 .. maxZ, even below the
        // written min zoom
        t += "                                    resolutions: [";
        for (int z = 0; z <= maxZ; ++z)
            t += (z > 0 ? "," : "") +
                 fmtG(gt[1] * (double)(1 << (maxZ - z)));
        t += "],\n";
        t += "                                    sizes: [";
        for (int z = 0; z <= maxZ; ++z)
        {
            t += (z > 0 ? "," : "");
            t += "[" + std::to_string(gtx[z]) + "," + std::to_string(gty[z]) +
                 "]";
        }
        t += "],\n";
        t += "                                    tileSize: [" +
             std::to_string(ts) + ", " + std::to_string(ts) + "]\n";
        t += "                                }),\n";
        t += "                                tileUrlFunction: function("
             "tileCoord) {\n";
        t += "                                    return ('./{z}/{x}/{y}"
             ".tif'\n";
        t += "                                        .replace('{z}', "
             "String(tileCoord[0]))\n";
        t += "                                        .replace('{x}', "
             "String(tileCoord[1]))\n";
        if (tms)
            t += "                                        .replace('{y}', "
                 "String(- 1 - tileCoord[2])));\n";
        else
            t += "                                        .replace('{y}', "
                 "String(tileCoord[2])));\n";
        t += "                                },\n";
        t += "                            })\n";
        t += "                        }),\n";
        t += "                    ]\n";
        t += "                }),\n";
        t += "            ],\n";
        t += "            view: new ol.View({\n";
        t += "                center: [" + fmtG((exmin + exmax) / 2) +
             ", " + fmtG((eymin + eymax) / 2) + "],\n";
        t += "                resolution: " +
             fmtG(gt[1] * (double)(1 << (maxZ - minZ))) + ",\n";
        if (hasSrs && !ds->srs.code().empty())
        {
            std::string unit = ds->srs.isGeographic() ? "deg" : "m";
            t += "                projection: new ol.proj.Projection({"
                 "code: '" +
                 ds->srs.authName() + ":" + ds->srs.code() + "', units:'" +
                 unit + "'}),\n";
        }
        t += "            })\n";
        t += "        });\n";
        t += "    </script>\n";
        t += "</body>\n";
        t += "</html>";
        FILE *f = fopen((output + "/openlayers.html").c_str(), "wb");
        if (f)
        {
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
    }

    if (wvStac && !tms)
    {
        // WGS84 bbox via densified boundary transform; placeholder when
        // the source carries no SRS
        double bw = -180.0, bs = -90.0, be = 180.0, bn = 90.0;
        if (hasSrs)
        {
            bool ok = false;
            Srs wgs84 = Srs::fromEpsg(4326, ok);
            CrsOpTl op;
            if (wmq || (ok && op.init(ds->srs, wgs84)))
            {
                bw = bs = HUGE_VAL;
                be = bn = -HUGE_VAL;
                const int N = 21;
                for (int i = 0; i <= N; ++i)
                {
                    double t2 = (double)i / N;
                    double xs[4] = {exmin + t2 * (exmax - exmin), exmax,
                                    exmin + t2 * (exmax - exmin), exmin};
                    double ys[4] = {eymax, eymax + t2 * (eymin - eymax),
                                    eymin, eymax + t2 * (eymin - eymax)};
                    for (int e = 0; e < 4; ++e)
                    {
                        double X = xs[e], Y = ys[e];
                        if (wmq)
                            wmInv(X, Y);
                        else
                            op.transform(X, Y);
                        if (X < bw)
                            bw = X;
                        if (X > be)
                            be = X;
                        if (Y < bs)
                            bs = Y;
                        if (Y > bn)
                            bn = Y;
                    }
                }
            }
        }
        std::string dtIso = "1970-01-01T00:00:00Z";
        {
            struct stat st;
            if (stat(input.c_str(), &st) == 0)
            {
                struct tm tmv;
                gmtime_r(&st.st_mtime, &tmv);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
                dtIso = buf;
            }
        }
        std::string wkt = hasSrs ? jsonEscTl(ds->srs.wkt1GdalFull()) : "";
        std::string t;
        t += "{\n";
        t += "  \"stac_version\":\"1.1.0\",\n";
        t += "  \"stac_extensions\":[\n";
        t += "    \"https://stac-extensions.github.io/tiled-assets/v1.0.0/"
             "schema.json\",\n";
        t += "    \"https://stac-extensions.github.io/projection/v2.0.0/"
             "schema.json\"\n";
        t += "  ],\n";
        std::string title = r.str("title");
        t += "  \"id\":\"" +
             jsonEscTl(title.empty() ? baseNameTl(input) : title) + "\",\n";
        t += "  \"type\":\"Feature\",\n";
        t += "  \"bbox\":[\n";
        t += "    " + fmtJ(bw) + ",\n";
        t += "    " + fmtJ(bs) + ",\n";
        t += "    " + fmtJ(be) + ",\n";
        t += "    " + fmtJ(bn) + "\n";
        t += "  ],\n";
        t += "  \"geometry\":{\n";
        t += "    \"type\":\"Polygon\",\n";
        t += "    \"coordinates\":[\n";
        t += "      [\n";
        const double ringPts[5][2] = {{bw, bs}, {bw, bn}, {be, bn},
                                      {be, bs}, {bw, bs}};
        for (int i = 0; i < 5; ++i)
        {
            t += "        [\n";
            t += "          " + fmtJ(ringPts[i][0]) + ",\n";
            t += "          " + fmtJ(ringPts[i][1]) + "\n";
            t += "        ]";
            t += i < 4 ? ",\n" : "\n";
        }
        t += "      ]\n";
        t += "    ]\n";
        t += "  },\n";
        t += "  \"properties\":{\n";
        for (const auto &kv : allMd)
            t += "    \"" + jsonEscTl(kv.first) + "\":\"" +
                 jsonEscTl(kv.second) + "\",\n";
        t += "    \"datetime\":\"" + dtIso + "\",\n";
        t += "    \"start_datetime\":\"0001-01-01T00:00:00.000Z\",\n";
        t += "    \"end_datetime\":\"9999-12-31T23:59:59.999Z\",\n";
        std::string cpy = r.str("copyright");
        if (!cpy.empty())
            t += "    \"copyright\":\"" + jsonEscTl(cpy) + "\",\n";
        std::string tmsName = wmq ? "GoogleMapsCompatible" : "raster";
        t += "    \"tiles:tile_matrix_sets\":{\n";
        t += "      \"" + tmsName + "\":{\n";
        t += "        \"type\":\"TileMatrixSetType\",\n";
        t += "        \"title\":\"" + tmsName + "\",\n";
        t += "        \"identifier\":\"" + tmsName + "\",\n";
        t += "        \"boundingBox\":{\n";
        t += "          \"type\":\"BoundingBoxType\",\n";
        if (wmq)
        {
            t += "          \"crs\":\"http://www.opengis.net/def/crs/"
                 "EPSG/0/3857\",\n";
            t += "          \"lowerCorner\":[\n";
            t += "            " + fmtJ(-kWmOrig) + ",\n";
            t += "            " + fmtJ(-kWmOrig) + "\n";
            t += "          ],\n";
            t += "          \"upperCorner\":[\n";
            t += "            " + fmtJ(kWmOrig) + ",\n";
            t += "            " + fmtJ(kWmOrig) + "\n";
            t += "          ]\n";
            t += "        },\n";
            t += "        \"supportedCRS\":\"http://www.opengis.net/def/"
                 "crs/EPSG/0/3857\",\n";
            t += "        \"wellKnownScaleSet\":\"http://www.opengis.net/"
                 "def/wkss/OGC/1.0/GoogleMapsCompatible\",\n";
        }
        else
        {
            t += "          \"crs\":\"" + wkt + "\",\n";
            t += "          \"lowerCorner\":[\n";
            t += "            " + fmtJ(xmin) + ",\n";
            t += "            " + fmtJ(ymin) + "\n";
            t += "          ],\n";
            t += "          \"upperCorner\":[\n";
            t += "            " + fmtJ(xmax) + ",\n";
            t += "            " + fmtJ(ymax) + "\n";
            t += "          ]\n";
            t += "        },\n";
            t += "        \"supportedCRS\":\"" + wkt + "\",\n";
        }
        t += "        \"tileMatrix\":[\n";
        for (int z = minZ; z <= maxZ; ++z)
        {
            const ZoomLevel &L = levels[z - minZ];
            double res = wmq ? wmRes(z) : gt[1] * (double)(1 << (maxZ - z));
            double scaleDenom = res / 0.00028;
            t += "          {\n";
            t += "            \"type\":\"TileMatrixType\",\n";
            t += "            \"identifier\":\"" + std::to_string(z) +
                 "\",\n";
            t += "            \"scaleDenominator\":" + fmtJ(scaleDenom) +
                 ",\n";
            t += "            \"topLeftCorner\":[\n";
            t += "              " + fmtJ(wmq ? -kWmOrig : xmin) + ",\n";
            t += "              " + fmtJ(wmq ? kWmOrig : ymax) + "\n";
            t += "            ],\n";
            t += "            \"tileWidth\":" + std::to_string(ts) + ",\n";
            t += "            \"tileHeight\":" + std::to_string(ts) +
                 ",\n";
            t += "            \"matrixWidth\":" + std::to_string(L.tx) +
                 ",\n";
            t += "            \"matrixHeight\":" + std::to_string(L.ty) +
                 "\n";
            t += "          }";
            t += z < maxZ ? ",\n" : "\n";
        }
        t += "        ]\n";
        t += "      }\n";
        t += "    },\n";
        t += "    \"tiles:tile_matrix_links\":{\n";
        t += "      \"" + tmsName + "\":{\n";
        t += "        \"url\":\"#" + tmsName + "\",\n";
        t += "        \"limits\":{\n";
        for (int z = minZ; z <= maxZ; ++z)
        {
            const ZoomLevel &L = levels[z - minZ];
            t += "          \"" + std::to_string(z) + "\":{\n";
            t += "            \"min_tile_col\":" +
                 std::to_string(L.mMinX) + ",\n";
            t += "            \"max_tile_col\":" +
                 std::to_string(L.mMaxX) + ",\n";
            t += "            \"min_tile_row\":" +
                 std::to_string(L.mMinY) + ",\n";
            t += "            \"max_tile_row\":" +
                 std::to_string(L.mMaxY) + "\n";
            t += "          }";
            t += z < maxZ ? ",\n" : "\n";
        }
        t += "        }\n";
        t += "      }\n";
        t += "    },\n";
        if (wmq)
            t += "    \"proj:code\":\"EPSG:3857\",\n";
        else if (hasSrs && !ds->srs.code().empty())
            t += "    \"proj:code\":\"" + ds->srs.authName() + ":" +
                 ds->srs.code() + "\",\n";
        t += "    \"proj:shape\":[\n";
        t += "      " + std::to_string((B.mMaxY + 1 - B.mMinY) * ts) +
             ",\n";
        t += "      " + std::to_string((B.mMaxX + 1 - B.mMinX) * ts) +
             "\n";
        t += "    ],\n";
        double pgt0 = gt[0], pgt1 = gt[1], pgt2 = gt[2], pgt3 = gt[3],
               pgt4 = gt[4], pgt5 = gt[5];
        if (wmq)
        {
            pgt0 = wmPosX(maxZ, B.mMinX);
            pgt1 = warpResEff;
            pgt2 = 0;
            pgt3 = kWmOrig;
            pgt4 = 0;
            pgt5 = -warpResEff;
        }
        t += "    \"proj:transform\":[\n";
        t += "      " + fmtJ(pgt1) + ",\n";
        t += "      " + fmtJ(pgt2) + ",\n";
        t += "      " + fmtJ(wmq ? pgt0
                                 : gt[0] + (double)B.mMinX * ts * gt[1]) +
             ",\n";
        t += "      " + fmtJ(pgt4) + ",\n";
        t += "      " + fmtJ(pgt5) + ",\n";
        // upstream shifts the y origin with a flipped sign
        t += "      " + fmtJ(pgt3 - (double)B.mMinY * ts * pgt5) + ",\n";
        t += "      0.0,\n";
        t += "      0.0,\n";
        t += "      0.0\n";
        t += "    ]\n";
        t += "  },\n";
        t += "  \"asset_templates\":{\n";
        t += "    \"bands\":{\n";
        std::string href = "./{TileMatrix}/{TileCol}/{TileRow}.tif";
        if (!url.empty())
            href = url + "/" + baseNameTl(output) +
                   "/{TileMatrix}/{TileCol}/{TileRow}.tif";
        t += "      \"href\":\"" + jsonEscTl(href) + "\",\n";
        t += "      \"type\":\"image/tiff; application=geotiff\",\n";
        t += "      \"bands\":[\n";
        for (int b = 0; b < nb - (srcHasAlpha ? 1 : 0); ++b)
        {
            t += "        {\n";
            t += "          \"name\":\"Band" + std::to_string(b + 1) +
                 "\",\n";
            t += "          \"data_type\":\"" +
                 std::string(stacTypeName(dt)) + "\"\n";
            t += "        }";
            t += b < nb - (srcHasAlpha ? 1 : 0) - 1 ? ",\n" : "\n";
        }
        t += "      ]\n";
        t += "    }\n";
        t += "  },\n";
        t += "  \"assets\":{\n";
        t += "  },\n";
        t += "  \"links\":[\n";
        t += "  ]\n";
        t += "}";
        FILE *f = fopen((output + "/stacta.json").c_str(), "wb");
        if (f)
        {
            fwrite(t.data(), 1, t.size(), f);
            fclose(f);
        }
    }
    return 0;
}

struct RegisterTile
{
    RegisterTile()
    {
        registerHandler("raster_tile", rasterTileHandler);
        registerPreValidator("raster_tile", rasterTilePreValidator);
        registerPostValidator("raster_tile", rasterTilePostValidator);
    }
} regTile;

}  // namespace

void registerRasterTileHandler() {}
