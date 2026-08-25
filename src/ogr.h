#pragma once
#include "json.h"
#include "srs.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum OgrFieldType
{
    OFTInteger = 0,
    OFTIntegerList = 1,
    OFTReal = 2,
    OFTRealList = 3,
    OFTString = 4,
    OFTStringList = 5,
    OFTBinary = 8,
    OFTDate = 9,
    OFTTime = 10,
    OFTDateTime = 11,
    OFTInteger64 = 12,
    OFTInteger64List = 13
};

enum OgrFieldSubType
{
    OFSTNone = 0,
    OFSTBoolean = 1,
    OFSTInt16 = 2,
    OFSTFloat32 = 3,
    OFSTJSON = 4,
    OFSTUUID = 5
};

// TZ flag values follow OGRField: 0 unknown, 1 local, 100 UTC, 100 +/- n
// per 15 minutes of offset
struct OgrDateTime
{
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0;
    double sec = 0.0;
    int tzFlag = 0;
    bool hasDate = false, hasTime = false;
};

struct OgrFieldDefn
{
    std::string name;
    std::string altName;
    int type = OFTString;
    int subType = OFSTNone;
    int width = 0;
    int precision = 0;
    // aggregated across features: -1 unset, 0 naive, 100.. offsets,
    // -2 mixed
    int tzAggr = -1;
    bool nullable = true;
    bool unique = false;
};

// geometry types: 0 unknown, 1..7 = point, linestring, polygon,
// multipoint, multilinestring, multipolygon, geometrycollection,
// 101 = none
struct OgrGeometry
{
    int type = 0;
    bool hasZ = false;
    // M only ever appears alongside Z (ESRIJSON 4-tuples); values live in
    // a parallel per-vertex array so stride-3 consumers drop M naturally
    bool hasM = false;
    // Point: 1 tuple; LineString / ring: n tuples (x,y,z packed)
    std::vector<double> coords;
    std::vector<double> m;
    bool empty = false;
    // Polygon: rings; Multi*/GeometryCollection: members
    std::vector<OgrGeometry> parts;
};

struct OgrFieldValue
{
    bool set = false;
    JVal v;  // raw JSON value
};

struct OgrFeature
{
    long long fid = -1;
    bool explicitFid = false;
    std::vector<OgrFieldValue> values;
    bool hasGeom = false;
    OgrGeometry geom;
    // OGRFeature::SetFrom-style writers ignore this; the GeoJSON
    // update rewrite serializes untouched features from their source
    // JSON object. Non-owning alias: valid only while the layer's
    // gjRoot (or the source document) is alive.
    std::shared_ptr<const JVal> gjNative;
    // OGRSQL rows that project OGR_STYLE also set the feature-level
    // style string the text dump prints; json output ignores it
    bool hasStyle = false;
    std::string style;
};

struct OgrLayer
{
    std::string name;
    // schema/template ingestion keeps the pre-rename name: create's
    // outer layer-failure wording reports it
    std::string origName;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<OgrFieldDefn> fields;
    int geomType = 0;  // 0 unknown even if no geometry; 101 = none
    bool geomHasZ = false;
    bool geomHasM = false;
    bool hasGeomField = true;
    // OGRSQL result layers name their anonymous geometry column
    // (_ogr_geometry_ from SELECT *); empty stays unnamed
    std::string geomColumnName;
    // SetAttributeFilter failed: the layer body is dropped from info
    // output (text) or reduced to its name (json)
    bool filterFailed = false;
    std::string fidColumn;
    std::vector<OgrFeature> features;
    bool hasExtent = false;
    double extent[4] = {0, 0, 0, 0};
    bool hasSrs = false;
    Srs srs;
    // deferred diagnostics; emission gates depend on the mode and on
    // whether the entry came from geometry parsing (see vector info)
    struct Diag
    {
        int sev;
        std::string msg;
        long long fid = -1;
        bool geom = false;
        // geometry diags that also surface at plain open (then repeat on
        // feature iteration)
        bool openAlso = false;
        // self-suppressing message: emitted at most once per process no
        // matter how many passes replay it
        bool once = false;
    };
    // whole parsed source document of a GeoJSON FeatureCollection:
    // update's in-place rewrite re-emits the header members (and keeps
    // the per-feature natives alive)
    std::shared_ptr<const JVal> gjRoot;
    // reopened output of a pipeline update step: a terminal write
    // re-emits the native document instead of the standard writer
    bool gjUpdateFlow = false;
    // native-enabled open (update step's output): writers merge each
    // feature's native members; survives every step except select/sql
    bool gjNativeMerge = false;
    std::vector<Diag> pendingDiags;
    // openAlso diags surface at open only while no feature carries a
    // structurally sound geometry (same condition that arms the
    // all-infinity extent fallback)
    bool geomDiagBase = true;
    // extra metadata domains shown only in json output (e.g. SHAPEFILE)
    std::vector<std::pair<std::string,
                          std::vector<std::pair<std::string, std::string>>>>
        extraMdDomains;
    // shapefile semantics: every field is present on every feature and
    // unset DBF cells surface as explicit nulls
    bool emitNullFields = false;
    // shapefile GetFeature semantics: out-of-range fid reports a read
    // error in addition to the not-found message
    bool directFidRange = false;
    // multi-member union layers pin the shapefile type from the layer
    // geometry type (Unknown becomes ARC) instead of deferring to the
    // first written feature
    bool shpPinType = false;
    // GeoJSONSeq: reported feature count = successfully parsed JSON
    // texts, which can exceed the real feature list
    long long countOverride = -1;
    // GeoJSONSeq re-parses the file on every pass, so parse failures and
    // per-feature diagnostics re-fire per pass in file order; featsBefore
    // gates events when a pass stops early (--limit / --fid)
    struct SeqEvent
    {
        long long featsBefore;
        int sev;
        std::string msg;
        bool isDiag;
        bool once = false;
    };
    bool seqRescan = false;
    std::vector<SeqEvent> seqEvents;
    // fire on every pass that materializes feature values (extent,
    // listing, fid scans), never on summary or at open
    std::vector<SeqEvent> matEvents;
    // codepage notes buffered until the layer's materialization point
    std::vector<std::pair<std::string, std::string>> debugNotes;
    // raw feature fetches performed (drives the close-time debug note)
    long long debugFeaturesRead = 0;
};

struct OgrDataset
{
    std::string path;
    std::string driverShort, driverLong;
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::vector<OgrLayer> layers;
    std::string debugPtr;  // this=%p stand-in used by debug traces
    bool debugAnnounced = false;
    // exhausted mid-write capture: stored extents stand in for the
    // consumed feature stream
    bool capturedStream = false;
    // layer-materialization debug (shapefile codepage notes); flushed
    // before the GDALOpen trace for single-file opens, at first layer
    // access for directory datasets
    bool debugDeferred = false;
    std::vector<std::pair<std::string, std::string>> pendingDebug;
};

// emit + clear buffered layer-materialization debug
void ogrFlushPendingDebug(OgrDataset &ds);
// GDALOpen trace (skipped inside quiet-handler probes); flushes pending
// notes first for single-file datasets
void ogrDebugAnnounceOpen(OgrDataset &ds);
// "N features read" notes (shapefile layers) followed by GDALClose
void vectorDebugClose(OgrDataset &ds);

// read-side -l selection: layers materialize in -l order (dups kept,
// exact match first, then case-insensitive); a miss reports
// "read: Cannot find source layer" without usage
int vectorReadSelectLayers(OgrDataset &ds,
                           const std::vector<std::string> &sel);
// OGR_SCHEMA export document (authid short form when the SRS carries an
// authority id, wkt+projjson block otherwise)
std::string vectorExportSchemaRender(const OgrDataset &ds);

// vector create shared core (leaf handler and pipeline step); the caller
// performs its own usage-phase validation, the core reports run-phase
// errors without usage
struct VectorCreateRun
{
    std::string output;
    std::string format;  // raw --of value, may be empty (extension guess)
    bool stepMode = false;
    bool terminalStep = false;
    bool quiet = false;
    bool overwrite = false, update = false, overwriteLayer = false;
    // pre-rendered template schema (like/pipeline chain)
    bool haveSchemaContent = false;
    std::string schemaContent;
    bool schemaSet = false;
    std::string schemaSpec;
    std::vector<std::string> layerSel;
    bool outputLayerSet = false;
    std::string outputLayer;
    bool geomTypeSet = false;
    std::string geomTypeName;
    bool geomFieldSet = false;
    std::string geomFieldName;
    bool crsSet = false;
    std::string crsInput;
    bool fidSet = false;
    std::string fid;
    std::vector<std::string> fieldDefs;
    std::vector<std::string> co, lco;
    std::string gdalgCli;  // leaf GDALG echo
    std::unique_ptr<OgrDataset> likeDs;   // pre-opened template dataset
    std::unique_ptr<OgrDataset> target;   // pre-opened update target
    std::unique_ptr<OgrDataset> handover;  // out: created dataset object
};
int vectorCreateCoreRun(VectorCreateRun &p);
// empty result means valid --field definition; otherwise the message
// body after "create: "
std::string vectorCreateFieldDefError(const std::string &def);

std::string ogrFieldTypeName(int t);
std::string ogrFieldSubTypeName(int st);
// "Unknown (any)" style
std::string ogrGeomTypeName(int t, bool hasZ, bool hasM = false);
std::string ogrFormatDouble(double v, int precision);
std::string ogrJsonDouble(double v);
std::string ogrJsonSpacedSerialize(const JVal &v);
std::string ogrWkt(const OgrGeometry &g);
std::string ogrWktLegacy(const OgrGeometry &g);
// GeoJSON export failure scan: 0 exportable, 1 empty point (quiet),
// 2 non-finite coordinate (warns "Infinite or NaN coordinate
// encountered" at the caller)
int geomJsonExportFail(const OgrGeometry &g);
bool diagOnceGate(bool once);
inline bool diagOnceGate(const OgrLayer::Diag &d)
{
    return diagOnceGate(d.once);
}
bool ogrParseDate(const std::string &s, OgrDateTime &dt);
std::string ogrDateTimeToString(const OgrDateTime &dt, int fieldType);

// standalone GeoJSON geometry object parse (diagnostics swallowed)
bool ogrGeometryFromJsonValue(const JVal &j, OgrGeometry &g);

// pre-open gate: does a JSON head look like any vector JSON driver's
// input (GeoJSON strong/weak, ESRIJSON, TopoJSON)
bool jsonVectorIdentify(const std::string &head);
std::unique_ptr<OgrDataset> openGeoJson(const std::string &path,
                                        std::string &err,
                                        bool weakPass = false);
std::unique_ptr<OgrDataset> openGeoJsonSeq(const std::string &path,
                                           std::string &err);
std::unique_ptr<OgrDataset> openEsriJson(const std::string &path,
                                         std::string &err);
std::unique_ptr<OgrDataset> openTopoJson(const std::string &path,
                                         std::string &err);
std::unique_ptr<OgrDataset> openVectorDataset(
    const std::string &path, std::string &err,
    const std::vector<std::string> &inputFormats,
    const std::vector<std::string> &openOptions = {},
    bool seqOpenErrors = true);
std::unique_ptr<OgrDataset> openShapefile(
    const std::string &path, std::string &err,
    const std::vector<std::string> &openOptions = {});

// convert-time transform plumbing (vector reproject and friends): the
// gate runs once per resolved layer after the same-dataset guard, the
// hook once per feature at the writers' iteration point so its errors
// interleave with progress the way the reference streams them
extern std::function<int(const OgrLayer &)> g_convertLayerGate;
extern std::function<void(const OgrLayer &, OgrFeature &)>
    g_convertFeatureHook;
// dataset-wide transform (vector filter/select/sql): runs after the
// source layer resolution (so -l lookup errors keep precedence) and may
// replace the layer set wholesale; nonzero return aborts before any
// output is created
extern std::function<int(OgrDataset &)> g_convertDatasetMutate;
extern std::unique_ptr<OgrDataset> g_convertSourceOverride;
// non-terminal pipeline writes hand their written dataset object to the
// next step; convert captures the driver-specific skeleton here
extern bool g_convertCaptureWritten;
extern std::unique_ptr<OgrDataset> g_convertWrittenDs;
// pipeline transition steps (contour/polygonize/footprint feeding an
// info terminal): the verb's convert delegate hands its source dataset
// back instead of writing
extern bool g_pipelineTransCapture;
extern std::unique_ptr<OgrDataset> g_pipelineTransCaptured;
// transition write terminals force the delegate quiet (the engine draws
// the bar itself) but the writer's Z-discard warning must still surface
extern bool g_transZWarnEnable;
// a rasterize transition materializes into a cwd temp file that the
// reference reports as having no associated files at the info terminal;
// info renders the live dataset's SRS (source-derived) rather than the
// EPSG-db reconstruction the geokeys would round-trip to
extern std::string g_infoFilesHide;
extern bool g_infoFilesHideDerived;
extern bool g_infoSrsOverrideSet;
extern Srs g_infoSrsOverride;
extern bool g_rasterizeLastSrsSet;
extern Srs g_rasterizeLastSrs;
// GeoJSON coordinate formatting (%.15f with roundoff trimming), shared by
// the writer and the JSON info geometry dump
std::string ogrJsonCoord(double v);

// empty result means valid; otherwise the message body after "cmd: "
std::string vectorOutputDriverResolve(const std::string &format,
                                      std::string &driver);

// partition parts carry the preserved source FIDs as explicit GeoJSON
// feature ids; the flag scopes the writer's id emission to those runs
extern bool g_gjForceFidIds;
// shapefile writer compatibility probes for partition's pre-checks: the
// SHPT a geometry type resolves to (-1 undecidable) and the writer's
// mismatch error text ("" when the geometry is writable as-is)
int shpTypeForGeomProbe(int geomType, bool hasZ, bool hasM);
std::string shpGeomMismatchError(int shpType, const OgrGeometry &g);
// serialized GeoJSON feature-line length, for partition's max-file-size
// rotation estimates (diagnostics suppressed)
size_t gjFeatureLineSize(const OgrLayer &lyr, const OgrFeature &f);

// vector filter/select/sql transform bodies, shared by the standalone
// verbs (convert dataset-mutate hook) and pipeline step chains; errors
// are reported, nonzero return means abort
int vectorFilterApplyStep(OgrDataset &d, const std::string &where,
                          const std::string &activeLayer,
                          const std::vector<std::string> &layerSel,
                          bool hasBbox, const double bbox[4]);
int vectorSelectApplyStep(OgrDataset &d,
                          const std::vector<std::string> &fields,
                          bool exclude, bool ignoreMissing,
                          const std::string &activeLayer,
                          const std::vector<std::string> &layerSel);
int vectorSqlApplyStep(OgrDataset &d,
                       const std::vector<std::string> &stmts,
                       const std::vector<std::string> &outNames,
                       const std::string &dialect,
                       const std::string &verb);
void vectorLayerRecomputeExtent(OgrLayer &lyr);
extern bool g_pipelineFailProgressForce;
// single-token pipelines let progress reach /vsistdout/ outputs
extern bool g_pipelineBarStdout;
// count-hiding pipeline steps defer the whole bar to after the last feature
extern bool g_pipelineWriteBarAtEnd;

// WKT-style geometry type name parse (POINT, MULTIPOINTZ, "POINT Z",
// Point25D, ...); false when the name is not a geometry type
bool ogrGeomTypeFromWktName(const std::string &name, int &type, bool &z,
                            bool &m);
// flat-type maps mirroring the reference's multi/single/linear/curve
// layer conversions (single of a collection is Unknown)
int ogrGtCollection(int t);
int ogrGtSingle(int t);
int ogrGtCurve(int t);
int ogrGtLinear(int t);
// force the coordinate dimension: absent Z slots appear as 0, stripped
// ones reset to 0 so later promotions read zeros
void ogrSetGeomDim(OgrGeometry &g, bool z, bool m);
// OGRGeometryFactory::forceTo for the linear type subset: wrap/unwrap,
// ring<->line, polygon merges and the in-order linestring merge; the
// geometry keeps its shape when the conversion is not possible
void ogrForceTo(OgrGeometry &g, int target);
// polygon with a single closed 4-point ring (triangle-convertible)
bool polyIsTriangle(const OgrGeometry &poly);
// OGRGeometry::segmentize: canonical whole-line direction, per-segment
// floor(sqrt(d2/m2) - 0.01) intermediate points, Z/M copied from the
// canonical segment start
void ogrSegmentize(OgrGeometry &g, double maxLength);

// geometry verb transform bodies (swap-xy/segmentize/make-point/
// explode-collections/set-geom-type), shared by the standalone verbs
// and pipeline step chains
int vectorSwapXyApplyStep(OgrDataset &d, const std::string &activeGeom,
                          const std::string &activeLayer,
                          const std::vector<std::string> &layerSel);
int vectorSegmentizeApplyStep(OgrDataset &d, double maxLength,
                              const std::string &activeGeom,
                              const std::string &activeLayer,
                              const std::vector<std::string> &layerSel);
int vectorMakePointApplyStep(OgrDataset &d, const std::string &xField,
                             const std::string &yField,
                             const std::string &zField,
                             const std::string &mField,
                             const std::string &dstCrs,
                             const std::vector<std::string> &layerSel);
int vectorExplodeApplyStep(OgrDataset &d, const std::string &geomType,
                           bool skipMismatch,
                           const std::string &activeGeom,
                           const std::string &activeLayer,
                           const std::vector<std::string> &layerSel);
struct SetGeomTypeOpts
{
    std::string geomType;
    bool multi = false, single = false;
    bool linear = false, curve = false;
    std::string dim;
    bool layerOnly = false, featureOnly = false;
    bool skip = false;
    std::string activeGeom, activeLayer;
    std::vector<std::string> layerSel;
};
int vectorSetGeomTypeApplyStep(OgrDataset &d, const SetGeomTypeOpts &o);

struct VectorEditOpts
{
    std::string geomType;
    std::string crs;
    std::vector<std::string> metadata, unsetMetadata;
    std::vector<std::string> layerMetadata, unsetLayerMetadata;
    bool unsetFid = false;
    std::string activeLayer;
    std::vector<std::string> layerSel;
};
int vectorEditApplyStep(OgrDataset &d, const VectorEditOpts &o);

struct VectorRenameLayerOpts
{
    std::string inputLayer, outputLayer;
    bool ascii = false, lowerCase = false, fnCompat = false;
    std::string reserved, replacement;
    bool hasReplacement = false;
    long long maxLength = -1;
};
int vectorRenameLayerValidate(const OgrDataset &d,
                              const VectorRenameLayerOpts &o);
int vectorRenameLayerApplyStep(OgrDataset &d,
                               const VectorRenameLayerOpts &o);

// a verb whose streamed feature translation fails registers the failure
// here; the write step replays the reference's choreography. Setup
// failures (bad field name) error once per scan, so passErrors re-fire
// on the write pass that follows a counting pass and the write ends in
// "Failed to write layer". A mid-stream value failure fires once and
// permanently exhausts the stream: a preceding counting pass leaves the
// write pass a clean empty layer, while a single-pass (quiet) write
// materializes the features translated before the failure (quietKept)
// and ends in error. --skip-errors turns the error endings into clean
// completions without silencing the per-scan messages.
struct ConvertTranslateFail
{
    bool active = false;
    std::string layer;
    std::vector<std::string> passErrors;
    bool oneShot = false;
    std::vector<OgrFeature> quietKept;
};
extern ConvertTranslateFail g_convertTranslateFail;

// clip drops every feature (no GEOS): envelope-disjoint ones silently,
// intersecting ones with a per-feature GEOS error at the stream's pull
// point. The writer replays each layer's errors at its iteration turn
// and ends failing layers in "Failed to write layer" unless
// --skip-errors keeps them clean (errors still emitted).
struct ConvertClipPending
{
    bool active = false;
    struct L
    {
        std::string layer;
        // severity+message pairs; CPLE class 6 = NotSupported
        std::vector<std::pair<int, std::string>> errors;
        bool fail = false;
    };
    std::vector<L> layers;
    const L *find(const std::string &n) const
    {
        if (!active)
            return nullptr;
        for (const auto &e : layers)
            if (e.layer == n)
                return &e;
        return nullptr;
    }
    bool anyFail() const
    {
        if (!active)
            return false;
        for (const auto &e : layers)
            if (e.fail)
                return true;
        return false;
    }
};
extern ConvertClipPending g_convertClipPending;
void convertClipEmitLayerErrors(const ConvertClipPending::L &e);

// terminal-dependent emission of clip's per-feature stream errors:
// 0 defers them to the writer (leaf/write pipelines), N>0 replays them
// immediately N times (info's count/extent/features pulls), -1 keeps
// them silent (schema-only terminals never pull features)
extern int g_vectorClipEmitPulls;

struct VectorClipOpts
{
    bool hasBbox = false;
    double bbox[4] = {0, 0, 0, 0};
    std::string bboxCrs;
    std::string geometry;
    std::string geometryCrs;
    bool hasLike = false;
    std::string like;
    std::string likeSql;
    std::string likeLayer;
    std::string likeWhere;
    std::string activeLayer;
};
int vectorClipApplyStep(OgrDataset &d, const VectorClipOpts &o,
                        const std::vector<std::string> &layerSel);

// raster clip's geometry text parse (WKT then GeoJSON) and envelope
// walk, shared with vector clip
bool clipGeometryParseText(const std::string &text, OgrGeometry &g);
bool clipGeometryEnvelope(const OgrGeometry &g, double &xmin, double &ymin,
                          double &xmax, double &ymax);

struct VectorCombineOpts
{
    std::vector<std::string> groupBy;
    bool keepNested = false;
};
int vectorCombineApplyStep(OgrDataset &d, const VectorCombineOpts &o,
                           const std::vector<std::string> &layerSel);

struct VectorUpdateOpts
{
    bool hasInputLayer = false;
    std::string inputLayer;
    bool hasOutputLayer = false;
    std::string outputLayer;
    std::string mode = "merge";
    std::vector<std::string> keys;
};
// merges src into the already-open target dataset and writes the target
// back (full rewrite when features changed, splice for pure appends,
// always a rewrite for the seq driver); ownBar draws the per-feature
// progress sweep
int vectorUpdateRun(OgrDataset &src, std::unique_ptr<OgrDataset> tgt,
                    const std::string &outPath,
                    const VectorUpdateOpts &o, bool quiet, bool ownBar);
// GeoJSON in-place rewrite: header members re-emitted from the source
// document (name from the layer, crs regenerated, bbox recomputed and
// space-padded), untouched features from their native JSON, touched
// ones from the standard writer, all in fid order
bool geoJsonUpdateRewrite(const OgrLayer &lyr, const JVal *root,
                          const std::string &path);
// document-building core of the rewrite (also feeds /vsistdout/)
std::string geoJsonUpdateRewriteDoc(const OgrLayer &lyr,
                                    const JVal *root);

// vector concat union engine: builds the merged in-memory dataset the
// write/info terminals consume. Reports its own errors (prefix
// "concat: ") and returns nonzero after the reference's single-error
// choreography.
struct VectorConcatOpts
{
    std::string mode;         // empty = merge-per-layer-name
    std::string outputLayer;  // single: name; stack: template
    std::string slfName, slfContent;
    std::string fieldStrategy;  // empty = union
    std::string srcCrs, dstCrs;
    // export-schema terminals never surface the UnionLayer SRS warnings
    bool srsWarnings = true;
    // MEM and stream outputs build the union silently
    bool typeWarnings = true;
};
int vectorConcatBuildUnion(
    const std::vector<std::string> &inputPaths,
    const std::vector<std::unique_ptr<OgrDataset>> &dss,
    const std::vector<std::string> &layerSel, const VectorConcatOpts &o,
    OgrDataset &out);
// output layer names the union will produce, in order (no warnings)
std::vector<std::string> vectorConcatGroupNames(
    const std::vector<std::string> &inputPaths,
    const std::vector<std::unique_ptr<OgrDataset>> &dss,
    const std::vector<std::string> &layerSel, const VectorConcatOpts &o);

// CRS-to-CRS transform handles shared with the reproject machinery
// (PROJ op + normalize-for-visualization, per-op failure counter)
void *vectorCrsOpCreate(const Srs &src, const Srs &dst);
bool vectorCrsOpApply(void *op, OgrGeometry &g);
void vectorCrsOpFree(void *op);

struct WarnLog
{
    std::vector<std::pair<long long, std::string>> msgs;
    long long fid = -1;
    // an inconvertible value fails the whole feature: it is warned
    // about, then dropped from the translated stream
    bool drop = false;
    void add(std::string m) { msgs.emplace_back(fid, std::move(m)); }
    void addCannot(std::string m)
    {
        drop = true;
        add(std::move(m));
    }
};
// SetFrom-style single-value coercion (used by set-field-type and
// update): src declares the stored type, dst is the target definition
void leafConvert(WarnLog &log, const std::string &lyrName,
                 const OgrFieldDefn &src, int dstType, int dstSub,
                 OgrFieldValue &fv);

struct VectorSetFieldTypeOpts
{
    bool hasFieldName = false;
    std::string fieldName;
    bool hasSrcType = false;
    std::string srcTypeName;
    bool hasDstType = false;
    std::string dstTypeName;
    bool hasActiveLayer = false;
    std::string activeLayer;
};
// CLI type-name lookup (case-insensitive; Boolean/Int16/Float32/JSON/UUID
// map to base type + subtype)
bool vectorFieldTypeNameParse(const std::string &name, int &type, int &sub);
// existence checks only; error text differs between the read-absorbed
// cast form and the SetFrom form, usage is the caller's business
int vectorSetFieldTypeValidate(OgrDataset &d,
                               const VectorSetFieldTypeOpts &o,
                               bool castMode);
// SetFrom conversion body; collected per-feature warnings replay
// emitCount times (the terminal's pulls), then once more for the
// feature extraFid names
void vectorSetFieldTypeConvert(OgrDataset &d,
                               const VectorSetFieldTypeOpts &o,
                               bool castMode, int emitCount,
                               long long extraFid);
// a contiguous group of read-absorbed cast steps folds into one schema
// override applied to the raw values: each step's matching sees the
// types as overridden so far, the final type per field wins, and the
// group's warnings emit as a unit
void vectorSetFieldTypeCastGroup(
    OgrDataset &d, const std::vector<VectorSetFieldTypeOpts> &group,
    int emitCount, long long extraFid);
// pre-mutate validation hook: convert runs it after the output
// exists-check and before source layer resolution, adding usage on
// failure
extern std::function<int(OgrDataset &)> g_convertDatasetPreCheck;
// terminal info step's --fid (-1 when absent): the fid scan re-pulls the
// named feature, replaying its cast warnings once more
extern long long g_vectorInfoFid;
// pulls the resolved output driver will make on the source stream (0
// for stream outputs nobody consumes and unresolvable drivers, else 1);
// convert computes it before running the mutate hook
extern int g_convertWritePulls;
// a chain made solely of read-absorbed casts keeps the read dataset
// identity for the info terminal (driver name, metadata domains)
extern bool g_vectorSftAllCast;
// a mutate-hook failure that models a read-side error skips the write
// terminal's fake-complete progress line
extern bool g_pipelineMutateSilentFail;
// true while the mutate hook runs under a bar-rendering write terminal:
// eagerly executed steps (update) advance the shared bar as they finish
extern bool g_convertMutateBarOk;
