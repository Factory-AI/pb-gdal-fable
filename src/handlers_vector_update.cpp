// gdal vector update: merges an input layer into an existing output
// dataset, matching features by FID or by --key field tuples

#include "cpl.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "progress.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <sys/stat.h>

#include <csignal>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{

int fieldIndexCi(const OgrLayer &l, const std::string &name)
{
    for (size_t i = 0; i < l.fields.size(); ++i)
        if (l.fields[i].name == name)
            return (int)i;
    for (size_t i = 0; i < l.fields.size(); ++i)
        if (strEqualNoCase(l.fields[i].name, name))
            return (int)i;
    return -1;
}

bool jvEq(const JVal &a, const JVal &b)
{
    if (a.type != b.type)
        return false;
    switch (a.type)
    {
        case JVal::NUL:
            return true;
        case JVal::BOOL:
            return a.b == b.b;
        case JVal::INT:
            return a.i == b.i;
        case JVal::DOUBLE:
            return a.d == b.d;
        case JVal::STRING:
            return a.s == b.s;
        case JVal::ARRAY:
            if (a.arr.size() != b.arr.size())
                return false;
            for (size_t i = 0; i < a.arr.size(); ++i)
                if (!jvEq(a.arr[i], b.arr[i]))
                    return false;
            return true;
        case JVal::OBJECT:
            if (a.obj.size() != b.obj.size())
                return false;
            for (size_t i = 0; i < a.obj.size(); ++i)
                if (a.obj[i].first != b.obj[i].first ||
                    !jvEq(a.obj[i].second, b.obj[i].second))
                    return false;
            return true;
    }
    return false;
}

bool geomEq(const OgrGeometry &a, const OgrGeometry &b)
{
    if (a.type != b.type || a.hasZ != b.hasZ || a.hasM != b.hasM)
        return false;
    if (a.coords != b.coords)
        return false;
    if (a.parts.size() != b.parts.size())
        return false;
    for (size_t i = 0; i < a.parts.size(); ++i)
        if (!geomEq(a.parts[i], b.parts[i]))
            return false;
    return true;
}

// canonical text of one key value; empty string marks a null (never
// matches, never enters the target map)
bool keySerial(const OgrFieldDefn &f, const OgrFieldValue &fv,
               std::string &out)
{
    if (!fv.set || fv.v.type == JVal::NUL)
        return false;
    const JVal &v = fv.v;
    switch (f.type)
    {
        case OFTInteger:
        case OFTInteger64:
            out += strPrintf("I%lld", v.type == JVal::INT ? v.i
                                      : v.type == JVal::BOOL
                                          ? (v.b ? 1 : 0)
                                          : (long long)v.d);
            break;
        case OFTReal:
            out += strPrintf("R%.17g", v.type == JVal::DOUBLE ? v.d
                                       : v.type == JVal::INT
                                           ? (double)v.i
                                           : 0.0);
            break;
        case OFTString:
            out += "S" + (v.type == JVal::STRING
                              ? v.s
                              : jsoncSerialize(v, false));
            break;
        default:
            out += "X" + jsoncSerialize(v, false);
            break;
    }
    out += '\x01';
    return true;
}

struct UpdatePlan
{
    OgrLayer *inLyr = nullptr;
    OgrLayer *outLyr = nullptr;
    std::vector<int> keyIn, keyOut;
};

int updateResolve(OgrDataset &src, OgrDataset &tgt,
                  const VectorUpdateOpts &o, UpdatePlan &p)
{
    if (o.hasInputLayer)
    {
        for (OgrLayer &l : src.layers)
            if (l.name == o.inputLayer)
                p.inLyr = &l;
        if (!p.inLyr)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "update: No layer named '" + o.inputLayer +
                            "' in input dataset.");
            return 1;
        }
    }
    else if (src.layers.size() == 1)
        p.inLyr = &src.layers[0];
    else
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "update: Please specify the 'input-layer' argument.");
        return 1;
    }
    if (o.hasOutputLayer)
    {
        for (OgrLayer &l : tgt.layers)
            if (l.name == o.outputLayer)
                p.outLyr = &l;
        if (!p.outLyr)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "update: No layer named '" + o.outputLayer +
                            "' in output dataset");
            return 1;
        }
    }
    else if (tgt.layers.size() == 1)
        p.outLyr = &tgt.layers[0];
    else
    {
        cplErrorStr(CE_Failure, CPLE_AppDefined,
                    "update: Please specify the 'output-layer' argument.");
        return 1;
    }
    for (const std::string &k : o.keys)
    {
        int ii = fieldIndexCi(*p.inLyr, k);
        if (ii < 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "update: Cannot find field '" + k +
                            "' in input layer");
            return 1;
        }
        int oi = fieldIndexCi(*p.outLyr, k);
        if (oi < 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "update: Cannot find field '" + k +
                            "' in output layer");
            return 1;
        }
        if (p.inLyr->fields[ii].type != p.outLyr->fields[oi].type)
        {
            cplErrorStr(CE_Failure, CPLE_NotSupported,
                        "update: Type of field '" + k +
                            "' is not the same in input and output "
                            "layers");
            return 1;
        }
        p.keyIn.push_back(ii);
        p.keyOut.push_back(oi);
    }
    return 0;
}

// coerce one input value into the output field type, SetFrom style
OgrFieldValue coerceValue(const std::string &lyrName,
                          const OgrFieldDefn &src,
                          const OgrFieldDefn &dst,
                          const OgrFieldValue &in)
{
    OgrFieldValue fv = in;
    if (!fv.set || fv.v.type == JVal::NUL)
    {
        fv.set = true;
        fv.v = JVal();
        return fv;
    }
    if (src.type == dst.type)
        return fv;
    WarnLog log;
    leafConvert(log, lyrName, src, dst.type, dst.subType, fv);
    for (const auto &m : log.msgs)
        cplErrorStr(CE_Warning, CPLE_AppDefined, m.second);
    return fv;
}

int vectorUpdateDelegateWrite(std::unique_ptr<OgrDataset> ds,
                              const std::string &outPath,
                              const std::string &driver, bool append)
{
    // the write rides the convert delegate; snapshot the pipeline and
    // convert hooks so a mid-chain update leaves the outer run intact
    std::string svGdalg = g_pipelineGdalgCli;
    std::string svPrefix = g_pipelineStepPrefix;
    bool svBarAtEnd = g_pipelineWriteBarAtEnd;
    ConvertTranslateFail svTf = g_convertTranslateFail;
    ConvertClipPending svCp = g_convertClipPending;
    std::function<int(OgrDataset &)> svMutate =
        std::move(g_convertDatasetMutate);
    std::unique_ptr<OgrDataset> svSrc = std::move(g_convertSourceOverride);

    ParseResult r2;
    auto put = [&](const std::string &k, std::vector<std::string> v) {
        ArgValue a;
        a.set = true;
        a.values = std::move(v);
        r2.byName[k] = a;
    };
    put("input", {""});
    put("output", {outPath});
    if (!driver.empty())
        put("output-format", {driver});
    put("quiet", {"true"});
    if (append)
        put("append", {"true"});
    else
        put("overwrite", {"true"});
    int rc = vvDelegateVerb(r2, "update", std::move(ds), "", driver,
                            false, {});

    g_pipelineGdalgCli = svGdalg;
    g_pipelineStepPrefix = svPrefix;
    g_pipelineWriteBarAtEnd = svBarAtEnd;
    g_convertTranslateFail = svTf;
    g_convertClipPending = svCp;
    g_convertDatasetMutate = std::move(svMutate);
    g_convertSourceOverride = std::move(svSrc);
    return rc;
}

}  // namespace

int vectorUpdateRun(OgrDataset &src, std::unique_ptr<OgrDataset> tgt,
                    const std::string &outPath,
                    const VectorUpdateOpts &o, bool quiet, bool ownBar)
{
    if (src.path == tgt->path)
    {
        cplErrorStr(CE_Failure, CPLE_NotSupported,
                    "update: Input and output datasets must be "
                    "different");
        return 1;
    }
    UpdatePlan p;
    if (int rc = updateResolve(src, *tgt, o, p))
        return rc;
    OgrLayer &in = *p.inLyr;
    OgrLayer &out = *p.outLyr;
    bool seqTarget = tgt->driverShort == "GeoJSONSeq";

    std::map<std::string, size_t> keyMap;
    std::map<long long, size_t> fidMap;
    if (!p.keyOut.empty())
    {
        std::set<std::string> dup;
        for (size_t i = 0; i < out.features.size(); ++i)
        {
            std::string ser;
            bool ok = true;
            for (size_t k = 0; k < p.keyOut.size() && ok; ++k)
                ok = keySerial(out.fields[p.keyOut[k]],
                               p.keyOut[k] <
                                       (int)out.features[i].values.size()
                                   ? out.features[i].values[p.keyOut[k]]
                                   : OgrFieldValue(),
                               ser);
            if (!ok)
                continue;
            if (dup.count(ser))
                continue;
            if (keyMap.count(ser))
            {
                keyMap.erase(ser);
                dup.insert(ser);
                continue;
            }
            keyMap[ser] = i;
        }
    }
    else
        for (size_t i = 0; i < out.features.size(); ++i)
            fidMap[out.features[i].fid] = i;

    // name-matched field pairs (input index -> output index)
    std::vector<std::pair<int, int>> pairs;
    for (size_t fi = 0; fi < in.fields.size(); ++fi)
    {
        int oi = fieldIndexCi(out, in.fields[fi].name);
        if (oi >= 0)
            pairs.emplace_back((int)fi, oi);
    }

    // appended features take the first free fid from an advancing
    // counter that starts at zero (MEM-layer style), which also decides
    // their position in a fid-ordered rewrite
    std::set<long long> usedFids;
    for (const OgrFeature &f : out.features)
        usedFids.insert(f.fid);
    long long appendNext = 0;
    size_t origCount = out.features.size();

    bool anyUpdate = false;
    TermProgress tp;
    size_t total = in.features.size();
    bool aborted = false;
    for (size_t i = 0; i < total && !aborted; ++i)
    {
        const OgrFeature &inf = in.features[i];
        bool matched = false;
        size_t tidx = 0;
        if (!p.keyOut.empty())
        {
            std::string ser;
            bool ok = true;
            for (size_t k = 0; k < p.keyIn.size() && ok; ++k)
                ok = keySerial(in.fields[p.keyIn[k]],
                               p.keyIn[k] < (int)inf.values.size()
                                   ? inf.values[p.keyIn[k]]
                                   : OgrFieldValue(),
                               ser);
            auto it = ok ? keyMap.find(ser) : keyMap.end();
            if (it != keyMap.end())
            {
                matched = true;
                tidx = it->second;
            }
        }
        else
        {
            auto it = fidMap.find(inf.fid);
            if (it != fidMap.end())
            {
                matched = true;
                tidx = it->second;
            }
        }
        if (matched && o.mode != "append-only")
        {
            OgrFeature &tf = out.features[tidx];
            OgrFeature nb = tf;
            nb.values.resize(out.fields.size());
            for (const auto &pr : pairs)
            {
                OgrFieldValue iv = pr.first < (int)inf.values.size()
                                       ? inf.values[pr.first]
                                       : OgrFieldValue();
                nb.values[pr.second] =
                    coerceValue(in.name, in.fields[pr.first],
                                out.fields[pr.second], iv);
            }
            nb.hasGeom = inf.hasGeom;
            nb.geom = inf.geom;
            bool equal = nb.hasGeom == tf.hasGeom &&
                         (!nb.hasGeom || geomEq(nb.geom, tf.geom)) &&
                         nb.values.size() == tf.values.size();
            for (size_t k = 0; equal && k < nb.values.size(); ++k)
            {
                const OgrFieldValue &a = nb.values[k];
                const OgrFieldValue &b = tf.values[k];
                bool an = !a.set || a.v.type == JVal::NUL;
                bool bn = !b.set || b.v.type == JVal::NUL;
                equal = an == bn && (an || jvEq(a.v, b.v));
            }
            if (!equal)
            {
                // the seq driver cannot rewrite one feature in place:
                // the run stops silently after this attempt
                if (seqTarget)
                    aborted = true;
                else
                {
                    nb.gjNative.reset();
                    tf = std::move(nb);
                    anyUpdate = true;
                }
            }
        }
        else if (!matched && o.mode != "update-only")
        {
            OgrFeature nf;
            long long cand = appendNext;
            while (usedFids.count(cand))
                ++cand;
            nf.fid = cand;
            appendNext = cand + 1;
            usedFids.insert(cand);
            nf.values.resize(out.fields.size());
            nf.hasGeom = inf.hasGeom;
            nf.geom = inf.geom;
            for (const auto &pr : pairs)
            {
                OgrFieldValue iv = pr.first < (int)inf.values.size()
                                       ? inf.values[pr.first]
                                       : OgrFieldValue();
                nf.values[pr.second] =
                    coerceValue(in.name, in.fields[pr.first],
                                out.fields[pr.second], iv);
            }
            // later input features can match what was just appended
            if (!p.keyOut.empty())
            {
                std::string ser;
                bool ok = true;
                for (size_t k = 0; k < p.keyOut.size() && ok; ++k)
                    ok = keySerial(out.fields[p.keyOut[k]],
                                   nf.values[p.keyOut[k]], ser);
                if (ok && !keyMap.count(ser))
                    keyMap[ser] = out.features.size();
            }
            else
                fidMap[nf.fid] = out.features.size();
            out.features.push_back(std::move(nf));
        }
        if (ownBar && !quiet)
            tp.update((double)(i + 1) / (double)total);
    }

    std::string driver = tgt->driverShort;
    bool anyAppend = out.features.size() > origCount;
    int abortedRc = aborted ? 1 : 0;
    if (anyUpdate && !aborted)
    {
        if (driver == "GeoJSON" && out.gjRoot)
            return geoJsonUpdateRewrite(out, out.gjRoot.get(), outPath)
                       ? 0
                       : 1;
        return vectorUpdateDelegateWrite(std::move(tgt), outPath, driver,
                                         false);
    }
    if (anyAppend)
    {
        // pure appends (and whatever landed before a seq abort) splice
        // into the existing file
        auto apDs = std::make_unique<OgrDataset>();
        OgrLayer lyr;
        lyr.name = out.name;
        lyr.fields = out.fields;
        lyr.geomType = out.geomType;
        lyr.geomHasZ = out.geomHasZ;
        lyr.geomHasM = out.geomHasM;
        lyr.hasGeomField = out.hasGeomField;
        lyr.hasSrs = out.hasSrs;
        lyr.srs = out.srs;
        lyr.features.assign(
            std::make_move_iterator(out.features.begin() + origCount),
            std::make_move_iterator(out.features.end()));
        apDs->layers.push_back(std::move(lyr));
        int rc = vectorUpdateDelegateWrite(std::move(apDs), outPath,
                                           driver, true);
        return rc ? rc : abortedRc;
    }
    return abortedRc;
}

namespace
{

int vectorUpdateHandler(const CmdSpec &cmd, ParseResult &r)
{
    std::vector<std::string> inputs = r.list("input");
    std::string input = inputs.empty() ? "" : inputs[0];
    std::string output = r.str("output");
    bool quiet = r.flag("quiet");

    std::unique_ptr<OgrDataset> src;
    if (int rc = vvOpenInputDs(cmd, r, input, src))
        return rc;

    // hidden --update flag: update mode defaults on; switched off, an
    // existing output refuses and a missing one dereferences the
    // never-created layer
    const ArgValue *ua = r.get("update");
    if (ua && ua->set && ua->str() == "false")
    {
        struct stat st;
        if (stat(output.c_str(), &st) == 0)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "update: Dataset '" + output +
                            "' already exists. You may specify the "
                            "--overwrite/--update option.");
            handlerPrintUsage();
            return 1;
        }
        raise(SIGSEGV);
    }

    std::unique_ptr<OgrDataset> tgt;
    std::string err;
    if (!strEndsWith(strToLower(output), ".gdalg.json"))
        tgt = openVectorDataset(output, err, {},
                                r.list("output-open-option"));
    else
    {
        struct stat st;
        if (stat(output.c_str(), &st) != 0)
            err = "missing";
    }
    if (!tgt)
    {
        struct stat st;
        if (err == "missing")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        datasetMissingMessage(output));
        else if (err != "reported" &&
                 stat(output.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            // the update-mode reopen hits the directory with fopen
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        output + ": Is a directory");
        else if (err != "reported")
            cplErrorStr(CE_Failure, CPLE_OpenFailed,
                        "`" + output +
                            "' not recognized as being in a supported "
                            "file format.");
        handlerPrintUsage();
        return 1;
    }

    VectorUpdateOpts o;
    const ArgValue *a;
    o.hasInputLayer = (a = r.get("input-layer")) && a->set;
    o.inputLayer = r.str("input-layer");
    o.hasOutputLayer = (a = r.get("output-layer")) && a->set;
    o.outputLayer = r.str("output-layer");
    o.mode = r.str("mode", "merge");
    o.keys = r.list("key");

    return vectorUpdateRun(*src, std::move(tgt), output, o, quiet, true);
}

}  // namespace

void registerVectorUpdateHandler()
{
    registerHandler("vector_update", vectorUpdateHandler);
}
