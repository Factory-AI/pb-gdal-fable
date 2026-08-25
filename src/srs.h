#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

// raw geokey values extracted from a GeoTIFF directory (short values,
// doubles from tag 34736, ascii segments from tag 34737)
struct GTiffKeyValues
{
    std::map<int, int> shorts;
    std::map<int, double> dbls;
    std::map<int, std::string> asciis;
};

struct PJconsts;
struct pj_ctx;

class Srs
{
  public:
    Srs() = default;
    ~Srs();
    // copies deep-clone the PROJ object
    Srs(const Srs &o);
    Srs &operator=(const Srs &o);
    Srs(Srs &&o) noexcept;
    Srs &operator=(Srs &&o) noexcept;
    Srs clone() const;

    static Srs fromUserInput(const std::string &def, bool &ok);
    // CLI-facing CRS parse mirroring OGRSpatialReference::SetFromUserInput:
    // only recognized syntaxes reach PROJ; loud surfaces PROJ's own error
    // lines (crs not found / WKT grammar) before the caller's failure
    static Srs fromCliInput(const std::string &def, bool &ok,
                            bool loud = false);
    // ESRI .prj import following the shapefile driver conventions:
    // AutoIdentifyEPSG-style WKT1 rebuild for geographic and UTM cases,
    // EPSG database replacement for other confident matches
    static Srs fromEsriPrj(const std::string &prj, bool &ok);
    static Srs fromEpsg(int code, bool &ok);
    static Srs fromAuthority(const std::string &auth, int code, bool &ok);
    // GTiff geokey reading path. Geographic codes other than 4326 are
    // rebuilt from components (recomputed inverse flattening, no usage);
    // projected CRSs keep the db object but WGS 84 datum ensembles render
    // as a plain datum.
    static Srs fromEpsgGTiff(int code, bool projected, bool &ok,
                             bool gtCitation = true);
    // user-defined geokey directories: WKT2 recomposition mirroring the
    // libgeotiff GTIFGetDefn readback (citation names, db datum names,
    // false easting/northing scaled to metres, degree-only conversion
    // naming)
    static Srs fromGTiffKeys(const GTiffKeyValues &kv, bool &ok);
    // horizontal EPSG code + vertical EPSG code: compound rebuilt from the
    // geokey-flavor horizontal plus the database vertical, then identified
    // against EPSG so catalogued pairs regain their compound code. A
    // non-positive vertCode builds the manual "unknown" vertical from the
    // units geokey; a GTCitation overrides the compound name.
    static Srs fromGTiffCompound(int horizCode, bool projected,
                                 int vertCode, bool &ok,
                                 const std::string &nameOverride = "",
                                 int vertUnitCode = 0,
                                 int vertDatumCode = 0);
    // database CRS round-tripped through GDAL-flavor WKT1 (GeoTIFF 1.0
    // vertical directories and mismatched angular-unit keys render the
    // horizontal this way); latLonBaseAxes renames the base axes to the
    // programmatic Latitude/lat flavor
    static Srs fromGTiffWkt1RoundTrip(int code, bool &ok,
                                      bool latLonBaseAxes = false);
    // "<pcs name> + <vertical name|unknown>", the citation content a
    // GeoTIFF 1.0 vertical directory is checked against
    static std::string gtiffCompoundNameFor(int pcs, int vertCode);
    // db type of a GeographicTypeGeoKey code: 0 unknown, 1 geographic/
    // geodetic, 2 geocentric, 3 other (projected etc.)
    static int gtiffGcsTypeClass(int code);
    // coded PCS whose short override keys force a GTIFGetDefn-style
    // component recomposition (conversion renamed to the method, rebuilt
    // base, registry-comparison warnings)
    static Srs fromGTiffProjectedRebuild(const GTiffKeyValues &kv, int pcs,
                                         bool &ok);
    // geodetic CRS EPSG code of a database projected CRS (0 when
    // unavailable)
    static int gcsCodeOfPcs(int pcs);
    // band unit name for legacy vertical keys: the database vertical CRS
    // axis unit, else the units-key uom name, else metre
    static std::string gtiffVerticalUnitName(int vertCode, int unitCode);
    // ProjLinearUnitsGeoKey disagreeing with the database unit: true plus
    // the override unit when they differ
    static bool pcsLinearUnitDiffers(int pcs, int code, std::string &name,
                                     double &factor);
    // GeogAngularUnitsGeoKey disagreeing with the database base unit
    // (factor comparison, so 9102 and 9122 both mean degree)
    static bool pcsAngularUnitDiffers(int pcs, int code);

    bool valid() const { return pj_ != nullptr; }
    // replaces the CS angular unit verbatim, bypassing the WKT parser's
    // snap of near-degree factors to the exact pi/180 double
    void setCustomAngularUnit(const std::string &name, double conv);
    std::string name() const;
    std::string authName() const;
    std::string code() const;
    std::string typeString() const;
    bool areaOfUse(double &w, double &s, double &e, double &n,
                   std::string &areaName) const;
    std::string wkt2_2019() const;
    std::string wkt1Gdal() const;
    // WKT1 with EPSG AUTHORITY nodes injected on datum/spheroid/primem,
    // matching OGRSpatialReference::exportToWkt for DB-resolved CRSs
    std::string wkt1GdalFull() const;
    std::string wkt1Esri() const;
    std::string wkt2SingleLine() const;
    std::string projjson() const;
    bool isGeographic() const;
    bool isGeographic3D() const;
    bool isProjected() const;
    bool isVertical() const;
    // GeoTIFF VerticalCSTypeGeoKey holding a geographic 3D code on a
    // projected directory: projected 3D rebuilt over the given base,
    // keeping the conversion identity, unit codes stripped from the axes
    Srs promotedTo3D(const Srs &geog3D) const;
    // SetLinearUnitsAndUpdateParameters flavor: converts conversion
    // parameters and CS axes to the new unit, renaming the conversion
    // "unknown" and dropping the CRS identifier; the CS unit carries
    // GDAL's swapped ID[code,"EPSG"] quirk
    void alterLinearUnit(const std::string &name, double factor, int code);
    // ellipsoid semi-major axis in metres (0 when unavailable)
    double semiMajor() const;
    // false when the root ID resolves in proj.db to an object of a
    // different type (textual geokey rebuilds keeping a geocentric code)
    bool idTypeMatchesDb() const;
    // root ID resolves in proj.db and the object is equivalent to the
    // registry definition (raster info text keeps the summary card only
    // then)
    bool matchesDbDefinition() const;
    // unit name of the vertical axis for compound CRSs ("" otherwise)
    std::string verticalUnitName() const;
    // [2,1] when CRS axis order is lat,lon (traditional GIS strategy)
    std::vector<int> dataAxisToSRSAxisMapping() const;
    int epsgCode() const;  // -1 if none
    // IsSame-flavor equivalence: both sides normalized to GIS axis order
    // then compared with PROJ's EQUIVALENT criterion
    bool isEquivalentTo(const Srs &o) const;

    // geodetic (lat/long) base CRS transform: returns lon/lat for projected
    bool toWgs84(double x, double y, double &lon, double &lat) const;
    bool toGeodetic(double x, double y, double &lon, double &lat) const;

    // envelope of the 4 bbox corners transformed into dst (GIS axis order
    // both sides)
    bool transformBoundsTo(const Srs &dst, double &xmin, double &ymin,
                           double &xmax, double &ymax) const;

    struct PJconsts *pj() const { return pj_; }

  private:
    struct PJconsts *pj_ = nullptr;
    bool wgs84DatumSwap_ = false;
    // 3D-promoted projected CRSs print their axis units without the EPSG
    // codes the constructed coordinate system carries
    bool strip3DUnitIds_ = false;
    // EPSG codes captured before a component rebuild so WKT1 output can
    // re-add the AUTHORITY nodes the rebuilt object lost
    std::string wkt1DatumCode_, wkt1EllpsCode_, wkt1PmCode_,
        wkt1LinUnitCode_;
};

struct pj_ctx *projCtx();

// true when the effective PROJ search path has no usable proj.db
bool projDbMissing();
