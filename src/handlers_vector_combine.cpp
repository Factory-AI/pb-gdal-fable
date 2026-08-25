#include "cpl.h"
#include "dataset.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// vector combine: merges each layer's features into one feature per
// group. Multipart geometries flatten one level unless --keep-nested;
// the container is the Multi* of a simple layer geometry type,
// GeometryCollection for everything else. Groups sort by the tuple of
// their key strings.

namespace
{

// GetFieldAsString-shaped key: unset and null collapse to ""
std::string combineKeyString(const OgrFieldDefn &fd, const OgrFeature &f,
                             size_t idx)
{
    if (idx >= f.values.size() || !f.values[idx].set)
        return "";
    const JVal &v = f.values[idx].v;
    if (v.type == JVal::NUL)
        return "";
    if (fd.type == OFTInteger || fd.type == OFTInteger64)
    {
        long long n = v.type == JVal::INT      ? v.i
                      : v.type == JVal::DOUBLE ? (long long)v.d
                      : v.type == JVal::BOOL   ? (v.b ? 1 : 0)
                      : strtoll(v.s.c_str(), nullptr, 10);
        return strPrintf("%lld", n);
    }
    if (fd.type == OFTReal)
    {
        double d = v.type == JVal::DOUBLE ? v.d
                   : v.type == JVal::INT  ? (double)v.i
                   : v.type == JVal::BOOL ? (v.b ? 1 : 0)
                                          : strtod(v.s.c_str(), nullptr);
        return strPrintf("%.15g", d);
    }
    switch (v.type)
    {
        case JVal::STRING:
            return v.s;
        case JVal::INT:
            return strPrintf("%lld", v.i);
        case JVal::DOUBLE:
            return strPrintf("%.15g", v.d);
        case JVal::BOOL:
            return v.b ? "1" : "0";
        default:
            return jsoncSerialize(v);
    }
}

// collection set3D: a single 3D member promotes the container and every
// member (z 0 padding), whatever the insertion order
void combinePromoteZ(OgrGeometry &g)
{
    g.hasZ = true;
    for (OgrGeometry &p : g.parts)
        combinePromoteZ(p);
}

}  // namespace

int vectorCombineApplyStep(OgrDataset &d, const VectorCombineOpts &o,
                           const std::vector<std::string> &layerSel)
{
    for (size_t li = 0; li < d.layers.size();)
    {
        OgrLayer &l = d.layers[li];
        if (!vvLayerSelected(layerSel, "", l))
        {
            ++li;
            continue;
        }
        if (!l.hasGeomField)
        {
            // implicitly included attribute-only layers drop silently;
            // naming one via -l is an error
            bool named = false;
            for (const auto &n : layerSel)
                if (n == l.name)
                    named = true;
            if (named)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "combine: Specified layer '" + l.name +
                                "' has no geometry field");
                return 1;
            }
            d.layers.erase(d.layers.begin() + li);
            continue;
        }
        std::vector<size_t> gidx;
        for (const std::string &nm : o.groupBy)
        {
            int idx = -1;
            for (size_t i = 0; i < l.fields.size(); ++i)
                if (l.fields[i].name == nm)
                {
                    idx = (int)i;
                    break;
                }
            if (idx < 0)
                for (size_t i = 0; i < l.fields.size(); ++i)
                    if (strEqualNoCase(l.fields[i].name, nm))
                    {
                        idx = (int)i;
                        break;
                    }
            if (idx < 0)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "combine: Specified attribute field '" + nm +
                                "' does not exist in layer '" + l.name +
                                "'");
                return 1;
            }
            gidx.push_back((size_t)idx);
        }
        int cont = l.geomType == 1   ? 4
                   : l.geomType == 2 ? 5
                   : l.geomType == 3 ? 6
                                     : 7;
        std::map<std::vector<std::string>, size_t> order;
        std::vector<OgrFeature> groups;
        for (const OgrFeature &f : l.features)
        {
            std::vector<std::string> key;
            for (size_t gi : gidx)
                key.push_back(combineKeyString(l.fields[gi], f, gi));
            auto it = order.find(key);
            size_t g;
            if (it != order.end())
                g = it->second;
            else
            {
                OgrFeature nf;
                // the combined feature keeps the group founder's native
                // members (update-flow writes re-merge them)
                nf.gjNative = f.gjNative;
                nf.hasGeom = true;
                nf.geom.type = cont;
                nf.values.resize(gidx.size());
                for (size_t k = 0; k < gidx.size(); ++k)
                {
                    // duplicate-named key fields keep only the last
                    // occurrence's value (SetField-by-name semantics)
                    bool last = true;
                    for (size_t j = k + 1; j < gidx.size(); ++j)
                        if (gidx[j] == gidx[k])
                            last = false;
                    if (last && gidx[k] < f.values.size())
                        nf.values[k] = f.values[gidx[k]];
                }
                g = groups.size();
                order.emplace(std::move(key), g);
                groups.push_back(std::move(nf));
            }
            if (!f.hasGeom)
                continue;
            OgrGeometry &cg = groups[g].geom;
            if (!o.keepNested && f.geom.type >= 4 && f.geom.type <= 7)
            {
                for (const OgrGeometry &p : f.geom.parts)
                {
                    if (p.hasZ)
                        cg.hasZ = true;
                    cg.parts.push_back(p);
                }
            }
            else
            {
                if (f.geom.hasZ)
                    cg.hasZ = true;
                cg.parts.push_back(f.geom);
            }
        }
        std::vector<OgrFieldDefn> nf;
        for (size_t gi : gidx)
            nf.push_back(l.fields[gi]);
        l.fields = std::move(nf);
        std::vector<OgrFeature> feats;
        long long fid = 0;
        for (auto &kv : order)
        {
            OgrFeature &f = groups[kv.second];
            if (f.geom.hasZ)
                combinePromoteZ(f.geom);
            f.fid = fid++;
            feats.push_back(std::move(f));
        }
        l.features = std::move(feats);
        l.geomType = cont;
        // the combined layer is a fresh in-memory one: source layer
        // metadata and driver-specific traits do not carry over
        l.metadata.clear();
        l.extraMdDomains.clear();
        l.fidColumn.clear();
        l.geomColumnName.clear();
        l.emitNullFields = false;
        l.directFidRange = false;
        l.countOverride = -1;
        vectorLayerRecomputeExtent(l);
        ++li;
    }
    return 0;
}

namespace
{

int vectorCombineHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");

    std::string driver;
    if (int rc = vvResolveVerbFormats(cmd, r, driver))
        return rc;
    if (r.str("output-format").empty() &&
        strEndsWith(strToLower(output), ".gdalg.json"))
        driver = "GDALG";

    std::unique_ptr<OgrDataset> ds;
    if (int rc = vvOpenInputDs(cmd, r, input, ds))
        return rc;

    VectorCombineOpts o;
    o.groupBy = r.list("group-by");
    o.keepNested = r.flag("keep-nested");

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!o.groupBy.empty())
    {
        // per-value quoting, joined with bare commas
        std::string joined;
        for (size_t i = 0; i < o.groupBy.size(); ++i)
        {
            if (i)
                joined += ",";
            joined += vvGq(o.groupBy[i]);
        }
        cli += " --group-by " + joined;
    }
    if (o.keepNested)
        cli += " --keep-nested";
    const ArgValue *ae = r.get("add-extra-fields");
    if (ae && ae->set)
        cli += " --add-extra-fields no";
    cli += " --output-format stream --output streamed_dataset";

    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [o, sel](OgrDataset &d) -> int
    { return vectorCombineApplyStep(d, o, sel); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

}  // namespace

void registerVectorCombineHandler()
{
    registerHandler("vector_combine", vectorCombineHandler);
    registerArgCheck(
        "vector_combine",
        [](const std::string &a, ParseResult &r) -> int
        {
            // output-format is validated at value-parse time below, in
            // token order with add-extra-fields
            if (a == "group-by")
            {
                const std::vector<std::string> &g = r.list("group-by");
                for (size_t i = 0; i < g.size(); ++i)
                    for (size_t j = i + 1; j < g.size(); ++j)
                        if (g[i] == g[j])
                        {
                            cplErrorStr(CE_Failure, CPLE_AppDefined,
                                        "'group-by' must be a list of "
                                        "unique values.");
                            handlerPrintUsage();
                            return 1;
                        }
            }
            else if (a == "input-format")
            {
                for (const auto &drv : r.list("input-format"))
                {
                    std::string ferr = inputFormatCapError(true, drv);
                    if (!ferr.empty())
                    {
                        cplErrorStr(CE_Failure, CPLE_AppDefined,
                                    "combine: " + ferr);
                        handlerPrintUsage();
                        return 1;
                    }
                }
            }
            return 0;
        });
    registerArgValueCheck(
        "vector_combine",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "output-format")
            {
                std::string driver;
                std::string ferr = vectorOutputDriverResolve(value, driver);
                if (!ferr.empty())
                    return ferr;
            }
            if (argName == "add-extra-fields")
            {
                std::string canon = value;
                for (const char *c :
                     {"no", "sometimes-identical", "always-identical"})
                    if (strEqualNoCase(value, c))
                        canon = c;
                if (canon != "no")
                    return "\x07The SQLITE driver must be available for "
                           "add-extra-fields=" +
                           canon;
            }
            return "";
        });
}
