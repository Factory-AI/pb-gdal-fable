// vector concat union engine: OGRUnionLayer-style schema merge, SRS
// consolidation and feature restreaming feeding the convert delegate

#include "cpl.h"
#include "ogr.h"
#include "srs.h"
#include "util.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct Member
{
    size_t dsIdx;
    size_t lyrIdx;
    const OgrLayer *lyr;
};

std::string basenameNoExt(const std::string &p)
{
    std::string s = p;
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
    size_t sl = s.find_last_of('/');
    if (sl != std::string::npos)
        s = s.substr(sl + 1);
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        s = s.substr(0, dot);
    return s;
}

std::string expandTemplate(const std::string &tmpl, const std::string &path,
                           size_t dsIdx, size_t lyrIdx,
                           const std::string &lyrName)
{
    std::string base = basenameNoExt(path);
    std::string autoName =
        base == lyrName ? lyrName : base + "_" + lyrName;
    std::string out;
    size_t i = 0;
    while (i < tmpl.size())
    {
        if (tmpl[i] == '{')
        {
            size_t e = tmpl.find('}', i);
            if (e != std::string::npos)
            {
                std::string k = tmpl.substr(i + 1, e - i - 1);
                std::string v;
                bool known = true;
                if (k == "DS_NAME")
                    v = path;
                else if (k == "DS_BASENAME")
                    v = base;
                else if (k == "DS_INDEX")
                    v = strPrintf("%d", (int)dsIdx);
                else if (k == "LAYER_NAME")
                    v = lyrName;
                else if (k == "LAYER_INDEX")
                    v = strPrintf("%d", (int)lyrIdx);
                else if (k == "AUTO_NAME")
                    v = autoName;
                else
                    known = false;
                if (known)
                {
                    out += v;
                    i = e + 1;
                    continue;
                }
            }
        }
        out += tmpl[i++];
    }
    return out;
}

bool subtypeCompat(int sub, int type)
{
    switch (sub)
    {
        case OFSTBoolean:
        case OFSTInt16:
            return type == OFTInteger || type == OFTIntegerList;
        case OFSTFloat32:
            return type == OFTReal || type == OFTRealList;
        case OFSTJSON:
        case OFSTUUID:
            return type == OFTString;
        default:
            return true;
    }
}

// promotion only when the incoming scalar widens the slot (Int->Int64,
// integers under Real); every other mix collapses to String, including
// Real arriving into an integer slot, Date/DateTime mixes and all list
// combinations
int mergeFieldType(int a, int b)
{
    if (a == b)
        return a;
    if ((a == OFTInteger && b == OFTInteger64) ||
        (a == OFTInteger64 && b == OFTInteger))
        return OFTInteger64;
    if (a == OFTReal && (b == OFTInteger || b == OFTInteger64))
        return OFTReal;
    return OFTString;
}

void mergeFieldDefn(OgrFieldDefn &dst, const OgrFieldDefn &src,
                    bool warn)
{
    int nt = mergeFieldType(dst.type, src.type);
    if (nt != dst.type && dst.subType != OFSTNone &&
        !subtypeCompat(dst.subType, nt))
    {
        if (warn)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "Type and subtype of field definition are not "
                        "compatible. Resetting to OFSTNone");
        dst.subType = OFSTNone;
    }
    bool numeric = nt == OFTInteger || nt == OFTInteger64 ||
                   nt == OFTReal;
    if (nt == OFTString &&
        (dst.type != OFTString || src.type != OFTString))
    {
        dst.width = 0;
        dst.precision = 0;
    }
    else if (nt != dst.type || dst.type != src.type)
    {
        if (numeric)
        {
            if (src.width > dst.width)
                dst.width = src.width;
            if (src.precision > dst.precision)
                dst.precision = src.precision;
        }
        else
        {
            dst.width = 0;
            dst.precision = 0;
        }
    }
    else if (dst.width != src.width || dst.precision != src.precision)
    {
        if (dst.type == OFTString)
        {
            dst.width = 0;
            dst.precision = 0;
        }
        else if (numeric)
        {
            if (src.width > dst.width)
                dst.width = src.width;
            if (src.precision > dst.precision)
                dst.precision = src.precision;
        }
        else
        {
            dst.width = 0;
            dst.precision = 0;
        }
    }
    if (dst.tzAggr != src.tzAggr)
        dst.tzAggr = dst.tzAggr == -1
                         ? src.tzAggr
                         : (src.tzAggr == -1 ? dst.tzAggr : -2);
    dst.type = nt;
}

struct Group
{
    std::string name;
    std::vector<Member> members;
};

std::vector<Group> concatGroups(
    const std::vector<std::string> &inputPaths,
    const std::vector<std::unique_ptr<OgrDataset>> &dss,
    const std::vector<std::string> &layerSel, const VectorConcatOpts &o)
{
    std::vector<Member> sel;
    for (size_t d = 0; d < dss.size(); ++d)
        for (size_t li = 0; li < dss[d]->layers.size(); ++li)
        {
            const OgrLayer &ly = dss[d]->layers[li];
            bool want = layerSel.empty();
            for (const auto &l : layerSel)
                if (ly.name == l)
                    want = true;
            if (want)
                sel.push_back({d, li, &ly});
        }

    std::vector<Group> groups;
    auto groupFor = [&](const std::string &n) -> Group & {
        for (auto &g : groups)
            if (g.name == n)
                return g;
        groups.push_back({n, {}});
        return groups.back();
    };
    if (o.mode == "single")
    {
        if (!sel.empty())
            groupFor(o.outputLayer.empty() ? "merged" : o.outputLayer)
                .members = sel;
    }
    else if (o.mode == "stack")
    {
        std::string tmpl =
            o.outputLayer.empty() ? "{AUTO_NAME}" : o.outputLayer;
        for (const Member &m : sel)
            groupFor(expandTemplate(tmpl, inputPaths[m.dsIdx], m.dsIdx,
                                    m.lyrIdx, m.lyr->name))
                .members.push_back(m);
    }
    else
        for (const Member &m : sel)
            groupFor(m.lyr->name).members.push_back(m);
    return groups;
}

}  // namespace

std::vector<std::string> vectorConcatGroupNames(
    const std::vector<std::string> &inputPaths,
    const std::vector<std::unique_ptr<OgrDataset>> &dss,
    const std::vector<std::string> &layerSel, const VectorConcatOpts &o)
{
    std::vector<std::string> names;
    for (const Group &g : concatGroups(inputPaths, dss, layerSel, o))
        names.push_back(g.name);
    return names;
}

int vectorConcatBuildUnion(
    const std::vector<std::string> &inputPaths,
    const std::vector<std::unique_ptr<OgrDataset>> &dss,
    const std::vector<std::string> &layerSel, const VectorConcatOpts &o,
    OgrDataset &out)
{
    std::vector<Group> groups =
        concatGroups(inputPaths, dss, layerSel, o);

    bool dstOk = false, srcOk = false;
    Srs dstSrs, srcSrs;
    if (!o.dstCrs.empty())
    {
        dstSrs = Srs::fromCliInput(o.dstCrs, dstOk);
        if (!dstOk)
            return 1;
        if (!o.srcCrs.empty())
        {
            srcSrs = Srs::fromCliInput(o.srcCrs, srcOk);
            if (!srcOk)
                return 1;
        }
    }

    std::string slfContent =
        o.slfContent.empty() ? "{AUTO_NAME}" : o.slfContent;

    for (Group &g : groups)
    {
        OgrLayer ul;
        ul.name = g.name;
        ul.emitNullFields = false;

        // SRS pass: the reference validates/consolidates spatial
        // references before the union schema is built
        std::vector<void *> ops(g.members.size(), nullptr);
        auto freeOps = [&]() {
            for (void *op : ops)
                if (op)
                    vectorCrsOpFree(op);
        };
        if (dstOk)
        {
            ul.hasSrs = true;
            ul.srs = dstSrs;
            for (size_t i = 0; i < g.members.size(); ++i)
            {
                const Member &m = g.members[i];
                if (m.lyr->geomType == 101)
                    continue;
                const Srs *msrs = srcOk ? &srcSrs
                                        : (m.lyr->hasSrs ? &m.lyr->srs
                                                         : nullptr);
                if (!msrs)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                "concat: Layer '" + m.lyr->name +
                                    "' of '" + inputPaths[m.dsIdx] +
                                    "' has no spatial reference system");
                    freeOps();
                    return 1;
                }
                if (!msrs->isEquivalentTo(dstSrs))
                {
                    ops[i] = vectorCrsOpCreate(*msrs, dstSrs);
                    if (!ops[i])
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "concat: Cannot reproject layer '" +
                                        m.lyr->name + "'");
                        freeOps();
                        return 1;
                    }
                }
            }
        }
        else
        {
            const OgrLayer *first = nullptr;
            for (const Member &m : g.members)
                if (m.lyr->geomType != 101)
                {
                    first = m.lyr;
                    break;
                }
            ul.hasSrs = first && first->hasSrs;
            if (ul.hasSrs)
                ul.srs = first->srs;
            for (size_t i = 0; i < g.members.size(); ++i)
            {
                const Member &m = g.members[i];
                if (m.lyr->geomType == 101)
                    continue;
                if (m.lyr->hasSrs != ul.hasSrs)
                {
                    if (o.srsWarnings)
                        cplErrorStr(CE_Warning, CPLE_AppDefined,
                                    "SRS of geometry field '" +
                                        m.lyr->geomColumnName +
                                        "' layer " + m.lyr->name +
                                        " not consistent with "
                                        "UnionLayer SRS");
                }
                else if (ul.hasSrs &&
                         !m.lyr->srs.isEquivalentTo(ul.srs))
                {
                    ops[i] = vectorCrsOpCreate(m.lyr->srs, ul.srs);
                    if (!ops[i])
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "concat: Cannot reproject layer '" +
                                        m.lyr->name + "'");
                        freeOps();
                        return 1;
                    }
                }
            }
        }

        // schema pass
        bool slf = !o.slfName.empty();
        if (slf)
        {
            OgrFieldDefn f;
            f.name = o.slfName;
            f.type = OFTString;
            ul.fields.push_back(f);
        }
        size_t base = slf ? 1 : 0;
        // per member: source field index -> union slot (-1 dropped)
        std::vector<std::vector<int>> slotMaps(g.members.size());
        bool intersect = o.fieldStrategy == "intersection";
        if (intersect && !g.members.empty())
        {
            const OgrLayer &l0 = *g.members[0].lyr;
            std::vector<bool> keep(l0.fields.size(), true);
            for (size_t i = 1; i < g.members.size(); ++i)
            {
                const OgrLayer &li = *g.members[i].lyr;
                for (size_t k = 0; k < l0.fields.size(); ++k)
                {
                    if (!keep[k])
                        continue;
                    bool found = false;
                    for (const auto &f : li.fields)
                        if (strEqualNoCase(f.name, l0.fields[k].name))
                        {
                            found = true;
                            break;
                        }
                    if (!found)
                        keep[k] = false;
                }
            }
            std::vector<int> firstMap(l0.fields.size(), -1);
            for (size_t k = 0; k < l0.fields.size(); ++k)
                if (keep[k])
                {
                    firstMap[k] = (int)ul.fields.size();
                    ul.fields.push_back(l0.fields[k]);
                }
            slotMaps[0] = std::move(firstMap);
            for (size_t i = 1; i < g.members.size(); ++i)
            {
                const OgrLayer &li = *g.members[i].lyr;
                std::vector<int> map(li.fields.size(), -1);
                for (size_t fi = 0; fi < li.fields.size(); ++fi)
                {
                    for (size_t s = base; s < ul.fields.size(); ++s)
                        if (ul.fields[s].name == li.fields[fi].name)
                        {
                            map[fi] = (int)s;
                            mergeFieldDefn(ul.fields[s], li.fields[fi],
                                           o.typeWarnings);
                            break;
                        }
                }
                slotMaps[i] = std::move(map);
            }
        }
        else
        {
            for (size_t i = 0; i < g.members.size(); ++i)
            {
                const OgrLayer &li = *g.members[i].lyr;
                std::vector<int> map(li.fields.size(), -1);
                for (size_t fi = 0; fi < li.fields.size(); ++fi)
                {
                    int slot = -1;
                    for (size_t s = base; s < ul.fields.size(); ++s)
                        if (ul.fields[s].name == li.fields[fi].name)
                        {
                            slot = (int)s;
                            break;
                        }
                    if (slot >= 0)
                        mergeFieldDefn(ul.fields[slot], li.fields[fi],
                                       o.typeWarnings);
                    else
                    {
                        slot = (int)ul.fields.size();
                        ul.fields.push_back(li.fields[fi]);
                    }
                    map[fi] = slot;
                }
                slotMaps[i] = std::move(map);
            }
        }

        // geometry type: the union takes the first geometry-bearing
        // member's declared type verbatim, no promotion across members
        ul.shpPinType = g.members.size() >= 2;
        bool anyGeom = false;
        for (const Member &m : g.members)
        {
            if (m.lyr->geomType == 101)
                continue;
            ul.geomType = m.lyr->geomType;
            ul.geomHasZ = m.lyr->geomHasZ;
            ul.geomHasM = m.lyr->geomHasM;
            ul.geomColumnName = m.lyr->geomColumnName;
            anyGeom = true;
            break;
        }
        if (!anyGeom)
        {
            ul.geomType = 101;
            ul.hasGeomField = false;
        }

        // feature pass: sequential FIDs, source values coerced into the
        // union slots, geometries reprojected where the SRS pass armed
        // a transform
        long long fid = 0;
        for (size_t i = 0; i < g.members.size(); ++i)
        {
            const Member &m = g.members[i];
            const std::vector<int> &map = slotMaps[i];
            std::string slfVal;
            if (slf)
                slfVal = expandTemplate(slfContent,
                                        inputPaths[m.dsIdx], m.dsIdx,
                                        m.lyrIdx, m.lyr->name);
            for (const OgrFeature &f : m.lyr->features)
            {
                OgrFeature nf;
                nf.fid = fid++;
                nf.values.resize(ul.fields.size());
                if (slf)
                {
                    nf.values[0].set = true;
                    nf.values[0].v.type = JVal::STRING;
                    nf.values[0].v.s = slfVal;
                }
                for (size_t fi = 0;
                     fi < map.size() && fi < f.values.size(); ++fi)
                {
                    int s = map[fi];
                    if (s < 0 || !f.values[fi].set)
                        continue;
                    OgrFieldValue fv = f.values[fi];
                    const OgrFieldDefn &sf = m.lyr->fields[fi];
                    const OgrFieldDefn &df = ul.fields[s];
                    bool dtFamily = sf.type == OFTDate ||
                                    sf.type == OFTDateTime ||
                                    sf.type == OFTTime;
                    if (sf.type != df.type && fv.v.type != JVal::NUL)
                    {
                        // the union translation re-serializes datetime
                        // values GetFieldAsString-style
                        if (dtFamily && df.type == OFTString &&
                            fv.v.type == JVal::STRING)
                        {
                            OgrDateTime dt;
                            if (ogrParseDate(fv.v.s, dt))
                                fv.v.s =
                                    ogrDateTimeToString(dt, sf.type);
                        }
                        else
                        {
                            WarnLog log;
                            leafConvert(log, ul.name, sf, df.type,
                                        df.subType, fv);
                            for (const auto &msg : log.msgs)
                                cplErrorStr(CE_Warning,
                                            CPLE_AppDefined,
                                            msg.second);
                        }
                    }
                    nf.values[s] = fv;
                }
                nf.hasGeom = f.hasGeom;
                nf.geom = f.geom;
                if (ops[i] && nf.hasGeom)
                {
                    if (!vectorCrsOpApply(ops[i], nf.geom))
                    {
                        nf.hasGeom = false;
                        nf.geom = OgrGeometry();
                    }
                }
                ul.features.push_back(std::move(nf));
            }
        }
        freeOps();
        vectorLayerRecomputeExtent(ul);
        out.layers.push_back(std::move(ul));
    }
    return 0;
}
