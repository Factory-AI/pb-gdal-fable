// gdal mdim info/convert/mosaic over the multidim VRT + MEM surface of
// the reference build (netCDF/HDF trimmed out)
#include "engine.h"
#include "util.h"
#include "cpl.h"
#include "jsonwriter.h"
#include "xml_min.h"
#include "srs.h"
#include "progress.h"
#include "dataset.h"

#include <algorithm>
#include <cmath>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

int rasterConvertHandlerEntry(const CmdSpec &spec, ParseResult &r);

namespace
{

// ------------------------------------------------------------------
// model
// ------------------------------------------------------------------

struct MdAttr
{
    std::string name;
    std::string dtype;  // String, Byte, Int16, ..., Float64
    std::vector<std::string> values;
};

struct MdSource
{
    std::string filename;
    bool relative = false;
    std::string arrayFull;
    std::string view;
    std::vector<int> transpose;
    std::vector<uint64_t> srcOffset, srcCount, srcStep, dstOffset;
    bool hasSlab = false;
};

struct MdArray
{
    std::string name;
    std::string dtype = "Float64";
    std::vector<std::string> dimRefs;  // as written (local or absolute)
    std::vector<std::string> dimFull;  // resolved full names
    std::vector<uint64_t> dimSizes;
    bool hasSrs = false;
    std::string srsWkt;  // single-line WKT2 as serialized
    std::vector<int> axisMapping;
    std::string unit;
    bool hasNodata = false;
    std::string nodataRaw;
    bool hasOffset = false, hasScale = false;
    double offset = 0, scale = 1;
    std::vector<MdAttr> attrs;
    enum Kind
    {
        None,
        Inline,
        InlineValueElements,
        Regular,
        Constant,
        Sourced
    } kind = None;
    std::vector<std::string> tokens;  // Inline / InlineValueElements
    std::vector<uint64_t> inlineOffset, inlineCount;
    bool hasInlineOffset = false;
    double regStart = 0, regIncrement = 0;
    std::string constRaw;
    std::vector<MdSource> sources;

    uint64_t totalSize() const
    {
        uint64_t n = 1;
        for (uint64_t s : dimSizes)
            n *= s;
        return n;
    }
};

struct MdGroup
{
    std::string name;
    std::string fullName;  // "/" or "/sub"
    struct Dim
    {
        std::string name, fullName, type, direction, indexingVar;
        uint64_t size = 0;
    };
    std::vector<Dim> dims;
    std::vector<MdAttr> attrs;
    std::vector<MdArray> arrays;
    std::vector<MdGroup> groups;

    const Dim *findDim(const std::string &n) const
    {
        for (const auto &d : dims)
            if (d.name == n)
                return &d;
        return nullptr;
    }
    const MdArray *findArray(const std::string &n) const
    {
        for (const auto &a : arrays)
            if (a.name == n)
                return &a;
        return nullptr;
    }
};

struct MdDataset
{
    MdGroup root;
    std::string path;  // as opened
    std::string dirName;
};

// ------------------------------------------------------------------
// value typing helpers
// ------------------------------------------------------------------

bool mdIsString(const std::string &t) { return t == "String"; }

bool mdIsInteger(const std::string &t)
{
    return t == "Byte" || t == "Int8" || t == "Int16" || t == "UInt16" ||
           t == "Int32" || t == "UInt32" || t == "Int64" || t == "UInt64";
}

bool mdValidType(const std::string &t)
{
    return mdIsString(t) || mdIsInteger(t) || t == "Float32" ||
           t == "Float64" || t == "Float16" || t == "CInt16" ||
           t == "CInt32" || t == "CFloat32" || t == "CFloat64";
}

int mdTypeSize(const std::string &t)
{
    if (t == "Byte" || t == "Int8")
        return 1;
    if (t == "Int16" || t == "UInt16" || t == "Float16")
        return 2;
    if (t == "Int32" || t == "UInt32" || t == "Float32" || t == "CInt16")
        return 4;
    if (t == "Int64" || t == "UInt64" || t == "Float64" || t == "CInt32" ||
        t == "CFloat32")
        return 8;
    if (t == "CFloat64")
        return 16;
    return 8;
}

// storage clamp mirroring GDALCopyWord double -> integer semantics
long long mdClampInt(const std::string &t, double v)
{
    double lo = 0, hi = 0;
    if (t == "Byte")
    {
        lo = 0;
        hi = 255;
    }
    else if (t == "Int8")
    {
        lo = -128;
        hi = 127;
    }
    else if (t == "Int16")
    {
        lo = -32768;
        hi = 32767;
    }
    else if (t == "UInt16")
    {
        lo = 0;
        hi = 65535;
    }
    else if (t == "Int32")
    {
        lo = -2147483648.0;
        hi = 2147483647.0;
    }
    else if (t == "UInt32")
    {
        lo = 0;
        hi = 4294967295.0;
    }
    if (std::isnan(v))
        return 0;
    if (v <= lo)
        return (long long)lo;
    if (v >= hi)
        return (long long)hi;
    return (long long)std::floor(v + 0.5);
}

// one typed scalar, stored losslessly for Int64/UInt64
struct MdVal
{
    double d = 0;
    long long ll = 0;
    unsigned long long ull = 0;
    std::string s;
};

MdVal mdParseToken(const std::string &t, const std::string &tok)
{
    MdVal v;
    if (mdIsString(t))
    {
        v.s = tok;
        return v;
    }
    if (t == "Int64")
    {
        v.ll = strtoll(tok.c_str(), nullptr, 10);
        v.d = (double)v.ll;
        return v;
    }
    if (t == "UInt64")
    {
        v.ull = strtoull(tok.c_str(), nullptr, 10);
        v.d = (double)v.ull;
        return v;
    }
    double d = strtod(tok.c_str(), nullptr);
    if (mdIsInteger(t))
    {
        v.ll = mdClampInt(t, d);
        v.d = (double)v.ll;
        return v;
    }
    if (t == "Float32")
        d = (double)(float)d;
    v.d = d;
    return v;
}

MdVal mdFromDouble(const std::string &t, double d)
{
    MdVal v;
    if (mdIsString(t))
    {
        v.s = strPrintf("%.17g", d);
        return v;
    }
    if (t == "Int64")
    {
        double c = d;
        if (std::isnan(c))
            c = 0;
        if (c <= -9223372036854775808.0)
            v.ll = INT64_MIN;
        else if (c >= 9223372036854775807.0)
            v.ll = INT64_MAX;
        else
            v.ll = (long long)std::floor(c + 0.5);
        v.d = (double)v.ll;
        return v;
    }
    if (t == "UInt64")
    {
        double c = d;
        if (std::isnan(c) || c <= 0)
            v.ull = 0;
        else if (c >= 18446744073709551615.0)
            v.ull = UINT64_MAX;
        else
            v.ull = (unsigned long long)std::floor(c + 0.5);
        v.d = (double)v.ull;
        return v;
    }
    if (mdIsInteger(t))
    {
        v.ll = mdClampInt(t, d);
        v.d = (double)v.ll;
        return v;
    }
    if (t == "Float32")
        d = (double)(float)d;
    v.d = d;
    return v;
}

// JSON rendering of one value per gdalmdiminfo rules
void mdWriteJsonValue(JsonStreamWriter &w, const std::string &t,
                      const MdVal &v)
{
    if (mdIsString(t))
    {
        w.addString(v.s);
        return;
    }
    if (t == "Int64")
    {
        w.addInt(v.ll);
        return;
    }
    if (t == "UInt64")
    {
        w.addRaw(strPrintf("%llu", v.ull));
        return;
    }
    if (mdIsInteger(t))
    {
        w.addInt(v.ll);
        return;
    }
    if (std::isnan(v.d))
    {
        w.addString("NaN");
        return;
    }
    if (std::isinf(v.d))
    {
        w.addString(v.d > 0 ? "Infinity" : "-Infinity");
        return;
    }
    if (t == "Float32")
        w.addRaw(strPrintf("%.9g", v.d));
    else
        w.addRaw(strPrintf("%.17g", v.d));
}

// ------------------------------------------------------------------
// XML reader
// ------------------------------------------------------------------

std::vector<std::string> mdSplitTokens(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace((unsigned char)s[i]))
            ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j]))
            ++j;
        if (j > i)
            out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::vector<uint64_t> mdParseUIntList(const std::string &s)
{
    std::vector<uint64_t> out;
    for (const auto &t : strSplit(s, ','))
        out.push_back(strtoull(t.c_str(), nullptr, 10));
    return out;
}

struct MdParseCtx
{
    std::string error;
    bool fail(const std::string &e)
    {
        if (error.empty())
            error = e;
        return false;
    }
};

bool mdParseAttr(const XmlNode &n, MdAttr &a, MdParseCtx &ctx)
{
    a.name = n.attr("name");
    a.dtype = n.attr("dataType");
    if (a.dtype.empty())
    {
        const XmlNode *dt = n.child("DataType");
        a.dtype = dt ? dt->text : "String";
    }
    for (const auto &c : n.children)
        if (c.name == "Value")
            a.values.push_back(c.text);
    if (a.values.empty())
    {
        const std::string v = n.attr("value", "\x01");
        if (v != "\x01")
            a.values.push_back(v);
    }
    (void)ctx;
    return true;
}

bool mdParseGroup(const XmlNode &node, MdGroup &g, const std::string &full,
                  MdParseCtx &ctx);

bool mdParseArray(const XmlNode &node, MdGroup &g, MdArray &a,
                  MdParseCtx &ctx)
{
    a.name = node.attr("name");
    for (const auto &c : node.children)
    {
        if (c.name == "DataType")
            a.dtype = c.text;
        else if (c.name == "DimensionRef")
        {
            const std::string ref = c.attr("ref");
            a.dimRefs.push_back(ref);
        }
        else if (c.name == "Dimension")
        {
            // inline (anonymous) dimension declaration
            MdGroup::Dim d;
            d.name = c.attr("name");
            d.size = strtoull(c.attr("size", "0").c_str(), nullptr, 10);
            if (d.size == 0)
                return ctx.fail(
                    "Invalid value for size attribute on Dimension");
            d.type = c.attr("type");
            d.direction = c.attr("direction");
            d.fullName =
                (g.fullName == "/" ? "/" : g.fullName + "/") + d.name;
            g.dims.push_back(d);
            a.dimRefs.push_back(d.name);
        }
        else if (c.name == "SRS")
        {
            a.hasSrs = true;
            a.srsWkt = c.text;
            const std::string m = c.attr("dataAxisToSRSAxisMapping");
            if (!m.empty())
                for (const auto &t : strSplit(m, ','))
                    a.axisMapping.push_back(atoi(t.c_str()));
        }
        else if (c.name == "Unit")
            a.unit = c.text;
        else if (c.name == "NoDataValue")
        {
            a.hasNodata = true;
            a.nodataRaw = c.text;
        }
        else if (c.name == "Offset")
        {
            a.hasOffset = true;
            a.offset = strtod(c.text.c_str(), nullptr);
        }
        else if (c.name == "Scale")
        {
            a.hasScale = true;
            a.scale = strtod(c.text.c_str(), nullptr);
        }
        else if (c.name == "RegularlySpacedValues")
        {
            a.kind = MdArray::Regular;
            a.regStart = strtod(c.attr("start", "0").c_str(), nullptr);
            a.regIncrement = strtod(c.attr("increment", "0").c_str(), nullptr);
        }
        else if (c.name == "InlineValues")
        {
            a.kind = MdArray::Inline;
            a.tokens = mdSplitTokens(c.text);
            const std::string off = c.attr("offset");
            if (!off.empty())
            {
                a.hasInlineOffset = true;
                a.inlineOffset = mdParseUIntList(off);
                a.inlineCount = mdParseUIntList(c.attr("count"));
            }
        }
        else if (c.name == "InlineValuesWithValueElement")
        {
            a.kind = MdArray::InlineValueElements;
            for (const auto &v : c.children)
                if (v.name == "Value")
                    a.tokens.push_back(v.text);
        }
        else if (c.name == "ConstantValue")
        {
            a.kind = MdArray::Constant;
            a.constRaw = c.text;
        }
        else if (c.name == "Source")
        {
            a.kind = MdArray::Sourced;
            MdSource s;
            for (const auto &sc : c.children)
            {
                if (sc.name == "SourceFilename")
                {
                    s.filename = sc.text;
                    s.relative = sc.attr("relativeToVRT",
                                         sc.attr("relativetoVRT", "0")) ==
                                 "1";
                }
                else if (sc.name == "SourceArray")
                    s.arrayFull = sc.text;
                else if (sc.name == "SourceView")
                    s.view = sc.text;
                else if (sc.name == "SourceTranspose")
                {
                    for (const auto &t : strSplit(sc.text, ','))
                        s.transpose.push_back(atoi(t.c_str()));
                }
                else if (sc.name == "SourceSlab")
                {
                    s.hasSlab = true;
                    s.srcOffset = mdParseUIntList(sc.attr("offset"));
                    s.srcCount = mdParseUIntList(sc.attr("count"));
                    s.srcStep = mdParseUIntList(sc.attr("step"));
                }
                else if (sc.name == "DestSlab")
                    s.dstOffset = mdParseUIntList(sc.attr("offset"));
            }
            a.sources.push_back(s);
        }
        else if (c.name == "Attribute")
        {
            MdAttr at;
            mdParseAttr(c, at, ctx);
            a.attrs.push_back(at);
        }
    }
    // resolve dims
    for (const auto &r : a.dimRefs)
    {
        const MdGroup::Dim *d = nullptr;
        if (!r.empty() && r[0] == '/')
        {
            // absolute reference: only same-group absolute paths appear
            // in practice; resolve by suffix within this group
            size_t sl = r.find_last_of('/');
            d = g.findDim(r.substr(sl + 1));
        }
        else
            d = g.findDim(r);
        if (!d)
            return ctx.fail(strPrintf(
                "Cannot find dimension %s", r.c_str()));
        a.dimFull.push_back(d->fullName);
        a.dimSizes.push_back(d->size);
    }
    if (!mdValidType(a.dtype))
        return ctx.fail(strPrintf("Invalid value for VRTMDArray.DataType: %s",
                                  a.dtype.c_str()));
    for (const auto &sv : a.sources)
        for (size_t k = 0;
             k < sv.dstOffset.size() && k < a.dimSizes.size(); ++k)
            if (sv.dstOffset[k] >= a.dimSizes[k])
                return ctx.fail("Wrong value in offset");
    // count validation for inline values
    if (a.kind == MdArray::Inline || a.kind == MdArray::InlineValueElements)
    {
        uint64_t expected = 1;
        if (a.hasInlineOffset)
            for (uint64_t c : a.inlineCount)
                expected *= c;
        else
            expected = a.totalSize();
        uint64_t scale = 1;
        if (a.dtype == "CInt16" || a.dtype == "CInt32" ||
            a.dtype == "CFloat32" || a.dtype == "CFloat64")
            scale = 1;  // complex parse counts pairs; reference errors on
                        // token mismatch against logical count
        (void)scale;
        if ((uint64_t)a.tokens.size() != expected)
            return ctx.fail(strPrintf(
                "Invalid number of values. Got %u, expected %u",
                (unsigned)a.tokens.size(), (unsigned)expected));
    }
    return true;
}

bool mdParseGroup(const XmlNode &node, MdGroup &g, const std::string &full,
                  MdParseCtx &ctx)
{
    g.name = node.attr("name");
    g.fullName = full;
    // dimensions must be parsed before arrays referencing them
    for (const auto &c : node.children)
    {
        if (c.name == "Dimension")
        {
            MdGroup::Dim d;
            d.name = c.attr("name");
            d.size = strtoull(c.attr("size", "0").c_str(), nullptr, 10);
            if (d.size == 0)
                return ctx.fail(
                    "Invalid value for size attribute on Dimension");
            d.type = c.attr("type");
            d.direction = c.attr("direction");
            d.indexingVar = c.attr("indexingVariable");
            d.fullName = (full == "/" ? "/" : full + "/") + d.name;
            g.dims.push_back(d);
        }
    }
    for (const auto &c : node.children)
    {
        if (c.name == "Attribute")
        {
            MdAttr a;
            mdParseAttr(c, a, ctx);
            g.attrs.push_back(a);
        }
        else if (c.name == "Array")
        {
            MdArray a;
            if (!mdParseArray(c, g, a, ctx))
                return false;
            g.arrays.push_back(a);
        }
        else if (c.name == "Group")
        {
            MdGroup sub;
            const std::string subFull =
                (full == "/" ? "/" : full + "/") + c.attr("name");
            if (!mdParseGroup(c, sub, subFull, ctx))
                return false;
            g.groups.push_back(sub);
        }
    }
    return true;
}

// returns false with err text when the file is a VRT but not a valid
// multidim one; notVrt=true when it isn't a multidim VRT at all
bool mdimOpen(const std::string &path, MdDataset &ds, std::string &err,
              bool &notVrt)
{
    notVrt = false;
    std::string content;
    if (!readFileToString(path, content))
    {
        notVrt = true;
        return false;
    }
    XmlNode root;
    if (!xmlParse(content, root) || root.name != "VRTDataset")
    {
        notVrt = true;
        return false;
    }
    const XmlNode *grp = root.child("Group");
    if (!grp)
    {
        notVrt = true;
        return false;
    }
    if (grp->attr("name") != "/")
    {
        err = "Root group should be named /";
        return false;
    }
    MdParseCtx ctx;
    ds.path = path;
    size_t sl = path.find_last_of('/');
    ds.dirName = sl == std::string::npos ? "." : path.substr(0, sl);
    if (!mdParseGroup(*grp, ds.root, "/", ctx))
    {
        err = ctx.error;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------
// value resolution (materializes an array into typed scalars)
// ------------------------------------------------------------------

struct MdViewDim
{
    uint64_t start = 0, size = 0, step = 1;
};

bool mdResolveValues(const MdDataset &ds, const MdArray &a,
                     std::vector<MdVal> &out, std::string &err);

// applies view/transpose/slab of one source into dst
bool mdApplySource(const MdDataset &ds, const MdArray &dst,
                   const MdSource &src, std::vector<MdVal> &out,
                   std::string &err)
{
    std::string p = src.filename;
    if (src.relative && !p.empty() && p[0] != '/')
        p = ds.dirName + "/" + p;
    MdDataset sub;
    bool notVrt = false;
    if (!mdimOpen(p, sub, err, notVrt))
    {
        if (err.empty())
            err = "Cannot open " + src.filename;
        return false;
    }
    // find source array by full path
    const MdGroup *g = &sub.root;
    std::vector<std::string> parts;
    for (const auto &t : strSplit(src.arrayFull, '/'))
        if (!t.empty())
            parts.push_back(t);
    if (parts.empty())
    {
        err = "Cannot find array " + src.arrayFull;
        return false;
    }
    for (size_t i = 0; i + 1 < parts.size(); ++i)
    {
        const MdGroup *next = nullptr;
        for (const auto &sg : g->groups)
            if (sg.name == parts[i])
                next = &sg;
        if (!next)
        {
            err = "Cannot find array " + src.arrayFull;
            return false;
        }
        g = next;
    }
    const MdArray *sa = g->findArray(parts.back());
    if (!sa)
    {
        err = "Cannot find array " + src.arrayFull;
        return false;
    }
    std::vector<MdVal> svals;
    if (!mdResolveValues(sub, *sa, svals, err))
        return false;
    // dims after view
    std::vector<MdViewDim> vd(sa->dimSizes.size());
    for (size_t i = 0; i < vd.size(); ++i)
    {
        vd[i].start = 0;
        vd[i].size = sa->dimSizes[i];
        vd[i].step = 1;
    }
    if (!src.view.empty())
    {
        std::string v = src.view;
        if (v.size() >= 2 && v.front() == '[' && v.back() == ']')
            v = v.substr(1, v.size() - 2);
        auto specs = strSplit(v, ',');
        for (size_t i = 0; i < specs.size() && i < vd.size(); ++i)
        {
            const std::string &sp = specs[i];
            if (sp == ":" || sp.empty())
                continue;
            // start:stop:step (numpy-ish, only forward slices appear)
            std::vector<std::string> f = strSplit(sp, ':');
            uint64_t start = f.size() > 0 && !f[0].empty()
                                 ? strtoull(f[0].c_str(), nullptr, 10)
                                 : 0;
            uint64_t stop = f.size() > 1 && !f[1].empty()
                                ? strtoull(f[1].c_str(), nullptr, 10)
                                : vd[i].size;
            uint64_t step = f.size() > 2 && !f[2].empty()
                                ? strtoull(f[2].c_str(), nullptr, 10)
                                : 1;
            if (f.size() == 1)
            {
                stop = start + 1;
            }
            vd[i].start = start;
            vd[i].step = step;
            vd[i].size = stop > start ? (stop - start + step - 1) / step : 0;
        }
    }
    // transpose maps view axes to source axes order
    std::vector<int> axes;
    if (!src.transpose.empty())
        axes = src.transpose;
    else
        for (size_t i = 0; i < vd.size(); ++i)
            axes.push_back((int)i);
    const size_t nd = dst.dimSizes.size();
    std::vector<uint64_t> so(nd, 0), sc(nd, 0), st(nd, 1), doff(nd, 0);
    for (size_t i = 0; i < nd; ++i)
    {
        so[i] = i < src.srcOffset.size() ? src.srcOffset[i] : 0;
        sc[i] = i < src.srcCount.size()
                    ? src.srcCount[i]
                    : (i < axes.size() && (size_t)axes[i] < vd.size()
                           ? vd[axes[i]].size
                           : 1);
        st[i] = i < src.srcStep.size() ? src.srcStep[i] : 1;
        doff[i] = i < src.dstOffset.size() ? src.dstOffset[i] : 0;
    }
    // iterate over the dst slab
    std::vector<uint64_t> idx(nd, 0);
    const std::vector<uint64_t> &srcDims = sa->dimSizes;
    while (true)
    {
        // compute source flat index
        uint64_t flat = 0;
        for (size_t i = 0; i < srcDims.size(); ++i)
        {
            // which dst axis maps to source axis i (after transpose)?
            size_t viewAxis = i;
            uint64_t coordInView = 0;
            // find position of source axis i in axes[]
            size_t pos = 0;
            for (size_t k = 0; k < axes.size(); ++k)
                if ((size_t)axes[k] == i)
                    pos = k;
            viewAxis = i;
            coordInView = pos < nd ? so[pos] + idx[pos] * st[pos] : 0;
            uint64_t coordInSrc =
                vd[viewAxis].start + coordInView * vd[viewAxis].step;
            flat = flat * srcDims[i] + coordInSrc;
        }
        // dst flat index
        uint64_t dflat = 0;
        for (size_t i = 0; i < nd; ++i)
            dflat = dflat * dst.dimSizes[i] + (doff[i] + idx[i]);
        if (flat < svals.size() && dflat < out.size())
            out[dflat] = mdFromDouble(
                dst.dtype, mdIsString(sa->dtype) ? 0 : svals[flat].d);
        if (mdIsString(dst.dtype) && flat < svals.size() &&
            dflat < out.size())
            out[dflat].s = svals[flat].s;
        // increment
        size_t k = nd;
        while (k > 0)
        {
            --k;
            if (++idx[k] < sc[k])
                break;
            idx[k] = 0;
            if (k == 0)
                return true;
        }
        if (nd == 0)
            return true;
    }
}

bool mdResolveValues(const MdDataset &ds, const MdArray &a,
                     std::vector<MdVal> &out, std::string &err)
{
    const uint64_t n = a.totalSize();
    out.assign((size_t)n, mdFromDouble(a.dtype, 0));
    switch (a.kind)
    {
        case MdArray::None:
            return true;
        case MdArray::Regular:
        {
            for (uint64_t i = 0; i < n; ++i)
                out[(size_t)i] = mdFromDouble(
                    a.dtype, a.regStart + a.regIncrement * (double)i);
            return true;
        }
        case MdArray::Constant:
        {
            MdVal v = mdParseToken(a.dtype, a.constRaw);
            for (uint64_t i = 0; i < n; ++i)
                out[(size_t)i] = v;
            return true;
        }
        case MdArray::Inline:
        case MdArray::InlineValueElements:
        {
            if (!a.hasInlineOffset)
            {
                for (size_t i = 0; i < a.tokens.size() && i < out.size();
                     ++i)
                    out[i] = mdParseToken(a.dtype, a.tokens[i]);
                return true;
            }
            // offset/count block placement (row-major)
            const size_t nd = a.dimSizes.size();
            std::vector<uint64_t> idx(nd, 0);
            size_t ti = 0;
            while (ti < a.tokens.size())
            {
                uint64_t flat = 0;
                for (size_t i = 0; i < nd; ++i)
                    flat = flat * a.dimSizes[i] +
                           ((i < a.inlineOffset.size() ? a.inlineOffset[i]
                                                       : 0) +
                            idx[i]);
                if (flat < out.size())
                    out[(size_t)flat] =
                        mdParseToken(a.dtype, a.tokens[ti]);
                ++ti;
                size_t k = nd;
                bool done = nd == 0;
                while (k > 0)
                {
                    --k;
                    if (++idx[k] <
                        (k < a.inlineCount.size() ? a.inlineCount[k] : 1))
                        break;
                    idx[k] = 0;
                    if (k == 0)
                        done = true;
                }
                if (done)
                    break;
            }
            return true;
        }
        case MdArray::Sourced:
        {
            for (const auto &s : a.sources)
                if (!mdApplySource(ds, a, s, out, err))
                    return false;
            return true;
        }
    }
    return true;
}

// ------------------------------------------------------------------
// info JSON
// ------------------------------------------------------------------

struct InfoOpts
{
    bool summary = false;
    bool detailed = false;
    bool stats = false;
    long long limit = 0;  // <=0: no limit
};

void mdWriteAttrValue(JsonStreamWriter &w, const MdAttr &a)
{
    if (a.values.size() == 1)
    {
        mdWriteJsonValue(w, a.dtype, mdParseToken(a.dtype, a.values[0]));
        return;
    }
    bool prev = w.setNewline(false);
    w.startArray();
    for (const auto &v : a.values)
        mdWriteJsonValue(w, a.dtype, mdParseToken(a.dtype, v));
    w.endArray();
    w.setNewline(prev);
}

void mdWriteAttributes(JsonStreamWriter &w,
                       const std::vector<MdAttr> &attrs, bool detailed)
{
    std::vector<const MdAttr *> sorted;
    for (const auto &a : attrs)
        sorted.push_back(&a);
    std::sort(sorted.begin(), sorted.end(),
              [](const MdAttr *x, const MdAttr *y)
              { return x->name < y->name; });
    w.addKey("attributes");
    w.startObject();
    for (const auto *a : sorted)
    {
        w.addKey(a->name);
        if (detailed)
        {
            w.startObject();
            w.addKey("datatype");
            w.addString(a->dtype);
            w.addKey("value");
            mdWriteAttrValue(w, *a);
            w.endObject();
        }
        else
            mdWriteAttrValue(w, *a);
    }
    w.endObject();
}

// values with per-dimension limit truncation
void mdWriteValuesRec(JsonStreamWriter &w, const MdArray &a,
                      const std::vector<MdVal> &vals, size_t dim,
                      uint64_t offset, long long limit)
{
    const size_t nd = a.dimSizes.size();
    const uint64_t size = nd ? a.dimSizes[dim] : 0;
    uint64_t stride = 1;
    for (size_t i = dim + 1; i < nd; ++i)
        stride *= a.dimSizes[i];
    const bool leaf = dim + 1 >= nd;
    uint64_t lead = size, tail = 0;
    bool truncated = false;
    if (limit > 0 && size > (uint64_t)limit)
    {
        truncated = true;
        lead = (uint64_t)((limit + 1) / 2);
        tail = (uint64_t)limit - lead;
    }
    w.startArray();
    bool prev = false;
    if (leaf)
        prev = w.setNewline(false);
    auto emit = [&](uint64_t i)
    {
        if (leaf)
            mdWriteJsonValue(w, a.dtype, vals[(size_t)(offset + i)]);
        else
            mdWriteValuesRec(w, a, vals, dim + 1, offset + i * stride,
                             limit);
    };
    for (uint64_t i = 0; i < lead; ++i)
        emit(i);
    if (truncated)
    {
        w.addString("[...]");
        for (uint64_t i = 0; i < tail; ++i)
            emit(size - tail + i);
    }
    w.endArray();
    if (leaf)
        w.setNewline(prev);
}

void mdWriteValues(JsonStreamWriter &w, const MdArray &a,
                   const std::vector<MdVal> &vals, long long limit)
{
    w.addKey("values");
    if (a.dimSizes.empty())
    {
        if (!vals.empty())
            mdWriteJsonValue(w, a.dtype, vals[0]);
        else
            w.addNull();
        return;
    }
    mdWriteValuesRec(w, a, vals, 0, 0, limit);
}

struct MdStats
{
    double min = 0, max = 0, mean = 0, stddev = 0;
    uint64_t valid = 0;
};

bool mdComputeStats(const MdArray &a, const std::vector<MdVal> &vals,
                    MdStats &st)
{
    if (mdIsString(a.dtype) || a.dtype[0] == 'C')
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Statistics can only be computed on non-complex "
                    "numeric data type");
        return false;
    }
    double nodata = 0;
    bool hasNd = a.hasNodata;
    if (hasNd)
        nodata = strtod(a.nodataRaw.c_str(), nullptr);
    double mean = 0, m2 = 0;
    uint64_t n = 0;
    double mn = 0, mx = 0;
    for (const auto &v : vals)
    {
        double d = v.d;
        if (std::isnan(d))
            continue;
        if (hasNd && d == nodata)
            continue;
        ++n;
        if (n == 1)
        {
            mn = mx = d;
        }
        else
        {
            if (d < mn)
                mn = d;
            if (d > mx)
                mx = d;
        }
        const double delta = d - mean;
        mean += delta / (double)n;
        m2 += delta * (d - mean);
    }
    if (n == 0)
        return false;
    st.min = mn;
    st.max = mx;
    st.mean = mean;
    st.stddev = std::sqrt(m2 / (double)n);
    st.valid = n;
    return true;
}

void mdWriteStats(JsonStreamWriter &w, const MdStats &st)
{
    w.addKey("statistics");
    w.startObject();
    w.addKey("min");
    w.addDouble(st.min);
    w.addKey("max");
    w.addDouble(st.max);
    w.addKey("mean");
    w.addDouble(st.mean);
    w.addKey("stddev");
    w.addDouble(st.stddev);
    w.addKey("valid_sample_count");
    w.addRaw(strPrintf("%llu", (unsigned long long)st.valid));
    w.endObject();
}

void mdWriteNoData(JsonStreamWriter &w, const MdArray &a)
{
    w.addKey("nodata_value");
    MdVal v = mdParseToken(a.dtype, a.nodataRaw);
    mdWriteJsonValue(w, a.dtype, v);
}

// summary body of an array (shared between group listing and
// indexing_variable blocks)
void mdWriteArrayBody(JsonStreamWriter &w, const MdDataset &ds,
                      const MdArray &a, const InfoOpts &o,
                      bool insideIndexingVar)
{
    w.addKey("datatype");
    w.addString(a.dtype);
    if (!a.dimFull.empty())
    {
        w.addKey("dimensions");
        w.startArray();
        for (const auto &d : a.dimFull)
            w.addString(d);
        w.endArray();
        w.addKey("dimension_size");
        w.startArray();
        for (uint64_t s : a.dimSizes)
            w.addRaw(strPrintf("%llu", (unsigned long long)s));
        w.endArray();
    }
    if (!a.attrs.empty() && !insideIndexingVar)
        mdWriteAttributes(w, a.attrs, o.detailed);
    if (!a.unit.empty() && !insideIndexingVar)
    {
        w.addKey("unit");
        w.addString(a.unit);
    }
    if (a.hasNodata && !insideIndexingVar)
        mdWriteNoData(w, a);
    if (a.hasOffset && !insideIndexingVar)
    {
        w.addKey("offset");
        w.addDouble(a.offset);
    }
    if (a.hasScale && !insideIndexingVar)
    {
        w.addKey("scale");
        w.addDouble(a.scale);
    }
    if (a.hasSrs && !insideIndexingVar)
    {
        w.addKey("srs");
        w.startObject();
        w.addKey("wkt");
        bool ok = false;
        Srs srs = Srs::fromUserInput(a.srsWkt, ok);
        w.addString(ok ? srs.wkt2SingleLine() : a.srsWkt);
        w.addKey("data_axis_to_srs_axis_mapping");
        bool prev = w.setNewline(false);
        w.startArray();
        for (int m : a.axisMapping)
            w.addInt(m);
        w.endArray();
        w.setNewline(prev);
        w.endObject();
    }
    if (o.detailed)
    {
        std::vector<MdVal> vals;
        std::string err;
        if (mdResolveValues(ds, a, vals, err))
            mdWriteValues(w, a, vals, o.limit);
    }
    if (o.stats)
    {
        std::vector<MdVal> vals;
        std::string err;
        MdStats st;
        if (mdResolveValues(ds, a, vals, err) &&
            mdComputeStats(a, vals, st))
            mdWriteStats(w, st);
    }
}

void mdWriteDimensions(JsonStreamWriter &w, const MdDataset &ds,
                       const MdGroup &g, const InfoOpts &o)
{
    std::vector<const MdGroup::Dim *> dims;
    for (const auto &d : g.dims)
        dims.push_back(&d);
    std::sort(dims.begin(), dims.end(),
              [](const MdGroup::Dim *a, const MdGroup::Dim *b)
              { return a->name < b->name; });
    w.addKey("dimensions");
    w.startArray();
    for (const auto *d : dims)
    {
        w.startObject();
        w.addKey("name");
        w.addString(d->name);
        w.addKey("full_name");
        w.addString(d->fullName);
        w.addKey("size");
        w.addRaw(strPrintf("%llu", (unsigned long long)d->size));
        if (!d->type.empty())
        {
            w.addKey("type");
            w.addString(d->type);
        }
        if (!d->direction.empty())
        {
            w.addKey("direction");
            w.addString(d->direction);
        }
        if (!d->indexingVar.empty())
        {
            const MdArray *iv = g.findArray(d->indexingVar);
            if (iv)
            {
                w.addKey("indexing_variable");
                w.startObject();
                w.addKey(iv->name);
                w.startObject();
                w.addKey("full_name");
                w.addString((g.fullName == "/" ? "/" : g.fullName + "/") +
                            iv->name);
                mdWriteArrayBody(w, ds, *iv, o, true);
                w.endObject();
                w.endObject();
            }
        }
        w.endObject();
    }
    w.endArray();
}

void mdWriteGroupBody(JsonStreamWriter &w, const MdDataset &ds,
                      const MdGroup &g, const InfoOpts &o)
{
    if (o.summary)
    {
        if (!g.arrays.empty())
        {
            w.addKey("arrays");
            w.startObject();
            for (const auto &a : g.arrays)
            {
                w.addKey(a.name);
                w.startObject();
                w.addKey("full_name");
                w.addString((g.fullName == "/" ? "/" : g.fullName + "/") +
                            a.name);
                w.endObject();
            }
            w.endObject();
        }
        if (!g.groups.empty())
        {
            w.addKey("groups");
            w.startObject();
            for (const auto &sub : g.groups)
            {
                w.addKey(sub.name);
                w.startObject();
                w.addKey("full_name");
                w.addString(sub.fullName);
                mdWriteGroupBody(w, ds, sub, o);
                w.endObject();
            }
            w.endObject();
        }
        return;
    }
    if (!g.attrs.empty())
        mdWriteAttributes(w, g.attrs, o.detailed);
    if (!g.dims.empty())
        mdWriteDimensions(w, ds, g, o);
    if (!g.arrays.empty())
    {
        w.addKey("arrays");
        w.startObject();
        for (const auto &a : g.arrays)
        {
            w.addKey(a.name);
            w.startObject();
            w.addKey("full_name");
            w.addString((g.fullName == "/" ? "/" : g.fullName + "/") +
                        a.name);
            mdWriteArrayBody(w, ds, a, o, false);
            w.endObject();
        }
        w.endObject();
    }
    if (!g.groups.empty())
    {
        w.addKey("groups");
        w.startObject();
        for (const auto &sub : g.groups)
        {
            w.addKey(sub.name);
            w.startObject();
            mdWriteGroupBody(w, ds, sub, o);
            w.endObject();
        }
        w.endObject();
    }
}

// find array + owning group by full path ("temp" resolves at root only)
bool mdFindArray(const MdGroup &root, const std::string &spec,
                 const MdGroup *&gOut, const MdArray *&aOut)
{
    std::vector<std::string> parts;
    for (const auto &t : strSplit(spec, '/'))
        if (!t.empty())
            parts.push_back(t);
    const MdGroup *g = &root;
    if (parts.empty())
        return false;
    for (size_t i = 0; i + 1 < parts.size(); ++i)
    {
        const MdGroup *next = nullptr;
        for (const auto &sg : g->groups)
            if (sg.name == parts[i])
                next = &sg;
        if (!next)
            return false;
        g = next;
    }
    const MdArray *a = g->findArray(parts.back());
    if (!a)
        return false;
    gOut = g;
    aOut = a;
    return true;
}

std::string mdimInfoJson(const MdDataset &ds, const InfoOpts &o,
                         const std::string &arraySpec, bool &arrayMissing)
{
    JsonStreamWriter w;
    arrayMissing = false;
    if (!arraySpec.empty())
    {
        const MdGroup *g = nullptr;
        const MdArray *a = nullptr;
        if (!mdFindArray(ds.root, arraySpec, g, a))
        {
            arrayMissing = true;
            return "";
        }
        w.startObject();
        w.addKey("type");
        w.addString("array");
        w.addKey("name");
        w.addString(a->name);
        w.addKey("datatype");
        w.addString(a->dtype);
        // expanded dimension objects
        if (!a->dimFull.empty())
        {
        w.addKey("dimensions");
        w.startArray();
        for (size_t i = 0; i < a->dimFull.size(); ++i)
        {
            const MdGroup::Dim *d = nullptr;
            for (const auto &dd : g->dims)
                if (dd.fullName == a->dimFull[i])
                    d = &dd;
            w.startObject();
            if (d)
            {
                w.addKey("name");
                w.addString(d->name);
                w.addKey("full_name");
                w.addString(d->fullName);
                w.addKey("size");
                w.addRaw(strPrintf("%llu", (unsigned long long)d->size));
                if (!d->type.empty())
                {
                    w.addKey("type");
                    w.addString(d->type);
                }
                if (!d->direction.empty())
                {
                    w.addKey("direction");
                    w.addString(d->direction);
                }
                if (!d->indexingVar.empty())
                {
                    const MdArray *iv = g->findArray(d->indexingVar);
                    if (iv)
                    {
                        w.addKey("indexing_variable");
                        w.startObject();
                        w.addKey(iv->name);
                        w.startObject();
                        w.addKey("full_name");
                        w.addString(
                            (g->fullName == "/" ? "/" : g->fullName + "/") +
                            iv->name);
                        mdWriteArrayBody(w, ds, *iv, o, true);
                        w.endObject();
                        w.endObject();
                    }
                }
            }
            w.endObject();
        }
        w.endArray();
        w.addKey("dimension_size");
        w.startArray();
        for (uint64_t s : a->dimSizes)
            w.addRaw(strPrintf("%llu", (unsigned long long)s));
        w.endArray();
        }
        if (!a->attrs.empty())
            mdWriteAttributes(w, a->attrs, o.detailed);
        if (!a->unit.empty())
        {
            w.addKey("unit");
            w.addString(a->unit);
        }
        if (a->hasNodata)
            mdWriteNoData(w, *a);
        if (a->hasOffset)
        {
            w.addKey("offset");
            w.addDouble(a->offset);
        }
        if (a->hasScale)
        {
            w.addKey("scale");
            w.addDouble(a->scale);
        }
        if (a->hasSrs)
        {
            w.addKey("srs");
            w.startObject();
            w.addKey("wkt");
            bool ok = false;
            Srs srs = Srs::fromUserInput(a->srsWkt, ok);
            w.addString(ok ? srs.wkt2SingleLine() : a->srsWkt);
            w.addKey("data_axis_to_srs_axis_mapping");
            bool prev = w.setNewline(false);
            w.startArray();
            for (int m : a->axisMapping)
                w.addInt(m);
            w.endArray();
            w.setNewline(prev);
            w.endObject();
        }
        if (o.detailed)
        {
            std::vector<MdVal> vals;
            std::string err;
            if (mdResolveValues(ds, *a, vals, err))
                mdWriteValues(w, *a, vals, o.limit);
        }
        if (o.stats)
        {
            std::vector<MdVal> vals;
            std::string err;
            MdStats st;
            if (mdResolveValues(ds, *a, vals, err) &&
                mdComputeStats(*a, vals, st))
                mdWriteStats(w, st);
        }
        w.endObject();
        return w.result();
    }
    w.startObject();
    w.addKey("type");
    w.addString("group");
    w.addKey("driver");
    w.addString("VRT");
    w.addKey("name");
    w.addString("/");
    mdWriteGroupBody(w, ds, ds.root, o);
    w.endObject();
    return w.result();
}

// ------------------------------------------------------------------
// handlers
// ------------------------------------------------------------------

int mdimInfoHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    const std::string input = r.str("input");
    MdDataset ds;
    std::string err;
    bool notVrt = false;
    if (!mdimOpen(input, ds, err, notVrt))
    {
        // the validation pass already reported this
        return 1;
    }
    InfoOpts o;
    o.summary = r.flag("summary");
    o.detailed = r.flag("detailed");
    o.stats = r.flag("stats");
    o.limit = 0;
    if (r.get("limit"))
        o.limit = atoll(r.str("limit").c_str());
    bool arrayMissing = false;
    const std::string arraySpec = r.str("array");
    std::string json = mdimInfoJson(ds, o, arraySpec, arrayMissing);
    if (arrayMissing)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Cannot find array " + arraySpec);
        return 1;
    }
    fputs((json + "\n").c_str(), stdout);
    return 0;
}

// ------------------------------------------------------------------
// canonical multidim VRT serializer (mdim convert / mosaic output)
// ------------------------------------------------------------------

std::string mdXmlFormat(const std::string &dtype, const MdVal &v)
{
    if (mdIsString(dtype))
        return v.s;
    if (dtype == "Int64")
        return strPrintf("%lld", v.ll);
    if (dtype == "UInt64")
        return strPrintf("%llu", v.ull);
    if (mdIsInteger(dtype))
        return strPrintf("%lld", v.ll);
    if (dtype == "Float32")
        return strPrintf("%.9g", v.d);
    return strPrintf("%.17g", v.d);
}

std::string mdJoinUInt(const std::vector<uint64_t> &v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i)
            out += ",";
        out += strPrintf("%llu", (unsigned long long)v[i]);
    }
    return out;
}

void mdSerializeAttr(std::string &out, const std::string &ind,
                     const MdAttr &a)
{
    out += ind + "<Attribute name=\"" + xmlEsc(a.name, true) + "\">\n";
    out += ind + "  <DataType>" + xmlEsc(a.dtype) + "</DataType>\n";
    for (const auto &v : a.values)
        out += ind + "  <Value>" +
               xmlEsc(mdXmlFormat(a.dtype, mdParseToken(a.dtype, v))) +
               "</Value>\n";
    out += ind + "</Attribute>\n";
}

void mdSerializeGroup(std::string &out, const MdGroup &g, int depth)
{
    std::string ind(depth * 2, ' ');
    const bool empty = g.dims.empty() && g.attrs.empty() &&
                       g.arrays.empty() && g.groups.empty();
    const std::string nameAttr =
        g.fullName == "/" ? "/" : g.name;
    if (empty)
    {
        out += ind + "<Group name=\"" + xmlEsc(nameAttr, true) + "\" />\n";
        return;
    }
    out += ind + "<Group name=\"" + xmlEsc(nameAttr, true) + "\">\n";
    const std::string ind2 = ind + "  ";
    std::vector<const MdGroup::Dim *> dims;
    for (const auto &d : g.dims)
        dims.push_back(&d);
    std::sort(dims.begin(), dims.end(),
              [](const MdGroup::Dim *a, const MdGroup::Dim *b)
              { return a->name < b->name; });
    for (const auto *d : dims)
    {
        out += ind2 + "<Dimension name=\"" + xmlEsc(d->name, true) + "\"";
        if (!d->type.empty())
            out += " type=\"" + xmlEsc(d->type, true) + "\"";
        if (!d->direction.empty())
            out += " direction=\"" + xmlEsc(d->direction, true) + "\"";
        out += strPrintf(" size=\"%llu\"", (unsigned long long)d->size);
        if (!d->indexingVar.empty())
            out += " indexingVariable=\"" + xmlEsc(d->indexingVar, true) +
                   "\"";
        out += " />\n";
    }
    std::vector<const MdAttr *> attrs;
    for (const auto &a : g.attrs)
        attrs.push_back(&a);
    std::sort(attrs.begin(), attrs.end(),
              [](const MdAttr *a, const MdAttr *b)
              { return a->name < b->name; });
    for (const auto *a : attrs)
        mdSerializeAttr(out, ind2, *a);
    for (const auto &a : g.arrays)
    {
        out += ind2 + "<Array name=\"" + xmlEsc(a.name, true) + "\">\n";
        const std::string ind3 = ind2 + "  ";
        out += ind3 + "<DataType>" + xmlEsc(a.dtype) + "</DataType>\n";
        for (size_t i = 0; i < a.dimRefs.size(); ++i)
        {
            // local refs echo the dim name; absolute stay absolute
            std::string ref = a.dimRefs[i];
            out += ind3 + "<DimensionRef ref=\"" + xmlEsc(ref, true) +
                   "\" />\n";
        }
        if (a.hasSrs)
        {
            bool ok = false;
            Srs srs = Srs::fromUserInput(a.srsWkt, ok);
            std::string mapping;
            for (size_t i = 0; i < a.axisMapping.size(); ++i)
                mapping += (i ? "," : "") +
                           strPrintf("%d", a.axisMapping[i]);
            out += ind3 + "<SRS dataAxisToSRSAxisMapping=\"" + mapping +
                   "\">" +
                   xmlEsc(ok ? srs.wkt2SingleLine() : a.srsWkt) +
                   "</SRS>\n";
        }
        if (!a.unit.empty())
            out += ind3 + "<Unit>" + xmlEsc(a.unit) + "</Unit>\n";
        if (a.hasNodata)
            out += ind3 + "<NoDataValue>" +
                   xmlEsc(mdXmlFormat(a.dtype,
                                      mdParseToken(a.dtype, a.nodataRaw))) +
                   "</NoDataValue>\n";
        if (a.hasOffset)
            out += ind3 + "<Offset>" + strPrintf("%.17g", a.offset) +
                   "</Offset>\n";
        if (a.hasScale)
            out += ind3 + "<Scale>" + strPrintf("%.17g", a.scale) +
                   "</Scale>\n";
        const std::string off =
            a.hasInlineOffset ? mdJoinUInt(a.inlineOffset)
                              : [&]
                                {
                                    std::vector<uint64_t> z(
                                        a.dimSizes.size(), 0);
                                    return mdJoinUInt(z);
                                }();
        const std::string cnt = a.hasInlineOffset
                                    ? mdJoinUInt(a.inlineCount)
                                    : mdJoinUInt(a.dimSizes);
        switch (a.kind)
        {
            case MdArray::Regular:
                out += ind3 + "<RegularlySpacedValues start=\"" +
                       strPrintf("%.17g", a.regStart) + "\" increment=\"" +
                       strPrintf("%.17g", a.regIncrement) + "\" />\n";
                break;
            case MdArray::Inline:
            {
                out += ind3 + "<InlineValues offset=\"" + off +
                       "\" count=\"" + cnt + "\">";
                for (size_t i = 0; i < a.tokens.size(); ++i)
                    out += (i ? " " : "") +
                           xmlEsc(mdXmlFormat(
                               a.dtype, mdParseToken(a.dtype, a.tokens[i])));
                out += "</InlineValues>\n";
                break;
            }
            case MdArray::InlineValueElements:
            {
                out += ind3 + "<InlineValuesWithValueElement offset=\"" +
                       off + "\" count=\"" + cnt + "\">\n";
                for (const auto &t : a.tokens)
                    out += ind3 + "  <Value>" + xmlEsc(t) + "</Value>\n";
                out += ind3 + "</InlineValuesWithValueElement>\n";
                break;
            }
            case MdArray::Constant:
                out += ind3 + "<ConstantValue offset=\"" + off +
                       "\" count=\"" + cnt + "\">" +
                       xmlEsc(mdXmlFormat(
                           a.dtype, mdParseToken(a.dtype, a.constRaw))) +
                       "</ConstantValue>\n";
                break;
            case MdArray::Sourced:
                for (const auto &s : a.sources)
                {
                    out += ind3 + "<Source>\n";
                    out += ind3 + "  <SourceFilename";
                    if (s.relative)
                        out += " relativetoVRT=\"1\"";
                    out += ">" + xmlEsc(s.filename) + "</SourceFilename>\n";
                    out += ind3 + "  <SourceArray>" + xmlEsc(s.arrayFull) +
                           "</SourceArray>\n";
                    if (!s.view.empty())
                        out += ind3 + "  <SourceView>" + xmlEsc(s.view) +
                               "</SourceView>\n";
                    if (!s.transpose.empty())
                    {
                        std::string tr;
                        for (size_t i = 0; i < s.transpose.size(); ++i)
                            tr += (i ? "," : "") +
                                  strPrintf("%d", s.transpose[i]);
                        out += ind3 + "  <SourceTranspose>" + tr +
                               "</SourceTranspose>\n";
                    }
                    if (s.hasSlab)
                        out += ind3 + "  <SourceSlab offset=\"" +
                               mdJoinUInt(s.srcOffset) + "\" count=\"" +
                               mdJoinUInt(s.srcCount) + "\" step=\"" +
                               mdJoinUInt(s.srcStep) + "\" />\n";
                    if (!s.dstOffset.empty())
                        out += ind3 + "  <DestSlab offset=\"" +
                               mdJoinUInt(s.dstOffset) + "\" />\n";
                    out += ind3 + "</Source>\n";
                }
                break;
            case MdArray::None:
                break;
        }
        std::vector<const MdAttr *> aattrs;
        for (const auto &at : a.attrs)
            aattrs.push_back(&at);
        std::sort(aattrs.begin(), aattrs.end(),
                  [](const MdAttr *x, const MdAttr *y)
                  { return x->name < y->name; });
        for (const auto *at : aattrs)
            mdSerializeAttr(out, ind3, *at);
        out += ind2 + "</Array>\n";
    }
    for (const auto &sub : g.groups)
        mdSerializeGroup(out, sub, depth + 1);
    out += ind + "</Group>\n";
}

std::string mdimSerializeVrt(const MdGroup &root)
{
    std::string out = "<VRTDataset>\n";
    mdSerializeGroup(out, root, 1);
    out += "</VRTDataset>\n";
    return out;
}

// ------------------------------------------------------------------
// selection (reference-mode output): --array/--group/--subset/--scale-axes
// ------------------------------------------------------------------

struct MdArraySel
{
    std::string name;
    std::string view;  // verbatim user text, e.g. "[::2,1:3]"
    std::vector<int> transpose;
};

bool mdParseArraySpec(const std::string &spec, MdArraySel &sel,
                      std::string &err)
{
    if (spec.find("name=") == std::string::npos)
    {
        sel.name = spec;
        return true;
    }
    // name=...,view=[...],transpose=[...] - values may hold commas inside
    // brackets
    size_t i = 0;
    while (i < spec.size())
    {
        size_t eq = spec.find('=', i);
        if (eq == std::string::npos)
            break;
        std::string key = spec.substr(i, eq - i);
        size_t j = eq + 1;
        int depth = 0;
        while (j < spec.size() && (depth > 0 || spec[j] != ','))
        {
            if (spec[j] == '[')
                ++depth;
            else if (spec[j] == ']')
                --depth;
            ++j;
        }
        std::string val = spec.substr(eq + 1, j - (eq + 1));
        if (key == "name")
            sel.name = val;
        else if (key == "view")
            sel.view = val;
        else if (key == "transpose")
        {
            std::string t = val;
            if (t.size() >= 2 && t.front() == '[' && t.back() == ']')
                t = t.substr(1, t.size() - 2);
            for (const auto &x : strSplit(t, ','))
                sel.transpose.push_back(atoi(x.c_str()));
        }
        else
        {
            err = "Unexpected array specification part: " + key + "=" + val;
            return false;
        }
        i = j + (j < spec.size() ? 1 : 0);
    }
    return true;
}

// per-dimension operation applied while building the reference output
struct MdDimOp
{
    int kind = 0;  // 0 keep, 1 slice, 2 single index
    bool rename = false;
    uint64_t start = 0, step = 1, size = 0, index = 0;
    std::string valueTok, valueType;  // DIM_<d>_VALUE payload
};

struct MdSubsetSpec
{
    bool single = false;
    double lo = 0, hi = 0;
};

std::string mdCanonicalOp(const MdDimOp &op)
{
    if (op.kind == 2)
        return strPrintf("%llu", (unsigned long long)op.index);
    return strPrintf("%llu:%llu:%llu", (unsigned long long)op.start,
                     (unsigned long long)(op.start + op.size * op.step),
                     (unsigned long long)op.step);
}

MdDimOp mdOpFromSpec(const std::string &sp, uint64_t dimSize)
{
    MdDimOp op;
    op.size = dimSize;
    if (sp == ":" || sp.empty())
        return op;
    if (sp.find(':') == std::string::npos)
    {
        op.kind = 2;
        op.index = strtoull(sp.c_str(), nullptr, 10);
        return op;
    }
    op.kind = 1;
    op.rename = true;
    std::vector<std::string> f = strSplit(sp, ':');
    while (f.size() < 3)
        f.push_back("");
    op.start = !f[0].empty() ? strtoull(f[0].c_str(), nullptr, 10) : 0;
    const uint64_t stop =
        !f[1].empty() ? strtoull(f[1].c_str(), nullptr, 10) : dimSize;
    op.step = !f[2].empty() ? strtoull(f[2].c_str(), nullptr, 10) : 1;
    op.size = stop > op.start ? (stop - op.start + op.step - 1) / op.step
                              : 0;
    return op;
}

struct MdSelCtx
{
    const MdDataset *ds = nullptr;
    std::map<std::string, MdSubsetSpec> subs;  // by dimension name
    std::map<std::string, uint64_t> scales;    // by dimension name
    std::map<std::string, MdDimOp> ops;        // memoized by full name
    std::string err;
};

bool mdBindDimOp(MdSelCtx &c, const MdGroup &g, const MdGroup::Dim &d,
                 MdDimOp &out)
{
    auto memo = c.ops.find(d.fullName);
    if (memo != c.ops.end())
    {
        out = memo->second;
        return true;
    }
    MdDimOp op;
    op.size = d.size;
    auto su = c.subs.find(d.name);
    if (su != c.subs.end())
    {
        const MdArray *iv =
            d.indexingVar.empty() ? nullptr : g.findArray(d.indexingVar);
        if (!iv || iv->dimSizes.size() != 1)
        {
            c.err = "Dimension " + d.name +
                    " has a subset specification, but lacks a single "
                    "dimension indexing variable";
            return false;
        }
        std::vector<MdVal> vals;
        if (!mdResolveValues(*c.ds, *iv, vals, c.err))
            return false;
        if (su->second.single)
        {
            bool found = false;
            for (size_t i = 0; i < vals.size() && !found; ++i)
                if (vals[i].d == su->second.lo)
                {
                    op.kind = 2;
                    op.index = i;
                    op.valueType = iv->dtype;
                    op.valueTok = mdXmlFormat(iv->dtype, vals[i]);
                    found = true;
                }
            if (!found)
            {
                c.err = "Subset specification results in an empty set";
                return false;
            }
        }
        else
        {
            const double lo = std::min(su->second.lo, su->second.hi);
            const double hi = std::max(su->second.lo, su->second.hi);
            uint64_t first = 0, last = 0;
            bool any = false;
            for (size_t i = 0; i < vals.size(); ++i)
                if (vals[i].d >= lo && vals[i].d <= hi)
                {
                    if (!any)
                        first = i;
                    last = i;
                    any = true;
                }
            if (!any)
            {
                c.err = "Subset specification results in an empty set";
                return false;
            }
            op.kind = 1;
            op.start = first;
            op.step = 1;
            op.size = last - first + 1;
        }
    }
    else
    {
        auto sc = c.scales.find(d.name);
        if (sc != c.scales.end())
        {
            const uint64_t f = sc->second;
            uint64_t ns = d.size / f;
            if (ns == 0)
                ns = 1;
            op.kind = 1;
            op.start = 0;
            op.step = f;
            op.size = ns;
        }
    }
    c.ops[d.fullName] = op;
    out = op;
    return true;
}

void mdInjectDimAttrs(MdArray &a, const std::string &dimName,
                      const MdDimOp &op)
{
    MdAttr ai;
    ai.name = "DIM_" + dimName + "_INDEX";
    ai.dtype = "Int32";
    ai.values = {strPrintf("%llu", (unsigned long long)op.index)};
    a.attrs.push_back(ai);
    if (!op.valueType.empty())
    {
        MdAttr av;
        av.name = "DIM_" + dimName + "_VALUE";
        av.dtype = op.valueType;
        av.values = {op.valueTok};
        a.attrs.push_back(av);
    }
}

std::string mdLastPathComp(const std::string &full)
{
    size_t p = full.find_last_of('/');
    return p == std::string::npos ? full : full.substr(p + 1);
}

void mdCopySourcedProps(MdArray &oa, const MdArray &a)
{
    oa.name = a.name;
    oa.dtype = a.dtype;
    oa.unit = a.unit;
    oa.hasNodata = a.hasNodata;
    oa.nodataRaw = a.nodataRaw;
    oa.hasOffset = a.hasOffset;
    oa.offset = a.offset;
    oa.hasScale = a.hasScale;
    oa.scale = a.scale;
    oa.hasSrs = a.hasSrs;
    oa.srsWkt = a.srsWkt;
    oa.axisMapping = a.axisMapping;
    oa.attrs = a.attrs;
    oa.kind = MdArray::Sourced;
}

void mdFinishSource(MdArray &oa, const MdSelCtx &c,
                    const std::string &arrayFull, const std::string &view,
                    const std::vector<int> &transpose)
{
    MdSource s;
    s.filename = c.ds->path;
    s.relative = !c.ds->path.empty() && c.ds->path[0] != '/';
    s.arrayFull = arrayFull;
    s.view = view;
    s.transpose = transpose;
    if (!oa.dimSizes.empty())
    {
        s.hasSlab = true;
        s.srcOffset.assign(oa.dimSizes.size(), 0);
        s.srcCount = oa.dimSizes;
        s.srcStep.assign(oa.dimSizes.size(), 1);
        s.dstOffset.assign(oa.dimSizes.size(), 0);
    }
    oa.sources.push_back(s);
}

// full re-sourced copy of a group tree (used when subset/scale-axes or
// --group narrow the output without --array)
bool mdCopyGroupSel(MdSelCtx &c, const MdGroup &src, MdGroup &out,
                    const std::string &prefix)
{
    out.name = src.name;
    out.fullName = src.fullName;
    out.attrs = src.attrs;
    for (const auto &d : src.dims)
    {
        MdDimOp op;
        if (!mdBindDimOp(c, src, d, op))
            return false;
        if (op.kind == 2)
            continue;
        MdGroup::Dim nd = d;
        if (op.kind == 1)
            nd.size = op.size;
        out.dims.push_back(nd);
    }
    for (const auto &a : src.arrays)
    {
        MdArray oa;
        mdCopySourcedProps(oa, a);
        std::string view;
        bool anyOp = false;
        std::vector<std::string> parts;
        for (size_t k = 0; k < a.dimSizes.size(); ++k)
        {
            MdDimOp op;
            auto it = c.ops.find(a.dimFull[k]);
            if (it != c.ops.end())
                op = it->second;
            else
                op.size = a.dimSizes[k];
            parts.push_back(op.kind ? mdCanonicalOp(op) : ":");
            if (op.kind)
                anyOp = true;
            if (op.kind == 2)
            {
                mdInjectDimAttrs(oa, mdLastPathComp(a.dimFull[k]), op);
                continue;
            }
            oa.dimRefs.push_back(mdLastPathComp(a.dimFull[k]));
            oa.dimFull.push_back(a.dimFull[k]);
            oa.dimSizes.push_back(op.kind == 1 ? op.size : a.dimSizes[k]);
        }
        if (anyOp)
        {
            view = "[";
            for (size_t i = 0; i < parts.size(); ++i)
                view += (i ? "," : "") + parts[i];
            view += "]";
        }
        mdFinishSource(oa, c, prefix + "/" + a.name, view, {});
        out.arrays.push_back(oa);
    }
    for (const auto &sg : src.groups)
    {
        MdGroup og;
        if (!mdCopyGroupSel(c, sg, og, prefix + "/" + sg.name))
            return false;
        out.groups.push_back(og);
    }
    return true;
}

bool mdimBuildSelection(const MdDataset &ds, ParseResult &r,
                        MdGroup &outRoot, std::string &err)
{
    MdSelCtx c;
    c.ds = &ds;
    for (const auto &sub : r.list("subset"))
    {
        size_t p = sub.find('(');
        if (p == std::string::npos || sub.back() != ')')
            continue;  // malformed specs are silently ignored
        std::vector<std::string> bounds =
            strSplit(sub.substr(p + 1, sub.size() - p - 2), ',');
        if (bounds.size() > 2)
        {
            err = "Invalid number of values in subset specification.";
            return false;
        }
        MdSubsetSpec sp;
        sp.single = bounds.size() < 2;
        for (size_t i = 0; i < bounds.size(); ++i)
        {
            char *endp = nullptr;
            const double v = strtod(bounds[i].c_str(), &endp);
            if (endp == bounds[i].c_str() || *endp != '\0')
            {
                err = "Non numeric bound in subset specification.";
                return false;
            }
            (i == 0 ? sp.lo : sp.hi) = v;
        }
        if (sp.single)
            sp.hi = sp.lo;
        c.subs[sub.substr(0, p)] = sp;
    }
    for (const auto &sa : r.list("scale-axes"))
    {
        size_t p = sa.find('(');
        if (p == std::string::npos || sa.back() != ')')
            continue;
        const std::string tok = sa.substr(p + 1, sa.size() - p - 2);
        char *endp = nullptr;
        const long long f = strtoll(tok.c_str(), &endp, 10);
        if (endp == tok.c_str() || *endp != '\0' || f <= 0)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "Only positive integer scale factor is supported");
            err = "reported";
            return false;
        }
        c.scales[sa.substr(0, p)] = (uint64_t)f;
    }

    const MdGroup *srcGroup = &ds.root;
    std::string groupPrefix;
    if (r.get("group"))
    {
        const auto gv = r.list("group");
        if (!gv.empty())
        {
            const MdGroup *g = &ds.root;
            std::string full;
            for (const auto &part : strSplit(gv[0], '/'))
            {
                if (part.empty())
                    continue;
                const MdGroup *next = nullptr;
                for (const auto &sg : g->groups)
                    if (sg.name == part)
                        next = &sg;
                if (!next)
                {
                    err = "Cannot find group " + gv[0];
                    return false;
                }
                g = next;
                full += "/" + part;
            }
            srcGroup = g;
            groupPrefix = full;
        }
    }

    outRoot = MdGroup();
    outRoot.name = "/";
    outRoot.fullName = "/";

    if (!r.get("array"))
    {
        if (!mdCopyGroupSel(c, *srcGroup, outRoot, groupPrefix))
        {
            err = c.err;
            return false;
        }
        outRoot.name = "/";
        outRoot.fullName = "/";
        return true;
    }

    outRoot.attrs = srcGroup->attrs;

    std::vector<MdArraySel> sels;
    for (const auto &spec : r.list("array"))
    {
        MdArraySel s;
        if (!mdParseArraySpec(spec, s, err))
            return false;
        sels.push_back(s);
    }

    auto ensureDim = [&](const MdGroup::Dim &src, const MdDimOp &op)
        -> std::string
    {
        std::string name = src.name;
        if (op.kind == 1 && op.rename)
            name = strPrintf("subset_%s_%llu_%llu_%llu", src.name.c_str(),
                             (unsigned long long)op.start,
                             (unsigned long long)op.step,
                             (unsigned long long)op.size);
        for (const auto &d : outRoot.dims)
            if (d.name == name)
                return name;
        MdGroup::Dim d;
        d.name = name;
        d.fullName = "/" + name;
        d.type = src.type;
        d.direction = src.direction;
        d.size = op.kind == 1 ? op.size : src.size;
        if (!src.indexingVar.empty())
            d.indexingVar = name == src.name ? src.indexingVar : name;
        outRoot.dims.push_back(d);
        return name;
    };

    auto addIndexingArray = [&](const MdArray &src, const std::string &nm,
                                const MdDimOp &op,
                                const std::string &srcFull)
    {
        for (const auto &a : outRoot.arrays)
            if (a.name == nm)
                return;
        MdArray a;
        mdCopySourcedProps(a, src);
        a.name = nm;
        a.dimRefs = {nm};
        a.dimFull = {"/" + nm};
        a.dimSizes = {op.kind == 1 ? op.size : src.dimSizes[0]};
        mdFinishSource(a, c, srcFull,
                       op.kind == 1 ? "[" + mdCanonicalOp(op) + "]" : "",
                       {});
        outRoot.arrays.push_back(a);
    };

    for (const auto &sel : sels)
    {
        const MdGroup *ag = srcGroup;
        std::string aprefix = groupPrefix;
        std::string leaf = sel.name;
        if (sel.name.find('/') != std::string::npos)
        {
            if (sel.name.empty() || sel.name[0] != '/')
            {
                err = "Cannot find array " + sel.name;
                return false;
            }
            std::vector<std::string> pcs;
            for (const auto &t : strSplit(sel.name, '/'))
                if (!t.empty())
                    pcs.push_back(t);
            for (size_t i = 0; i + 1 < pcs.size(); ++i)
            {
                const MdGroup *next = nullptr;
                for (const auto &sg : ag->groups)
                    if (sg.name == pcs[i])
                        next = &sg;
                if (!next)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "Cannot find group " + pcs[i]);
                    err = "Cannot find array " + sel.name;
                    return false;
                }
                ag = next;
                aprefix += "/" + pcs[i];
            }
            leaf = pcs.empty() ? "" : pcs.back();
        }
        const MdArray *src = ag->findArray(leaf);
        if (!src)
        {
            err = "Cannot find array " + sel.name;
            return false;
        }

        std::vector<std::string> exps(src->dimSizes.size(), ":");
        const bool hasExplicit = !sel.view.empty();
        if (hasExplicit)
        {
            std::string v = sel.view;
            if (v.size() >= 2 && v.front() == '[' && v.back() == ']')
                v = v.substr(1, v.size() - 2);
            auto specs = strSplit(v, ',');
            for (size_t i = 0; i < specs.size() && i < exps.size(); ++i)
                exps[i] = specs[i];
        }
        std::vector<MdDimOp> ops(src->dimSizes.size());
        std::vector<const MdGroup::Dim *> sdims(src->dimSizes.size(),
                                                nullptr);
        bool anyImplicit = false;
        for (size_t k = 0; k < src->dimSizes.size(); ++k)
        {
            for (const auto &d : ag->dims)
                if (d.fullName == src->dimFull[k])
                    sdims[k] = &d;
            if (exps[k] != ":")
            {
                ops[k] = mdOpFromSpec(exps[k], src->dimSizes[k]);
                if (ops[k].kind == 2 && sdims[k] &&
                    !sdims[k]->indexingVar.empty())
                {
                    const MdArray *iv =
                        ag->findArray(sdims[k]->indexingVar);
                    std::vector<MdVal> vals;
                    std::string e2;
                    if (iv && iv->dimSizes.size() == 1 &&
                        mdResolveValues(ds, *iv, vals, e2) &&
                        ops[k].index < vals.size())
                    {
                        ops[k].valueType = iv->dtype;
                        ops[k].valueTok =
                            mdXmlFormat(iv->dtype, vals[ops[k].index]);
                    }
                }
            }
            else if (sdims[k])
            {
                if (!mdBindDimOp(c, *ag, *sdims[k], ops[k]))
                {
                    err = c.err;
                    return false;
                }
                if (ops[k].kind)
                    anyImplicit = true;
            }
            else
                ops[k].size = src->dimSizes[k];
        }

        std::vector<size_t> order;
        if (!sel.transpose.empty())
            for (int t : sel.transpose)
                order.push_back((size_t)t);
        else
            for (size_t i = 0; i < src->dimSizes.size(); ++i)
                order.push_back(i);

        MdArray outA;
        mdCopySourcedProps(outA, *src);
        for (size_t oi = 0; oi < order.size(); ++oi)
        {
            const size_t k = order[oi];
            if (k >= ops.size())
                continue;
            const MdDimOp &op = ops[k];
            if (op.kind == 2)
                continue;
            if (!sdims[k])
                continue;
            const std::string dn = ensureDim(*sdims[k], op);
            outA.dimRefs.push_back(dn);
            outA.dimFull.push_back("/" + dn);
            outA.dimSizes.push_back(op.kind == 1 ? op.size
                                                 : src->dimSizes[k]);
            if (!sdims[k]->indexingVar.empty())
            {
                const MdArray *iv = ag->findArray(sdims[k]->indexingVar);
                if (iv)
                    addIndexingArray(*iv, dn, op,
                                     aprefix + "/" + iv->name);
            }
        }
        for (size_t k = 0; k < ops.size(); ++k)
            if (ops[k].kind == 2)
                mdInjectDimAttrs(outA,
                                 sdims[k] ? sdims[k]->name
                                          : mdLastPathComp(src->dimFull[k]),
                                 ops[k]);

        std::string view;
        if (hasExplicit)
            view = sel.view;
        else if (anyImplicit)
        {
            view = "[";
            for (size_t k = 0; k < ops.size(); ++k)
                view += std::string(k ? "," : "") +
                        (ops[k].kind ? mdCanonicalOp(ops[k]) : ":");
            view += "]";
        }
        mdFinishSource(outA, c, aprefix + "/" + src->name, view,
                       sel.transpose);
        bool already = false;
        for (const auto &ea : outRoot.arrays)
            if (ea.name == outA.name)
                already = true;
        if (!already)
            outRoot.arrays.push_back(outA);
    }
    return true;
}

// ------------------------------------------------------------------
// classic bridges
// ------------------------------------------------------------------

DType mdDType(const std::string &t)
{
    if (t == "Byte")
        return DType::Byte;
    if (t == "Int8")
        return DType::Int8;
    if (t == "Int16")
        return DType::Int16;
    if (t == "UInt16")
        return DType::UInt16;
    if (t == "Int32")
        return DType::Int32;
    if (t == "UInt32")
        return DType::UInt32;
    if (t == "Int64")
        return DType::Int64;
    if (t == "UInt64")
        return DType::UInt64;
    if (t == "Float32")
        return DType::Float32;
    if (t == "Float16")
        return DType::Float16;
    return DType::Float64;
}

class MdimClassicDataset final : public RasterDatasetBase
{
  public:
    std::vector<std::vector<double>> data;

    bool readBand(int band, std::vector<double> &out) override
    {
        out = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        for (double &v : out)
            v = rasterFinishReal(v, t);
        return true;
    }

    bool readBandRaw(int band, std::vector<uint8_t> &out) override
    {
        const std::vector<double> &vals = data[(size_t)band - 1];
        DType t = bands[(size_t)band - 1].type;
        size_t sz = (size_t)dtypeSizeBytes(t);
        out.assign(vals.size() * sz, 0);
        for (size_t i = 0; i < vals.size(); ++i)
            rasterEncodeReal(t, out.data() + i * sz,
                             rasterFinishReal(vals[i], t), 0);
        return true;
    }
};

// classic raster input: gdalmdimtranslate falls back to the plain
// convert machinery (byte-identical to `gdal raster convert`)
int mdimClassicConvert(const CmdSpec &cmd, ParseResult &r)
{
    return rasterConvertHandlerEntry(cmd, r);
}

const MdArray *mdPickClassicArray(const MdGroup &root, std::string &err)
{
    std::vector<const MdArray *> candidates;
    for (const auto &a : root.arrays)
    {
        bool isIndexing = false;
        for (const auto &d : root.dims)
            if (d.indexingVar == a.name)
                isIndexing = true;
        if (!isIndexing)
            candidates.push_back(&a);
    }
    if (candidates.size() > 1)
    {
        err = "Several arrays exist. Select one for output to "
              "non-multidimensional driver";
        return nullptr;
    }
    if (candidates.empty())
    {
        err = "No arrays exist";
        return nullptr;
    }
    return candidates[0];
}

int mdimToClassic(const MdDataset &ds, const MdGroup &root, ParseResult &r)
{
    std::string err;
    const MdArray *a = mdPickClassicArray(root, err);
    if (!a)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, err);
        return 1;
    }
    if (a->dimSizes.size() < 2)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "Invalid iXDim and/or iYDim");
        return 1;
    }
    const size_t nd = a->dimSizes.size();
    const uint64_t w = a->dimSizes[nd - 1];
    const uint64_t h = a->dimSizes[nd - 2];
    uint64_t nBands = 1;
    for (size_t i = 0; i + 2 < nd; ++i)
        nBands *= a->dimSizes[i];
    std::vector<MdVal> vals;
    if (!mdResolveValues(ds, *a, vals, err))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined, err);
        return 1;
    }
    auto base = std::make_unique<MdimClassicDataset>();
    base->path = ds.path;
    base->driverShort = "VRT";
    base->driverLong = "Virtual Raster";
    base->width = (int)w;
    base->height = (int)h;
    // geotransform from regularly spaced indexing variables of the two
    // fastest dims
    const MdGroup::Dim *dy = nullptr, *dx = nullptr;
    for (const auto &d : root.dims)
    {
        if (d.fullName == a->dimFull[nd - 2])
            dy = &d;
        if (d.fullName == a->dimFull[nd - 1])
            dx = &d;
    }
    auto regSpacing = [&](const MdGroup::Dim *d, double &start,
                          double &step) -> bool
    {
        if (!d || d->indexingVar.empty())
            return false;
        const MdArray *iv = root.findArray(d->indexingVar);
        if (!iv || iv->dimSizes.size() != 1 || iv->dimSizes[0] < 2)
            return false;
        std::vector<MdVal> ivv;
        std::string e2;
        if (!mdResolveValues(ds, *iv, ivv, e2))
            return false;
        start = ivv[0].d;
        step = ivv[1].d - ivv[0].d;
        for (size_t i = 2; i < ivv.size(); ++i)
        {
            const double dd = ivv[i].d - ivv[i - 1].d;
            if (std::fabs(dd - step) > 1e-3 * std::fabs(step))
                return false;
        }
        return step != 0;
    };
    double xs = 0, xst = 0, ys = 0, yst = 0;
    if (regSpacing(dx, xs, xst) && regSpacing(dy, ys, yst))
    {
        base->hasGT = true;
        base->gt[0] = xs - xst / 2;
        base->gt[1] = xst;
        base->gt[2] = 0;
        base->gt[3] = ys - yst / 2;
        base->gt[4] = 0;
        base->gt[5] = yst;
    }
    if (a->hasSrs)
    {
        bool ok = false;
        Srs srs = Srs::fromUserInput(a->srsWkt, ok);
        if (ok)
        {
            base->srs = srs.clone();
            base->hasSrs = true;
        }
    }
    for (const auto &at : a->attrs)
    {
        std::string v;
        for (size_t i = 0; i < at.values.size(); ++i)
            v += (i ? "," : "") +
                 mdXmlFormat(at.dtype, mdParseToken(at.dtype, at.values[i]));
        base->setMd("", at.name, v);
    }
    const uint64_t bandPix = w * h;
    for (uint64_t b = 0; b < nBands; ++b)
    {
        Band bd;
        bd.index = (int)b + 1;
        bd.type = mdDType(a->dtype);
        bd.blockX = (int)w;
        bd.blockY = (int)h;
        bd.colorInterp = b == 0 ? "Gray" : "Undefined";
        if (nd > 2)
        {
            std::vector<uint64_t> bidx(nd - 2, 0);
            uint64_t rem = b;
            for (size_t i = nd - 2; i-- > 0;)
            {
                bidx[i] = rem % a->dimSizes[i];
                rem /= a->dimSizes[i];
            }
            for (size_t i = 0; i + 2 < nd; ++i)
            {
                const std::string dn = mdLastPathComp(a->dimFull[i]);
                bd.setMd("", "DIM_" + dn + "_INDEX",
                         strPrintf("%llu", (unsigned long long)bidx[i]));
                const MdGroup::Dim *dd = nullptr;
                for (const auto &d : root.dims)
                    if (d.fullName == a->dimFull[i])
                        dd = &d;
                const MdArray *iv = dd && !dd->indexingVar.empty()
                                        ? root.findArray(dd->indexingVar)
                                        : nullptr;
                if (iv && iv->dimSizes.size() == 1)
                {
                    std::vector<MdVal> ivv;
                    std::string e2;
                    if (mdResolveValues(ds, *iv, ivv, e2) &&
                        bidx[i] < ivv.size())
                        bd.setMd("", "DIM_" + dn + "_VALUE",
                                 mdXmlFormat(iv->dtype, ivv[bidx[i]]));
                }
            }
        }
        if (a->hasNodata)
        {
            bd.hasNodata = true;
            bd.nodata = strtod(a->nodataRaw.c_str(), nullptr);
        }
        if (a->hasOffset)
        {
            bd.hasOffset = true;
            bd.offset = a->offset;
        }
        if (a->hasScale)
        {
            bd.hasScale = true;
            bd.scale = a->scale;
        }
        if (!a->unit.empty())
            bd.unitType = a->unit;
        base->bands.push_back(bd);
        std::vector<double> px((size_t)bandPix, 0);
        for (uint64_t i = 0; i < bandPix; ++i)
            px[(size_t)i] = vals[(size_t)(b * bandPix + i)].d;
        base->data.push_back(std::move(px));
    }
    std::unique_ptr<RasterDatasetBase> up = std::move(base);
    return rasterConvertWriteOutput(up, r, r.str("input"), r.str("output"),
                                    r.flag("quiet"), r.flag("overwrite"),
                                    false, "", "", nullptr, nullptr);
}

// ------------------------------------------------------------------
// convert
// ------------------------------------------------------------------

int mdimConvertHandler(const CmdSpec &cmd, ParseResult &r)
{
    const std::string input = r.str("input");
    const std::string output = r.str("output");
    const std::string of = r.str("output-format");
    const bool quiet = r.flag("quiet");

    MdDataset ds;
    std::string err;
    bool notVrt = false;
    if (!mdimOpen(input, ds, err, notVrt))
    {
        if (!err.empty())
            return 1;  // reported at validation time
        // classic raster input: handled by the classic delegate
        return mdimClassicConvert(cmd, r);
    }

    std::string drv;
    if (!of.empty())
        drv = strEqualNoCase(of, "MEM") ? "MEM" : "VRT";
    else
    {
        size_t dot = output.find_last_of('.');
        std::string ext =
            dot == std::string::npos ? "" : output.substr(dot + 1);
        if (strEqualNoCase(ext, "vrt"))
            drv = "VRT";
        else if (strEqualNoCase(ext, "nc"))
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "Output driver `netCDF' not recognised or does "
                        "not support output file creation.");
            return 1;
        }
        else if (ext.empty() || strEqualNoCase(ext, "tif") ||
                 strEqualNoCase(ext, "tiff") ||
                 strEqualNoCase(ext, "gtiff"))
            drv = "CLASSIC";
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Cannot determine output driver for dataset "
                        "name '" +
                            output + "'");
            return 1;
        }
    }

    // MEM materialization still refuses an existing output path, with
    // its own message and no usage block
    if (drv == "MEM" && !r.flag("overwrite"))
    {
        struct stat outSt;
        if (stat(output.c_str(), &outSt) == 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "File '" + output +
                            "' already exists. Specify the --overwrite "
                            "option to overwrite it.");
            return 1;
        }
    }

    const bool selection = r.get("array") || r.get("group") ||
                           r.get("subset") || r.get("scale-axes");
    MdGroup outRoot;
    if (selection)
    {
        if (!mdimBuildSelection(ds, r, outRoot, err))
        {
            if (!err.empty() && err != "reported")
                cplErrorStr(CE_Failure, CPLE_AppDefined, err);
            return 1;
        }
    }
    else
        outRoot = ds.root;

    if (drv == "CLASSIC")
        return mdimToClassic(ds, outRoot, r);
    if (drv == "MEM")
    {
        // materialization only; nothing lands on disk
        if (!quiet)
        {
            TermProgress p;
            p.update(1.0);
        }
        return 0;
    }
    if (!writeStringToFile(output, mdimSerializeVrt(outRoot)))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Cannot create " + output);
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------
// mosaic
// ------------------------------------------------------------------

struct MdMosDimLabels
{
    bool regular = false;
    double inc = 0;
    std::vector<MdVal> vals;
};

int mdimMosaicHandler(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    const auto inputs = r.list("input");
    const std::string output = r.str("output");
    const std::string of = r.str("output-format");
    const bool quiet = r.flag("quiet");

    std::vector<MdDataset> dss(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        struct stat st;
        if (stat(inputs[i].c_str(), &st) != 0)
        {
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        inputs[i] + ": No such file or directory");
            return 1;
        }
        std::string err;
        bool notVrt = false;
        if (!mdimOpen(inputs[i], dss[i], err, notVrt))
        {
            if (!err.empty())
                cplErrorStr(CE_Failure, CPLE_AppDefined, err);
            else
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "`" + inputs[i] +
                                "' not recognized as being in a supported "
                                "file format.");
            return 1;
        }
    }

    std::string drv;
    if (!of.empty())
        drv = strEqualNoCase(of, "MEM") ? "MEM" : "VRT";
    else
    {
        size_t dot = output.find_last_of('.');
        std::string ext =
            dot == std::string::npos ? "" : output.substr(dot + 1);
        if (strEqualNoCase(ext, "vrt"))
            drv = "VRT";
        else
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "mosaic: Cannot guess driver for " + output);
            return 1;
        }
    }

    // arrays to mosaic, discovered from the first dataset
    std::vector<std::string> names;
    if (r.get("array"))
    {
        for (const auto &n : r.list("array"))
            names.push_back(!n.empty() && n[0] == '/' ? n.substr(1) : n);
    }
    else
    {
        for (const auto &a : dss[0].root.arrays)
            if (a.dimSizes.size() >= 2)
                names.push_back(a.name);
        if (names.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "mosaic: No array of dimension count >= 2 found "
                        "in dataset " +
                            inputs[0]);
            return 1;
        }
    }

    MdGroup outRoot;
    outRoot.name = "/";
    outRoot.fullName = "/";

    auto labelsFor = [&](const MdDataset &d, const MdGroup::Dim &dim,
                         MdMosDimLabels &out) -> bool
    {
        const MdArray *iv =
            dim.indexingVar.empty() ? nullptr
                                    : d.root.findArray(dim.indexingVar);
        if (!iv)
            return false;
        std::string e2;
        if (!mdResolveValues(d, *iv, out.vals, e2))
            return false;
        if (out.vals.size() >= 2)
        {
            const double d0 = out.vals[1].d - out.vals[0].d;
            bool reg = true;
            for (size_t i = 2; i < out.vals.size(); ++i)
                if (out.vals[i].d - out.vals[i - 1].d != d0)
                    reg = false;
            if (reg)
            {
                out.regular = true;
                out.inc = d0;
                return true;
            }
        }
        out.regular = false;
        return true;
    };

    struct DimAgg
    {
        bool hasIndex = false;
        bool regular = false;
        double inc = 0, startg = 0;
        uint64_t size = 0;
        std::vector<MdVal> vals;               // irregular labels
        std::string ivDtype;
        std::vector<int64_t> offsets;          // per dataset
    };

    for (const auto &name : names)
    {
        std::vector<const MdArray *> per(dss.size(), nullptr);
        for (size_t d = 0; d < dss.size(); ++d)
        {
            per[d] = dss[d].root.findArray(name);
            if (!per[d])
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "mosaic: Cannot find array /" + name +
                                " in dataset " + inputs[d]);
                return 1;
            }
            if (per[d]->dtype != per[0]->dtype)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "mosaic: Array " + name + " of dataset " +
                                inputs[d] +
                                " does not have the same data type as in "
                                "other datasets");
                return 1;
            }
        }
        const MdArray *fa = per[0];
        const size_t nd = fa->dimSizes.size();
        std::vector<DimAgg> aggs(nd);
        std::vector<const MdGroup::Dim *> fdims(nd, nullptr);
        for (size_t k = 0; k < nd; ++k)
        {
            for (const auto &dd : dss[0].root.dims)
                if (dd.fullName == fa->dimFull[k])
                    fdims[k] = &dd;
            DimAgg &ag = aggs[k];
            ag.offsets.assign(dss.size(), 0);
            const std::string dimName =
                fdims[k] ? fdims[k]->name : mdLastPathComp(fa->dimFull[k]);
            MdMosDimLabels first;
            const bool hasIdx =
                fdims[k] && labelsFor(dss[0], *fdims[k], first);
            if (!hasIdx)
            {
                // unindexed dims must agree in size across datasets
                for (size_t d = 1; d < dss.size(); ++d)
                    if (per[d]->dimSizes[k] != fa->dimSizes[k])
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "mosaic: Dimension " + dimName + " of array " +
                                name + " of dataset " + inputs[d] +
                                " does not have the same size as in other "
                                "datasets");
                        return 1;
                    }
                ag.size = fa->dimSizes[k];
                continue;
            }
            ag.hasIndex = true;
            const MdArray *fiv = dss[0].root.findArray(
                fdims[k]->indexingVar);
            ag.ivDtype = fiv ? fiv->dtype : "Float64";
            ag.regular = first.regular;
            ag.inc = first.inc;
            ag.vals = first.vals;
            if (ag.regular)
            {
                double startg = first.vals.empty() ? 0 : first.vals[0].d;
                double endg =
                    first.vals.empty() ? 0 : first.vals.back().d;
                std::vector<double> starts(dss.size(),
                                           first.vals.empty()
                                               ? 0
                                               : first.vals[0].d);
                for (size_t d = 1; d < dss.size(); ++d)
                {
                    const MdGroup::Dim *dd2 = nullptr;
                    for (const auto &dd : dss[d].root.dims)
                        if (dd.name == dimName)
                            dd2 = &dd;
                    MdMosDimLabels cur;
                    if (!dd2 || !labelsFor(dss[d], *dd2, cur) ||
                        !cur.regular)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "mosaic: Dimension " + dimName + " of array " +
                                name + " of dataset " + inputs[d] +
                                " has irregularly-spaced values, contrary "
                                "to other datasets");
                        return 1;
                    }
                    if (cur.inc != ag.inc)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "mosaic: Dimension " + dimName + " of array " +
                                name + " of dataset " + inputs[d] +
                                " is indexed by a variable with spacing " +
                                strPrintf("%g", cur.inc) +
                                ", whereas it is " +
                                strPrintf("%g", ag.inc) +
                                " in other datasets");
                        return 1;
                    }
                    if (!cur.vals.empty())
                    {
                        starts[d] = cur.vals[0].d;
                        startg = std::min(startg, cur.vals[0].d);
                        endg = std::max(endg, cur.vals.back().d);
                    }
                }
                ag.startg = startg;
                ag.size =
                    (uint64_t)((int64_t)((endg - startg) / ag.inc) + 1);
                for (size_t d = 0; d < dss.size(); ++d)
                    ag.offsets[d] = (int64_t)(
                        (starts[d] - startg) / ag.inc + 0.5);
            }
            else
            {
                for (size_t d = 1; d < dss.size(); ++d)
                {
                    const MdGroup::Dim *dd2 = nullptr;
                    for (const auto &dd : dss[d].root.dims)
                        if (dd.name == dimName)
                            dd2 = &dd;
                    MdMosDimLabels cur;
                    if (!dd2 || !labelsFor(dss[d], *dd2, cur))
                        continue;
                    if (cur.regular)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "mosaic: Dimension " + dimName + " of array " +
                                name + " of dataset " + inputs[d] +
                                " has regularly spaced labels, contrary "
                                "to other datasets");
                        return 1;
                    }
                    bool same = cur.vals.size() == ag.vals.size();
                    for (size_t i = 0; same && i < cur.vals.size(); ++i)
                        if (cur.vals[i].d != ag.vals[i].d)
                            same = false;
                    if (!same)
                    {
                        cplErrorStr(
                            CE_Failure, CPLE_AppDefined,
                            "mosaic: Dataset " + inputs[d] +
                                ": values in indexing variable " +
                                fdims[k]->indexingVar + " of dimension " +
                                dimName +
                                " are not the same as in other datasets");
                        return 1;
                    }
                }
                ag.size = fa->dimSizes[k];
            }
        }

        // emit dims + indexing arrays in the array's axis order
        MdArray outA;
        mdCopySourcedProps(outA, *fa);
        for (size_t k = 0; k < nd; ++k)
        {
            const std::string dimName =
                fdims[k] ? fdims[k]->name : mdLastPathComp(fa->dimFull[k]);
            bool dimSeen = false;
            for (const auto &dd : outRoot.dims)
                if (dd.name == dimName)
                    dimSeen = true;
            if (!dimSeen)
            {
                MdGroup::Dim nd2;
                nd2.name = dimName;
                nd2.fullName = "/" + dimName;
                if (fdims[k])
                {
                    nd2.type = fdims[k]->type;
                    nd2.direction = fdims[k]->direction;
                    nd2.indexingVar = fdims[k]->indexingVar;
                }
                nd2.size = aggs[k].size;
                outRoot.dims.push_back(nd2);
                if (aggs[k].hasIndex)
                {
                    MdArray ia;
                    ia.name = fdims[k]->indexingVar;
                    ia.dtype = aggs[k].ivDtype;
                    ia.dimRefs = {dimName};
                    ia.dimFull = {"/" + dimName};
                    ia.dimSizes = {aggs[k].size};
                    if (aggs[k].regular)
                    {
                        ia.kind = MdArray::Regular;
                        ia.regStart = aggs[k].startg;
                        ia.regIncrement = aggs[k].inc;
                    }
                    else
                    {
                        ia.kind = MdArray::Inline;
                        for (const auto &v : aggs[k].vals)
                            ia.tokens.push_back(
                                mdXmlFormat(ia.dtype, v));
                        ia.hasInlineOffset = true;
                        ia.inlineOffset = {0};
                        ia.inlineCount = {aggs[k].size};
                    }
                    bool arrSeen = false;
                    for (const auto &ea : outRoot.arrays)
                        if (ea.name == ia.name)
                            arrSeen = true;
                    if (!arrSeen)
                        outRoot.arrays.push_back(ia);
                }
            }
            outA.dimRefs.push_back(dimName);
            outA.dimFull.push_back("/" + dimName);
            outA.dimSizes.push_back(aggs[k].size);
        }
        for (const auto &ea : outRoot.arrays)
            if (ea.name == outA.name)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "An array with same name (" + outA.name +
                                ") already exists");
                return 1;
            }
        for (size_t d = 0; d < dss.size(); ++d)
        {
            MdSource s;
            s.filename = inputs[d];
            s.relative = !inputs[d].empty() && inputs[d][0] != '/';
            s.arrayFull = "/" + name;
            s.hasSlab = true;
            s.srcOffset.assign(nd, 0);
            s.srcCount = per[d]->dimSizes;
            s.srcStep.assign(nd, 1);
            s.dstOffset.assign(nd, 0);
            for (size_t k = 0; k < nd; ++k)
                s.dstOffset[k] = (uint64_t)aggs[k].offsets[d];
            outA.sources.push_back(s);
        }
        outRoot.arrays.push_back(outA);
    }

    if (drv == "MEM")
    {
        if (!quiet)
        {
            TermProgress p;
            p.update(1.0);
        }
        return 0;
    }
    if (!writeStringToFile(output, mdimSerializeVrt(outRoot)))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "Cannot create " + output);
        return 1;
    }
    // the mosaic writer reopens what it wrote; union bugs on descending
    // axes surface here as reader errors on the (kept) output file
    {
        MdDataset check;
        std::string err2;
        bool notVrt2 = false;
        if (!mdimOpen(output, check, err2, notVrt2))
        {
            if (!err2.empty())
                cplErrorStr(CE_Failure, CPLE_AppDefined, err2);
            return 1;
        }
    }
    return 0;
}

void mdimRegisterValueChecks();

}  // namespace

// engine hook: open a multidim input during the validation pass; returns
// true when it opened, false with err filled (empty err = generic
// "not recognized" message)
bool mdimValidationOpen(const std::string &path, std::string &err)
{
    MdDataset ds;
    bool notVrt = false;
    if (mdimOpen(path, ds, err, notVrt))
        return true;
    return false;
}

const char *mdimDriversJson();

void registerMdimHandlers()
{
    registerHandler("mdim_info", mdimInfoHandler);
    registerHandler("mdim_convert", mdimConvertHandler);
    registerHandler("mdim_mosaic", mdimMosaicHandler);
    mdimRegisterValueChecks();
}

namespace
{

void mdimRegisterValueChecks()
{
    auto ofCheck = [](const std::string &argName,
                      const std::string &value) -> std::string
    {
        if (argName != "output-format")
            return "";
        bool ras = false, vec = false;
        if (!knownDriverCaps(value, ras, vec))
            return "Invalid value for argument 'output-format'. Driver '" +
                   value + "' does not exist.";
        if (!strEqualNoCase(value, "VRT") && !strEqualNoCase(value, "MEM"))
            return "Invalid value for argument 'output-format'. Driver '" +
                   value +
                   "' does not expose the required "
                   "'DCAP_CREATE_MULTIDIMENSIONAL' capability.";
        return "";
    };
    registerArgValueCheck("mdim_convert", ofCheck);
    registerArgValueCheck("mdim_mosaic", ofCheck);
}

}  // namespace

const char *mdimDriversJson()
{
    return "[\n"
           "  {\n"
           "    \"short_name\":\"VRT\",\n"
           "    \"long_name\":\"Virtual Raster\",\n"
           "    \"scopes\":[\n"
           "      \"raster\",\n"
           "      \"multidimensional_raster\"\n"
           "    ],\n"
           "    \"capabilities\":[\n"
           "      \"open\",\n"
           "      \"create\",\n"
           "      \"create_copy\",\n"
           "      \"update\",\n"
           "      \"virtual_io\"\n"
           "    ],\n"
           "    \"file_extensions\":[\n"
           "      \"vrt\"\n"
           "    ]\n"
           "  },\n"
           "  {\n"
           "    \"short_name\":\"MEM\",\n"
           "    \"long_name\":\"In Memory raster, vector and "
           "multidimensional raster\",\n"
           "    \"scopes\":[\n"
           "      \"raster\",\n"
           "      \"multidimensional_raster\",\n"
           "      \"vector\"\n"
           "    ],\n"
           "    \"capabilities\":[\n"
           "      \"open\",\n"
           "      \"create\"\n"
           "    ]\n"
           "  }\n"
           "]";
}
