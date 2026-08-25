#pragma once
#include "spec.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ArgValue
{
    const ArgSpec *spec = nullptr;
    bool set = false;
    std::vector<std::string> values;  // raw string values

    std::string str() const { return values.empty() ? "" : values[0]; }
    bool boolean() const { return set && str() == "true"; }
};

struct ParseResult
{
    std::map<std::string, ArgValue> byName;
    std::vector<std::string> order;  // first-set order on the command line

    const ArgValue *get(const std::string &name) const
    {
        auto it = byName.find(name);
        return it == byName.end() || !it->second.set ? nullptr : &it->second;
    }
    std::string str(const std::string &name, const std::string &def = "") const
    {
        const ArgValue *v = get(name);
        return v ? v->str() : def;
    }
    bool flag(const std::string &name) const
    {
        const ArgValue *v = get(name);
        return v && v->str() == "true";
    }
    std::vector<std::string> list(const std::string &name) const
    {
        const ArgValue *v = get(name);
        return v ? v->values : std::vector<std::string>();
    }
};

using Handler = std::function<int(const CmdSpec &, ParseResult &)>;

void registerHandler(const std::string &id, Handler h);
// runs at the start of leaf validation, before dataset existence checks;
// a nonzero return aborts with that exit code
void registerPreValidator(const std::string &id, Handler h);
// per-argument value check, interleaved with the fixed-count validation in
// spec order; called once per set argument, nonzero return aborts
using ArgCheck = std::function<int(const std::string &, ParseResult &)>;
void registerArgCheck(const std::string &id, ArgCheck f);
// parse-time value validation, run the moment an argument receives its
// value (command-line token order); a non-empty return aborts with
// "<alg>: <msg>" + usage
using ArgValueCheck =
    std::function<std::string(const std::string &argName,
                              const std::string &value)>;
void registerArgValueCheck(const std::string &id, ArgValueCheck f);
// runs after the validation-time output processing (exists refusal /
// overwrite delete); prints stacked errors and returns true to join the
// shared validation failure + usage block
struct ParseResult;
using PostValidator = std::function<bool(const CmdSpec &, ParseResult &,
                                         bool inputOpenFailed)>;
void registerPostValidator(const std::string &id, PostValidator f);
void registerAllHandlers();
// prints "Usage: ...\nTry '... --help' for help.\n" for the current leaf
void handlerPrintUsage();
const std::string &handlerInvokedCli();
// e.g. generic `gdal convert` dispatching to its raster mode appends
// "raster" to the usage/invoked CLI
void handlerAppendUsageSub(const std::string &sub);
bool datasetIdentify(const std::string &path,
                     const std::vector<std::string> &types);
// GDALDriver::QuietDelete emulation used by convert-like --overwrite
void overwriteDeleteFileset(const std::string &path);

// zonal-stats output processing wedged between the input and zones
// dataset opens (handlers_raster_zonalstats.cpp)
void zonalStatsValidateOutput(ParseResult &r, bool &failed);
// "Dataset" (identifiable), "File" or "Directory" for exists-refusal texts
std::string outputExistsKind(const std::string &path);
// leaves whose output dataset arg is processed at validation time
bool convertLikeOutputCmd(const std::string &id);
// multidim VRT open probe used by the validation pass; false with err
// filled = parse error to report verbatim, false with empty err = not a
// multidim dataset at all
bool mdimValidationOpen(const std::string &path, std::string &err);
// verbatim `gdal mdim --drivers` JSON (no trailing newline)
const char *mdimDriversJson();
// vector verbs that only fail with a GEOS-missing error in this build
bool geosStubVectorCmd(const std::string &id);
// generic `gdal info` raster-vs-vector dispatch probes
bool infoDispatchRaster(const std::string &path);
bool infoDispatchVector(const std::string &path);
// generic `gdal convert` dispatch: "raster", "vector" or "" (unopenable)
std::string convertDispatchProbe(const std::string &path);

class RasterDatasetBase;
struct ParseResult;
// validation-stage hook run between the output-exists check and the
// overwrite delete; errors it prints defer the abort so several
// validation failures stack before the single usage block
using PreWriteValidate =
    std::function<void(const std::string &resolvedDrv, bool &failed)>;
int rasterConvertWriteOutput(
    std::unique_ptr<RasterDatasetBase> &ds, ParseResult &r,
    const std::string &input, const std::string &output, bool quiet,
    bool overwrite, bool append, const std::string &drv,
    const std::string &gdalgExtra,
    const std::function<int(std::unique_ptr<RasterDatasetBase> &)>
        &materialize,
    const PreWriteValidate &preWriteValidate = nullptr);

using MetaDomainDecl = std::vector<std::pair<std::string, std::string>>;
void emitVrtMetadataEcho(
    std::string &x, const std::map<std::string, MetaDomainDecl> &md,
    const std::vector<std::string> &order, const std::string &indent);

// ---- pipeline support ----
// current usage/invoked-cli for the running leaf (set by the engine)
extern std::string g_handlerUsageText;
extern std::string g_infoDispatchOpenUsage;
extern std::string g_handlerInvokedCli;
// pipeline delegation state consulted by the convert handlers
extern bool g_pipelineMode;
extern bool g_dashStdout;
extern bool g_pipelineConcatInfo;

// empty result means valid; otherwise the message body after "cmd: "
std::string inputFormatCapError(bool wantVector, const std::string &drv);
// true when the driver exists in the trimmed build; fills capabilities
bool knownDriverCaps(const std::string &drv, bool &ras, bool &vec);
// 'Memory' alias resolves to MEM with a once-per-process warning
void memoryDriverDeprecationWarnOnce();
// empty result means valid raster output driver; resolves Memory alias
std::string rasterOutFormatIssue(const std::string &format,
                                 std::string &canon);
extern bool g_pipelineCommitted;
// a step reported a failure but let the run complete (reproject -tap)
extern bool g_pipelineDeferredFail;
extern std::string g_pipelineGdalgCli;
extern std::string g_pipelineStepPrefix;
// prints the full 0...100 progress line
void printProgress();
// transform steps between read and write/info, applied by the convert
// and info handlers at their materialize point
extern std::function<int(std::unique_ptr<RasterDatasetBase> &)>
    g_pipelineTailMaterialize;
// total pipeline step count (read + transforms + terminal); VRT output
// is refused above 3
extern int g_pipelineTotalSteps;
using PipeStepArgs = std::map<std::string, std::vector<std::string>>;

// vector reproject support: convert-time gate/hook installation for the
// leaf and pipeline write paths, eager application for the info path
struct VectorReprojectStep
{
    std::string srcCrs, dstCrs, activeLayer;
};
void vectorReprojectInstall(const std::vector<VectorReprojectStep> &steps);
void vectorReprojectUninstall();
struct OgrDataset;
// which consumers the terminal info step will actually pull, so the eager
// transform reproduces the reference's lazy error stream
struct VectorReprojectInfoNeeds
{
    bool extent = true;
    bool features = false;
    long long limit = -1;
    bool hasFid = false;
    long long fid = 0;
    std::vector<std::string> layerFilter;
};
// transforms the dataset in place (gate semantics incl. the no-SRS error,
// grid-sampled extent reprojection, feature-pull emulation); 0 = ok
int vectorReprojectApply(OgrDataset &ds,
                         const std::vector<VectorReprojectStep> &steps,
                         const VectorReprojectInfoNeeds &needs);
// " ! reproject --canonical-args" echo for GDALG command lines
std::string vectorReprojectStepEcho(const PipeStepArgs &args);
extern bool g_pipelineStreamInfo;
// a layer filter given on the info step itself (not on read) keeps the
// dataset's driver identity in the info header
extern bool g_pipelineInfoDriverKeep;
// a layer filter given only on the read step keeps the json
// relationships block (the filter materialized before info ran)
extern bool g_pipelineReadLayerFilter;
// info-step -l names applied after the read selection when both steps
// carry a layer filter (fetch-one-by-one semantics)
extern std::vector<std::string> g_pipelineInfoStepLayers;
// a non-terminal write step precedes the current one (GDALG serialization
// emits its non-streaming warning)
extern bool g_pipelineHasMidWrite;

bool rasterTailStepKnown(const std::string &name);
// dem-family steps (slope/aspect/hillshade/roughness/tpi/tri)
bool rasterDemStepName(const std::string &name);
// set while a pipeline write chain contains a dem step: its VRT-output
// refusal (message prefixed with the step name)
extern std::string g_pipelineDemVrtVerb;
// " ! name --canonical-args" echo for GDALG command lines
std::string rasterTailStepEcho(const std::string &name,
                               const PipeStepArgs &args);
// edit step: replaces ds with an in-memory VRT copy carrying the edits
bool editNodataParse(const std::string &s, double &v);
std::string editStepEcho(const PipeStepArgs &args);
int editApplyPipeStep(const PipeStepArgs &args,
                      std::unique_ptr<RasterDatasetBase> &ds);
// wraps ds; prints its own error (no usage, no progress) on failure
int rasterTailApplyPipeStep(const std::string &name,
                            const PipeStepArgs &args,
                            std::unique_ptr<RasterDatasetBase> &ds);
// band-select step: token syntax screens and the wrapping apply
bool rasterSelectBandTokensValid(const std::vector<std::string> &vals);
bool rasterSelectMaskTokenValid(const std::string &v);
int rasterSelectApplyPipeStep(const PipeStepArgs &args,
                              std::unique_ptr<RasterDatasetBase> &ds);
// nameless-VRT presentation for `... ! transform ! info`
void presentAsTranslatedVrt(RasterDatasetBase &ds);
// "convert" normally, the step name when delegated from a pipeline
std::string convertMsgPrefix();
std::string suggestOptionName(const std::string &name,
                              const std::vector<std::string> &candidates);
Handler findHandler(const std::string &id);
std::string tmsDiagnosticLine();
// runs the pipeline leaf; returns only when tokens ask for --help/usage
void pipelineLeaf(const CmdSpec &cmd, const std::string &usageFail,
                  const std::string &invokedCli,
                  const std::vector<std::string> &tokens);
// `gdal read ...` alias; false = parse failure (caller reports unknown cmd)
bool pipelineAliasRun(const std::vector<std::string> &args);

int runGdalMain(int argc, char **argv);
