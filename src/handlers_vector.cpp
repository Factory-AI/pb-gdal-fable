#include "cpl.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "ogrsql.h"
#include "util.h"
#include "vsi.h"
#include <climits>
#include <cmath>
#include <cstring>
#include <sys/stat.h>

std::string crsAreaDisplayName(const std::string &raw);
bool crsProjectionInfoShared(const Srs &srs, std::string &convName,
                             std::string &methodName);
std::string crsUnitsNameShared(const Srs &srs);

namespace
{

JVal jint(long long i)
{
    JVal v;
    v.type = JVal::INT;
    v.i = i;
    return v;
}

JVal jstr(const std::string &s)
{
    JVal v;
    v.type = JVal::STRING;
    v.s = s;
    return v;
}

JVal jobj()
{
    JVal v;
    v.type = JVal::OBJECT;
    return v;
}

JVal jarr()
{
    JVal v;
    v.type = JVal::ARRAY;
    return v;
}

const char *kVectorDrivers[] = {"ESRI Shapefile", "GeoJSON", "MEM"};

void warnUnsupportedOpenOptions(const char *driverName,
                                const std::vector<std::string> &openOptions)
{
    static const char *geojsonOO[] = {
        "FLATTEN_NESTED_ATTRIBUTES", "NESTED_ATTRIBUTE_SEPARATOR",
        "FEATURE_SERVER_PAGING",     "NATIVE_DATA",
        "ARRAY_AS_STRING",           "DATE_AS_STRING",
        "FOREIGN_MEMBERS",           "OGR_SCHEMA"};
    static const char *shapefileOO[] = {
        "ENCODING",         "DBF_DATE_LAST_UPDATE", "ADJUST_TYPE",
        "ADJUST_GEOM_TYPE", "AUTO_REPACK",          "DBF_EOF_CHAR"};
    bool isShp = strcmp(driverName, "ESRI Shapefile") == 0;
    for (const auto &kv : openOptions)
    {
        std::string key = kv.substr(0, kv.find('='));
        bool known = false;
        if (isShp)
        {
            for (const char *k : shapefileOO)
                if (strEqualNoCase(key, k))
                    known = true;
        }
        else
        {
            for (const char *k : geojsonOO)
                if (strEqualNoCase(key, k))
                    known = true;
        }
        if (!known)
        {
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        std::string("driver ") + driverName +
                            " does not support open option " + key);
            continue;
        }
        if (isShp && strEqualNoCase(key, "ADJUST_GEOM_TYPE"))
        {
            size_t eq = kv.find('=');
            std::string val = eq == std::string::npos ? "" : kv.substr(eq + 1);
            if (!strEqualNoCase(val, "FIRST_SHAPE") &&
                !strEqualNoCase(val, "ALL_SHAPES") &&
                !strEqualNoCase(val, "NO"))
                cplErrorStr(CE_Warning, CPLE_NotSupported,
                            "'" + val +
                                "' is an unexpected value for "
                                "ADJUST_GEOM_TYPE open option of type "
                                "string-select.");
        }
    }
}

std::unique_ptr<OgrDataset> openVector(const std::string &path,
                                       std::string &err,
                                       const std::vector<std::string> &ifmt,
                                       const std::vector<std::string> &oo,
                                       bool seqOpenErrors = true)
{
    auto allowed = [&](const char *name) {
        if (gdalSkipHas(name))
            return false;
        if (ifmt.empty())
            return true;
        for (const auto &d : ifmt)
            if (strEqualNoCase(d, name))
                return true;
        return false;
    };
    auto announce = [&](std::unique_ptr<OgrDataset> &d) {
        if (d)
            ogrDebugAnnounceOpen(*d);
    };
    struct stat st;
    if (vsiIsVirtual(path))
    {
        if (!vsiExists(path))
        {
            err = "missing";
            return nullptr;
        }
    }
    else if (stat(path.c_str(), &st) != 0)
    {
        err = "missing";
        return nullptr;
    }
    if (allowed("GeoJSONSeq"))
    {
        auto ds = openGeoJsonSeq(path, err);
        if (ds)
        {
            // the opening scan replays parse failures; update-mode
            // target opens skip the scan
            if (seqOpenErrors)
                for (const auto &lyr : ds->layers)
                    for (const auto &e : lyr.seqEvents)
                        if (!e.isDiag)
                            cplErrorStr((CPLErrClass)e.sev,
                                        CPLE_AppDefined, e.msg);
            warnUnsupportedOpenOptions("GeoJSONSeq", oo);
            announce(ds);
        }
        if (ds || err == "reported")
            return ds;
    }
    if (allowed("GeoJSON"))
    {
        auto ds = openGeoJson(path, err);
        if (ds)
        {
            warnUnsupportedOpenOptions("GeoJSON", oo);
            announce(ds);
        }
        if (ds || err == "reported")
            return ds;
    }
    if (allowed("ESRIJSON"))
    {
        auto ds = openEsriJson(path, err);
        if (ds)
        {
            warnUnsupportedOpenOptions("ESRIJSON", oo);
            announce(ds);
        }
        if (ds || err == "reported")
            return ds;
    }
    if (allowed("TopoJSON"))
    {
        auto ds = openTopoJson(path, err);
        if (ds)
        {
            warnUnsupportedOpenOptions("TopoJSON", oo);
            announce(ds);
        }
        if (ds || err == "reported")
            return ds;
    }
    if (allowed("GeoJSON"))
    {
        auto ds = openGeoJson(path, err, true);
        if (ds)
        {
            warnUnsupportedOpenOptions("GeoJSON", oo);
            announce(ds);
        }
        if (ds || err == "reported")
            return ds;
    }
    if (allowed("ESRI Shapefile"))
    {
        auto ds = openShapefile(path, err, oo);
        if (ds)
        {
            warnUnsupportedOpenOptions("ESRI Shapefile", oo);
            announce(ds);
        }
        if (ds || err == "reported")
            return ds;
    }
    err = "";
    return nullptr;
}

std::string fieldTypeDisplay(const OgrFieldDefn &f)
{
    std::string t = ogrFieldTypeName(f.type);
    if (f.subType != OFSTNone)
        t += "(" + ogrFieldSubTypeName(f.subType) + ")";
    return t;
}

std::string tzSuffix(const OgrFieldDefn &f)
{
    if (f.type != OFTDateTime)
        return "";
    if (f.tzAggr == -2)
        return " (mixed timezones)";
    if (f.tzAggr == 100)
        return " (UTC)";
    if (f.tzAggr > 1)
    {
        int off = (f.tzAggr - 100) * 15;
        char sign = off < 0 ? '-' : '+';
        int a = off < 0 ? -off : off;
        return strPrintf(" (%c%02d:%02d)", sign, a / 60, a % 60);
    }
    return "";
}

// text dump of a field value per OGRFeature::DumpReadable conventions
std::string fieldValueText(const OgrFieldDefn &f, const JVal &v)
{
    if (v.type == JVal::NUL)
        return "(null)";
    switch (f.type)
    {
        case OFTInteger:
        case OFTInteger64:
        {
            long long i = v.type == JVal::BOOL ? (v.b ? 1 : 0)
                          : v.type == JVal::INT ? v.i
                          : v.type == JVal::DOUBLE ? (long long)v.d
                                                   : atoll(v.s.c_str());
            return strPrintf("%lld", i);
        }
        case OFTReal:
        {
            double d = v.type == JVal::INT      ? (double)v.i
                       : v.type == JVal::DOUBLE ? v.d
                                                : atof(v.s.c_str());
            if (f.width > 0 && f.precision > 0)
                return strPrintf("%.*f", f.precision, d);
            if (f.subType == OFSTFloat32)
            {
                // dump clamps to float at print time and keeps a
                // decimal marker on integral values
                double c = (double)(float)d;
                std::string s = ogrFormatDouble(c, 8);
                if (std::isfinite(c) &&
                    s.find_first_of(".eE") == std::string::npos)
                    s += ".0";
                return s;
            }
            return ogrFormatDouble(d, 15);
        }
        case OFTIntegerList:
        case OFTInteger64List:
        {
            std::vector<long long> vals;
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    vals.push_back(e.type == JVal::BOOL ? (e.b ? 1 : 0)
                                   : e.type == JVal::INT ? e.i
                                   : e.type == JVal::DOUBLE ? (long long)e.d
                                                            : 0);
            else
                vals.push_back(v.type == JVal::INT ? v.i
                               : v.type == JVal::BOOL ? (v.b ? 1 : 0)
                                                      : 0);
            std::string out = strPrintf("(%d:", (int)vals.size());
            for (size_t i = 0; i < vals.size(); i++)
            {
                if (i)
                    out += ",";
                out += strPrintf("%lld", vals[i]);
            }
            out += ")";
            return out;
        }
        case OFTRealList:
        {
            std::vector<double> vals;
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    vals.push_back(e.type == JVal::INT ? (double)e.i
                                   : e.type == JVal::DOUBLE ? e.d
                                                            : 0.0);
            else
                vals.push_back(v.type == JVal::INT ? (double)v.i
                               : v.type == JVal::DOUBLE ? v.d
                                                        : 0.0);
            std::string out = strPrintf("(%d:", (int)vals.size());
            for (size_t i = 0; i < vals.size(); i++)
            {
                if (i)
                    out += ",";
                out += ogrFormatDouble(vals[i], 15);
            }
            out += ")";
            return out;
        }
        case OFTStringList:
        {
            std::vector<std::string> vals;
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    vals.push_back(e.type == JVal::STRING
                                       ? e.s
                                       : jsoncSerialize(e, false));
            std::string out = strPrintf("(%d:", (int)vals.size());
            for (size_t i = 0; i < vals.size(); i++)
            {
                if (i)
                    out += ",";
                out += vals[i];
            }
            out += ")";
            return out;
        }
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
        {
            OgrDateTime dt;
            if (v.type == JVal::STRING && ogrParseDate(v.s, dt))
                return ogrDateTimeToString(dt, f.type);
            return v.type == JVal::STRING ? v.s : "";
        }
        default:
        {
            if (v.type == JVal::STRING)
                return v.s;
            return ogrJsonSpacedSerialize(v);
        }
    }
}

void dumpFeatureText(std::string &out, const OgrLayer &lyr,
                     const OgrFeature &feat)
{
    out += strPrintf("OGRFeature(%s):%lld\n", lyr.name.c_str(), feat.fid);
    for (size_t i = 0; i < lyr.fields.size(); i++)
    {
        if (i >= feat.values.size() || !feat.values[i].set)
            continue;
        const OgrFieldDefn &f = lyr.fields[i];
        out += strPrintf("  %s (%s) = %s\n", f.name.c_str(),
                         fieldTypeDisplay(f).c_str(),
                         fieldValueText(f, feat.values[i].v).c_str());
    }
    if (feat.hasStyle)
        out += "  Style = " + feat.style + "\n";
    if (feat.hasGeom)
        out += "  " + ogrWkt(feat.geom) + "\n";
    out += "\n";
}

void crsBlockText(std::string &out, const Srs &srs,
                  const std::string &crsFormat)
{
    std::string auth = srs.authName(), code = srs.code();
    bool asProjjson = strEqualNoCase(crsFormat, "PROJJSON");
    bool asWkt = strEqualNoCase(crsFormat, "WKT2") ||
                 (!asProjjson && (auth.empty() || code.empty()));
    if (asProjjson)
    {
        out += "Layer Coordinate Reference System PROJJSON:\n" +
               srs.projjson() + "\n";
        return;
    }
    if (asWkt)
    {
        out += "Layer Coordinate Reference System WKT:\n" +
               srs.wkt2_2019() + "\n";
        return;
    }
    out += "Layer Coordinate Reference System:\n";
    out += strPrintf("  - name: %s\n", srs.name().c_str());
    if (!auth.empty() && !code.empty())
        out += strPrintf("  - ID: %s:%s\n", auth.c_str(), code.c_str());
    out += strPrintf("  - type: %s\n", srs.typeString().c_str());
    if (srs.isProjected())
    {
        std::string conv, method;
        if (crsProjectionInfoShared(srs, conv, method))
        {
            std::string convNorm = conv;
            for (char &c : convNorm)
                if (c == '-')
                    c = ' ';
            if (convNorm == method)
                out += strPrintf("  - projection type: %s\n", conv.c_str());
            else
                out += strPrintf("  - projection type: %s, %s\n",
                                 conv.c_str(), method.c_str());
        }
        std::string units = crsUnitsNameShared(srs);
        if (!units.empty())
            out += strPrintf("  - units: %s\n", units.c_str());
    }
    double w, s, e, n;
    std::string areaName;
    if (srs.areaOfUse(w, s, e, n, areaName))
        out += strPrintf("  - area of use: %s, west %.2f, south %.2f, east "
                         "%.2f, north %.2f\n",
                         crsAreaDisplayName(areaName).c_str(), w, s, e, n);
}

struct VectorInfoOpts
{
    bool features = false;
    bool summary = false;
    bool hasLimit = false;
    long long limit = -1;
    bool hasFid = false;
    long long fid = -1;
    std::string crsFormat = "AUTO";
    std::vector<std::string> layerFilter;
    // -l materializes the layer selection up front; these flags carry
    // the flavor: filterMode (leaf/info-step -l) renders full blocks
    // under --summary and omits json relationships, driverDrop (leaf
    // and read-step -l) nulls the driver identity
    bool hasLayerArg = false;
    bool filterMode = false;
    bool driverDrop = false;
};

bool layerSelected(const VectorInfoOpts &o, const std::string &name)
{
    if (o.layerFilter.empty())
        return true;
    for (const auto &l : o.layerFilter)
        if (l == name)
            return true;
    return false;
}

std::string buildText(const OgrDataset &ds, const VectorInfoOpts &o)
{
    std::string out;
    out += strPrintf("INFO: Open of `%s'\n      using driver `%s' "
                     "successful.\n",
                     g_pipelineConcatInfo ? "" : ds.path.c_str(),
                     !o.driverDrop && !g_pipelineConcatInfo &&
                             !g_pipelineStreamInfo
                         ? ds.driverShort.c_str()
                         : "(null)");
    if (!ds.metadata.empty() && !o.summary)
    {
        out += "Metadata:\n";
        for (const auto &kv : ds.metadata)
            out += strPrintf("  %s=%s\n", kv.first.c_str(),
                             kv.second.c_str());
    }
    bool sumList = o.summary && !o.filterMode;
    if (sumList)
    {
        for (size_t i = 0; i < ds.layers.size(); i++)
        {
            const OgrLayer &lyr = ds.layers[i];
            if (!layerSelected(o, lyr.name))
                continue;
            std::string g = ogrGeomTypeName(lyr.geomType, lyr.geomHasZ,
                                            lyr.geomHasM);
            if (lyr.geomType == 0)
                out += strPrintf("%d: %s\n", (int)i + 1, lyr.name.c_str());
            else
                out += strPrintf("%d: %s (%s)\n", (int)i + 1,
                                 lyr.name.c_str(), g.c_str());
        }
        return out;
    }
    for (const OgrLayer &lyr : ds.layers)
    {
        if (!layerSelected(o, lyr.name))
            continue;
        if (lyr.filterFailed)
            continue;
        // the blank separator precedes every layer block, so an empty
        // dataset ends right after the INFO header
        out += "\n";
        out += strPrintf("Layer name: %s\n", lyr.name.c_str());
        if (!lyr.metadata.empty() && !o.summary)
        {
            out += "Metadata:\n";
            for (const auto &kv : lyr.metadata)
                out += strPrintf("  %s=%s\n", kv.first.c_str(),
                                 kv.second.c_str());
        }
        out += strPrintf("Geometry: %s\n",
                         ogrGeomTypeName(lyr.geomType, lyr.geomHasZ,
                                         lyr.geomHasM)
                             .c_str());
        out += strPrintf("Feature Count: %lld\n",
                         lyr.countOverride >= 0
                             ? lyr.countOverride
                             : (long long)lyr.features.size());
        if (lyr.hasExtent)
            out += strPrintf("Extent: (%f, %f) - (%f, %f)\n", lyr.extent[0],
                             lyr.extent[1], lyr.extent[2], lyr.extent[3]);
        if (!lyr.hasSrs)
            out += "Layer Coordinate Reference System: none\n";
        if (lyr.hasSrs)
        {
            crsBlockText(out, lyr.srs, o.crsFormat);
            auto mapping = lyr.srs.dataAxisToSRSAxisMapping();
            std::string m;
            for (size_t i = 0; i < mapping.size(); ++i)
            {
                if (i)
                    m += ",";
                m += strPrintf("%d", mapping[i]);
            }
            out += strPrintf("Data axis to CRS axis mapping: %s\n",
                             m.c_str());
        }
        if (!lyr.fidColumn.empty())
            out += strPrintf("FID Column = %s\n", lyr.fidColumn.c_str());
        if (!lyr.geomColumnName.empty())
            out += strPrintf("Geometry Column = %s\n",
                             lyr.geomColumnName.c_str());
        for (const OgrFieldDefn &f : lyr.fields)
        {
            std::string alt =
                f.altName.empty()
                    ? ""
                    : strPrintf(", alternative name=\"%s\"",
                                f.altName.c_str());
            if (f.type == OFTDate || f.type == OFTTime ||
                f.type == OFTDateTime)
                out += strPrintf("%s: %s%s%s\n", f.name.c_str(),
                                 fieldTypeDisplay(f).c_str(),
                                 tzSuffix(f).c_str(), alt.c_str());
            else
                out += strPrintf("%s: %s (%d.%d)%s\n", f.name.c_str(),
                                 fieldTypeDisplay(f).c_str(), f.width,
                                 f.precision, alt.c_str());
        }
        if (o.features || o.hasFid)
        {
            long long emitted = 0;
            for (const OgrFeature &feat : lyr.features)
            {
                if (o.hasFid && feat.fid != o.fid)
                    continue;
                if (o.hasLimit && emitted >= o.limit)
                    break;
                dumpFeatureText(out, lyr, feat);
                emitted++;
            }
            // negative ids are OGRNullFID territory: no lookup happens
            // and no message is printed
            if (o.hasFid && o.fid >= 0 && emitted == 0)
                out += strPrintf(
                    "Unable to locate feature id %lld on this layer.\n",
                    o.fid);
        }
    }
    return out;
}

JVal jdbl(double d)
{
    JVal j;
    j.type = JVal::DOUBLE;
    j.d = d;
    j.s = ogrJsonDouble(d);
    return j;
}

JVal fieldValueJson(const OgrFieldDefn &f, const JVal &v)
{
    if (v.type == JVal::NUL)
        return JVal();
    auto toI = [](const JVal &e) -> long long {
        return e.type == JVal::BOOL     ? (e.b ? 1 : 0)
               : e.type == JVal::INT    ? e.i
               : e.type == JVal::DOUBLE ? (long long)e.d
                                        : atoll(e.s.c_str());
    };
    auto toD = [](const JVal &e) -> double {
        return e.type == JVal::BOOL     ? (e.b ? 1 : 0)
               : e.type == JVal::INT    ? (double)e.i
               : e.type == JVal::DOUBLE ? e.d
                                        : atof(e.s.c_str());
    };
    switch (f.type)
    {
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
        {
            OgrDateTime dt;
            if (v.type == JVal::STRING && ogrParseDate(v.s, dt))
                return jstr(ogrDateTimeToString(dt, f.type));
            return v.type == JVal::STRING ? jstr(v.s) : JVal();
        }
        case OFTReal:
            // Float32 renders unclamped: json shows the stored double
            return jdbl(toD(v));
        case OFTInteger:
        case OFTInteger64:
            if (f.subType == OFSTBoolean)
            {
                JVal b;
                b.type = JVal::BOOL;
                b.b = toI(v) != 0;
                return b;
            }
            return jint(toI(v));
        case OFTIntegerList:
        case OFTInteger64List:
        {
            JVal a = jarr();
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    a.arr.push_back(jint(toI(e)));
            else
                a.arr.push_back(jint(toI(v)));
            return a;
        }
        case OFTRealList:
        {
            JVal a = jarr();
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    a.arr.push_back(jdbl(toD(e)));
            else
                a.arr.push_back(jdbl(toD(v)));
            return a;
        }
        case OFTStringList:
        {
            JVal a = jarr();
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    a.arr.push_back(e.type == JVal::STRING
                                        ? e
                                        : jstr(ogrJsonSpacedSerialize(e)));
            else
                a.arr.push_back(v.type == JVal::STRING
                                    ? v
                                    : jstr(ogrJsonSpacedSerialize(v)));
            return a;
        }
        case OFTString:
            // String(JSON) echoes the raw value; a stored string that
            // parses as JSON embeds its parsed form, a plain String
            // field quotes the json-c text form of scalars stored raw
            if (f.subType == OFSTJSON)
            {
                if (v.type != JVal::STRING)
                    return v;
                bool ok = false;
                JVal parsed = JVal::parse(v.s, &ok);
                return ok ? parsed : v;
            }
            if (v.type == JVal::STRING)
                return v;
            return jstr(ogrJsonSpacedSerialize(v));
        default:
            return v;
    }
}

JVal geomToJson(const OgrGeometry &g);

JVal jdbl(double d);

JVal coordTupleJson(const double *c, bool hasZ)
{
    JVal a = jarr();
    for (int i = 0; i < (hasZ ? 3 : 2); i++)
    {
        JVal j;
        j.type = JVal::DOUBLE;
        j.d = c[i];
        j.s = ogrJsonCoord(c[i]);
        a.arr.push_back(std::move(j));
    }
    return a;
}

JVal coordListJson(const OgrGeometry &g, bool hasZ)
{
    JVal a = jarr();
    for (size_t i = 0; i + 2 < g.coords.size() + 1; i += 3)
        a.arr.push_back(coordTupleJson(&g.coords[i], hasZ));
    return a;
}

JVal geomToJson(const OgrGeometry &g)
{
    static const char *names[] = {"",
                                  "Point",
                                  "LineString",
                                  "Polygon",
                                  "MultiPoint",
                                  "MultiLineString",
                                  "MultiPolygon",
                                  "GeometryCollection"};
    JVal o = jobj();
    o.obj.emplace_back("type", jstr(names[g.type]));
    if (g.type == 7)
    {
        JVal geoms = jarr();
        for (const OgrGeometry &p : g.parts)
            geoms.arr.push_back(geomToJson(p));
        o.obj.emplace_back("geometries", std::move(geoms));
        return o;
    }
    JVal coords;
    switch (g.type)
    {
        case 1:
            coords = g.coords.empty() ? jarr()
                                      : coordTupleJson(&g.coords[0], g.hasZ);
            break;
        case 2:
            coords = coordListJson(g, g.hasZ);
            break;
        case 3:
        case 5:
        {
            coords = jarr();
            for (const OgrGeometry &r : g.parts)
                coords.arr.push_back(coordListJson(r, g.hasZ));
            break;
        }
        case 4:
        {
            coords = jarr();
            for (const OgrGeometry &p : g.parts)
                coords.arr.push_back(
                    p.coords.empty() ? jarr()
                                     : coordTupleJson(&p.coords[0], g.hasZ));
            break;
        }
        case 6:
        {
            coords = jarr();
            for (const OgrGeometry &p : g.parts)
            {
                JVal poly = jarr();
                for (const OgrGeometry &r : p.parts)
                    poly.arr.push_back(coordListJson(r, g.hasZ));
                coords.arr.push_back(std::move(poly));
            }
            break;
        }
    }
    o.obj.emplace_back("coordinates", std::move(coords));
    return o;
}

std::string geomFieldTypeJson(int t, bool hasZ, bool hasM = false)
{
    static const char *names[] = {"Geometry",
                                  "Point",
                                  "LineString",
                                  "Polygon",
                                  "MultiPoint",
                                  "MultiLineString",
                                  "MultiPolygon",
                                  "GeometryCollection",
                                  "CircularString",
                                  "CompoundCurve",
                                  "CurvePolygon",
                                  "MultiCurve",
                                  "MultiSurface",
                                  "Curve",
                                  "Surface",
                                  "PolyhedralSurface",
                                  "TIN",
                                  "Triangle"};
    std::string base = t >= 0 && t <= 17 ? names[t] : "Geometry";
    if (hasZ)
        base += "Z";
    if (hasM)
        base += "M";
    return base;
}

JVal vectorCsFullJson(const Srs &srs)
{
    JVal cs = jobj();
    cs.obj.emplace_back("wkt", jstr(srs.wkt2_2019()));
    std::string pjson = srs.projjson();
    bool pok = false;
    JVal parsed = JVal::parse(pjson, &pok);
    if (pok)
        cs.obj.emplace_back("projjson", std::move(parsed));
    JVal mapping = jarr();
    for (int v : srs.dataAxisToSRSAxisMapping())
        mapping.arr.push_back(jint(v));
    cs.obj.emplace_back("dataAxisToSRSAxisMapping", std::move(mapping));
    return cs;
}

JVal vectorFieldDefnJson(const OgrFieldDefn &f)
{
    JVal fj = jobj();
    fj.obj.emplace_back("name", jstr(f.name));
    fj.obj.emplace_back("type", jstr(ogrFieldTypeName(f.type)));
    if (f.subType != OFSTNone)
        fj.obj.emplace_back("subType",
                            jstr(ogrFieldSubTypeName(f.subType)));
    if (f.width > 0)
        fj.obj.emplace_back("width", jint(f.width));
    if (f.precision > 0)
        fj.obj.emplace_back("precision", jint(f.precision));
    JVal b;
    b.type = JVal::BOOL;
    b.b = f.nullable;
    fj.obj.emplace_back("nullable", std::move(b));
    JVal u;
    u.type = JVal::BOOL;
    u.b = f.unique;
    fj.obj.emplace_back("uniqueConstraint", std::move(u));
    if (!f.altName.empty())
        fj.obj.emplace_back("alias", jstr(f.altName));
    if (f.type == OFTDateTime && f.tzAggr == 100)
        fj.obj.emplace_back("timezone", jstr("UTC"));
    else if (f.type == OFTDateTime && f.tzAggr == -2)
        fj.obj.emplace_back("timezone", jstr("mixed timezones"));
    else if (f.type == OFTDateTime && f.tzAggr > 100)
    {
        int off = (f.tzAggr - 100) * 15;
        char sign = off < 0 ? '-' : '+';
        int a = off < 0 ? -off : off;
        fj.obj.emplace_back(
            "timezone",
            jstr(strPrintf("%c%02d:%02d", sign, a / 60, a % 60)));
    }
    else if (f.type == OFTDateTime && f.tzAggr > 1 && f.tzAggr < 100)
    {
        int off = (f.tzAggr - 100) * 15;
        char sign = off < 0 ? '-' : '+';
        int a = off < 0 ? -off : off;
        fj.obj.emplace_back(
            "timezone",
            jstr(strPrintf("%c%02d:%02d", sign, a / 60, a % 60)));
    }
    return fj;
}

std::string buildJson(const OgrDataset &ds, const VectorInfoOpts &o)
{
    JVal root = jobj();
    root.obj.emplace_back("description",
                          jstr(g_pipelineConcatInfo ? "" : ds.path));
    if (!o.driverDrop && !g_pipelineConcatInfo && !g_pipelineStreamInfo)
    {
        root.obj.emplace_back("driverShortName", jstr(ds.driverShort));
        root.obj.emplace_back("driverLongName", jstr(ds.driverLong));
    }
    JVal layers = jarr();
    for (const OgrLayer &lyr : ds.layers)
    {
        if (!layerSelected(o, lyr.name))
            continue;
        JVal L = jobj();
        L.obj.emplace_back("name", jstr(lyr.name));
        if (lyr.filterFailed)
        {
            layers.arr.push_back(std::move(L));
            continue;
        }
        if (o.summary && !o.filterMode)
        {
            JVal gt = jarr();
            if (lyr.geomType != 101)
                gt.arr.push_back(jstr(ogrGeomTypeName(
                    lyr.geomType, lyr.geomHasZ, lyr.geomHasM)));
            L.obj.emplace_back("geometryType", std::move(gt));
            layers.arr.push_back(std::move(L));
            continue;
        }
        JVal md = jobj();
        if (!lyr.metadata.empty())
        {
            JVal dom = jobj();
            for (const auto &kv : lyr.metadata)
                dom.obj.emplace_back(kv.first, jstr(kv.second));
            md.obj.emplace_back("", std::move(dom));
        }
        for (const auto &domPair : lyr.extraMdDomains)
        {
            JVal dom = jobj();
            for (const auto &kv : domPair.second)
                dom.obj.emplace_back(kv.first, jstr(kv.second));
            md.obj.emplace_back(domPair.first, std::move(dom));
        }
        if (!o.summary)
            L.obj.emplace_back("metadata", std::move(md));
        JVal gfs = jarr();
        if (lyr.hasGeomField)
        {
            JVal gf = jobj();
            gf.obj.emplace_back("name", jstr(lyr.geomColumnName));
            gf.obj.emplace_back(
                "type", jstr(geomFieldTypeJson(lyr.geomType, lyr.geomHasZ,
                                               lyr.geomHasM)));
            gf.obj.emplace_back("nullable", [] {
                JVal b;
                b.type = JVal::BOOL;
                b.b = true;
                return b;
            }());
            if (lyr.hasExtent)
            {
                // raw %.17g without OGRFormatDouble run-trimming, so a
                // computed 20.1 stays 20.100000000000001
                auto jext = [](double d) {
                    JVal j;
                    j.type = JVal::DOUBLE;
                    j.d = d;
                    if (std::isnan(d))
                        j.s = "NaN";
                    else if (std::isinf(d))
                        j.s = d > 0 ? "Infinity" : "-Infinity";
                    else
                    {
                        j.s = strPrintf("%.17g", d);
                        if (j.s.find('.') == std::string::npos &&
                            j.s.find('e') == std::string::npos)
                            j.s += ".0";
                    }
                    return j;
                };
                JVal ext = jarr();
                for (int i = 0; i < 4; i++)
                    ext.arr.push_back(jext(lyr.extent[i]));
                gf.obj.emplace_back("extent", std::move(ext));
            }
            if (!lyr.hasSrs)
                gf.obj.emplace_back("coordinateSystem", JVal());
            if (lyr.hasSrs)
                gf.obj.emplace_back("coordinateSystem",
                                    vectorCsFullJson(lyr.srs));
            gfs.arr.push_back(std::move(gf));
        }
        L.obj.emplace_back("geometryFields", std::move(gfs));
        L.obj.emplace_back("featureCount",
                           jint(lyr.countOverride >= 0
                                    ? lyr.countOverride
                                    : (long long)lyr.features.size()));
        if (!lyr.fidColumn.empty())
            L.obj.emplace_back("fidColumnName", jstr(lyr.fidColumn));
        JVal fields = jarr();
        for (const OgrFieldDefn &f : lyr.fields)
            fields.arr.push_back(vectorFieldDefnJson(f));
        L.obj.emplace_back("fields", std::move(fields));
        if (o.features)
        {
            JVal feats = jarr();
            long long emitted = 0;
            for (const OgrFeature &feat : lyr.features)
            {
                if (o.hasFid && feat.fid != o.fid)
                    continue;
                if (o.hasLimit && emitted >= o.limit)
                    break;
                JVal fo = jobj();
                fo.obj.emplace_back("type", jstr("Feature"));
                JVal props = jobj();
                for (size_t i = 0; i < lyr.fields.size(); i++)
                {
                    if (i >= feat.values.size() || !feat.values[i].set)
                        continue;
                    props.obj.emplace_back(
                        lyr.fields[i].name,
                        fieldValueJson(lyr.fields[i], feat.values[i].v));
                }
                fo.obj.emplace_back("properties", std::move(props));
                fo.obj.emplace_back("fid", jint(feat.fid));
                // export failures fall back to null plus a WKT echo: an
                // empty point (standalone or nested) has no GeoJSON
                // representation and fails quietly; a non-finite
                // coordinate fails with a one-line warning
                int expFail = feat.hasGeom ? geomJsonExportFail(feat.geom)
                                           : 0;
                if (expFail == 2)
                    cplErrorStr(CE_Warning, CPLE_AppDefined,
                                "Infinite or NaN coordinate encountered");
                if (feat.hasGeom && !expFail)
                    fo.obj.emplace_back("geometry", geomToJson(feat.geom));
                else if (feat.hasGeom && feat.geom.type == 7)
                {
                    // a failing GeometryCollection keeps its type and
                    // nulls the member list (no WKT echo)
                    JVal go;
                    go.type = JVal::OBJECT;
                    go.obj.emplace_back("type",
                                        jstr("GeometryCollection"));
                    go.obj.emplace_back("geometries", JVal());
                    fo.obj.emplace_back("geometry", std::move(go));
                }
                else
                    fo.obj.emplace_back("geometry", JVal());
                if (feat.hasGeom && expFail && feat.geom.type != 7)
                    fo.obj.emplace_back("wkt_geometry",
                                        jstr(ogrWktLegacy(feat.geom)));
                feats.arr.push_back(std::move(fo));
                emitted++;
            }
            L.obj.emplace_back("features", std::move(feats));
        }
        layers.arr.push_back(std::move(L));
    }
    root.obj.emplace_back("layers", std::move(layers));
    if (!o.summary)
    {
        JVal dsMd = jobj();
        if (!ds.metadata.empty())
        {
            JVal dom = jobj();
            for (const auto &kv : ds.metadata)
                dom.obj.emplace_back(kv.first, jstr(kv.second));
            dsMd.obj.emplace_back("", std::move(dom));
        }
        root.obj.emplace_back("metadata", std::move(dsMd));
        root.obj.emplace_back("domains", jobj());
        if (!o.filterMode)
            root.obj.emplace_back("relationships", jobj());
    }
    return jsoncSerialize(root, false) + "\n";
}

int vectorInfoHandler(const CmdSpec &, ParseResult &r)
{
    for (const auto &d : r.list("input-format"))
    {
        std::string err = inputFormatCapError(true, d);
        if (!err.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined, "info: " + err);
            handlerPrintUsage();
            return 1;
        }
    }

    auto inputs = r.list("input");
    if (inputs.size() != 1)
    {
        std::string msg = strPrintf(
            "read: %d values have been specified for argument 'input', "
            "whereas exactly 1 was expected.",
            (int)inputs.size());
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
        return 1;
    }
    std::string path = inputs[0];

    bool crsFormatJsonMutex =
        strEqualNoCase(r.str("output-format", "text"), "json") &&
        r.get("crs-format") &&
        !strEqualNoCase(r.str("crs-format", "AUTO"), "AUTO");

    std::string err;
    std::unique_ptr<OgrDataset> ds;
    if (g_convertSourceOverride)
        ds = std::move(g_convertSourceOverride);
    else
        ds = openVector(path, err, r.list("input-format"),
                        r.list("open-option"));
    if (!ds)
    {
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(path));
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + path +
                            "' not recognized as being in a supported file "
                            "format.");
        if (crsFormatJsonMutex)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "info: 'crs-format' cannot be set when 'format' is "
                        "set to 'json'");
        if (!g_infoDispatchOpenUsage.empty())
            g_handlerUsageText = g_infoDispatchOpenUsage;
        handlerPrintUsage();
        return 1;
    }

    if (crsFormatJsonMutex)
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "info: 'crs-format' cannot be set when 'format' is "
                    "set to 'json'");
        handlerPrintUsage();
        return 1;
    }

    // empty-string sql/where/dialect behave as unset
    std::string sqlStmt = r.get("sql") ? r.str("sql") : "";
    std::string whereClause = r.get("where") ? r.str("where") : "";
    std::string dialect = r.get("dialect") ? r.str("dialect") : "";
    bool summaryFlag = r.flag("summary");
    // summary mode never executes the statement, not even to report the
    // unsupported-dialect errors
    bool sqlExec = !sqlStmt.empty() && !summaryFlag;

    ogrFlushPendingDebug(*ds);

    VectorInfoOpts o;
    o.features = r.flag("features");
    o.summary = summaryFlag;
    o.crsFormat = r.str("crs-format", "AUTO");
    if (r.get("limit"))
    {
        o.limit = atoll(r.str("limit").c_str());
        o.hasLimit = o.limit > 0;
        o.features = true;
    }
    if (r.get("fid"))
    {
        o.hasFid = true;
        o.fid = atoll(r.str("fid").c_str());
    }
    o.layerFilter = r.list("input-layer");
    std::vector<std::string> fetchSel = g_pipelineInfoStepLayers;
    o.hasLayerArg = !o.layerFilter.empty() || !fetchSel.empty();
    o.filterMode = (!o.layerFilter.empty() &&
                    !g_pipelineReadLayerFilter) ||
                   !fetchSel.empty();
    o.driverDrop =
        !o.layerFilter.empty() && !g_pipelineInfoDriverKeep;
    // the info-step's own -l fetches layers one by one; the leaf and
    // the read step validate against the source layer list instead
    bool fetchMode = g_pipelineInfoDriverKeep;
    bool fetchFailed = false;
    auto fetchSelect = [&](const std::vector<std::string> &want) {
        std::vector<OgrLayer> sel;
        for (const std::string &lf : want)
        {
            const OgrLayer *hit = nullptr;
            for (const OgrLayer &lyr : ds->layers)
                if (lyr.name == lf)
                {
                    hit = &lyr;
                    break;
                }
            if (!hit)
                for (const OgrLayer &lyr : ds->layers)
                    if (strEqualNoCase(lyr.name, lf))
                    {
                        hit = &lyr;
                        break;
                    }
            if (!hit)
            {
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            "Couldn't fetch requested layer " + lf +
                                ".");
                fetchFailed = true;
                break;
            }
            sel.push_back(*hit);
        }
        ds->layers = std::move(sel);
    };
    if (!o.layerFilter.empty())
    {
        if (fetchMode)
            fetchSelect(o.layerFilter);
        else if (vectorReadSelectLayers(*ds, o.layerFilter))
        {
            vectorDebugClose(*ds);
            return 1;
        }
        o.layerFilter.clear();
    }
    if (!fetchFailed && !fetchSel.empty())
        fetchSelect(fetchSel);
    if (fetchFailed &&
        strEqualNoCase(r.str("output-format", "text"), "json"))
    {
        vectorDebugClose(*ds);
        return 1;
    }

    // legacy ogrinfo option vector: a value starting with '-' is taken
    // for the next option, the whole option set dies, and the default
    // rendering (numbered layer list) is produced instead
    {
        const struct
        {
            const char *name;
            const std::string &val;
        } legacy[] = {{"sql", sqlStmt},
                      {"where", whereClause},
                      {"dialect", dialect}};
        for (const auto &lo : legacy)
        {
            if (lo.val.empty() || lo.val[0] != '-')
                continue;
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        strPrintf("Too few arguments for '-%s'.",
                                  lo.name));
            cplDebug("OGR", strPrintf("GetLayerCount() = %d\n",
                                      (int)ds->layers.size()));
            std::string out = strPrintf(
                "INFO: Open of `%s'\n      using driver `%s' "
                "successful.\n",
                g_pipelineConcatInfo ? "" : ds->path.c_str(),
                !o.driverDrop && !g_pipelineConcatInfo &&
                        !g_pipelineStreamInfo
                    ? ds->driverShort.c_str()
                    : "(null)");
            for (size_t i = 0; i < ds->layers.size(); i++)
            {
                const OgrLayer &lyr = ds->layers[i];
                std::string g = ogrGeomTypeName(lyr.geomType,
                                                lyr.geomHasZ,
                                                lyr.geomHasM);
                if (lyr.geomType == 0)
                    out += strPrintf("%d: %s\n", (int)i + 1,
                                     lyr.name.c_str());
                else
                    out += strPrintf("%d: %s (%s)\n", (int)i + 1,
                                     lyr.name.c_str(), g.c_str());
            }
            fwrite(out.data(), 1, out.size(), stdout);
            vectorDebugClose(*ds);
            return 0;
        }
    }

    if (sqlExec && strEqualNoCase(dialect, "SQLITE"))
    {
        if (!strEqualNoCase(r.str("output-format", "text"), "json"))
        {
            std::string hdr = strPrintf(
                "INFO: Open of `%s'\n      using driver `%s' "
                "successful.\n",
                path.c_str(), ds->driverShort.c_str());
            fwrite(hdr.data(), 1, hdr.size(), stdout);
        }
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "The SQLite driver needs to be compiled to support "
                    "the SQLite SQL dialect");
        vectorDebugClose(*ds);
        return 1;
    }

    if (!whereClause.empty() && !dialect.empty())
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "-dialect is ignored with -where. Use -sql instead");

    // GetLayerCount is skipped both by the SQL path and by explicit
    // layer selection
    if (!sqlExec && !o.hasLayerArg)
        cplDebug("OGR", strPrintf("GetLayerCount() = %d\n",
                                  (int)ds->layers.size()));

    bool jsonOut = r.str("output-format", "text") == "json";

    if (sqlExec)
    {
        if (!dialect.empty() && !strEqualNoCase(dialect, "OGRSQL"))
            cplErrorStr(CE_Warning, CPLE_NotSupported,
                        "Dialect '" + dialect +
                            "' is unsupported. Only supported dialects "
                            "are 'OGRSQL'. Defaulting to OGRSQL");
        std::unique_ptr<OgrLayer> res = ogrExecuteSql(*ds, sqlStmt);
        if (!res)
        {
            vectorDebugClose(*ds);
            if (!jsonOut)
            {
                std::string hdr = strPrintf(
                    "INFO: Open of `%s'\n      using driver `%s' "
                    "successful.\n",
                    path.c_str(), ds->driverShort.c_str());
                fwrite(hdr.data(), 1, hdr.size(), stdout);
            }
            return 1;
        }
        ds->layers.clear();
        ds->layers.push_back(std::move(*res));
    }
    else if (!whereClause.empty() && !(o.summary && !o.filterMode))
    {
        for (OgrLayer &lyr : ds->layers)
        {
            if (!layerSelected(o, lyr.name))
                continue;
            if (!ogrApplyAttributeFilter(lyr, whereClause))
                lyr.filterFailed = true;
            else
                lyr.countOverride = -1;
        }
    }

    bool featureMode = o.features || (o.hasFid && !jsonOut);
    for (const OgrLayer &lyr : ds->layers)
    {
        if (!lyr.seqRescan)
            continue;
        auto emitPass = [&](long long maxFeatsBefore) {
            for (const auto &e : lyr.seqEvents)
                if (e.featsBefore <= maxFeatsBefore &&
                    diagOnceGate(e.once))
                    cplErrorStr((CPLErrClass)e.sev, CPLE_AppDefined,
                                e.msg);
        };
        const long long kFull = LLONG_MAX;
        if (!(o.summary && !o.filterMode))
            emitPass(kFull);
        if (o.features)
            emitPass(o.hasLimit ? o.limit - 1 : kFull);
        else if (o.hasFid && !jsonOut)
        {
            long long stop = kFull;
            for (size_t k = 0; k < lyr.features.size(); ++k)
                if (lyr.features[k].fid == o.fid)
                {
                    stop = (long long)k;
                    break;
                }
            emitPass(stop);
        }
    }
    for (const OgrLayer &lyr : ds->layers)
    {
        if (lyr.matEvents.empty())
            continue;
        auto emitMat = [&](long long maxIdx) {
            for (const auto &e : lyr.matEvents)
                if (e.featsBefore <= maxIdx)
                    cplErrorStr((CPLErrClass)e.sev, CPLE_AppDefined,
                                e.msg);
        };
        const long long kFull = LLONG_MAX;
        if (!(o.summary && !o.filterMode))
            emitMat(kFull);
        if (o.features)
            emitMat(kFull);
        else if (o.hasFid && !jsonOut)
        {
            emitMat(kFull);
            for (size_t k = 0; k < lyr.features.size(); ++k)
                if (lyr.features[k].fid == o.fid)
                {
                    for (const auto &e : lyr.matEvents)
                        if (e.featsBefore == (long long)k)
                            cplErrorStr((CPLErrClass)e.sev,
                                        CPLE_AppDefined, e.msg);
                    break;
                }
        }
    }
    if (!(o.summary && !o.filterMode))
        for (const OgrLayer &lyr : ds->layers)
        {
            for (const auto &d : lyr.pendingDiags)
                if ((!d.geom || (d.openAlso && lyr.geomDiagBase)) &&
                    diagOnceGate(d))
                    cplErrorStr((CPLErrClass)d.sev, CPLE_AppDefined,
                                d.msg);
            if (featureMode)
                for (const auto &d : lyr.pendingDiags)
                    if (d.geom && diagOnceGate(d))
                        cplErrorStr((CPLErrClass)d.sev, CPLE_AppDefined,
                                    d.msg);
            if (o.hasFid && !jsonOut)
                for (const auto &d : lyr.pendingDiags)
                    if (d.geom && (d.fid == o.fid || d.once) &&
                        diagOnceGate(d))
                        cplErrorStr((CPLErrClass)d.sev, CPLE_AppDefined,
                                    d.msg);
            if (o.hasFid && !jsonOut && lyr.directFidRange &&
                layerSelected(o, lyr.name) &&
                o.fid >= (long long)lyr.features.size())
                cplErrorStr(CE_Failure, CPLE_AppDefined,
                            strPrintf("Attempt to read shape with feature "
                                      "id (%lld) out of available range.",
                                      o.fid));
        }

    std::string fmt = r.str("output-format", "text");
    std::string out = strEqualNoCase(fmt, "json") ? buildJson(*ds, o)
                                                  : buildText(*ds, o);
    fwrite(out.data(), 1, out.size(), stdout);
    for (OgrLayer &lyr : ds->layers)
    {
        if (!layerSelected(o, lyr.name))
            continue;
        if (o.features)
            lyr.debugFeaturesRead =
                o.hasLimit &&
                        o.limit < (long long)lyr.features.size()
                    ? o.limit
                    : (long long)lyr.features.size();
        else if (o.hasFid && !jsonOut && o.fid >= 0 &&
                 o.fid < (long long)lyr.features.size())
            lyr.debugFeaturesRead = 1;
    }
    vectorDebugClose(*ds);
    return fetchFailed ? 1 : 0;
}

int vectorExportSchemaHandler(const CmdSpec &, ParseResult &r)
{
    for (const auto &d : r.list("input-format"))
    {
        std::string err = inputFormatCapError(true, d);
        if (!err.empty())
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "export-schema: " + err);
            handlerPrintUsage();
            return 1;
        }
    }
    auto inputs = r.list("input");
    if (inputs.size() != 1)
    {
        // the leaf runs an internal read+write pipeline: read complains
        // twice around write's at-most-1 gripe
        std::string rmsg = strPrintf(
            "read: %d values have been specified for argument 'input', "
            "whereas exactly 1 was expected.",
            (int)inputs.size());
        cplErrorStr(CE_Failure, CPLE_AppDefined, rmsg);
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    strPrintf("write: %d values have been specified for "
                              "argument 'input', whereas at most 1 was "
                              "expected.",
                              (int)inputs.size()));
        cplErrorStr(CE_Failure, CPLE_AppDefined, rmsg);
        return 1;
    }
    std::string path = inputs[0];
    std::string err;
    std::unique_ptr<OgrDataset> ds = openVector(
        path, err, r.list("input-format"), r.list("open-option"));
    if (!ds)
    {
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(path));
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + path +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }
    ogrFlushPendingDebug(*ds);
    if (vectorReadSelectLayers(*ds, r.list("input-layer")))
    {
        vectorDebugClose(*ds);
        return 1;
    }
    std::string out = vectorExportSchemaRender(*ds);
    fwrite(out.data(), 1, out.size(), stdout);
    vectorDebugClose(*ds);
    return 0;
}

}  // namespace

int vectorReadSelectLayers(OgrDataset &ds,
                           const std::vector<std::string> &sel)
{
    if (sel.empty())
        return 0;
    std::vector<OgrLayer> out;
    for (const std::string &lf : sel)
    {
        const OgrLayer *hit = nullptr;
        for (const OgrLayer &lyr : ds.layers)
            if (lyr.name == lf)
            {
                hit = &lyr;
                break;
            }
        if (!hit)
            for (const OgrLayer &lyr : ds.layers)
                if (strEqualNoCase(lyr.name, lf))
                {
                    hit = &lyr;
                    break;
                }
        if (!hit)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "read: Cannot find source layer '" + lf + "'");
            return 1;
        }
        out.push_back(*hit);
    }
    ds.layers = std::move(out);
    return 0;
}

std::string vectorExportSchemaRender(const OgrDataset &ds)
{
    JVal root = jobj();
    JVal layers = jarr();
    for (const OgrLayer &lyr : ds.layers)
    {
        JVal L = jobj();
        L.obj.emplace_back("name", jstr(lyr.name));
        L.obj.emplace_back("schemaType", jstr("Full"));
        JVal gfs = jarr();
        if (lyr.hasGeomField)
        {
            JVal gf = jobj();
            gf.obj.emplace_back("name", jstr(lyr.geomColumnName));
            gf.obj.emplace_back(
                "type", jstr(geomFieldTypeJson(lyr.geomType, lyr.geomHasZ,
                                               lyr.geomHasM)));
            JVal b;
            b.type = JVal::BOOL;
            b.b = true;
            gf.obj.emplace_back("nullable", std::move(b));
            if (!lyr.hasSrs)
                gf.obj.emplace_back("coordinateSystem", JVal());
            else
            {
                std::string an = lyr.srs.authName();
                std::string ac = lyr.srs.code();
                if (!an.empty() && !ac.empty())
                {
                    JVal cs = jobj();
                    cs.obj.emplace_back("authid", jstr(an + ":" + ac));
                    gf.obj.emplace_back("coordinateSystem",
                                        std::move(cs));
                }
                else
                    gf.obj.emplace_back("coordinateSystem",
                                        vectorCsFullJson(lyr.srs));
            }
            gfs.arr.push_back(std::move(gf));
        }
        L.obj.emplace_back("geometryFields", std::move(gfs));
        JVal fields = jarr();
        for (const OgrFieldDefn &f : lyr.fields)
            fields.arr.push_back(vectorFieldDefnJson(f));
        L.obj.emplace_back("fields", std::move(fields));
        layers.arr.push_back(std::move(L));
    }
    root.obj.emplace_back("layers", std::move(layers));
    return jsoncSerialize(root, false) + "\n";
}

void ogrFlushPendingDebug(OgrDataset &ds)
{
    for (const auto &n : ds.pendingDebug)
        cplDebug(n.first, n.second);
    ds.pendingDebug.clear();
}

void ogrDebugAnnounceOpen(OgrDataset &ds)
{
    // internal probe opens (quiet handler) never announce; the legacy
    // GDALOpen debug line belongs to user-level opens only
    if (cplQuietActive() || ds.debugAnnounced)
        return;
    if (!ds.debugDeferred)
        ogrFlushPendingDebug(ds);
    ds.debugPtr = cplDebugPtr();
    ds.debugAnnounced = true;
    cplDebug("GDAL", "GDALOpen(" + ds.path + ", this=" + ds.debugPtr +
                         ") succeeds as " + ds.driverShort + ".");
}

void vectorDebugClose(OgrDataset &ds)
{
    ogrFlushPendingDebug(ds);
    if (ds.driverShort == "ESRI Shapefile")
        for (const OgrLayer &lyr : ds.layers)
            if (lyr.debugFeaturesRead > 0)
                cplDebug("Shape",
                         strPrintf("%lld features read on layer '%s'.",
                                   lyr.debugFeaturesRead,
                                   lyr.name.c_str()));
    if (ds.debugAnnounced)
        cplDebug("GDAL",
                 "GDALClose(" + ds.path + ", this=" + ds.debugPtr + ")");
}

std::unique_ptr<OgrDataset> openVectorDataset(
    const std::string &path, std::string &err,
    const std::vector<std::string> &inputFormats,
    const std::vector<std::string> &openOptions, bool seqOpenErrors)
{
    return openVector(path, err, inputFormats, openOptions,
                      seqOpenErrors);
}

void registerVectorHandlers()
{
    registerHandler("vector_info", vectorInfoHandler);
    registerHandler("vector_export-schema", vectorExportSchemaHandler);
}
