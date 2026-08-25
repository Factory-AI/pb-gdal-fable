#include "cpl.h"
#include "engine.h"
#include "ogr.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// vector geometry verbs (swap-xy / segmentize / make-point /
// explode-collections / set-geom-type): dataset transforms applied at
// the convert handler's mutate point, mirroring the reference's
// streamed-layer wrappers

ConvertTranslateFail g_convertTranslateFail;

namespace
{

// the active-geometry gate compares against the layer's geometry column
// name, which is empty for GeoJSON sources; a non-matching name turns
// the step into a passthrough
bool geomLayerSelected(const std::vector<std::string> &sel,
                       const std::string &activeLayer,
                       const std::string &activeGeom, const OgrLayer &l)
{
    if (!vvLayerSelected(sel, activeLayer, l))
        return false;
    return activeGeom.empty() || activeGeom == l.geomColumnName;
}

void swapXyRec(OgrGeometry &g)
{
    for (size_t i = 0; i + 1 < g.coords.size(); i += 3)
        std::swap(g.coords[i], g.coords[i + 1]);
    for (OgrGeometry &p : g.parts)
        swapXyRec(p);
}

}  // namespace

int vectorSwapXyApplyStep(OgrDataset &d, const std::string &activeGeom,
                          const std::string &activeLayer,
                          const std::vector<std::string> &layerSel)
{
    for (OgrLayer &l : d.layers)
    {
        if (!geomLayerSelected(layerSel, activeLayer, activeGeom, l))
            continue;
        for (OgrFeature &f : l.features)
            if (f.hasGeom)
                swapXyRec(f.geom);
        if (l.hasExtent)
        {
            std::swap(l.extent[0], l.extent[1]);
            std::swap(l.extent[2], l.extent[3]);
        }
    }
    return 0;
}

int vectorSegmentizeApplyStep(OgrDataset &d, double maxLength,
                              const std::string &activeGeom,
                              const std::string &activeLayer,
                              const std::vector<std::string> &layerSel)
{
    for (OgrLayer &l : d.layers)
    {
        if (!geomLayerSelected(layerSel, activeLayer, activeGeom, l))
            continue;
        for (OgrFeature &f : l.features)
            if (f.hasGeom)
                ogrSegmentize(f.geom, maxLength);
    }
    return 0;
}

namespace
{

std::string mpFieldAsString(const JVal &v)
{
    switch (v.type)
    {
        case JVal::STRING: return v.s;
        case JVal::INT: return strPrintf("%lld", v.i);
        case JVal::DOUBLE: return ogrFormatDouble(v.d, 15);
        case JVal::BOOL: return v.b ? "1" : "0";
        default: return "";
    }
}

// GetFieldAsDouble semantics: null/unset reads 0, string-typed fields
// go through the string representation and reject non-numeric text
int mpFieldNumeric(const OgrFieldDefn &fd, const OgrFeature &f,
                   size_t idx, double &out)
{
    out = 0.0;
    if (idx >= f.values.size())
        return 0;
    const OgrFieldValue &fv = f.values[idx];
    if (!fv.set || fv.v.type == JVal::NUL)
        return 0;
    if (fd.type == OFTString || fd.type == OFTStringList)
    {
        std::string s = mpFieldAsString(fv.v);
        if (cplValueType(s) == 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Invalid value in field " + fd.name + ": " + s +
                            " ");
            return 1;
        }
        out = strtod(s.c_str(), nullptr);
        return 0;
    }
    switch (fv.v.type)
    {
        case JVal::INT: out = (double)fv.v.i; break;
        case JVal::DOUBLE: out = fv.v.d; break;
        case JVal::BOOL: out = fv.v.b ? 1.0 : 0.0; break;
        case JVal::STRING: out = strtod(fv.v.s.c_str(), nullptr); break;
        default: break;
    }
    return 0;
}

}  // namespace

int vectorMakePointApplyStep(OgrDataset &d, const std::string &xField,
                             const std::string &yField,
                             const std::string &zField,
                             const std::string &mField,
                             const std::string &dstCrs,
                             const std::vector<std::string> &layerSel)
{
    g_convertTranslateFail = ConvertTranslateFail();
    for (OgrLayer &l : d.layers)
    {
        if (!vvLayerSelected(layerSel, "", l))
            continue;
        struct Req
        {
            const char *letter;
            const std::string *name;
            long long idx = -1;
        };
        std::vector<Req> reqs = {{"X", &xField}, {"Y", &yField}};
        if (!zField.empty())
            reqs.push_back({"Z", &zField});
        if (!mField.empty())
            reqs.push_back({"M", &mField});
        bool anyMissing = false;
        for (Req &q : reqs)
        {
            for (size_t i = 0; i < l.fields.size(); ++i)
                if (l.fields[i].name == *q.name)
                {
                    q.idx = (long long)i;
                    break;
                }
            if (q.idx < 0)
                anyMissing = true;
        }
        if (anyMissing)
        {
            for (const Req &q : reqs)
                if (q.idx < 0)
                {
                    cplErrorStr(CE_Failure, CPLE_AppDefined,
                                strPrintf("Specified %s field name '%s' "
                                          "does not exist",
                                          q.letter, q.name->c_str()));
                    break;
                }
            std::vector<std::string> passErrs;
            for (const Req &q : reqs)
                if (q.idx < 0)
                    passErrs.push_back("Invalid index : -1");
            for (const auto &e : passErrs)
                cplErrorStr(CE_Failure, CPLE_AppDefined, e);
            if (!g_convertTranslateFail.active)
            {
                g_convertTranslateFail.active = true;
                g_convertTranslateFail.layer = l.name;
                g_convertTranslateFail.passErrors = passErrs;
            }
            l.features.clear();
            l.countOverride = 0;
            l.hasExtent = false;
            continue;
        }
        bool hasZ = !zField.empty(), hasM = !mField.empty();
        std::vector<OgrFeature> kept;
        bool failed = false;
        for (const OgrFeature &f : l.features)
        {
            double vx = 0, vy = 0, vz = 0, vm = 0;
            if (mpFieldNumeric(l.fields[(size_t)reqs[0].idx], f,
                               (size_t)reqs[0].idx, vx) ||
                mpFieldNumeric(l.fields[(size_t)reqs[1].idx], f,
                               (size_t)reqs[1].idx, vy) ||
                (hasZ && mpFieldNumeric(l.fields[(size_t)reqs[2].idx], f,
                                        (size_t)reqs[2].idx, vz)) ||
                (hasM &&
                 mpFieldNumeric(l.fields[(size_t)reqs[hasZ ? 3 : 2].idx],
                                f, (size_t)reqs[hasZ ? 3 : 2].idx, vm)))
            {
                failed = true;
                break;
            }
            OgrFeature nf;
            nf.fid = -1;
            nf.explicitFid = false;
            nf.values = f.values;
            nf.hasGeom = true;
            nf.geom.type = 1;
            nf.geom.hasZ = hasZ;
            nf.geom.hasM = hasM;
            nf.geom.coords = {vx, vy, vz};
            if (hasM)
                nf.geom.m = {vm};
            kept.push_back(std::move(nf));
        }
        l.geomType = 1;
        l.geomHasZ = hasZ;
        l.geomHasM = hasM;
        l.hasGeomField = true;
        l.geomColumnName = "geometry";
        l.hasSrs = false;
        l.srs = Srs();
        if (!dstCrs.empty())
        {
            bool ok = false;
            Srs srs = Srs::fromCliInput(dstCrs, ok, true);
            if (ok)
            {
                l.hasSrs = true;
                l.srs = std::move(srs);
            }
        }
        if (failed)
        {
            if (!g_convertTranslateFail.active)
            {
                g_convertTranslateFail.active = true;
                g_convertTranslateFail.layer = l.name;
                g_convertTranslateFail.oneShot = true;
                g_convertTranslateFail.quietKept = kept;
            }
            l.countOverride = (long long)kept.size();
            l.features.clear();
            l.hasExtent = false;
        }
        else
        {
            l.features = std::move(kept);
            l.countOverride = -1;
            vectorLayerRecomputeExtent(l);
        }
    }
    return 0;
}

int vectorExplodeApplyStep(OgrDataset &d, const std::string &geomType,
                           bool skipMismatch,
                           const std::string &activeGeom,
                           const std::string &activeLayer,
                           const std::vector<std::string> &layerSel)
{
    int T = 0;
    bool tz = false, tm = false;
    if (!geomType.empty() &&
        !ogrGeomTypeFromWktName(geomType, T, tz, tm))
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "explode-collections: Invalid geometry type '" +
                        geomType + "'");
        return 1;
    }
    for (OgrLayer &l : d.layers)
    {
        if (!geomLayerSelected(layerSel, activeLayer, activeGeom, l))
            continue;
        if (T)
        {
            l.geomType = T;
            l.geomHasZ = tz;
            l.geomHasM = tm;
        }
        else
            l.geomType = ogrGtSingle(l.geomType);
        std::vector<OgrFeature> out;
        long long fid = 1;
        for (OgrFeature &f : l.features)
        {
            bool coll = f.hasGeom && f.geom.type >= 4 &&
                        f.geom.type <= 7 && !f.geom.parts.empty();
            if (coll)
            {
                for (const OgrGeometry &mem : f.geom.parts)
                {
                    OgrGeometry g = mem;
                    if (T)
                    {
                        ogrSetGeomDim(g, tz, tm);
                        ogrForceTo(g, T);
                    }
                    if (skipMismatch && T && g.type != T)
                        continue;
                    OgrFeature nf;
                    nf.fid = fid++;
                    nf.explicitFid = false;
                    nf.values = f.values;
                    nf.hasGeom = true;
                    nf.geom = std::move(g);
                    out.push_back(std::move(nf));
                }
            }
            else
            {
                // non-collection features bypass both the forced
                // conversion and the mismatch skip
                OgrFeature nf = f;
                nf.fid = fid++;
                nf.explicitFid = false;
                out.push_back(std::move(nf));
            }
        }
        l.features = std::move(out);
        vectorLayerRecomputeExtent(l);
    }
    return 0;
}

int vectorSetGeomTypeApplyStep(OgrDataset &d, const SetGeomTypeOpts &o)
{
    bool hasGt = !o.geomType.empty();
    int T = 0;
    bool tz = false, tm = false;
    if (hasGt)
    {
        if (o.multi || o.single || o.linear || o.curve || !o.dim.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "set-geom-type: --geometry-type cannot be used "
                        "with any of --multi/single/linear/multi/dim");
            return 1;
        }
        if (!ogrGeomTypeFromWktName(o.geomType, T, tz, tm))
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "set-geom-type: Invalid geometry type '" +
                            o.geomType + "'");
            return 1;
        }
    }
    // -1 keep, else bit0 = Z, bit1 = M
    int dimMode = o.dim.empty() ? -1
                  : o.dim == "XY" ? 0
                  : o.dim == "XYZ" ? 1
                  : o.dim == "XYM" ? 2
                                   : 3;
    for (OgrLayer &l : d.layers)
    {
        if (!geomLayerSelected(o.layerSel, o.activeLayer, o.activeGeom,
                               l))
            continue;
        bool dropped = false;
        if (!o.layerOnly)
        {
            std::vector<OgrFeature> out;
            for (OgrFeature &f : l.features)
            {
                if (f.hasGeom)
                {
                    int target;
                    bool z, m;
                    if (hasGt)
                    {
                        target = T;
                        z = tz;
                        m = tm;
                    }
                    else
                    {
                        target = f.geom.type;
                        if (o.multi)
                            target = ogrGtCollection(target);
                        else if (o.single)
                            target = ogrGtSingle(target);
                        if (o.linear)
                            target = ogrGtLinear(target);
                        else if (o.curve)
                            target = ogrGtCurve(target);
                        z = f.geom.hasZ;
                        m = f.geom.hasM;
                        if (dimMode >= 0)
                        {
                            z = (dimMode & 1) != 0;
                            m = (dimMode & 2) != 0;
                        }
                    }
                    // a generic GEOMETRY target keeps features untouched
                    bool generic = hasGt && T == 0;
                    if (!generic)
                    {
                        // a Z-bearing target promotes to 3D up front,
                        // even when the base conversion then fails
                        if (z && !f.geom.hasZ)
                            ogrSetGeomDim(f.geom, true, f.geom.hasM);
                        if (target != 0 && f.geom.type != target)
                            ogrForceTo(f.geom, target);
                        // the full dimension normalization (Z strip, M
                        // add/strip) runs when the conversion took or
                        // the target family is linear; failed nonlinear
                        // conversions return the geometry untouched
                        bool converted =
                            target == 0 || f.geom.type == target;
                        if ((converted || target < 8) &&
                            (z != f.geom.hasZ || m != f.geom.hasM))
                            ogrSetGeomDim(f.geom, z, m);
                    }
                    if (o.skip && target != 0 && f.geom.type != target)
                    {
                        dropped = true;
                        continue;
                    }
                }
                out.push_back(std::move(f));
            }
            l.features = std::move(out);
        }
        if (!o.featureOnly)
        {
            if (hasGt)
            {
                l.geomType = T;
                if (T != 0)
                {
                    l.geomHasZ = tz;
                    l.geomHasM = tm;
                }
            }
            else
            {
                int t = l.geomType;
                if (t != 101)
                {
                    if (o.multi)
                        t = ogrGtCollection(t);
                    else if (o.single)
                        t = ogrGtSingle(t);
                    if (o.linear)
                        t = ogrGtLinear(t);
                    else if (o.curve)
                        t = ogrGtCurve(t);
                    l.geomType = t;
                }
                if (dimMode >= 0)
                {
                    l.geomHasZ = (dimMode & 1) != 0;
                    l.geomHasM = (dimMode & 2) != 0;
                }
            }
        }
        if (dropped)
            vectorLayerRecomputeExtent(l);
    }
    return 0;
}

namespace
{

void mdUpsert(std::vector<std::pair<std::string, std::string>> &md,
              const std::vector<std::string> &sets,
              const std::vector<std::string> &unsets)
{
    for (const auto &raw : sets)
    {
        size_t eq = raw.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = raw.substr(0, eq), v = raw.substr(eq + 1);
        bool found = false;
        // CSLSetNameValue: case-insensitive key match, a replace keeps
        // the slot but takes the new key spelling
        for (auto &kv : md)
            if (strEqualNoCase(kv.first, k))
            {
                kv.first = k;
                kv.second = v;
                found = true;
                break;
            }
        if (!found)
            md.emplace_back(k, v);
    }
    // removals always win regardless of option order on the command line
    for (const auto &k : unsets)
        for (size_t i = 0; i < md.size(); ++i)
            if (strEqualNoCase(md[i].first, k))
            {
                md.erase(md.begin() + i);
                break;
            }
}

}  // namespace

int vectorEditApplyStep(OgrDataset &d, const VectorEditOpts &o)
{
    mdUpsert(d.metadata, o.metadata, o.unsetMetadata);
    int gt = 0;
    bool gz = false, gm = false;
    bool hasGt = !o.geomType.empty() &&
                 ogrGeomTypeFromWktName(o.geomType, gt, gz, gm);
    for (OgrLayer &l : d.layers)
    {
        if (!vvLayerSelected(o.layerSel, o.activeLayer, l))
            continue;
        mdUpsert(l.metadata, o.layerMetadata, o.unsetLayerMetadata);
        if (hasGt)
        {
            l.geomType = gt;
            if (gt != 0)
            {
                l.geomHasZ = gz;
                l.geomHasM = gm;
            }
        }
        if (!o.crs.empty())
        {
            if (o.crs == "none" || o.crs == "null")
            {
                l.hasSrs = false;
                l.srs = Srs();
            }
            else
            {
                bool ok = false;
                Srs srs = Srs::fromCliInput(o.crs, ok, true);
                if (ok)
                {
                    l.srs = std::move(srs);
                    l.hasSrs = true;
                }
            }
        }
        if (o.unsetFid)
        {
            l.fidColumn.clear();
            for (OgrFeature &f : l.features)
            {
                f.fid = -1;
                f.explicitFid = false;
            }
        }
    }
    return 0;
}

namespace
{

// derived by probing: Latin-1 Supplement + Latin Extended-A
// transliterations the reference's --ascii mode applies (note the
// reference maps U+00FF to 'u')
static const char *const kAsciiMap[0x17F - 0xC0] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I",
    "I", "I", nullptr, "N", "O", "O", "O", "O", "O", nullptr, "O", "U",
    "U", "U", "U", "Y", nullptr, "SS", "a", "a", "a", "a", "a", "a", "ae",
    "c", "e", "e", "e", "e", "i", "i", "i", "i", nullptr, "n", "o", "o",
    "o", "o", "o", nullptr, "o", "u", "u", "u", "u", "y", nullptr, "u",
    "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c",
    "D", "d", "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e",
    "G", "g", "G", "g", "G", "g", "G", "g", "H", "h", "H", "h", "I", "i",
    "I", "i", "I", "i", "I", "i", "I", "i", "IJ", "ij", "J", "j", "K", "k",
    "k", "L", "l", "L", "l", "L", "l", "L", "l", "L", "l", "N", "n", "N",
    "n", "N", "n", nullptr, nullptr, nullptr, "O", "o", "O", "o", "O", "o",
    "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s",
    "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u",
    "U", "u", "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z",
    "z", "Z", "z"
};

std::string renameLayerTransform(const std::string &in,
                                 const VectorRenameLayerOpts &o)
{
    std::string s = in;
    if (!o.reserved.empty())
    {
        std::string out;
        for (char c : s)
        {
            if (o.reserved.find(c) != std::string::npos)
                out += o.replacement;
            else
                out += c;
        }
        s = std::move(out);
    }
    if (o.ascii)
    {
        // blind UTF-8 decode: continuation bytes are consumed unchecked,
        // a sequence truncated by end-of-string vanishes silently, and
        // stray continuation/invalid lead bytes take the replacement
        std::string out;
        size_t i = 0, n = s.size();
        while (i < n)
        {
            unsigned char b = (unsigned char)s[i];
            if (b < 0x80)
            {
                out += (char)b;
                ++i;
                continue;
            }
            int len = b >= 0xF8 ? 1
                      : b >= 0xF0 ? 4
                      : b >= 0xE0 ? 3
                      : b >= 0xC0 ? 2
                                  : 1;
            if (len == 1)
            {
                out += o.replacement;
                ++i;
                continue;
            }
            if (i + len > n)
                break;
            unsigned cp = b & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (int k = 1; k < len; ++k)
                cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
            i += len;
            if (cp < 0x80)
                out += (char)cp;
            else if (cp >= 0xC0 && cp <= 0x17E && kAsciiMap[cp - 0xC0])
                out += kAsciiMap[cp - 0xC0];
            else
                out += o.replacement;
        }
        s = std::move(out);
    }
    if (o.lowerCase)
        for (char &c : s)
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
    if (o.fnCompat)
    {
        static const std::string illegal = "\\/:*?\"<>|^";
        std::string out;
        for (char c : s)
        {
            if ((unsigned char)c < 0x20 || c == '\x7f' ||
                illegal.find(c) != std::string::npos)
                out += o.replacement;
            else
                out += c;
        }
        if (!out.empty() && (out.back() == ' ' || out.back() == '.'))
            out += o.replacement.empty() ? "_" : o.replacement;
        s = std::move(out);
    }
    if (o.maxLength >= 0 && (long long)s.size() > o.maxLength)
        s.resize((size_t)o.maxLength);
    return s;
}

}  // namespace

int vectorRenameLayerValidate(const OgrDataset &d,
                              const VectorRenameLayerOpts &o)
{
    if (!o.inputLayer.empty())
    {
        bool found = false;
        for (const OgrLayer &l : d.layers)
            if (l.name == o.inputLayer)
                found = true;
        if (!found)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "rename-layer: Input layer '" + o.inputLayer +
                            "' does not exist");
            return 1;
        }
    }
    else if (!o.outputLayer.empty() && d.layers.size() > 1)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "rename-layer: Argument input-layer must be "
                    "specified when output-layer is specified and there "
                    "is more than one layer");
        return 1;
    }
    return 0;
}

int vectorRenameLayerApplyStep(OgrDataset &d,
                               const VectorRenameLayerOpts &o)
{
    if (!o.inputLayer.empty() && o.outputLayer.empty())
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "rename-layer: Argument output-layer must be "
                    "specified when input-layer is specified");
        return 1;
    }
    if (int rc = vectorRenameLayerValidate(d, o))
        return rc;
    if (!o.inputLayer.empty())
    {
        // the selected layer takes the explicit name and moves to the
        // front; the rest keep their order and get the transforms
        for (size_t i = 0; i < d.layers.size(); ++i)
            if (d.layers[i].name == o.inputLayer)
            {
                d.layers[i].name = o.outputLayer;
                std::rotate(d.layers.begin(), d.layers.begin() + i,
                            d.layers.begin() + i + 1);
                break;
            }
        for (size_t i = 1; i < d.layers.size(); ++i)
            d.layers[i].name = renameLayerTransform(d.layers[i].name, o);
    }
    else if (!o.outputLayer.empty())
    {
        if (!d.layers.empty())
            d.layers[0].name = o.outputLayer;
    }
    else
    {
        for (OgrLayer &l : d.layers)
            l.name = renameLayerTransform(l.name, o);
    }
    return 0;
}

namespace
{

int vectorSwapXyHandler(const CmdSpec &cmd, ParseResult &r)
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

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + vvGq(r.str("active-layer"));
    if (!r.str("active-geometry").empty())
        cli += " --active-geometry " + vvGq(r.str("active-geometry"));
    cli += " --output-format stream --output streamed_dataset";

    std::string ag = r.str("active-geometry");
    std::string al = r.str("active-layer");
    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [ag, al, sel](OgrDataset &d) -> int
    { return vectorSwapXyApplyStep(d, ag, al, sel); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

int vectorSegmentizeHandler(const CmdSpec &cmd, ParseResult &r)
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

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + vvGq(r.str("active-layer"));
    if (!r.str("active-geometry").empty())
        cli += " --active-geometry " + vvGq(r.str("active-geometry"));
    cli += " --max-length " + vvFmtReal(r.str("max-length"));
    cli += " --output-format stream --output streamed_dataset";

    double maxLength = strtod(r.str("max-length").c_str(), nullptr);
    std::string ag = r.str("active-geometry");
    std::string al = r.str("active-layer");
    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [maxLength, ag, al, sel](OgrDataset &d) -> int
    { return vectorSegmentizeApplyStep(d, maxLength, ag, al, sel); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

int vectorMakePointHandler(const CmdSpec &cmd, ParseResult &r)
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

    std::string cli = vvGdalgHead(r, input, true, true);
    cli += " --x " + vvGq(r.str("x"));
    cli += " --y " + vvGq(r.str("y"));
    if (!r.str("z").empty())
        cli += " --z " + vvGq(r.str("z"));
    if (!r.str("m").empty())
        cli += " --m " + vvGq(r.str("m"));
    if (!r.str("dst-crs").empty())
        cli += " --dst-crs " + vvGq(r.str("dst-crs"));
    cli += " --output-format stream --output streamed_dataset";

    std::string xf = r.str("x"), yf = r.str("y"), zf = r.str("z"),
                mf = r.str("m"), dc = r.str("dst-crs");
    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [xf, yf, zf, mf, dc, sel](OgrDataset &d) -> int
    { return vectorMakePointApplyStep(d, xf, yf, zf, mf, dc, sel); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

int vectorExplodeHandler(const CmdSpec &cmd, ParseResult &r)
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

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + vvGq(r.str("active-layer"));
    if (!r.str("active-geometry").empty())
        cli += " --active-geometry " + vvGq(r.str("active-geometry"));
    if (!r.str("geometry-type").empty())
        cli += " --geometry-type " + vvGq(r.str("geometry-type"));
    if (r.flag("skip-on-type-mismatch"))
        cli += " --skip-on-type-mismatch";
    cli += " --output-format stream --output streamed_dataset";

    std::string gt = r.str("geometry-type");
    bool skip = r.flag("skip-on-type-mismatch");
    std::string ag = r.str("active-geometry");
    std::string al = r.str("active-layer");
    std::vector<std::string> sel = r.list("input-layer");
    auto mutate = [gt, skip, ag, al, sel](OgrDataset &d) -> int
    { return vectorExplodeApplyStep(d, gt, skip, ag, al, sel); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

int vectorSetGeomTypeHandler(const CmdSpec &cmd, ParseResult &r)
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

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + vvGq(r.str("active-layer"));
    if (!r.str("active-geometry").empty())
        cli += " --active-geometry " + vvGq(r.str("active-geometry"));
    if (r.flag("layer-only"))
        cli += " --layer-only";
    if (r.flag("feature-only"))
        cli += " --feature-only";
    if (!r.str("geometry-type").empty())
        cli += " --geometry-type " + vvGq(r.str("geometry-type"));
    if (r.flag("multi"))
        cli += " --multi";
    if (r.flag("single"))
        cli += " --single";
    if (r.flag("linear"))
        cli += " --linear";
    if (r.flag("curve"))
        cli += " --curve";
    if (!r.str("dim").empty())
        cli += " --dim " + vvGq(r.str("dim"));
    if (r.flag("skip"))
        cli += " --skip";
    cli += " --output-format stream --output streamed_dataset";

    SetGeomTypeOpts o;
    o.geomType = r.str("geometry-type");
    o.multi = r.flag("multi");
    o.single = r.flag("single");
    o.linear = r.flag("linear");
    o.curve = r.flag("curve");
    o.dim = r.str("dim");
    o.layerOnly = r.flag("layer-only");
    o.featureOnly = r.flag("feature-only");
    o.skip = r.flag("skip");
    o.activeGeom = r.str("active-geometry");
    o.activeLayer = r.str("active-layer");
    o.layerSel = r.list("input-layer");
    auto mutate = [o](OgrDataset &d) -> int
    { return vectorSetGeomTypeApplyStep(d, o); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

int vectorEditHandler(const CmdSpec &cmd, ParseResult &r)
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

    std::string cli = vvGdalgHead(r, input, true, true);
    if (!r.str("active-layer").empty())
        cli += " --active-layer " + vvGq(r.str("active-layer"));
    if (!r.str("geometry-type").empty())
        cli += " --geometry-type " + vvGq(r.str("geometry-type"));
    if (!r.str("crs").empty())
        cli += " --crs " + vvGq(r.str("crs"));
    for (const auto &v : r.list("metadata"))
        cli += " --metadata " + vvGq(v);
    auto listEcho = [&](const char *key) {
        std::string joined;
        for (const auto &raw : r.list(key))
            for (const auto &p : strSplit(raw, ','))
            {
                if (!joined.empty())
                    joined += ",";
                joined += vvGq(p);
            }
        return joined;
    };
    if (!r.list("unset-metadata").empty())
        cli += " --unset-metadata " + listEcho("unset-metadata");
    for (const auto &v : r.list("layer-metadata"))
        cli += " --layer-metadata " + vvGq(v);
    if (!r.list("unset-layer-metadata").empty())
        cli += " --unset-layer-metadata " +
               listEcho("unset-layer-metadata");
    if (r.flag("unset-fid"))
        cli += " --unset-fid";
    cli += " --output-format stream --output streamed_dataset";

    VectorEditOpts o;
    o.geomType = r.str("geometry-type");
    o.crs = r.str("crs");
    o.metadata = r.list("metadata");
    o.unsetMetadata = r.list("unset-metadata");
    o.layerMetadata = r.list("layer-metadata");
    o.unsetLayerMetadata = r.list("unset-layer-metadata");
    o.unsetFid = r.flag("unset-fid");
    o.activeLayer = r.str("active-layer");
    o.layerSel = r.list("input-layer");
    auto mutate = [o](OgrDataset &d) -> int
    { return vectorEditApplyStep(d, o); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                          mutate);
}

VectorRenameLayerOpts renameLayerOptsFromResult(ParseResult &r)
{
    VectorRenameLayerOpts o;
    o.inputLayer = r.str("input-layer");
    o.outputLayer = r.str("output-layer");
    o.ascii = r.flag("ascii");
    o.lowerCase = r.flag("lower-case");
    o.fnCompat = r.flag("filename-compatible");
    o.reserved = r.str("reserved-characters");
    o.hasReplacement = r.get("replacement-character") != nullptr;
    o.replacement = r.str("replacement-character");
    if (r.get("max-length"))
        o.maxLength = strtoll(r.str("max-length").c_str(), nullptr, 10);
    return o;
}

int vectorRenameLayerHandler(const CmdSpec &cmd, ParseResult &r)
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

    VectorRenameLayerOpts o = renameLayerOptsFromResult(r);
    std::unique_ptr<OgrDataset> ds;
    if (int rc = vvOpenInputDs(cmd, r, input, ds))
        return rc;
    if (vectorRenameLayerValidate(*ds, o))
    {
        handlerPrintUsage();
        return 1;
    }

    std::string cli = vvGdalgHead(r, input, false, true);
    if (!o.inputLayer.empty())
        cli += " --input-layer " + vvGq(o.inputLayer);
    if (o.ascii)
        cli += " --ascii";
    if (o.lowerCase)
        cli += " --lower-case";
    if (o.fnCompat)
        cli += " --filename-compatible";
    if (r.get("reserved-characters"))
        cli += " --reserved-characters " + vvGq(o.reserved);
    if (o.hasReplacement)
        cli += " --replacement-character " + vvGq(o.replacement);
    if (r.get("max-length"))
        cli += " --max-length " + r.str("max-length");
    cli += " --output-format stream --output streamed_dataset";

    auto mutate = [o](OgrDataset &d) -> int
    { return vectorRenameLayerApplyStep(d, o); };
    return vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, false,
                          mutate);
}

}  // namespace

void registerVectorGeomVerbHandlers()
{
    registerHandler("vector_swap-xy", vectorSwapXyHandler);
    registerArgCheck("vector_swap-xy",
                     [](const std::string &a, ParseResult &r)
                     { return vvVerbFormatArgCheck("swap-xy", a, r); });
    registerHandler("vector_segmentize", vectorSegmentizeHandler);
    registerArgCheck("vector_segmentize",
                     [](const std::string &a, ParseResult &r)
                     { return vvVerbFormatArgCheck("segmentize", a, r); });
    registerHandler("vector_make-point", vectorMakePointHandler);
    registerArgCheck("vector_make-point",
                     [](const std::string &a, ParseResult &r)
                     { return vvVerbFormatArgCheck("make-point", a, r); });
    registerArgValueCheck(
        "vector_make-point",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName != "dst-crs")
                return "";
            bool ok = false;
            Srs::fromCliInput(value, ok, true);
            if (!ok)
                return "Invalid value for 'dst-crs' argument";
            return "";
        });
    registerHandler("vector_explode-collections", vectorExplodeHandler);
    registerArgCheck("vector_explode-collections",
                     [](const std::string &a, ParseResult &r) {
                         return vvVerbFormatArgCheck("explode-collections",
                                                     a, r);
                     });
    registerHandler("vector_set-geom-type", vectorSetGeomTypeHandler);
    registerArgCheck("vector_set-geom-type",
                     [](const std::string &a, ParseResult &r) {
                         return vvVerbFormatArgCheck("set-geom-type", a,
                                                     r);
                     });
    registerArgValueCheck(
        "vector_set-geom-type",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName != "geometry-type")
                return "";
            int t;
            bool z, m;
            if (!ogrGeomTypeFromWktName(value, t, z, m))
                return "Invalid geometry type '" + value + "'";
            return "";
        });
    registerHandler("vector_rename-layer", vectorRenameLayerHandler);
    registerArgCheck("vector_rename-layer",
                     [](const std::string &a, ParseResult &r)
                     { return vvVerbFormatArgCheck("rename-layer", a, r); });
    // the pairing check fires after the input open probe even when the
    // open itself failed; both errors share one usage block
    registerPostValidator(
        "vector_rename-layer",
        [](const CmdSpec &, ParseResult &r, bool) -> bool
        {
            if (!r.str("input-layer").empty() &&
                r.str("output-layer").empty())
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "rename-layer: Argument output-layer must "
                            "be specified when input-layer is "
                            "specified");
                return true;
            }
            return false;
        });
    registerArgValueCheck(
        "vector_rename-layer",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "replacement-character" && value.size() > 1)
                return "\x06Value of argument 'replacement-character' "
                       "is '" +
                       value + "', but should have no more than 1 "
                               "character";
            return "";
        });
    registerHandler("vector_edit", vectorEditHandler);
    registerArgCheck("vector_edit",
                     [](const std::string &a, ParseResult &r)
                     { return vvVerbFormatArgCheck("edit", a, r); });
    registerArgValueCheck(
        "vector_edit",
        [](const std::string &argName,
           const std::string &value) -> std::string
        {
            if (argName == "crs" && value != "none" && value != "null")
            {
                bool ok = false;
                Srs::fromCliInput(value, ok, true);
                if (!ok)
                    return "Invalid value for 'crs' argument";
            }
            if (argName == "geometry-type")
            {
                int t;
                bool z, m;
                if (!ogrGeomTypeFromWktName(value, t, z, m))
                    return "Invalid geometry type '" + value + "'";
            }
            return "";
        });
}
