#include "ogr.h"
#include "util.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// OGRGeometryFactory::forceTo and OGRGeometry::segmentize equivalents
// over the in-memory geometry model, calibrated against the reference's
// observable conversion matrix (see NOTES.md, set-geom-type)

namespace
{

bool isEmptyGeom(const OgrGeometry &g)
{
    return g.coords.empty() && g.parts.empty();
}

OgrGeometry wrapInto(OgrGeometry g, int container)
{
    OgrGeometry out;
    out.type = container;
    out.hasZ = g.hasZ;
    out.hasM = g.hasM;
    out.parts.push_back(std::move(g));
    return out;
}

bool ringClosed(const OgrGeometry &ls)
{
    size_t np = ls.coords.size() / 3;
    if (np < 3)
        return false;
    size_t last = (np - 1) * 3;
    if (ls.coords[0] != ls.coords[last] ||
        ls.coords[1] != ls.coords[last + 1])
        return false;
    if (ls.hasZ && ls.coords[2] != ls.coords[last + 2])
        return false;
    return true;
}

// the in-order linestring merge of forceToLineString: appends when the
// running end meets a later start, prepends when a later end meets the
// running start, never reverses; a single leftover member of any type
// detaches from its container
void mergeLineStrings(OgrGeometry &g)
{
    auto isLine = [](const OgrGeometry &p)
    { return p.type == 2 && p.coords.size() / 3 >= 2; };
    auto eq = [](const OgrGeometry &a, size_t ia, const OgrGeometry &b,
                 size_t ib)
    {
        return a.coords[ia * 3] == b.coords[ib * 3] &&
               a.coords[ia * 3 + 1] == b.coords[ib * 3 + 1] &&
               a.coords[ia * 3 + 2] == b.coords[ib * 3 + 2];
    };
    std::vector<OgrGeometry> &m = g.parts;
    for (size_t i0 = 0; i0 < m.size(); ++i0)
    {
        if (!isLine(m[i0]))
            continue;
        size_t i1 = i0 + 1;
        while (i1 < m.size())
        {
            if (!isLine(m[i1]))
            {
                ++i1;
                continue;
            }
            size_t n0 = m[i0].coords.size() / 3;
            size_t n1 = m[i1].coords.size() / 3;
            if (eq(m[i0], n0 - 1, m[i1], 0))
            {
                m[i0].coords.insert(m[i0].coords.end(),
                                    m[i1].coords.begin() + 3,
                                    m[i1].coords.end());
                if (m[i0].hasM || m[i1].hasM)
                {
                    m[i0].m.resize(n0, 0.0);
                    std::vector<double> tail = m[i1].m;
                    tail.resize(n1, 0.0);
                    m[i0].m.insert(m[i0].m.end(), tail.begin() + 1,
                                   tail.end());
                    m[i0].hasM = true;
                }
                m[i0].hasZ = m[i0].hasZ || m[i1].hasZ;
                m.erase(m.begin() + i1);
                i1 = i0 + 1;
            }
            else if (eq(m[i1], n1 - 1, m[i0], 0))
            {
                OgrGeometry merged = m[i1];
                merged.coords.insert(merged.coords.end(),
                                     m[i0].coords.begin() + 3,
                                     m[i0].coords.end());
                if (merged.hasM || m[i0].hasM)
                {
                    merged.m.resize(n1, 0.0);
                    std::vector<double> tail = m[i0].m;
                    tail.resize(n0, 0.0);
                    merged.m.insert(merged.m.end(), tail.begin() + 1,
                                    tail.end());
                    merged.hasM = true;
                }
                merged.hasZ = merged.hasZ || m[i0].hasZ;
                m[i0] = std::move(merged);
                m.erase(m.begin() + i1);
                i1 = i0 + 1;
            }
            else
                ++i1;
        }
    }

    if (m.size() == 1)
    {
        OgrGeometry only = std::move(m[0]);
        g = std::move(only);
    }
}

}  // namespace

// OGRFromOGCGeomType semantics: the base name only needs to be a
// case-insensitive PREFIX of the value, and the Z/M flags come from the
// LAST one or two characters of the whole string ("LINESTRING QAZ" is
// 3D, "LINESTRING MZ" is 3D-not-measured, "LINESTRING25D" is plain XY,
// "LINESTRINGM Z" loses its M)
bool ogrGeomTypeFromWktName(const std::string &name, int &type, bool &z,
                            bool &m)
{
    std::string s;
    for (char c : name)
        s += (char)toupper((unsigned char)c);
    z = m = false;
    if (!s.empty())
    {
        const char last = s.back();
        if (last == 'M')
        {
            m = true;
            if (s.size() > 1 && s[s.size() - 2] == 'Z')
                z = true;
        }
        else if (last == 'Z')
            z = true;
    }
    struct Base
    {
        const char *n;
        int t;
    };
    static const Base bases[] = {
        {"GEOMETRYCOLLECTION", 7}, {"MULTILINESTRING", 5},
        {"POLYHEDRALSURFACE", 15},  {"CIRCULARSTRING", 8},
        {"COMPOUNDCURVE", 9},       {"CURVEPOLYGON", 10},
        {"MULTISURFACE", 12},       {"MULTIPOLYGON", 6},
        {"MULTIPOINT", 4},          {"MULTICURVE", 11},
        {"LINESTRING", 2},          {"GEOMETRY", 0},
        {"TRIANGLE", 17},           {"POLYGON", 3},
        {"SURFACE", 14},            {"POINT", 1},
        {"CURVE", 13},              {"TIN", 16}};
    for (const Base &b : bases)
    {
        size_t bl = strlen(b.n);
        if (s.compare(0, bl, b.n) != 0)
            continue;
        type = b.t;
        // "GEOMETRY*" (non-collection) is fully generic: dim suffixes
        // are ignored ("GEOMETRYZM" behaves like plain "GEOMETRY")
        if (b.t == 0)
            z = m = false;
        return true;
    }
    z = m = false;
    return false;
}

int ogrGtCollection(int t)
{
    switch (t)
    {
        case 1: return 4;
        case 2: return 5;
        case 3: return 6;
        case 8: return 11;
        case 9: return 11;
        case 10: return 12;
        default: return t;
    }
}

int ogrGtSingle(int t)
{
    switch (t)
    {
        case 4: return 1;
        case 5: return 2;
        case 6: return 3;
        case 7: return 0;
        case 11: return 9;
        case 12: return 10;
        default: return t;
    }
}

int ogrGtCurve(int t)
{
    switch (t)
    {
        case 2: return 9;
        case 3: return 10;
        case 5: return 11;
        case 6: return 12;
        default: return t;
    }
}

int ogrGtLinear(int t)
{
    switch (t)
    {
        case 8: return 2;
        case 9: return 2;
        case 10: return 3;
        case 11: return 5;
        case 12: return 6;
        default: return t;
    }
}

bool polyIsTriangle(const OgrGeometry &poly)
{
    if (poly.parts.size() != 1)
        return false;
    const OgrGeometry &r = poly.parts[0];
    return r.coords.size() == 12 && r.coords[0] == r.coords[9] &&
           r.coords[1] == r.coords[10];
}

void ogrSetGeomDim(OgrGeometry &g, bool z, bool m)
{
    if (!z)
        for (size_t i = 2; i < g.coords.size(); i += 3)
            g.coords[i] = 0.0;
    g.hasZ = z;
    if (m)
        g.m.resize(g.coords.size() / 3, 0.0);
    else
        g.m.clear();
    g.hasM = m;
    for (OgrGeometry &p : g.parts)
        ogrSetGeomDim(p, z, m);
}

void ogrForceTo(OgrGeometry &g, int target)
{
    if (target == 0)
        return;
    int s = g.type;
    if (s == target)
        return;
    bool z = g.hasZ, m = g.hasM;
    if (isEmptyGeom(g))
    {
        g.type = target;
        g.empty = true;
        return;
    }
    auto morphMembers = [&](int newType)
    {
        g.type = newType;
    };
    switch (s)
    {
        case 1:
            if (target == 4 || target == 7)
                g = wrapInto(std::move(g), target);
            return;
        case 2:
            if (target == 5 || target == 7)
            {
                g = wrapInto(std::move(g), target);
                return;
            }
            if (target == 9)
            {
                g = wrapInto(std::move(g), 9);
                return;
            }
            if (target == 11)
            {
                g = wrapInto(std::move(g), 11);
                return;
            }
            if ((target == 3 || target == 6) && ringClosed(g))
            {
                OgrGeometry poly = wrapInto(std::move(g), 3);
                if (target == 6)
                    poly = wrapInto(std::move(poly), 6);
                g = std::move(poly);
            }
            return;
        case 3:
            if (target == 6 || target == 7)
            {
                g = wrapInto(std::move(g), target);
                return;
            }
            if (target == 2)
            {
                if (g.parts.size() == 1)
                {
                    OgrGeometry ring = std::move(g.parts[0]);
                    ring.type = 2;
                    g = std::move(ring);
                }
                return;
            }
            if (target == 5)
            {
                for (OgrGeometry &r : g.parts)
                    r.type = 2;
                morphMembers(5);
                return;
            }
            if (target == 10)
            {
                morphMembers(10);
                return;
            }
            if (target == 12)
            {
                g = wrapInto(std::move(g), 12);
                return;
            }
            if (target == 15)
            {
                g = wrapInto(std::move(g), 15);
                return;
            }
            if ((target == 16 || target == 17) && polyIsTriangle(g))
            {
                if (target == 17)
                    morphMembers(17);
                else
                    g = wrapInto(std::move(g), 16);
                return;
            }
            return;
        case 4:
        case 5:
        case 6:
        {
            if (target == 7)
            {
                morphMembers(7);
                return;
            }
            if ((s == 5 && target == 11) || (s == 6 && target == 12))
            {
                morphMembers(target);
                return;
            }
            if (s == 6 && target == 15)
            {
                morphMembers(15);
                return;
            }
            if (s == 6 && target == 16)
            {
                bool allTri = !g.parts.empty();
                for (const OgrGeometry &p : g.parts)
                    if (!polyIsTriangle(p))
                        allTri = false;
                if (allTri)
                    morphMembers(16);
                return;
            }
            if (g.parts.size() == 1)
            {
                OgrGeometry r = g.parts[0];
                r.hasZ = z;
                r.hasM = m;
                ogrForceTo(r, target);
                if (r.type == target)
                {
                    g = std::move(r);
                    return;
                }
            }
            if (s == 5 && target == 2)
            {
                mergeLineStrings(g);
                return;
            }
            if (s == 6 && target == 3)
            {
                OgrGeometry poly;
                poly.type = 3;
                poly.hasZ = z;
                poly.hasM = m;
                for (OgrGeometry &p : g.parts)
                    for (OgrGeometry &r : p.parts)
                        poly.parts.push_back(std::move(r));
                g = std::move(poly);
                return;
            }
            if (s == 6 && target == 5)
            {
                OgrGeometry mls;
                mls.type = 5;
                mls.hasZ = z;
                mls.hasM = m;
                for (OgrGeometry &p : g.parts)
                    for (OgrGeometry &r : p.parts)
                    {
                        r.type = 2;
                        mls.parts.push_back(std::move(r));
                    }
                g = std::move(mls);
                return;
            }
            return;
        }
        case 7:
        {
            if (g.parts.size() == 1)
            {
                OgrGeometry r = g.parts[0];
                ogrForceTo(r, target);
                if (r.type == target)
                {
                    g = std::move(r);
                    return;
                }
            }
            if (target == 2)
            {
                mergeLineStrings(g);
                return;
            }
            if (target == 3)
            {
                OgrGeometry poly;
                poly.type = 3;
                poly.hasZ = z;
                poly.hasM = m;
                for (OgrGeometry &p : g.parts)
                    if (p.type == 3)
                        for (OgrGeometry &r : p.parts)
                            poly.parts.push_back(std::move(r));
                poly.empty = poly.parts.empty();
                g = std::move(poly);
                return;
            }
            if (target == 4 || target == 5 || target == 6)
            {
                int member = target - 3;
                for (const OgrGeometry &p : g.parts)
                    if (p.type != member)
                        return;
                morphMembers(target);
                return;
            }
            return;
        }
        case 9:
            if (target == 2)
                mergeLineStrings(g);
            return;
        case 10:
            if (target == 3)
                morphMembers(3);
            return;
        case 11:
            if (target == 5)
                morphMembers(5);
            return;
        case 12:
            if (target == 6)
                morphMembers(6);
            return;
        default:
            return;
    }
}

void ogrSegmentize(OgrGeometry &g, double maxLength)
{
    if (g.type == 2)
    {
        size_t np = g.coords.size() / 3;
        if (np < 2)
            return;
        bool hasM = g.hasM;
        std::vector<double> c = g.coords;
        std::vector<double> mv = g.m;
        if (hasM)
            mv.resize(np, 0.0);
        bool reversed = c[0] < c[(np - 1) * 3] ||
                        (c[0] == c[(np - 1) * 3] &&
                         c[1] < c[(np - 1) * 3 + 1]);
        if (reversed)
        {
            std::vector<double> rc(c.size());
            for (size_t i = 0; i < np; ++i)
                for (int k = 0; k < 3; ++k)
                    rc[i * 3 + k] = c[(np - 1 - i) * 3 + k];
            c = std::move(rc);
            if (hasM)
            {
                std::vector<double> rm(np);
                for (size_t i = 0; i < np; ++i)
                    rm[i] = mv[np - 1 - i];
                mv = std::move(rm);
            }
        }
        std::vector<double> oc;
        std::vector<double> om;
        double m2 = maxLength * maxLength;
        for (size_t i = 0; i + 1 < np; ++i)
        {
            oc.insert(oc.end(), c.begin() + i * 3, c.begin() + i * 3 + 3);
            if (hasM)
                om.push_back(mv[i]);
            double dx = c[(i + 1) * 3] - c[i * 3];
            double dy = c[(i + 1) * 3 + 1] - c[i * 3 + 1];
            double d2 = dx * dx + dy * dy;
            if (!(d2 > m2) || m2 <= 0)
                continue;
            double fn = floor(sqrt(d2 / m2) - 1e-2);
            if (!(fn >= 1))
                continue;
            int n = fn > 2147483647.0 ? 2147483647 : (int)fn;
            for (int j = 1; j <= n; ++j)
            {
                oc.push_back(c[i * 3] + j * dx / (n + 1));
                oc.push_back(c[i * 3 + 1] + j * dy / (n + 1));
                oc.push_back(c[i * 3 + 2]);
                if (hasM)
                    om.push_back(mv[i]);
            }
        }
        oc.insert(oc.end(), c.begin() + (np - 1) * 3, c.end());
        if (hasM)
            om.push_back(mv[np - 1]);
        size_t onp = oc.size() / 3;
        if (reversed)
        {
            std::vector<double> rc(oc.size());
            for (size_t i = 0; i < onp; ++i)
                for (int k = 0; k < 3; ++k)
                    rc[i * 3 + k] = oc[(onp - 1 - i) * 3 + k];
            oc = std::move(rc);
            if (hasM)
            {
                std::vector<double> rm(onp);
                for (size_t i = 0; i < onp; ++i)
                    rm[i] = om[onp - 1 - i];
                om = std::move(rm);
            }
        }
        g.coords = std::move(oc);
        g.m = std::move(om);
        return;
    }
    for (OgrGeometry &p : g.parts)
        ogrSegmentize(p, maxLength);
}
