#pragma once
// shared machinery of the raster->vector verbs (polygonize, footprint):
// GDALRasterPolygonEnumerator-style connected-component labeling and the
// region-on-left boundary walk that reproduces the reference's ring
// vertex ordering
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// GP_NODATA_MARKER: masked pixels take this value before labeling and
// regions carrying it are skipped at emission (even when the data
// legitimately contains it)
constexpr long long kRpfMarkerInt = -51502112;

// a line-delimited file holding a single Feature opens through the plain
// GeoJSON driver; the append flows then rewrite it as a standard
// collection instead of growing the sequence
inline bool rpfGjRootIsCollection(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[97];
    size_t n = fread(buf, 1, 96, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "FeatureCollection") != nullptr;
}

struct RpfEqInt
{
    bool operator()(long long a, long long b) const { return a == b; }
};

// GDALFloatEquals: lexicographically reordered bit patterns within
// MAX_ULPS (10); infinities only equal themselves
struct RpfEqFloat
{
    bool operator()(float a, float b) const
    {
        if (a == b)
            return true;
        if (std::isinf(a) || std::isinf(b))
            return false;
        int32_t ai, bi;
        memcpy(&ai, &a, 4);
        memcpy(&bi, &b, 4);
        if (ai < 0)
            ai = (int32_t)0x80000000 - ai;
        if (bi < 0)
            bi = (int32_t)0x80000000 - bi;
        long long d = (long long)ai - (long long)bi;
        if (d < 0)
            d = -d;
        return d <= 10;
    }
};

struct RpfEqDouble
{
    bool operator()(double a, double b) const
    {
        if (a == b)
            return true;
        if (std::isinf(a) || std::isinf(b))
            return false;
        int64_t ai, bi;
        memcpy(&ai, &a, 8);
        memcpy(&bi, &b, 8);
        if (ai < 0)
            ai = (int64_t)0x8000000000000000ULL - ai;
        if (bi < 0)
            bi = (int64_t)0x8000000000000000ULL - bi;
        // values this far apart cannot overflow the difference: the
        // reordered space is monotone and |ai|,|bi| < 2^63
        long long d = ai - bi;
        if (d < 0)
            d = -d;
        return d <= 10;
    }
};

// two-line scan with the reference's branch order and merge map: raw ids
// are handed out left-to-right / top-to-bottom, merges collapse the
// source chain onto the destination root
template <class T, class EQ>
inline void rpfLabel(const std::vector<T> &v, int w, int h, bool conn8,
                     EQ eq, std::vector<int> &canon)
{
    std::vector<int> ids((size_t)w * h, 0);
    std::vector<int> map;
    auto newPoly = [&]()
    {
        map.push_back((int)map.size());
        return (int)map.size() - 1;
    };
    auto merge = [&](int src, int dst)
    {
        int dr = dst;
        while (map[dr] != dr)
            dr = map[dr];
        int cur = src;
        while (map[cur] != cur)
        {
            int nx = map[cur];
            map[cur] = dr;
            cur = nx;
        }
        map[cur] = dr;
    };
    for (int y = 0; y < h; y++)
    {
        const T *tv = &v[(size_t)y * w];
        int *ti = &ids[(size_t)y * w];
        if (y == 0)
        {
            for (int x = 0; x < w; x++)
                ti[x] = (x == 0 || !eq(tv[x], tv[x - 1])) ? newPoly()
                                                          : ti[x - 1];
            continue;
        }
        const T *lv = &v[(size_t)(y - 1) * w];
        const int *li = &ids[(size_t)(y - 1) * w];
        for (int x = 0; x < w; x++)
        {
            if (x > 0 && eq(tv[x], tv[x - 1]))
            {
                ti[x] = ti[x - 1];
                // the last-line chain collapses onto this pixel's root:
                // merged regions keep the newest id for the final
                // same-scanline emission ordering
                if (eq(lv[x], tv[x]) && map[li[x]] != map[ti[x]])
                    merge(li[x], ti[x]);
                if (conn8 && eq(lv[x - 1], tv[x]) &&
                    map[li[x - 1]] != map[ti[x]])
                    merge(li[x - 1], ti[x]);
                if (conn8 && x < w - 1 && eq(lv[x + 1], tv[x]) &&
                    map[li[x + 1]] != map[ti[x]])
                    merge(li[x + 1], ti[x]);
            }
            else if (eq(lv[x], tv[x]))
                ti[x] = li[x];
            else if (x > 0 && conn8 && eq(lv[x - 1], tv[x]))
            {
                ti[x] = li[x - 1];
                if (x < w - 1 && eq(lv[x + 1], tv[x]) &&
                    map[li[x + 1]] != map[ti[x]])
                    merge(li[x + 1], ti[x]);
            }
            else if (x < w - 1 && conn8 && eq(lv[x + 1], tv[x]))
                ti[x] = li[x + 1];
            else
                ti[x] = newPoly();
        }
    }
    for (size_t i = 0; i < map.size(); i++)
    {
        int r = (int)i;
        while (map[r] != r)
            r = map[r];
        map[i] = r;
    }
    canon.resize(ids.size());
    for (size_t i = 0; i < ids.size(); i++)
        canon[i] = map[ids[i]];
}

struct RpfRegionInfo
{
    int id = 0;
    int lastRow = 0;
    size_t lastIdx = 0;
};

// all regions ordered by (last scanline touched, canonical id): the
// reference's emission order
std::vector<RpfRegionInfo> rpfRegionOrder(const std::vector<int> &canon,
                                          int w, int h);

// boundary rings of one region as pixel-corner vertices (closing vertex
// repeated); first ring is the exterior, later rings are holes in
// (y,x)-min-corner order
std::vector<std::vector<std::pair<int, int>>> rpfTraceRegion(
    const std::vector<int> &canon, int w, int h, int region);

// GeoJSON/Shapefile/GeoJSONSeq extension guess shared by both verbs
// (.gdalg.json handled by the callers)
std::string rpfGuessDriver(const std::string &output);
