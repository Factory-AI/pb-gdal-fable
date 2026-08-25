#include "srs.h"
#include "cpl.h"
#include "json.h"
#include "jsonc.h"
#include "proj_min.h"
#include "util.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{

// libproj's C api reports some failures through ctx->logger directly,
// bypassing the log level; a no-op logger must stay installed so those
// never reach stderr unprefixed
void projQuietLogger(void *, int, const char *)
{
}

}  // namespace

PJ_CONTEXT *projCtx()
{
    static PJ_CONTEXT *ctx = [] {
        PJ_CONTEXT *c = proj_context_create();
        proj_context_use_proj4_init_rules(c, 1);
        std::string pd = configGet("PROJ_DATA", "");
        if (pd.empty())
            pd = configGet("PROJ_LIB", "");
        if (!pd.empty())
        {
            const char *paths[1] = {pd.c_str()};
            proj_context_set_search_paths(c, 1, paths);
        }
        proj_log_func(c, nullptr, projQuietLogger);
        proj_log_level(c, 0);  // PJ_LOG_NONE: GDAL routes PROJ logs to debug
        return c;
    }();
    return ctx;
}

bool projDbMissing()
{
    const char *db = proj_context_get_database_path(projCtx());
    return !db || !*db;
}

Srs::~Srs()
{
    if (pj_)
        proj_destroy(pj_);
}

Srs::Srs(const Srs &o)
{
    *this = o;
}

Srs &Srs::operator=(const Srs &o)
{
    if (this != &o)
        *this = o.clone();
    return *this;
}

Srs::Srs(Srs &&o) noexcept
    : pj_(o.pj_),
      wgs84DatumSwap_(o.wgs84DatumSwap_),
      strip3DUnitIds_(o.strip3DUnitIds_),
      wkt1DatumCode_(std::move(o.wkt1DatumCode_)),
      wkt1EllpsCode_(std::move(o.wkt1EllpsCode_)),
      wkt1PmCode_(std::move(o.wkt1PmCode_)),
      wkt1LinUnitCode_(std::move(o.wkt1LinUnitCode_))
{
    o.pj_ = nullptr;
}

Srs &Srs::operator=(Srs &&o) noexcept
{
    if (this != &o)
    {
        if (pj_)
            proj_destroy(pj_);
        pj_ = o.pj_;
        wgs84DatumSwap_ = o.wgs84DatumSwap_;
        strip3DUnitIds_ = o.strip3DUnitIds_;
        wkt1DatumCode_ = std::move(o.wkt1DatumCode_);
        wkt1EllpsCode_ = std::move(o.wkt1EllpsCode_);
        wkt1PmCode_ = std::move(o.wkt1PmCode_);
        wkt1LinUnitCode_ = std::move(o.wkt1LinUnitCode_);
        o.pj_ = nullptr;
    }
    return *this;
}

Srs Srs::clone() const
{
    Srs s;
    if (pj_)
        s.pj_ = proj_clone(projCtx(), pj_);
    s.wgs84DatumSwap_ = wgs84DatumSwap_;
    s.strip3DUnitIds_ = strip3DUnitIds_;
    s.wkt1DatumCode_ = wkt1DatumCode_;
    s.wkt1EllpsCode_ = wkt1EllpsCode_;
    s.wkt1PmCode_ = wkt1PmCode_;
    s.wkt1LinUnitCode_ = wkt1LinUnitCode_;
    return s;
}

Srs Srs::fromUserInput(const std::string &def, bool &ok)
{
    Srs s;
    s.pj_ = proj_create(projCtx(), def.c_str());
    ok = s.pj_ != nullptr;
    if (s.pj_ && proj_is_crs(s.pj_) && proj_is_deprecated(s.pj_) &&
        strncasecmp(def.c_str(), "EPSG:", 5) == 0)
    {
        PJ_OBJ_LIST *l = proj_get_non_deprecated(projCtx(), s.pj_);
        if (l && proj_list_get_count(l) > 0)
        {
            PJ *repl = proj_list_get(projCtx(), l, 0);
            const char *auth = proj_get_id_auth_name(repl, 0);
            const char *code = proj_get_id_code(repl, 0);
            cplErrorStr(
                CE_Warning, CPLE_AppDefined,
                "CRS " + def + " is deprecated. Its non-deprecated "
                "replacement " + (auth ? auth : "") + ":" +
                    (code ? code : "") + " will be used instead. To use the "
                    "original CRS, set the OSR_USE_NON_DEPRECATED "
                    "configuration option to NO.");
            if (configGet("OSR_USE_NON_DEPRECATED", "YES") != "NO")
            {
                proj_destroy(s.pj_);
                s.pj_ = repl;
            }
            else
                proj_destroy(repl);
        }
        if (l)
            proj_list_destroy(l);
    }
    return s;
}

namespace
{

bool looksLikeWktCrs(const std::string &def)
{
    static const char *const kw[] = {
        "GEOGCS",        "GEOCCS",        "PROJCS",       "BOUNDCRS",
        "VERT_CS",       "VERTCS",        "COMPD_CS",     "LOCAL_CS",
        "FITTED_CS",     "GEODCRS",       "GEODETICCRS",  "GEOGCRS",
        "GEOGRAPHICCRS", "PROJCRS",       "PROJECTEDCRS", "VERTCRS",
        "VERTICALCRS",   "COMPOUNDCRS",   "ENGCRS",       "ENGINEERINGCRS",
        "DERIVEDPROJCRS"};
    for (const char *k : kw)
    {
        size_t n = strlen(k);
        if (strncasecmp(def.c_str(), k, n) == 0)
        {
            const char *p = def.c_str() + n;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                ++p;
            if (*p == '[')
                return true;
        }
    }
    return false;
}

}  // namespace

Srs Srs::fromCliInput(const std::string &def, bool &ok, bool loud)
{
    ok = false;
    if (def.empty())
        return Srs();
    if (looksLikeWktCrs(def))
    {
        Srs s = fromUserInput(def, ok);
        if (!ok && loud)
        {
            PROJ_STRING_LIST warnings = nullptr, errors = nullptr;
            PJ *obj = proj_create_from_wkt(projCtx(), def.c_str(), nullptr,
                                           &warnings, &errors);
            if (obj)
                proj_destroy(obj);
            for (PROJ_STRING_LIST e = errors; e && *e; ++e)
                cplErrorStr(CE_Failure, CPLE_AppDefined, *e);
            proj_string_list_destroy(warnings);
            proj_string_list_destroy(errors);
        }
        return s;
    }
    if (def[0] == '+')
    {
        // proj strings parse as operations unless forced to a CRS, the
        // way SetFromUserInput's proj4 import does
        std::string d = def;
        if (def.find("type=crs") == std::string::npos)
            d += " +type=crs";
        Srs s = fromUserInput(d, ok);
        if (!ok && loud)
        {
            std::vector<std::string> logged;
            proj_log_func(projCtx(), &logged,
                          [](void *u, int lvl, const char *msg) {
                              if (lvl == 1 && msg)
                                  static_cast<std::vector<std::string> *>(u)
                                      ->push_back(msg);
                          });
            proj_log_level(projCtx(), 1);
            bool ok2 = false;
            fromUserInput(d, ok2);
            proj_log_level(projCtx(), 0);
            proj_log_func(projCtx(), nullptr, projQuietLogger);
            std::string prev;
            for (const auto &m : logged)
            {
                if (m == prev)
                    continue;
                prev = m;
                cplErrorStr(CE_Failure, CPLE_AppDefined, "PROJ: " + m);
            }
        }
        return s;
    }
    if (strncasecmp(def.c_str(), "urn:", 4) == 0)
    {
        Srs s = fromUserInput(def, ok);
        if (!ok && loud &&
            strncasecmp(def.c_str(), "urn:ogc:def:crs", 15) == 0)
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        projDbMissing()
                            ? "PROJ: proj_create_from_database: Cannot "
                              "find proj.db"
                            : "PROJ: proj_create_from_database: crs "
                              "not found");
        return s;
    }
    if (def[0] == '{')
        return fromUserInput(def, ok);
    for (const char *n : {"WGS84", "WGS72", "NAD27", "NAD83"})
        if (strcasecmp(def.c_str(), n) == 0)
            return fromUserInput(def, ok);
    size_t colon = def.find(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < def.size())
    {
        bool authOk = true;
        for (size_t i = 0; i < colon; ++i)
        {
            unsigned char c = (unsigned char)def[i];
            if (!isalnum(c) && c != '_' && c != '-')
                authOk = false;
        }
        if (authOk)
        {
            Srs s = fromUserInput(def, ok);
            if (!ok && loud)
            {
                bool compound =
                    def.find('+', colon) != std::string::npos;
                const char *msg;
                if (projDbMissing())
                {
                    bool epsg = (colon == 4 &&
                                 strncasecmp(def.c_str(), "EPSG", 4) == 0) ||
                                (colon == 5 &&
                                 strncasecmp(def.c_str(), "EPSGA", 5) == 0);
                    msg = compound
                              ? "PROJ: proj_create: no database context "
                                "specified"
                          : epsg
                              ? "PROJ: proj_create_from_database: Cannot "
                                "find proj.db"
                              : "PROJ: proj_get_authorities_from_database: "
                                "Cannot find proj.db";
                }
                else
                    msg = compound
                              ? "PROJ: proj_create: crs not found"
                              : "PROJ: proj_create_from_database: crs "
                                "not found";
                cplErrorStr(CE_Failure, CPLE_AppDefined, msg);
            }
            return s;
        }
    }
    return Srs();
}

Srs Srs::fromEpsg(int codeNum, bool &ok)
{
    Srs s;
    s.pj_ = proj_create_from_database(projCtx(), "EPSG",
                                      strPrintf("%d", codeNum).c_str(),
                                      PJ_CATEGORY_CRS, 0, nullptr);
    ok = s.pj_ != nullptr;
    return s;
}

Srs Srs::fromAuthority(const std::string &auth, int codeNum, bool &ok)
{
    Srs s;
    s.pj_ = proj_create_from_database(projCtx(), auth.c_str(),
                                      strPrintf("%d", codeNum).c_str(),
                                      PJ_CATEGORY_CRS, 0, nullptr);
    ok = s.pj_ != nullptr;
    return s;
}

namespace
{

// strip flat nodes like ,AUTHORITY["EPSG","6326"] or trailing AXIS pairs
void stripFlatNodes(std::string &w, const char *keyword)
{
    std::string pat = std::string(",") + keyword + "[";
    size_t pos;
    while ((pos = w.find(pat)) != std::string::npos)
    {
        size_t close = w.find(']', pos);
        if (close == std::string::npos)
            break;
        w.erase(pos, close - pos + 1);
    }
}

std::string wktSection(const std::string &w, const std::string &keyword,
                       size_t *outStart = nullptr)
{
    size_t pos = w.find(keyword + "[");
    if (pos == std::string::npos)
        return "";
    int depth = 0;
    bool inStr = false;
    for (size_t i = pos + keyword.size(); i < w.size(); i++)
    {
        char c = w[i];
        if (inStr)
        {
            if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
            inStr = true;
        else if (c == '[')
            depth++;
        else if (c == ']')
        {
            depth--;
            if (depth == 0)
            {
                if (outStart)
                    *outStart = pos;
                return w.substr(pos, i - pos + 1);
            }
        }
    }
    return "";
}

double wktParam(const std::string &w, const char *name, bool &found)
{
    std::string pat = std::string("PARAMETER[\"") + name + "\",";
    size_t pos = w.find(pat);
    found = pos != std::string::npos;
    if (!found)
        return 0.0;
    return atof(w.c_str() + pos + pat.size());
}

// GDAL GetEPSGGeogCS style identification from the WKT1 form
int identifyGeogCs(const std::string &w)
{
    std::string datum = wktSection(w, "DATUM");
    size_t a = datum.find("AUTHORITY[\"EPSG\",\"");
    if (a != std::string::npos)
    {
        int code = atoi(datum.c_str() + a + 18);
        if (code >= 6000 && code <= 6999)
            return code - 2000;
    }
    if (datum.find("World Geodetic System 1984") != std::string::npos ||
        (datum.find("WGS") != std::string::npos &&
         datum.find("84") != std::string::npos))
        return 4326;
    if (datum.find("North American Datum 1983") != std::string::npos ||
        (datum.find("NAD") != std::string::npos &&
         datum.find("83") != std::string::npos))
        return 4269;
    if (datum.find("North American Datum 1927") != std::string::npos ||
        (datum.find("NAD") != std::string::npos &&
         datum.find("27") != std::string::npos))
        return 4267;
    if (datum.find("World Geodetic System 1972") != std::string::npos ||
        (datum.find("WGS") != std::string::npos &&
         datum.find("72") != std::string::npos))
        return 4322;
    return 0;
}

// GetUTMZone-equivalent from the WKT1 projection parameters
int identifyUtm(const std::string &w, bool &north)
{
    if (w.find("PROJECTION[\"Transverse_Mercator\"]") == std::string::npos)
        return 0;
    bool f = false;
    if (wktParam(w, "latitude_of_origin", f) != 0.0 || !f)
        return 0;
    if (wktParam(w, "scale_factor", f) != 0.9996 || !f)
        return 0;
    if (wktParam(w, "false_easting", f) != 500000.0 || !f)
        return 0;
    double fn = wktParam(w, "false_northing", f);
    if (!f || (fn != 0.0 && fn != 10000000.0))
        return 0;
    north = fn == 0.0;
    double cm = wktParam(w, "central_meridian", f);
    if (!f)
        return 0;
    double zoneD = (cm + 183.0) / 6.0;
    int zone = (int)(zoneD + 0.5);
    if (zone < 1 || zone > 60 || zoneD != (double)zone)
        return 0;
    return zone;
}

}  // namespace

Srs Srs::fromEsriPrj(const std::string &prjIn, bool &ok)
{
    Srs out;
    ok = false;
    // ESRI compound "GEOGCS[...],VERTCS[...]": only the horizontal part
    // is used
    std::string prj = wktSection(prjIn, "PROJCS");
    if (prj.empty())
        prj = wktSection(prjIn, "GEOGCS");
    if (prj.empty())
        prj = prjIn;
    PJ *p = proj_create(projCtx(), prj.c_str());
    if (!p)
        return out;
    PJ_TYPE t = proj_get_type(p);
    bool projected = t == PJ_TYPE_PROJECTED_CRS;
    bool geographic = t == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
                      t == PJ_TYPE_GEOGRAPHIC_CRS ||
                      t == PJ_TYPE_GEOGRAPHIC_3D_CRS;
    if (!projected && !geographic)
    {
        out.pj_ = p;
        ok = true;
        return out;
    }
    const char *o1[] = {"MULTILINE=NO", nullptr};
    const char *w1c = proj_as_wkt(projCtx(), p, PJ_WKT1_GDAL, o1);
    std::string w1 = w1c ? w1c : "";
    if (w1.empty())
    {
        out.pj_ = p;
        ok = true;
        return out;
    }
    int gcs = identifyGeogCs(w1);
    int top = 0;
    if (geographic)
        top = gcs;
    else if (gcs != 0)
    {
        bool north = true;
        int zone = identifyUtm(w1, north);
        if (zone != 0)
        {
            if (gcs == 4326)
                top = (north ? 32600 : 32700) + zone;
            else if (gcs == 4269 && north)
                top = 26900 + zone;
            else if (gcs == 4267 && north)
                top = 26700 + zone;
            else if (gcs == 4322)
                top = (north ? 32200 : 32300) + zone;
        }
    }
    if (top != 0)
    {
        std::string w = w1;
        if (geographic)
        {
            stripFlatNodes(w, "AUTHORITY");
            size_t u = w.find("UNIT[\"Degree\"");
            if (u != std::string::npos)
                w.replace(u, 13, "UNIT[\"degree\"");
            w.insert(w.size() - 1,
                     ",AXIS[\"Latitude\",NORTH],AXIS[\"Longitude\",EAST]");
        }
        else
            stripFlatNodes(w, "AXIS");
        PJ *p2 = proj_create(projCtx(), w.c_str());
        if (p2 && !geographic)
        {
            // graft an identified geographic base CRS: keeps the
            // conversion parameters from the WKT1 form while the base
            // gets the canonical primem/ID
            std::string bw = wktSection(w, "GEOGCS");
            if (!bw.empty())
            {
                stripFlatNodes(bw, "AUTHORITY");
                size_t u = bw.find("UNIT[\"Degree\"");
                if (u != std::string::npos)
                    bw.replace(u, 13, "UNIT[\"degree\"");
                bw.insert(bw.size() - 1,
                          ",AXIS[\"Latitude\",NORTH],"
                          "AXIS[\"Longitude\",EAST]");
                PJ *b1 = proj_create(projCtx(), bw.c_str());
                if (b1)
                {
                    PJ *b2 = proj_alter_id(projCtx(), b1, "EPSG",
                                           strPrintf("%d", gcs).c_str());
                    if (b2)
                    {
                        PJ *p2b = proj_crs_alter_geodetic_crs(projCtx(),
                                                              p2, b2);
                        if (p2b)
                        {
                            proj_destroy(p2);
                            p2 = p2b;
                        }
                        proj_destroy(b2);
                    }
                    proj_destroy(b1);
                }
            }
        }
        if (p2)
        {
            PJ *p3 = proj_alter_id(projCtx(), p2, "EPSG",
                                   strPrintf("%d", top).c_str());
            if (p3)
            {
                proj_destroy(p2);
                proj_destroy(p);
                out.pj_ = p3;
                ok = true;
                return out;
            }
            proj_destroy(p2);
        }
        out.pj_ = p;
        ok = true;
        return out;
    }
    // FindBestMatch-style database replacement
    int *conf = nullptr;
    PJ_OBJ_LIST *lst = proj_identify(projCtx(), p, "EPSG", nullptr, &conf);
    if (lst)
    {
        int n = proj_list_get_count(lst);
        int best = -1;
        for (int i = 0; i < n; i++)
            if (conf[i] >= 90 && (best < 0 || conf[i] > conf[best]))
                best = i;
        if (best >= 0)
        {
            PJ *m = proj_list_get(projCtx(), lst, best);
            if (m)
            {
                proj_destroy(p);
                p = m;
            }
        }
        proj_list_destroy(lst);
        proj_int_list_destroy(conf);
    }
    out.pj_ = p;
    ok = true;
    return out;
}

static size_t matchBracket(const std::string &s, size_t open)
{
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < s.size(); ++i)
    {
        char c = s[i];
        if (inStr)
        {
            if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
            inStr = true;
        else if (c == '[')
            ++depth;
        else if (c == ']')
        {
            if (--depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// forced (non-ensemble) datum WKT of the CRS's geodetic base, without the
// trailing ID[...] node; empty if the base has no datum ensemble
static std::string forcedDatumWkt(PJ *crs, std::string *datumName = nullptr)
{
    PJ *base = proj_crs_get_geodetic_crs(projCtx(), crs);
    if (!base)
        return "";
    PJ *ens = proj_crs_get_datum_ensemble(projCtx(), base);
    if (!ens)
    {
        proj_destroy(base);
        return "";
    }
    proj_destroy(ens);
    PJ *forced = proj_crs_get_datum_forced(projCtx(), base);
    proj_destroy(base);
    if (!forced)
        return "";
    if (datumName)
    {
        const char *n = proj_get_name(forced);
        *datumName = n ? n : "";
    }
    const char *dw = proj_as_wkt(projCtx(), forced, PJ_WKT2_2019, nullptr);
    std::string datumWkt = dw ? dw : "";
    proj_destroy(forced);
    size_t idPos = datumWkt.rfind(",\n    ID[");
    if (idPos != std::string::npos)
    {
        size_t open = datumWkt.find('[', idPos + 3);
        size_t close = matchBracket(datumWkt, open);
        if (close != std::string::npos)
            datumWkt = datumWkt.substr(0, idPos) +
                       datumWkt.substr(close + 1);
    }
    return datumWkt;
}

static std::string spliceEnsemble(const std::string &crsWkt,
                                  const std::string &datumWkt)
{
    size_t ensPos = crsWkt.find("ENSEMBLE[");
    if (ensPos == std::string::npos || datumWkt.empty())
        return crsWkt;
    size_t lineStart = crsWkt.rfind('\n', ensPos);
    size_t indent =
        ensPos - (lineStart == std::string::npos ? 0 : lineStart + 1);
    size_t close = matchBracket(crsWkt, crsWkt.find('[', ensPos));
    if (close == std::string::npos)
        return crsWkt;
    std::string indented;
    for (char ch : datumWkt)
    {
        indented += ch;
        if (ch == '\n')
            indented.append(indent, ' ');
    }
    return crsWkt.substr(0, ensPos) + indented + crsWkt.substr(close + 1);
}

// PROJ's WKT formatter: %.15g, retried at %.14g when the result carries a
// run of ten nines (fp noise)
static std::string wktNumber(double v)
{
    std::string s = strPrintf("%.15g", v);
    if (s.find("9999999999") != std::string::npos)
        s = strPrintf("%.14g", v);
    return s;
}

static std::string removeUsageNode(const std::string &wkt)
{
    size_t usagePos = wkt.find("USAGE[");
    if (usagePos == std::string::npos)
        return wkt;
    size_t close = matchBracket(wkt, wkt.find('[', usagePos));
    if (close == std::string::npos)
        return wkt;
    size_t start = usagePos;
    while (start > 0 && (wkt[start - 1] == ' ' || wkt[start - 1] == '\n'))
        --start;
    if (start > 0 && wkt[start - 1] == ',')
        --start;
    size_t after = close + 1;
    if (after < wkt.size() && wkt[after] == ',')
        ++after;
    return wkt.substr(0, start) + wkt.substr(after);
}

// the constructed 3D coordinate system materializes EPSG unit codes on
// its axes that the reference never prints
static void stripCs3DUnitIds(std::string &wkt)
{
    size_t cs = wkt.find("CS[Cartesian,3]");
    if (cs == std::string::npos)
        return;
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (size_t p = wkt.find("LENGTHUNIT[", cs);
             p != std::string::npos; p = wkt.find("LENGTHUNIT[", p + 1))
        {
            size_t close = matchBracket(wkt, wkt.find('[', p));
            if (close == std::string::npos)
                break;
            size_t idp = wkt.find("ID[", p);
            if (idp == std::string::npos || idp > close)
                continue;
            size_t idClose = matchBracket(wkt, wkt.find('[', idp));
            if (idClose == std::string::npos)
                break;
            size_t start = idp;
            while (start > 0 &&
                   (wkt[start - 1] == ' ' || wkt[start - 1] == '\n'))
                --start;
            if (start > 0 && wkt[start - 1] == ',')
                --start;
            wkt = wkt.substr(0, start) + wkt.substr(idClose + 1);
            changed = true;
            break;
        }
    }
}

static void rebuildEllipsoidInWkt(std::string &wkt, PJ *crs)
{
    PJ *ell = proj_get_ellipsoid(projCtx(), crs);
    if (!ell)
        return;
    double a = 0, b = 0, invf = 0;
    int bComputed = 0;
    proj_ellipsoid_get_parameters(projCtx(), ell, &a, &b, &bComputed, &invf);
    proj_destroy(ell);
    if (a <= b || b <= 0)
        return;
    double newInvf = a / (a - b);
    // near-WGS84 flattening snaps to the canonical value
    if (std::fabs(newInvf - 298.257223563) < 1e-7)
        newInvf = 298.257223563;
    size_t ePos = wkt.find("ELLIPSOID[");
    if (ePos == std::string::npos)
        return;
    // ELLIPSOID["name",a,invf,... -> replace the two numbers (semi-major
    // in metres) and force a metre unit
    size_t p = wkt.find('"', ePos);
    p = wkt.find('"', p + 1);
    size_t comma1 = wkt.find(',', p);
    size_t comma2 = wkt.find(',', comma1 + 1);
    size_t end = comma2 + 1;
    while (end < wkt.size() && wkt[end] != ',' && wkt[end] != ']')
        ++end;
    wkt = wkt.substr(0, comma1 + 1) + wktNumber(a) + "," +
          wktNumber(newInvf) + wkt.substr(end);
    size_t uPos = wkt.find("LENGTHUNIT[", ePos);
    size_t eClose = matchBracket(wkt, wkt.find('[', ePos));
    if (uPos != std::string::npos && eClose != std::string::npos &&
        uPos < eClose)
    {
        size_t uClose = matchBracket(wkt, wkt.find('[', uPos));
        if (uClose != std::string::npos)
            wkt = wkt.substr(0, uPos) + "LENGTHUNIT[\"metre\",1" +
                  wkt.substr(uClose);
    }
}

// angular unit conversion factor (to radians) of the CRS's geodetic
// coordinate system
static double geodeticAngularFactor(PJ *crs)
{
    double factor = 0;
    PJ *geod = proj_crs_get_geodetic_crs(projCtx(), crs);
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), geod ? geod : crs);
    if (geod)
        proj_destroy(geod);
    if (!cs)
        return 0;
    proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr, nullptr,
                          &factor, nullptr, nullptr, nullptr);
    proj_destroy(cs);
    return factor;
}

static bool isDegreeFactor(double factor)
{
    return std::fabs(factor * 180.0 / M_PI - 1.0) < 1e-10;
}

static std::string epsgIdOf(PJ *obj)
{
    if (!obj)
        return "";
    const char *auth = proj_get_id_auth_name(obj, 0);
    const char *code = proj_get_id_code(obj, 0);
    if (!auth || !code || strcmp(auth, "EPSG") != 0)
        return "";
    return code;
}

Srs Srs::fromEpsgGTiff(int codeNum, bool projected, bool &ok,
                       bool gtCitation)
{
    Srs s = fromEpsg(codeNum, ok);
    if (!ok || !s.pj_)
        return s;
    auto captureCodes = [](Srs &t, bool proj)
    {
        PJ *base = proj_crs_get_geodetic_crs(projCtx(), t.pj_);
        PJ *geod = base ? base : t.pj_;
        PJ *datum = proj_crs_get_datum(projCtx(), geod);
        if (!datum)
            datum = proj_crs_get_datum_forced(projCtx(), geod);
        t.wkt1DatumCode_ = epsgIdOf(datum);
        if (datum)
            proj_destroy(datum);
        PJ *ell = proj_get_ellipsoid(projCtx(), t.pj_);
        t.wkt1EllpsCode_ = epsgIdOf(ell);
        if (ell)
            proj_destroy(ell);
        if (proj)
        {
            PJ *pm = proj_get_prime_meridian(projCtx(), t.pj_);
            t.wkt1PmCode_ = epsgIdOf(pm);
            if (pm)
                proj_destroy(pm);
            PJ *cs = proj_crs_get_coordinate_system(projCtx(), t.pj_);
            if (cs)
            {
                const char *ua = nullptr, *uc = nullptr;
                if (proj_cs_get_axis_info(projCtx(), cs, 0, nullptr,
                                          nullptr, nullptr, nullptr,
                                          nullptr, &ua, &uc) &&
                    ua && uc && strcmp(ua, "EPSG") == 0)
                    t.wkt1LinUnitCode_ = uc;
                proj_destroy(cs);
            }
        }
        if (base)
            proj_destroy(base);
    };
    if (projected)
    {
        double angFactor = geodeticAngularFactor(s.pj_);
        if (angFactor > 0 && !isDegreeFactor(angFactor))
        {
            // non-degree base (grads): decomposed and rebuilt, losing the
            // conversion name, axis abbreviations and usage
            const char *cw =
                proj_as_wkt(projCtx(), s.pj_, PJ_WKT2_2019, nullptr);
            if (!cw)
                return s;
            std::string wkt = cw;
            std::string dwkt = forcedDatumWkt(s.pj_);
            if (!dwkt.empty())
                wkt = spliceEnsemble(wkt, dwkt);
            wkt = removeUsageNode(wkt);
            rebuildEllipsoidInWkt(wkt, s.pj_);
            size_t cPos = wkt.find("CONVERSION[\"");
            if (cPos != std::string::npos)
            {
                size_t nameStart = cPos + 12;
                size_t nameEnd = wkt.find('"', nameStart);
                if (nameEnd != std::string::npos)
                    wkt = wkt.substr(0, nameStart) + "unnamed" +
                          wkt.substr(nameEnd);
            }
            for (size_t aPos = wkt.find("AXIS[\"");
                 aPos != std::string::npos;
                 aPos = wkt.find("AXIS[\"", aPos + 6))
            {
                size_t nameStart = aPos + 6;
                size_t nameEnd = wkt.find('"', nameStart);
                if (nameEnd == std::string::npos)
                    break;
                std::string name = wkt.substr(nameStart,
                                              nameEnd - nameStart);
                size_t abbrev = name.rfind(" (");
                if (abbrev != std::string::npos && name.back() == ')')
                    wkt = wkt.substr(0, nameStart) +
                          name.substr(0, abbrev) + wkt.substr(nameEnd);
            }
            captureCodes(s, true);
            PJ *rebuilt = proj_create(projCtx(), wkt.c_str());
            if (rebuilt)
            {
                proj_destroy(s.pj_);
                s.pj_ = rebuilt;
            }
            return s;
        }
        // legacy geotiff UTM PCS codes are decomposed and rebuilt when a
        // GTCitation key rides along (any content), which renders the
        // WGS 84 ensemble as a plain datum
        bool wgs84Utm = (codeNum >= 32601 && codeNum <= 32660) ||
                        (codeNum >= 32701 && codeNum <= 32760);
        if (wgs84Utm && gtCitation && !forcedDatumWkt(s.pj_).empty())
            s.wgs84DatumSwap_ = true;
        return s;
    }
    if (codeNum == 4326 || codeNum == 4322)
        return s;
    if (proj_get_type(s.pj_) == PJ_TYPE_GEOGRAPHIC_3D_CRS)
        return s;
    PJ *datum = proj_crs_get_datum(projCtx(), s.pj_);
    if (datum)
    {
        bool dynamic = proj_get_type(datum) ==
                       PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME;
        proj_destroy(datum);
        if (dynamic)
            return s;
    }
    // lon-lat ordered geographic CRSs ("(lon-lat)" variants) are kept
    // db-verbatim, usage node included
    if (PJ *cs0 = proj_crs_get_coordinate_system(projCtx(), s.pj_))
    {
        const char *dir0 = nullptr;
        bool ok0 = proj_cs_get_axis_info(projCtx(), cs0, 0, nullptr,
                                         nullptr, &dir0, nullptr, nullptr,
                                         nullptr, nullptr);
        bool lonFirst = ok0 && dir0 && strcmp(dir0, "north") != 0;
        proj_destroy(cs0);
        if (lonFirst)
            return s;
    }

    // rebuild from components: forced datum, recomputed inverse
    // flattening, no usage
    const char *cw = proj_as_wkt(projCtx(), s.pj_, PJ_WKT2_2019, nullptr);
    if (!cw)
        return s;
    std::string wkt = cw;
    std::string dwkt = forcedDatumWkt(s.pj_);
    if (!dwkt.empty())
        wkt = spliceEnsemble(wkt, dwkt);
    wkt = removeUsageNode(wkt);
    rebuildEllipsoidInWkt(wkt, s.pj_);

    captureCodes(s, false);
    PJ *rebuilt = proj_create(projCtx(), wkt.c_str());
    if (rebuilt)
    {
        proj_destroy(s.pj_);
        s.pj_ = rebuilt;
    }
    return s;
}

namespace
{

// shortest %g form that parses back to the same double
std::string gtNum(double v)
{
    for (int prec = 15; prec <= 17; ++prec)
    {
        std::string s = strPrintf("%.*g", prec, v);
        if (strtod(s.c_str(), nullptr) == v)
            return s;
    }
    return strPrintf("%.17g", v);
}

constexpr double kDegRad = 0.0174532925199433;

struct GtAngUnit
{
    std::string name;
    double factor = kDegRad;
    int id = 0;
};

bool gtAngUnitFromCode(int code, GtAngUnit &u)
{
    switch (code)
    {
        case 9101:
            u = {"radian", 57.29577951308232 * kDegRad, 9101};
            return true;
        case 9102:
        case 9122:
            u = {"degree", kDegRad, 9122};
            return true;
        case 9103:
            u = {"arc-minute", (1.0 / 60.0) * kDegRad, 9103};
            return true;
        case 9104:
            u = {"arc-second", (1.0 / 3600.0) * kDegRad, 9104};
            return true;
        case 9105:
            u = {"grad", 0.9 * kDegRad, 9105};
            return true;
        case 9106:
            u = {"gon", 0.9 * kDegRad, 9106};
            return true;
    }
    return false;
}

std::string gtAngNode(const GtAngUnit &u, bool withId)
{
    std::string s = "ANGLEUNIT[\"" + u.name + "\"," + gtNum(u.factor);
    if (withId && u.id > 0)
        s += ",ID[\"EPSG\"," + strPrintf("%d", u.id) + "]";
    return s + "]";
}

// the flattening is recomputed through the semi-minor axis
// (298.257222101 -> 298.257222101004) except for the exact WGS84 pair
double gtChainInvf(double a, double b)
{
    if (fabs(a - b) < 1.5e-7)
        return 0.0;
    if (a == 6378137.0 && fabs(b - 6356752.314245179) < 1e-9)
        return 298.257223563;
    return a / (a - b);
}

// ellipsoid override keys matching exactly one EPSG ellipsoid's value
// pair read the db flattening back verbatim; duplicated pairs (GRS 1980
// vs CGCS2000, WGS 84 vs GEM 10C) stay on the recomputed chain
struct GtEllRow
{
    double a, b, invf;
};

const GtEllRow *gtEpsgEllMatch(double a, double b)
{
    static const std::vector<GtEllRow> rows = []
    {
        std::vector<GtEllRow> v;
        PROJ_STRING_LIST codes = proj_get_codes_from_database(
            projCtx(), "EPSG", PJ_TYPE_ELLIPSOID, 1);
        for (char **p = codes; p && *p; ++p)
        {
            PJ *e = proj_create_from_database(
                projCtx(), "EPSG", *p, PJ_CATEGORY_ELLIPSOID, 0, nullptr);
            if (!e)
                continue;
            GtEllRow r{};
            if (proj_ellipsoid_get_parameters(projCtx(), e, &r.a, &r.b,
                                              nullptr, &r.invf))
                v.push_back(r);
            proj_destroy(e);
        }
        proj_string_list_destroy(codes);
        return v;
    }();
    const GtEllRow *hit = nullptr;
    for (const GtEllRow &r : rows)
    {
        if (r.a != a || r.b != b)
            continue;
        if (hit)
            return nullptr;
        hit = &r;
    }
    return hit;
}

// the four datums libgeotiff resolves without touching proj.db
bool gtDatumHardcoded(int code, std::string &datumName, int &ellCode)
{
    switch (code)
    {
        case 6326:
            datumName = "World Geodetic System 1984";
            ellCode = 7030;
            return true;
        case 6322:
            datumName = "World Geodetic System 1972";
            ellCode = 7043;
            return true;
        case 6269:
            datumName = "North American Datum 1983";
            ellCode = 7019;
            return true;
        case 6267:
            datumName = "North American Datum 1927";
            ellCode = 7008;
            return true;
    }
    return false;
}

PJ *gtDbObject(PJ_CATEGORY cat, int code)
{
    return proj_create_from_database(projCtx(), "EPSG",
                                     strPrintf("%d", code).c_str(), cat, 0,
                                     nullptr);
}

// a coded geodetic CRS key on a geocentric directory only contributes its
// datum (or ensemble) code
int gtDatumCodeOfCrs(int code)
{
    PJ *crs = gtDbObject(PJ_CATEGORY_CRS, code);
    if (!crs)
    {
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "PROJ: proj_create_from_database: crs not found");
        return 0;
    }
    PJ *d = proj_crs_get_datum(projCtx(), crs);
    if (!d)
        d = proj_crs_get_datum_ensemble(projCtx(), crs);
    int out = 0;
    if (d)
    {
        const char *c = proj_get_id_code(d, 0);
        out = c ? atoi(c) : 0;
        proj_destroy(d);
    }
    proj_destroy(crs);
    return out;
}

// GeogCitation splits on '|' into "Key = value" members; any recognized
// member suppresses the raw-name fallback, which is whatever segment the
// parser's quirky loop extracted last (the final segment is skipped when
// the previous one ended exactly one character before the end)
struct GtCitation
{
    std::map<std::string, std::string> members;
    std::string lastSegment;
    bool memberFound = false;
};

GtCitation gtParseCitation(const std::string &cit)
{
    GtCitation c;
    static const char *const kMarkers[] = {
        "PCS Name = ", "Projection Name = ", "LUnits = ", "GCS Name = ",
        "Datum = ",    "Ellipsoid = ",       "Primem = ", "AUnits = "};
    size_t pos = 0;
    while (pos + 1 < cit.size())
    {
        size_t bar = cit.find('|', pos);
        if (bar == std::string::npos)
        {
            c.lastSegment = cit.substr(pos);
            pos = cit.size();
        }
        else
        {
            c.lastSegment = cit.substr(pos, bar - pos);
            pos = bar + 1;
        }
        for (const char *m : kMarkers)
        {
            size_t at = c.lastSegment.find(m);
            if (at == std::string::npos)
                continue;
            c.memberFound = true;
            c.members.insert({std::string(m, strlen(m) - 3),
                              c.lastSegment.substr(at + strlen(m))});
        }
    }
    return c;
}

struct GtKeyReader
{
    const GTiffKeyValues &kv;
    int getS(int id, int def) const
    {
        auto it = kv.shorts.find(id);
        return it == kv.shorts.end() ? def : it->second;
    }
    bool hasD(int id) const { return kv.dbls.count(id) != 0; }
    double getD(int id, double def) const
    {
        auto it = kv.dbls.find(id);
        return it == kv.dbls.end() ? def : it->second;
    }
    std::string getA(int id) const
    {
        auto it = kv.asciis.find(id);
        if (it == kv.asciis.end())
            return std::string();
        std::string v = it->second;
        // ascii geokey values end with '|' standing in for NUL
        if (!v.empty() && v.back() == '|')
            v.pop_back();
        return v;
    }
};

struct GtGeogParts
{
    std::string name;
    std::string datumNode;
    std::string pmNode;
    GtAngUnit axisUnit;
    GtAngUnit pmUnit;
    bool degree = false;
    bool datumKnown = false;
    bool ellKnown = false;
    bool angExplicit = false;
    int angCode = 0;
    int baseId = 0;
    std::string angNode;
};

// shared DATUM/ELLIPSOID/PRIMEM composition for geographic, projected-base
// and geocentric directories
GtGeogParts gtComposeGeogParts(const GtKeyReader &r, bool geocentric = false)
{
    GtGeogParts out;
    GtCitation cit = gtParseCitation(r.getA(2049));
    auto citGet = [&cit](const char *k) -> std::string
    {
        auto it = cit.members.find(k);
        return it == cit.members.end() ? std::string() : it->second;
    };

    int datumCode = r.getS(2050, 32767);
    bool datumKnown = datumCode > 0 && datumCode < 32767;
    int ellCode = r.getS(2056, 32767);
    bool ellKnown = ellCode > 0 && ellCode < 32767;

    // datum names resolve only for the hardcoded four and static db
    // frames; dynamic frames and unknown codes read back as "unnamed"
    std::string datumName, dbEllName;
    int impliedEll = 0;
    PJ *dbDatum = nullptr;
    if (datumKnown && !gtDatumHardcoded(datumCode, datumName, impliedEll))
    {
        dbDatum = gtDbObject(PJ_CATEGORY_DATUM, datumCode);
        if (!dbDatum)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "PROJ: proj_create_from_database: datum not found");
        if (dbDatum &&
            proj_get_type(dbDatum) != PJ_TYPE_GEODETIC_REFERENCE_FRAME)
        {
            proj_destroy(dbDatum);
            dbDatum = nullptr;
        }
        if (dbDatum)
        {
            const char *n = proj_get_name(dbDatum);
            datumName = n ? n : "";
        }
    }
    if (datumName.empty())
        datumName = "unnamed";
    std::string citDatum = citGet("Datum");
    if (!citDatum.empty())
        datumName = citDatum;

    int dbEllCode = ellKnown ? ellCode : impliedEll;
    double dbA = 0.0, dbB = 0.0;
    bool haveDb = false;
    if (dbEllCode > 0)
    {
        PJ *ell = gtDbObject(PJ_CATEGORY_ELLIPSOID, dbEllCode);
        if (ell)
        {
            const char *en = proj_get_name(ell);
            dbEllName = en ? en : "";
            proj_ellipsoid_get_parameters(projCtx(), ell, &dbA, &dbB,
                                          nullptr, nullptr);
            haveDb = true;
            proj_destroy(ell);
        }
    }
    if (dbDatum && !haveDb)
    {
        PJ *ell = proj_get_ellipsoid(projCtx(), dbDatum);
        if (ell)
        {
            const char *en = proj_get_name(ell);
            dbEllName = en ? en : "";
            proj_ellipsoid_get_parameters(projCtx(), ell, &dbA, &dbB,
                                          nullptr, nullptr);
            haveDb = true;
            proj_destroy(ell);
        }
    }
    if (dbDatum)
        proj_destroy(dbDatum);

    std::string citEll = citGet("Ellipsoid");
    std::string ellName = citEll.empty() ? dbEllName : citEll;

    double a = 0.0, invf = 0.0;
    bool aFromKey = r.hasD(2057);
    bool ellRetrieved = aFromKey || haveDb;
    if (ellRetrieved)
    {
        a = aFromKey ? r.getD(2057, 0.0) : dbA;
        double b;
        if (r.hasD(2058))
            b = r.getD(2058, a);
        else if (r.hasD(2059))
        {
            double f = r.getD(2059, 0.0);
            b = f != 0.0 ? a * (1.0 - 1.0 / f) : a;
        }
        else
            b = dbB;
        invf = gtChainInvf(a, b);
        if (datumKnown &&
            (aFromKey || r.hasD(2058) || r.hasD(2059)))
        {
            const GtEllRow *m = gtEpsgEllMatch(a, b);
            if (m)
                invf = m->invf;
        }
        // a citation Ellipsoid member restores the raw key instead
        if (!citEll.empty() && r.hasD(2059))
            invf = r.getD(2059, 0.0);
    }
    else
    {
        ellName = "unretrievable - using WGS84";
        a = 6378137.0;
        invf = 298.257223563;
    }
    if (ellName.empty())
        ellName = "unnamed";

    out.name = citGet("GCS Name");
    if (out.name.empty() && !cit.memberFound)
        out.name = cit.lastSegment;
    if (out.name.empty())
        out.name = datumName != "unnamed" ? datumName : "unknown";

    out.datumKnown = datumKnown;
    out.ellKnown = ellKnown;
    // angular unit: known code, custom size, or the unnamed fallback that
    // keeps the prime meridian in degrees
    int angCode = r.getS(2054, 0);
    out.angCode = angCode;
    GtAngUnit au;
    if (angCode > 0 && gtAngUnitFromCode(angCode, au))
    {
        out.axisUnit = au;
        out.pmUnit = au;
        out.degree = angCode == 9102 || angCode == 9122;
        out.angExplicit = true;
    }
    else if (r.hasD(2055))
    {
        std::string an = citGet("AUnits");
        if (an.empty())
            an = "unknown";
        out.axisUnit = {an, r.getD(2055, 1.0), 0};
        out.pmUnit = out.axisUnit;
        out.angExplicit = true;
    }
    else if (ellRetrieved && !geocentric)
    {
        // a retrieved ellipsoid keeps the whole degree-sized unit unnamed
        out.axisUnit = {"unknown", kDegRad, 0};
        out.pmUnit = out.axisUnit;
        out.degree = true;
    }
    else
    {
        out.axisUnit = {"unknown", kDegRad, 0};
        out.pmUnit = {"degree", kDegRad, 9122};
        out.degree = true;
    }

    std::string ellUnit = "LENGTHUNIT[\"metre\",1";
    if (!datumKnown && !ellKnown)
        ellUnit += ",ID[\"EPSG\",9001]";
    ellUnit += "]";
    out.datumNode = "DATUM[\"" + datumName + "\",ELLIPSOID[\"" + ellName +
                    "\"," + gtNum(a) + "," + gtNum(invf) + "," + ellUnit;
    if (ellKnown)
        out.datumNode += ",ID[\"EPSG\"," + strPrintf("%d", ellCode) + "]";
    out.datumNode += "]";
    if (datumKnown)
        out.datumNode += ",ID[\"EPSG\"," + strPrintf("%d", datumCode) + "]";
    out.datumNode += "]";

    std::string pmName = citGet("Primem");
    if (pmName.empty())
        pmName = "Greenwich";
    out.pmNode = "PRIMEM[\"" + pmName + "\"," + gtNum(r.getD(2061, 0.0)) +
                 "," + gtAngNode(out.pmUnit, true) + "]";
    return out;
}

// programmatically assembled CRSs carry Latitude/lat base axes where
// text-parsed ones default to the database Geodetic latitude/Lat pair;
// the WKT parser ignores an explicit BASEGEOGCRS coordinate system, so
// the base has to be swapped at the object level
PJ *gtBaseAxesToLatLon(PJ *crs)
{
    PJ *geod = proj_crs_get_geodetic_crs(projCtx(), crs);
    if (!geod)
        return nullptr;
    const char *gw = proj_as_wkt(projCtx(), geod, PJ_WKT2_2019, nullptr);
    std::string g = gw ? gw : "";
    proj_destroy(geod);
    auto rep = [&g](const char *from, const char *to)
    {
        size_t p = g.find(from);
        if (p == std::string::npos)
            return false;
        g.replace(p, strlen(from), to);
        return true;
    };
    if (!rep("AXIS[\"geodetic latitude (Lat)\"", "AXIS[\"Latitude (lat)\""))
        return nullptr;
    rep("AXIS[\"geodetic longitude (Lon)\"", "AXIS[\"Longitude (lon)\"");
    // a WKT identifier makes the parser resolve the axes from the
    // database again; drop it from the text and re-add it object-level
    std::string baseAuth, baseCode;
    size_t idPos = g.rfind(",\n    ID[\"");
    if (idPos != std::string::npos)
    {
        size_t open = g.find('[', idPos + 3);
        size_t close = matchBracket(g, open);
        if (close != std::string::npos)
        {
            size_t q1 = g.find('"', open);
            size_t q2 = g.find('"', q1 + 1);
            baseAuth = g.substr(q1 + 1, q2 - q1 - 1);
            size_t comma = g.find(',', q2);
            baseCode = g.substr(comma + 1, close - comma - 1);
            g = g.substr(0, idPos) + g.substr(close + 1);
        }
    }
    PJ *ng = proj_create(projCtx(), g.c_str());
    if (!ng)
        return nullptr;
    if (!baseAuth.empty() && !baseCode.empty())
    {
        PJ *ngId = proj_alter_id(projCtx(), ng, baseAuth.c_str(),
                                 baseCode.c_str());
        if (ngId)
        {
            proj_destroy(ng);
            ng = ngId;
        }
    }
    PJ *out = proj_crs_alter_geodetic_crs(projCtx(), crs, ng);
    proj_destroy(ng);
    // the swap rebuilds the CRS without its identifiers; restore the root
    // ID so nested conversion/unit IDs stay suppressed in WKT output
    const char *an = proj_get_id_auth_name(crs, 0);
    const char *cd = proj_get_id_code(crs, 0);
    if (out && an && cd)
    {
        PJ *withId = proj_alter_id(projCtx(), out, an, cd);
        if (withId)
        {
            proj_destroy(out);
            out = withId;
        }
    }
    return out;
}

// custom projections over a coded geographic CS rebuild the base textually
// from proj.db: db names and unit doubles, chained flattening, no
// component IDs, the base keeping its EPSG ID; 2057-2059 still override
// the ellipsoid, drawing a registry-mismatch warning
bool gtGeogPartsFromEpsg(const GtKeyReader &r, int code, GtGeogParts &out)
{
    PJ *crs = gtDbObject(PJ_CATEGORY_CRS, code);
    if (!crs)
    {
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "PROJ: proj_create_from_database: crs not found");
        return false;
    }
    const char *nm = proj_get_name(crs);
    out.name = nm && nm[0] ? nm : "unknown";
    out.baseId = code;

    std::string datumName;
    bool resolved = false;
    PJ *datum = proj_crs_get_datum(projCtx(), crs);
    if (!datum)
        datum = proj_crs_get_datum_forced(projCtx(), crs);
    if (datum)
    {
        const char *dc = proj_get_id_code(datum, 0);
        int datumCode = dc ? atoi(dc) : 0;
        int impliedEll = 0;
        if (datumCode > 0 &&
            gtDatumHardcoded(datumCode, datumName, impliedEll))
            resolved = true;
        else if (datumCode > 0)
        {
            PJ *dbDatum = gtDbObject(PJ_CATEGORY_DATUM, datumCode);
            if (dbDatum)
            {
                const char *n = proj_get_name(dbDatum);
                if (proj_get_type(dbDatum) ==
                        PJ_TYPE_GEODETIC_REFERENCE_FRAME &&
                    n && n[0])
                {
                    datumName = n;
                    resolved = true;
                }
                proj_destroy(dbDatum);
            }
        }
        proj_destroy(datum);
    }
    if (!resolved)
        datumName = "unnamed";

    std::string ellName;
    double regA = 0.0, regB = 0.0;
    bool haveReg = false;
    PJ *ell = proj_get_ellipsoid(projCtx(), crs);
    if (ell)
    {
        const char *en = proj_get_name(ell);
        if (en && resolved)
            ellName = en;
        haveReg = proj_ellipsoid_get_parameters(projCtx(), ell, &regA,
                                                &regB, nullptr,
                                                nullptr) != 0;
        proj_destroy(ell);
    }
    bool haveDb = resolved && haveReg;

    double a = 0.0, invf = 0.0;
    bool aFromKey = r.hasD(2057);
    if (aFromKey || haveDb)
    {
        a = aFromKey ? r.getD(2057, 0.0) : regA;
        double b;
        if (r.hasD(2058))
            b = r.getD(2058, a);
        else if (r.hasD(2059))
        {
            double f = r.getD(2059, 0.0);
            b = f != 0.0 ? a * (1.0 - 1.0 / f) : a;
        }
        else
            b = haveDb ? regB : a;
        invf = gtChainInvf(a, b);
        if (aFromKey || r.hasD(2058) || r.hasD(2059))
        {
            const GtEllRow *m = gtEpsgEllMatch(a, b);
            if (m)
                invf = m->invf;
        }
    }
    else
    {
        ellName = "unretrievable - using WGS84";
        a = 6378137.0;
        invf = 298.257223563;
    }
    if (ellName.empty())
        ellName = "unnamed";

    if (haveDb && (aFromKey || r.hasD(2058) || r.hasD(2059)))
    {
        double regInvf = gtChainInvf(regA, regB);
        if (fabs(a - regA) > 1e-8 || fabs(invf - regInvf) > 1e-8)
            cplErrorStr(
                CE_Warning, CPLE_AppDefined,
                strPrintf(
                    "The definition of geographic CRS EPSG:%d got from "
                    "GeoTIFF keys is not the same as the one from the "
                    "EPSG registry, which may cause issues during "
                    "reprojection operations. Set GTIFF_SRS_SOURCE "
                    "configuration option to EPSG to use official "
                    "parameters (overriding the ones from GeoTIFF keys), "
                    "or to GEOKEYS to use custom values from GeoTIFF keys "
                    "and drop the EPSG code.",
                    code));
    }

    out.datumNode = "DATUM[\"" + datumName + "\",ELLIPSOID[\"" + ellName +
                    "\"," + gtNum(a) + "," + gtNum(invf) +
                    ",LENGTHUNIT[\"metre\",1]]]";

    std::string pmName = "Greenwich";
    double pmLon = 0.0;
    PJ *pm = proj_get_prime_meridian(projCtx(), crs);
    if (pm)
    {
        const char *pn = proj_get_name(pm);
        if (pn && pn[0])
            pmName = pn;
        double conv = 0.0;
        proj_prime_meridian_get_parameters(projCtx(), pm, &pmLon, &conv,
                                           nullptr);
        proj_destroy(pm);
    }

    out.axisUnit = {"degree", kDegRad, 0};
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), crs);
    if (cs)
    {
        double axConv = 0.0;
        const char *axName = nullptr;
        if (proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr,
                                  nullptr, &axConv, &axName, nullptr,
                                  nullptr) &&
            axName && axConv > 0.0)
            out.axisUnit = {axName, axConv, 0};
        proj_destroy(cs);
    }
    out.angNode = "ANGLEUNIT[\"" + out.axisUnit.name + "\"," +
                  gtNum(out.axisUnit.factor) + "]";
    out.degree = out.axisUnit.name == "degree";
    out.pmUnit = out.axisUnit;
    // the prime meridian is rendered in the CRS angular unit, not its own
    out.pmNode = "PRIMEM[\"" + pmName + "\"," + gtNum(pmLon) + "," +
                 out.angNode + "]";
    proj_destroy(crs);
    return true;
}

struct GtLinUnit
{
    std::string name;
    double size = 1.0;
    int id = 0;
};

GtLinUnit gtLinUnitOf(const GtKeyReader &r)
{
    GtCitation cit = gtParseCitation(r.getA(3073));
    auto lit = cit.members.find("LUnits");
    bool haveLu = lit != cit.members.end();
    int code = r.getS(3076, 0);
    if (code > 0 && code != 32767)
    {
        // a LUnits citation member discards the coded unit entirely
        if (haveLu)
            return {"unknown", 1.0, 0};
        const char *n = nullptr;
        double f = 0.0;
        if (proj_uom_get_info_from_database(projCtx(), "EPSG",
                                            strPrintf("%d", code).c_str(),
                                            &n, &f, nullptr) &&
            n)
            return {n, f, code};
        // the failed lookup surfaces twice: once raw from PROJ's default
        // logger, once through the error handler
        fprintf(stderr, "proj_uom_get_info_from_database: unit of measure "
                        "not found\n");
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "PROJ: proj_uom_get_info_from_database: unit of "
                    "measure not found");
        return {"unknown", 1.0, 0};
    }
    GtLinUnit u;
    u.size = r.getD(3077, 1.0);
    if (code == 32767 && haveLu)
        u.name = lit->second;
    if (u.name.empty())
        u.name = "unknown";
    return u;
}

std::string gtLinNode(const GtLinUnit &u)
{
    std::string s = "LENGTHUNIT[\"" + u.name + "\"," + gtNum(u.size);
    if (u.id > 0)
        s += ",ID[\"EPSG\"," + strPrintf("%d", u.id) + "]";
    return s + "]";
}

// geocentric axis units resolve GeogLinearUnits through proj.db; an
// absent key and the user-defined size key both fall to "unknown",
// unlike projected linear units
GtLinUnit gtGeogLinUnit(const GtKeyReader &r)
{
    int code = r.getS(2052, 0);
    if (code > 0 && code != 32767)
    {
        const char *n = nullptr;
        double f = 0.0;
        if (proj_uom_get_info_from_database(projCtx(), "EPSG",
                                            strPrintf("%d", code).c_str(),
                                            &n, &f, nullptr) &&
            n)
            return {n, f, 0};
        cplErrorStr(CE_Warning, CPLE_AppDefined,
                    "PROJ: proj_uom_get_info_from_database: unit of "
                    "measure not found");
    }
    return {"unknown", 1.0, 0};
}

// A angular, S scale (default 1), L linear (scaled to metres)
struct GtParamDef
{
    int epsg;
    int geokey;
    char kind;
};

struct GtMethodDef
{
    int ct;
    const char *name;
    int code;
    std::vector<GtParamDef> params;
};

const char *gtParamName(int epsg)
{
    switch (epsg)
    {
        case 8801:
            return "Latitude of natural origin";
        case 8802:
            return "Longitude of natural origin";
        case 8805:
            return "Scale factor at natural origin";
        case 8806:
            return "False easting";
        case 8807:
            return "False northing";
        case 8811:
            return "Latitude of projection centre";
        case 8812:
            return "Longitude of projection centre";
        case 8813:
            return "Azimuth of initial line";
        case 8814:
            return "Angle from Rectified to Skew Grid";
        case 8815:
            return "Scale factor on initial line";
        case 8821:
            return "Latitude of false origin";
        case 8822:
            return "Longitude of false origin";
        case 8823:
            return "Latitude of 1st standard parallel";
        case 8824:
            return "Latitude of 2nd standard parallel";
        case 8826:
            return "Easting at false origin";
        case 8827:
            return "Northing at false origin";
        case 8832:
            return "Latitude of standard parallel";
        case 8833:
            return "Longitude of origin";
    }
    return "";
}

bool gtMethodFromCt(const GtKeyReader &r, int ct, GtMethodDef &m)
{
    switch (ct)
    {
        case 1:
            m = {ct, "Transverse Mercator", 9807, {}};
            break;
        case 27:
            m = {ct, "Transverse Mercator (South Orientated)", 9808, {}};
            break;
        case 9:
            m = {ct, "Lambert Conic Conformal (1SP)", 9801, {}};
            break;
        case 16:
            m = {ct, "Oblique Stereographic", 9809, {}};
            break;
        case 14:
            m = {ct,
                 "Stereographic",
                 0,
                 {{8801, 3089, 'A'},
                  {8802, 3088, 'A'},
                  {8805, 3092, 'S'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 7:
            if (r.hasD(3078))
                m = {ct,
                     "Mercator (variant B)",
                     9805,
                     {{8823, 3078, 'A'},
                      {8802, 3080, 'A'},
                      {8806, 3082, 'L'},
                      {8807, 3083, 'L'}}};
            else
                m = {ct,
                     "Mercator (variant A)",
                     9804,
                     {{8801, 3081, 'A'},
                      {8802, 3080, 'A'},
                      {8805, 3092, 'S'},
                      {8806, 3082, 'L'},
                      {8807, 3083, 'L'}}};
            return true;
        case 15:
            if (r.getD(3092, 1.0) == 1.0)
                m = {ct,
                     "Polar Stereographic (variant B)",
                     9829,
                     {{8832, 3081, 'A'},
                      {8833, 3095, 'A'},
                      {8806, 3082, 'L'},
                      {8807, 3083, 'L'}}};
            else
                m = {ct,
                     "Polar Stereographic (variant A)",
                     9810,
                     {{8801, 3081, 'A'},
                      {8802, 3095, 'A'},
                      {8805, 3092, 'S'},
                      {8806, 3082, 'L'},
                      {8807, 3083, 'L'}}};
            return true;
        case 8:
            m = {ct,
                 "Lambert Conic Conformal (2SP)",
                 9802,
                 {{8821, 3085, 'A'},
                  {8822, 3084, 'A'},
                  {8823, 3078, 'A'},
                  {8824, 3079, 'A'},
                  {8826, 3086, 'L'},
                  {8827, 3087, 'L'}}};
            return true;
        case 10:
            m = {ct,
                 "Lambert Azimuthal Equal Area",
                 9820,
                 {{8801, 3089, 'A'},
                  {8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 11:
            m = {ct,
                 "Albers Equal Area",
                 9822,
                 {{8821, 3081, 'A'},
                  {8822, 3080, 'A'},
                  {8823, 3078, 'A'},
                  {8824, 3079, 'A'},
                  {8826, 3082, 'L'},
                  {8827, 3083, 'L'}}};
            return true;
        case 18:
            m = {ct,
                 "Cassini-Soldner",
                 9806,
                 {{8801, 3081, 'A'},
                  {8802, 3080, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 22:
            m = {ct,
                 "American Polyconic",
                 9818,
                 {{8801, 3081, 'A'},
                  {8802, 3080, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 12:
            m = {ct,
                 "Modified Azimuthal Equidistant",
                 9832,
                 {{8801, 3089, 'A'},
                  {8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 20:
            m = {ct,
                 "Miller Cylindrical",
                 0,
                 {{8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 17:
            // a nonzero natural-origin latitude cannot ride the standard
            // EPSG:1028 parameter set, so it degrades to a generic list
            if (r.getD(3089, 0.0) != 0.0)
            {
                m = {ct,
                     "Equidistant Cylindrical",
                     1028,
                     {{8801, 3089, 'A'},
                      {8802, 3088, 'A'},
                      {8823, 3078, 'A'},
                      {8806, 3082, 'L'},
                      {8807, 3083, 'L'}}};
                return true;
            }
            m = {ct,
                 "Equidistant Cylindrical",
                 1028,
                 {{8823, 3078, 'A'},
                  {8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 23:
            m = {ct,
                 "Robinson",
                 0,
                 {{8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 25:
            m = {ct,
                 "Van Der Grinten",
                 0,
                 {{8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 21:
            m = {ct,
                 "Orthographic",
                 9840,
                 {{8801, 3089, 'A'},
                  {8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 19:
            m = {ct,
                 "Gnomonic",
                 0,
                 {{8801, 3089, 'A'},
                  {8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 24:
            m = {ct,
                 "Sinusoidal",
                 0,
                 {{8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 13:
            m = {ct,
                 "Equidistant Conic",
                 0,
                 {{8801, 3081, 'A'},
                  {8802, 3080, 'A'},
                  {8823, 3078, 'A'},
                  {8824, 3079, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 3:
            m = {ct,
                 "Hotine Oblique Mercator (variant A)",
                 9812,
                 {{8811, 3089, 'A'},
                  {8812, 3088, 'A'},
                  {8813, 3094, 'A'},
                  {8814, 3096, 'A'},
                  {8815, 3093, 'S'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 26:
            m = {ct,
                 "New Zealand Map Grid",
                 9811,
                 {{8801, 3089, 'A'},
                  {8802, 3088, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        case 28:
            m = {ct,
                 "Lambert Cylindrical Equal Area",
                 9835,
                 {{8823, 3078, 'A'},
                  {8802, 3080, 'A'},
                  {8806, 3082, 'L'},
                  {8807, 3083, 'L'}}};
            return true;
        default:
            return false;
    }
    m.params = {{8801, 3081, 'A'},
                {8802, 3080, 'A'},
                {8805, 3092, 'S'},
                {8806, 3082, 'L'},
                {8807, 3083, 'L'}};
    return true;
}

}  // namespace

Srs Srs::fromGTiffKeys(const GTiffKeyValues &kv, bool &ok)
{
    ok = false;
    Srs s;
    GtKeyReader r{kv};

    int mt = r.getS(1024, 0);
    // unmappable methods travel as an ESRI PE string citation under a
    // user-defined model type
    std::string pcs = r.getA(3073);
    const char *kPePfx = "ESRI PE String = ";
    if (mt == 32767 && pcs.compare(0, strlen(kPePfx), kPePfx) == 0)
    {
        s.pj_ = proj_create(projCtx(), pcs.substr(strlen(kPePfx)).c_str());
        ok = s.pj_ != nullptr;
        return s;
    }

    // GeogLinearUnits resolves for geographic and geocentric directories,
    // warning on unknown codes even when nothing consumes the unit;
    // projected directories never touch it
    int geogLinCode = r.getS(2052, 0);
    GtLinUnit geogLin{"unknown", 1.0, 0};
    if (mt == 3 || (mt == 2 && geogLinCode > 0 && geogLinCode != 32767))
        geogLin = gtGeogLinUnit(r);
    // geocentric codes under a geographic directory rebuild textually as
    // latitude-first geographic CRSs that keep the code
    if (mt == 2 && r.getS(2048, 0) > 0 && r.getS(2048, 0) < 32767)
    {
        GtGeogParts g;
        if (!gtGeogPartsFromEpsg(r, r.getS(2048, 0), g))
            return s;
        std::string axis = g.angNode;
        std::string wkt = "GEOGCRS[\"" + g.name + "\"," + g.datumNode +
                          "," + g.pmNode +
                          ",CS[ellipsoidal,2],AXIS[\"latitude\",north,"
                          "ORDER[1]," +
                          axis + "],AXIS[\"longitude\",east,ORDER[2]," +
                          axis + "]";
        if (g.baseId > 0)
            wkt += ",ID[\"EPSG\"," + strPrintf("%d", g.baseId) + "]";
        wkt += "]";
        s.pj_ = proj_create(projCtx(), wkt.c_str());
        ok = s.pj_ != nullptr;
        return s;
    }
    if (mt == 2 && (r.getS(2048, 0) == 32767 || r.getS(2048, 0) == 0))
    {
        GtGeogParts g = gtComposeGeogParts(r);
        std::string axis = gtAngNode(g.axisUnit, true);
        std::string wkt = "GEOGCRS[\"" + g.name + "\"," + g.datumNode + "," +
                          g.pmNode +
                          ",CS[ellipsoidal,2],AXIS[\"latitude\",north,"
                          "ORDER[1]," +
                          axis + "],AXIS[\"longitude\",east,ORDER[2]," +
                          axis + "]]";
        s.pj_ = proj_create(projCtx(), wkt.c_str());
        if (s.pj_ && g.axisUnit.id == 0)
            s.setCustomAngularUnit(g.axisUnit.name, g.axisUnit.factor);
        ok = s.pj_ != nullptr;
        return s;
    }
    if (mt == 3 && r.getS(2048, 0) <= 32767)
    {
        int gcs3 = r.getS(2048, 0);
        GTiffKeyValues kv2 = kv;
        if (gcs3 > 0 && gcs3 < 32767 && !kv2.shorts.count(2050))
        {
            int dc = gtDatumCodeOfCrs(gcs3);
            if (dc > 0)
                kv2.shorts[2050] = dc;
        }
        GtKeyReader r2{kv2};
        std::string name = r2.getA(1026);
        if (name.empty())
            name = r2.getA(2049);
        if (name.empty())
            name = "unnamed";
        GtGeogParts g = gtComposeGeogParts(r2, true);
        std::string ax = "LENGTHUNIT[\"" + geogLin.name + "\"," +
                         gtNum(geogLin.size) + "]";
        std::string wkt =
            "GEODCRS[\"" + name + "\"," + g.datumNode + "," + g.pmNode +
            ",CS[Cartesian,3],"
            "AXIS[\"(X)\",geocentricX,ORDER[1]," +
            ax + "],AXIS[\"(Y)\",geocentricY,ORDER[2]," + ax +
            "],AXIS[\"(Z)\",geocentricZ,ORDER[3]," + ax + "]]";
        s.pj_ = proj_create(projCtx(), wkt.c_str());
        ok = s.pj_ != nullptr;
        return s;
    }
    if (mt != 1 || r.getS(3072, 0) != 32767)
        return s;

    GtLinUnit lu = gtLinUnitOf(r);
    GtGeogParts g;
    int gcs = r.getS(2048, 0);
    bool baseCoded = gcs > 0 && gcs != 32767;
    bool knownBase = baseCoded && gtGeogPartsFromEpsg(r, gcs, g);
    if (!knownBase)
        g = gtComposeGeogParts(r);
    GtMethodDef m;
    int utmProj = r.getS(3074, 0);
    bool utm = utmProj >= 16001 && utmProj <= 16160 &&
               (utmProj <= 16060 || utmProj >= 16101);
    struct Pv
    {
        int epsg;
        char kind;
        double value;
    };
    std::vector<Pv> vals;
    if (utm)
    {
        bool north = utmProj < 16100;
        int zone = utmProj - (north ? 16000 : 16100);
        m = {1, "Transverse Mercator", 9807, {}};
        // zone-implied angles convert into the base angular unit; the
        // false easting/northing constants never scale with 3076
        double lon0 = zone * 6.0 - 183.0;
        if (knownBase && !g.degree && g.axisUnit.factor > 0.0)
            lon0 = lon0 * kDegRad / g.axisUnit.factor;
        vals = {{8801, 'A', 0.0},
                {8802, 'A', lon0},
                {8805, 'S', 0.9996},
                {8806, 'L', 500000.0},
                {8807, 'L', north ? 0.0 : 10000000.0}};
    }
    else if (gtMethodFromCt(r, r.getS(3075, 0), m))
    {
        for (const auto &p : m.params)
        {
            double v = r.getD(p.geokey, p.kind == 'S' ? 1.0 : 0.0);
            if (p.kind == 'L')
                v *= lu.size;
            vals.push_back({p.epsg, p.kind, v});
        }
    }
    else
        return s;

    // the PCS citation names the CRS: a "PCS Name" member, or the raw
    // string when no known marker appears; the GT citation is a fallback
    GtCitation pcsCit = gtParseCitation(pcs);
    std::string name;
    auto itPcs = pcsCit.members.find("PCS Name");
    if (itPcs != pcsCit.members.end())
        name = itPcs->second;
    else if (!pcsCit.memberFound)
        name = pcs;
    if (name.empty())
        name = r.getA(1026);
    if (name.empty())
        name = "unnamed";
    std::string nameEsc;
    for (char ch : name)
    {
        nameEsc += ch;
        if (ch == '"')
            nameEsc += '"';
    }
    std::string convName = g.degree ? m.name : "unnamed";
    if (m.ct == 17 && r.getD(3089, 0.0) != 0.0)
        convName = "unnamed";
    // the base axis order is only visible through PROJJSON; coded bases
    // and fully unknown degree-based directories come out latitude-first
    bool latFirst = gcs != 32767 ||
                    (!g.datumKnown && !g.ellKnown && g.angCode == 9102);
    std::string baseAxis =
        g.angNode.empty() ? gtAngNode(g.axisUnit, true) : g.angNode;
    std::string axLon = "AXIS[\"longitude\",east,ORDER[";
    std::string axLat = "AXIS[\"latitude\",north,ORDER[";
    std::string baseCs =
        latFirst ? axLat + "1]," + baseAxis + "]," + axLon + "2]," +
                       baseAxis + "]"
                 : axLon + "1]," + baseAxis + "]," + axLat + "2]," +
                       baseAxis + "]";
    std::string baseId =
        g.baseId > 0 ? ",ID[\"EPSG\"," + strPrintf("%d", g.baseId) + "]"
                     : "";
    std::string wkt = "PROJCRS[\"" + nameEsc + "\",BASEGEOGCRS[\"" + g.name +
                      "\"," + g.datumNode + "," + g.pmNode +
                      ",CS[ellipsoidal,2]," + baseCs + baseId +
                      "],CONVERSION[\"" + convName + "\",METHOD[\"" +
                      m.name + "\"";
    if (m.code > 0)
        wkt += ",ID[\"EPSG\"," + strPrintf("%d", m.code) + "]";
    wkt += "]";
    for (const auto &p : vals)
    {
        wkt += ",PARAMETER[\"" + std::string(gtParamName(p.epsg)) + "\"," +
               gtNum(p.value) + ",";
        if (p.kind == 'A')
        {
            // without an explicit angular unit key the conversion
            // parameters fall back to plain degrees
            if (!g.angNode.empty())
                wkt += g.angNode;
            else if (g.angExplicit)
                wkt += gtAngNode(g.axisUnit, false);
            else
                wkt += gtAngNode({"degree", kDegRad, 0}, false);
        }
        else if (p.kind == 'S')
            wkt += "SCALEUNIT[\"unity\",1]";
        else
            wkt += "LENGTHUNIT[\"metre\",1]";
        wkt += ",ID[\"EPSG\"," + strPrintf("%d", p.epsg) + "]]";
    }
    // south-orientated, polar and azimuthal-equal-area conversions carry
    // PROJ's method-specific coordinate systems (bare units, no codes)
    std::string linBare =
        "LENGTHUNIT[\"" + lu.name + "\"," + gtNum(lu.size) + "]";
    wkt += "],CS[Cartesian,2],";
    if (m.ct == 27)
    {
        wkt += "AXIS[\"easting (Y)\",west,ORDER[1]," + linBare +
               "],AXIS[\"northing (X)\",south,ORDER[2]," + linBare + "]]";
    }
    else if (m.ct == 15)
    {
        bool southPole = r.getD(3081, 0.0) < 0.0;
        std::string deg =
            "ANGLEUNIT[\"degree\",0.0174532925199433,ID[\"EPSG\",9122]]";
        const char *dir = southPole ? "north" : "south";
        const char *mN = southPole ? "0" : "180";
        wkt += std::string("AXIS[\"(E)\",") + dir + ",MERIDIAN[90," + deg +
               "],ORDER[1]," + linBare + "],AXIS[\"(N)\"," + dir +
               ",MERIDIAN[" + mN + "," + deg + "],ORDER[2]," + linBare +
               "]]";
    }
    else if (m.ct == 10)
    {
        wkt += "AXIS[\"(E)\",east,ORDER[1]," + linBare +
               "],AXIS[\"(N)\",north,ORDER[2]," + linBare + "]]";
    }
    else
    {
        // an explicit user-defined base, a db-resolved base, or a
        // recognized citation member keeps the spelled-out axis names;
        // other absent or failed codes abbreviate
        std::string lin = gtLinNode(lu);
        bool namedAxes =
            gcs == 32767 || knownBase || pcsCit.memberFound;
        const char *aE = namedAxes ? "easting" : "(E)";
        const char *aN = namedAxes ? "northing" : "(N)";
        wkt += "AXIS[\"" + std::string(aE) + "\",east,ORDER[1]," + lin +
               "],AXIS[\"" + aN + "\",north,ORDER[2]," + lin + "]]";
    }
    s.pj_ = proj_create(projCtx(), wkt.c_str());
    if (s.pj_ && m.ct == 17 && r.getD(3089, 0.0) == 0.0)
    {
        // PROJ's canonical Equidistant Cylindrical conversion carries a
        // hidden zero natural-origin latitude with a blank unit that WKT
        // cannot express; rebuild around it, reusing the parsed CS
        PJ *cs = proj_crs_get_coordinate_system(projCtx(), s.pj_);
        std::string bwkt = "GEOGCRS[\"" + g.name + "\"," + g.datumNode +
                           "," + g.pmNode + ",CS[ellipsoidal,2]," + baseCs +
                           baseId + "]";
        PJ *base = proj_create(projCtx(), bwkt.c_str());
        PJ *conv = proj_create_conversion_equidistant_cylindrical(
            projCtx(), r.getD(3078, 0.0), r.getD(3088, 0.0),
            r.getD(3082, 0.0) * lu.size, r.getD(3083, 0.0) * lu.size,
            g.axisUnit.name.c_str(), g.axisUnit.factor, "metre", 1.0);
        if (cs && base && conv)
        {
            PJ *crs = proj_create_projected_crs(projCtx(), name.c_str(),
                                                base, conv, cs);
            if (crs)
            {
                proj_destroy(s.pj_);
                s.pj_ = crs;
            }
        }
        if (cs)
            proj_destroy(cs);
        if (base)
            proj_destroy(base);
        if (conv)
            proj_destroy(conv);
    }
    ok = s.pj_ != nullptr;
    return s;
}

namespace
{

const char kGtSrsMismatchFmt[] =
    "The definition of %s CRS EPSG:%d got from GeoTIFF keys is not the "
    "same as the one from the EPSG registry, which may cause issues "
    "during reprojection operations. Set GTIFF_SRS_SOURCE configuration "
    "option to EPSG to use official parameters (overriding the ones from "
    "GeoTIFF keys), or to GEOKEYS to use custom values from GeoTIFF keys "
    "and drop the EPSG code.";

} // namespace

Srs Srs::fromGTiffProjectedRebuild(const GTiffKeyValues &kv, int pcs,
                                   bool &ok)
{
    ok = false;
    Srs s;
    GtKeyReader r{kv};
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, pcs);
    if (!db)
        return s;
    const char *dn = proj_get_name(db);
    std::string pcsName = dn && dn[0] ? dn : "unknown";

    int gcs = r.getS(2048, 0);
    if (gcs <= 0)
    {
        PJ *geo = proj_crs_get_geodetic_crs(projCtx(), db);
        if (geo)
        {
            const char *c = proj_get_id_code(geo, 0);
            gcs = c ? atoi(c) : 0;
            proj_destroy(geo);
        }
    }

    // base: db-composed for a coded geographic CRS; a user-defined datum
    // or ellipsoid key keeps only the coded name/ID shell over composed
    // parts; a code of a different object type warns as a geographic
    // registry mismatch and suppresses the projected comparison
    bool userDatum =
        r.getS(2050, 0) == 32767 || r.getS(2056, 0) == 32767;
    GtGeogParts g;
    bool haveParts = false;
    bool dbBase = false;
    bool geogWarned = false;
    if (gcs > 0 && gcs != 32767)
    {
        PJ *gdb = gtDbObject(PJ_CATEGORY_CRS, gcs);
        if (!gdb)
        {
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        "PROJ: proj_create_from_database: crs not found");
        }
        else
        {
            PJ_TYPE t = proj_get_type(gdb);
            bool isGeog = t == PJ_TYPE_GEOGRAPHIC_2D_CRS ||
                          t == PJ_TYPE_GEOGRAPHIC_3D_CRS ||
                          t == PJ_TYPE_GEODETIC_CRS;
            if (!isGeog)
            {
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf(kGtSrsMismatchFmt, "geographic", gcs));
                geogWarned = true;
                g = gtComposeGeogParts(r);
                g.baseId = gcs;
                haveParts = true;
            }
            else if (userDatum)
            {
                // user-defined datum/ellipsoid under a coded GCS keeps
                // only the name/ID shell; the composed datum can no
                // longer match the registry
                cplErrorStr(
                    CE_Warning, CPLE_AppDefined,
                    strPrintf(kGtSrsMismatchFmt, "geographic", gcs));
                geogWarned = true;
                g = gtComposeGeogParts(r);
                const char *gn = proj_get_name(gdb);
                if (gn && gn[0])
                    g.name = gn;
                g.baseId = gcs;
                haveParts = true;
            }
            else
            {
                haveParts = gtGeogPartsFromEpsg(r, gcs, g);
                dbBase = haveParts;
            }
            proj_destroy(gdb);
        }
    }
    if (!haveParts)
        g = gtComposeGeogParts(r);

    // CS linear unit: the units geokey when coded, else the database axis
    GtLinUnit lu{"metre", 1.0, 0};
    bool luFromKey = r.getS(3076, 0) > 0;
    if (luFromKey)
        lu = gtLinUnitOf(r);
    else
    {
        PJ *cs = proj_crs_get_coordinate_system(projCtx(), db);
        if (cs)
        {
            const char *un = nullptr;
            double f = 0.0;
            if (proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr,
                                      nullptr, &f, &un, nullptr,
                                      nullptr) &&
                un && f > 0.0)
                lu = {un, f, 0};
            proj_destroy(cs);
        }
    }

    // conversion: UTM-coded and coordinate-transformation keys keep the
    // key-composed parameters; a coded projection resolves through the
    // database (failures fall back to the PCS conversion as "unnamed");
    // otherwise the PCS's own conversion, always named after the method
    struct Pv
    {
        int code;
        std::string name;
        double value;
        std::string unitNode;
    };
    std::vector<Pv> vals;
    std::string methodName;
    int methodCode = 0;
    bool convUnnamed = false;
    int csCt = 0;
    double poleLat = 90.0;
    int projKey = r.getS(3074, 0);
    int ctKey = r.getS(3075, 0);
    bool utm = projKey >= 16001 && projKey <= 16160 &&
               (projKey <= 16060 || projKey >= 16101);
    GtMethodDef m;
    auto keyUnitNode = [&](char kind) -> std::string
    {
        if (kind == 'A')
        {
            if (!g.angNode.empty())
                return g.angNode;
            if (g.angExplicit)
                return gtAngNode(g.axisUnit, false);
            return gtAngNode({"degree", kDegRad, 0}, false);
        }
        if (kind == 'S')
            return "SCALEUNIT[\"unity\",1]";
        return "LENGTHUNIT[\"metre\",1]";
    };
    if (utm)
    {
        bool north = projKey < 16100;
        int zone = projKey - (north ? 16000 : 16100);
        methodName = "Transverse Mercator";
        methodCode = 9807;
        csCt = 1;
        double lon0 = zone * 6.0 - 183.0;
        if (!g.degree && g.axisUnit.factor > 0.0)
            lon0 = lon0 * kDegRad / g.axisUnit.factor;
        const struct
        {
            int code;
            char kind;
            double value;
        } utmVals[] = {{8801, 'A', 0.0},
                       {8802, 'A', lon0},
                       {8805, 'S', 0.9996},
                       {8806, 'L', 500000.0},
                       {8807, 'L', north ? 0.0 : 10000000.0}};
        for (const auto &p : utmVals)
            vals.push_back(
                {p.code, gtParamName(p.code), p.value, keyUnitNode(p.kind)});
        if (!g.degree)
            convUnnamed = true;
    }
    else if (ctKey > 0 && ctKey != 32767 && gtMethodFromCt(r, ctKey, m))
    {
        methodName = m.name;
        methodCode = m.code;
        csCt = m.ct;
        for (const auto &p : m.params)
        {
            double v = r.getD(p.geokey, p.kind == 'S' ? 1.0 : 0.0);
            if (p.kind == 'L')
                v *= lu.size;
            vals.push_back(
                {p.epsg, gtParamName(p.epsg), v, keyUnitNode(p.kind)});
        }
        if (!g.degree)
            convUnnamed = true;
        poleLat = r.getD(3081, 90.0);
    }
    else
    {
        PJ *op = nullptr;
        if (projKey > 0 && projKey != 32767)
        {
            op = gtDbObject(PJ_CATEGORY_COORDINATE_OPERATION, projKey);
            if (op && proj_get_type(op) != PJ_TYPE_CONVERSION)
            {
                proj_destroy(op);
                op = nullptr;
            }
            if (!op)
                convUnnamed = true;
        }
        if (!op)
            op = proj_crs_get_coordoperation(projCtx(), db);
        if (!op)
        {
            proj_destroy(db);
            return s;
        }
        const char *mn = nullptr, *ma = nullptr, *mc = nullptr;
        proj_coordoperation_get_method_info(projCtx(), op, &mn, &ma, &mc);
        methodName = mn && mn[0] ? mn : "unnamed";
        methodCode =
            ma && mc && strcmp(ma, "EPSG") == 0 ? atoi(mc) : 0;
        if (methodCode == 9808)
            csCt = 27;
        else if (methodCode == 9810 || methodCode == 9829)
            csCt = 15;
        else if (methodCode == 9820)
            csCt = 10;
        int n = proj_coordoperation_get_param_count(projCtx(), op);
        for (int i = 0; i < n; i++)
        {
            const char *pn = nullptr, *pa = nullptr, *pc = nullptr;
            const char *un = nullptr, *ucat = nullptr;
            double val = 0.0, uconv = 0.0;
            if (!proj_coordoperation_get_param(projCtx(), op, i, &pn, &pa,
                                               &pc, &val, nullptr, &uconv,
                                               &un, nullptr, nullptr,
                                               &ucat))
                continue;
            Pv p;
            p.code = pa && pc && strcmp(pa, "EPSG") == 0 ? atoi(pc) : 0;
            p.name = pn ? pn : "";
            p.value = val;
            std::string cat = ucat ? ucat : "";
            std::string uname = un ? un : "unknown";
            if (cat == "angular")
                p.unitNode =
                    "ANGLEUNIT[\"" + uname + "\"," + gtNum(uconv) + "]";
            else if (cat == "scale")
                p.unitNode =
                    "SCALEUNIT[\"" + uname + "\"," + gtNum(uconv) + "]";
            else
                p.unitNode =
                    "LENGTHUNIT[\"" + uname + "\"," + gtNum(uconv) + "]";
            if ((p.code == 8801 || p.code == 8832) && val < 0.0)
                poleLat = val;
            vals.push_back(std::move(p));
        }
        proj_destroy(op);
    }

    std::string nameEsc;
    for (char ch : pcsName)
    {
        nameEsc += ch;
        if (ch == '"')
            nameEsc += '"';
    }
    std::string convName = convUnnamed ? "unnamed" : methodName;
    std::string baseAxis =
        g.angNode.empty() ? gtAngNode(g.axisUnit, true) : g.angNode;
    std::string baseId =
        g.baseId > 0 ? ",ID[\"EPSG\"," + strPrintf("%d", g.baseId) + "]"
                     : "";
    std::string wkt =
        "PROJCRS[\"" + nameEsc + "\",BASEGEOGCRS[\"" + g.name + "\"," +
        g.datumNode + "," + g.pmNode +
        ",CS[ellipsoidal,2],AXIS[\"latitude\",north,ORDER[1]," + baseAxis +
        "],AXIS[\"longitude\",east,ORDER[2]," + baseAxis + "]" + baseId +
        "],CONVERSION[\"" + convName + "\",METHOD[\"" + methodName + "\"";
    if (methodCode > 0)
        wkt += ",ID[\"EPSG\"," + strPrintf("%d", methodCode) + "]";
    wkt += "]";
    for (const auto &p : vals)
    {
        wkt += ",PARAMETER[\"" + p.name + "\"," + gtNum(p.value) + "," +
               p.unitNode;
        if (p.code > 0)
            wkt += ",ID[\"EPSG\"," + strPrintf("%d", p.code) + "]";
        wkt += "]";
    }
    std::string linBare =
        "LENGTHUNIT[\"" + lu.name + "\"," + gtNum(lu.size) + "]";
    // a fully db-resolved base or a recognized citation member keeps the
    // spelled-out axis names; composed fallbacks abbreviate
    GtCitation pcsCit = gtParseCitation(r.getA(3073));
    bool namedAxes = dbBase || pcsCit.memberFound;
    std::string csDefault;
    {
        std::string lin = gtLinNode(lu);
        const char *aE = namedAxes ? "easting" : "(E)";
        const char *aN = namedAxes ? "northing" : "(N)";
        csDefault = "AXIS[\"" + std::string(aE) + "\",east,ORDER[1]," +
                    lin + "],AXIS[\"" + aN + "\",north,ORDER[2]," + lin +
                    "]";
    }
    std::string csActual = csDefault;
    if (csCt == 27)
    {
        csActual = "AXIS[\"easting (Y)\",west,ORDER[1]," + linBare +
                   "],AXIS[\"northing (X)\",south,ORDER[2]," + linBare +
                   "]";
    }
    else if (csCt == 15)
    {
        bool southPole = poleLat < 0.0;
        std::string deg =
            "ANGLEUNIT[\"degree\",0.0174532925199433,ID[\"EPSG\",9122]]";
        const char *dir = southPole ? "north" : "south";
        const char *mN = southPole ? "0" : "180";
        csActual = std::string("AXIS[\"(E)\",") + dir + ",MERIDIAN[90," +
                   deg + "],ORDER[1]," + linBare + "],AXIS[\"(N)\"," +
                   dir + ",MERIDIAN[" + mN + "," + deg + "],ORDER[2]," +
                   linBare + "]";
    }
    else if (csCt == 10)
    {
        csActual = "AXIS[\"(E)\",east,ORDER[1]," + linBare +
                   "],AXIS[\"(N)\",north,ORDER[2]," + linBare + "]";
    }
    std::string head = wkt + "],CS[Cartesian,2],";
    std::string tail = ",ID[\"EPSG\"," + strPrintf("%d", pcs) + "]]";
    wkt = head + csActual + tail;
    s.pj_ = proj_create(projCtx(), wkt.c_str());
    if (s.pj_ && userDatum)
    {
        PJ *fixed = gtBaseAxesToLatLon(s.pj_);
        if (fixed)
        {
            proj_destroy(s.pj_);
            s.pj_ = fixed;
        }
    }
    // registry comparison the GDAL way: the compared rebuild always
    // carries the plain east/north axes (method-specific coordinate
    // systems are invisible to it) and both sides normalize to GIS axis
    // order first
    if (s.pj_ && !geogWarned)
    {
        bool same = false;
        PJ *cmp = csActual == csDefault
                      ? proj_clone(projCtx(), s.pj_)
                      : proj_create(projCtx(),
                                    (head + csDefault + tail).c_str());
        if (cmp)
        {
            PJ *a = proj_normalize_for_visualization(projCtx(), cmp);
            PJ *b = proj_normalize_for_visualization(projCtx(), db);
            if (a && b)
                same = proj_is_equivalent_to_with_ctx(projCtx(), a, b, 1);
            if (a)
                proj_destroy(a);
            if (b)
                proj_destroy(b);
            proj_destroy(cmp);
        }
        if (!same)
            cplErrorStr(CE_Warning, CPLE_AppDefined,
                        strPrintf(kGtSrsMismatchFmt, "projected", pcs));
    }
    proj_destroy(db);
    ok = s.pj_ != nullptr;
    return s;
}

namespace
{

// the horizontal component is round-tripped through GDAL-flavor WKT1,
// which strips datum/unit codes but keeps the CRS-level identifiers
PJ *gtWkt1RoundTripObj(int code)
{
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, code);
    if (!db)
        return nullptr;
    const char *w1 = proj_as_wkt(projCtx(), db, PJ_WKT1_GDAL, nullptr);
    PJ *rt = w1 ? proj_create(projCtx(), w1) : nullptr;
    proj_destroy(db);
    return rt;
}

} // namespace

Srs Srs::fromGTiffWkt1RoundTrip(int code, bool &ok, bool latLonBaseAxes)
{
    ok = false;
    Srs out;
    out.pj_ = gtWkt1RoundTripObj(code);
    if (out.pj_ && latLonBaseAxes)
    {
        PJ *fixed = gtBaseAxesToLatLon(out.pj_);
        if (fixed)
        {
            proj_destroy(out.pj_);
            out.pj_ = fixed;
        }
    }
    ok = out.pj_ != nullptr;
    return out;
}

int Srs::gtiffGcsTypeClass(int code)
{
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, code);
    if (!db)
        return 0;
    PJ_TYPE t = proj_get_type(db);
    proj_destroy(db);
    if (t == PJ_TYPE_GEOGRAPHIC_2D_CRS || t == PJ_TYPE_GEOGRAPHIC_3D_CRS ||
        t == PJ_TYPE_GEODETIC_CRS)
        return 1;
    if (t == PJ_TYPE_GEOCENTRIC_CRS)
        return 2;
    return 3;
}

std::string Srs::gtiffCompoundNameFor(int pcs, int vertCode)
{
    std::string horiz = "unknown";
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, pcs);
    if (db)
    {
        const char *n = proj_get_name(db);
        if (n && n[0])
            horiz = n;
        proj_destroy(db);
    }
    std::string vert = "unknown";
    if (vertCode > 0)
    {
        PJ *v = gtDbObject(PJ_CATEGORY_CRS, vertCode);
        if (v)
        {
            const char *n = proj_get_name(v);
            if (n && n[0])
                vert = n;
            proj_destroy(v);
        }
    }
    return horiz + " + " + vert;
}

Srs Srs::fromGTiffCompound(int horizCode, bool projected, int vertCode,
                           bool &ok, const std::string &nameOverride,
                           int vertUnitCode, int vertDatumCode)
{
    (void)projected;
    ok = false;
    Srs out;
    PJ *horiz = gtWkt1RoundTripObj(horizCode);
    if (!horiz)
        return out;
    PJ *vert = nullptr;
    std::string vertName;
    if (vertCode > 0)
    {
        vert = gtDbObject(PJ_CATEGORY_CRS, vertCode);
        if (!vert || proj_get_type(vert) != PJ_TYPE_VERTICAL_CRS)
        {
            proj_destroy(horiz);
            if (vert)
                proj_destroy(vert);
            return out;
        }
        const char *vn = proj_get_name(vert);
        vertName = vn ? vn : "";
        // the vertical member round-trips through WKT1 too, dropping the
        // axis abbreviation and usage while keeping the identifier
        const char *vw1 = proj_as_wkt(projCtx(), vert, PJ_WKT1_GDAL,
                                      nullptr);
        PJ *vrt = vw1 ? proj_create(projCtx(), vw1) : nullptr;
        if (vrt)
        {
            proj_destroy(vert);
            vert = vrt;
        }
    }
    else
    {
        // user-defined vertical: unnamed CRS, datum resolved from the
        // vertical datum geokey when coded (else "unknown"), the axis
        // unit from the vertical units geokey
        vertName = "unknown";
        std::string vdatum = "VDATUM[\"unknown\"]";
        if (vertDatumCode > 0)
        {
            PJ *d = gtDbObject(PJ_CATEGORY_DATUM, vertDatumCode);
            if (d)
            {
                const char *dn = proj_get_name(d);
                if (dn && dn[0])
                    vdatum = "VDATUM[\"" + std::string(dn) +
                             "\",ID[\"EPSG\"," +
                             strPrintf("%d", vertDatumCode) + "]]";
                proj_destroy(d);
            }
        }
        std::string unit = "LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]";
        if (vertUnitCode > 0)
        {
            const char *n = nullptr;
            double f = 0.0;
            if (proj_uom_get_info_from_database(
                    projCtx(), "EPSG",
                    strPrintf("%d", vertUnitCode).c_str(), &n, &f,
                    nullptr) &&
                n)
                unit = "LENGTHUNIT[\"" + std::string(n) + "\"," + gtNum(f) +
                       ",ID[\"EPSG\"," + strPrintf("%d", vertUnitCode) +
                       "]]";
        }
        std::string vw = "VERTCRS[\"\"," + vdatum +
                         ",CS[vertical,1],AXIS[\"up\",up," + unit + "]]";
        vert = proj_create(projCtx(), vw.c_str());
        if (!vert)
        {
            proj_destroy(horiz);
            return out;
        }
    }
    const char *hn = proj_get_name(horiz);
    std::string name = nameOverride;
    if (name.empty())
        name = std::string(hn ? hn : "") + " + " + vertName;
    PJ *comp =
        proj_create_compound_crs(projCtx(), name.c_str(), horiz, vert);
    proj_destroy(horiz);
    proj_destroy(vert);
    if (!comp)
        return out;
    int *conf = nullptr;
    PJ_OBJ_LIST *lst = proj_identify(projCtx(), comp, "EPSG", nullptr, &conf);
    if (lst)
    {
        int n = proj_list_get_count(lst);
        for (int i = 0; i < n; i++)
        {
            if (conf[i] == 100)
            {
                PJ *match = proj_list_get(projCtx(), lst, i);
                if (match)
                {
                    proj_destroy(comp);
                    comp = match;
                }
                break;
            }
        }
        proj_list_destroy(lst);
        proj_int_list_destroy(conf);
    }
    out.pj_ = comp;
    ok = true;
    return out;
}

int Srs::gcsCodeOfPcs(int pcs)
{
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, pcs);
    if (!db)
        return 0;
    int out = 0;
    PJ *g = proj_crs_get_geodetic_crs(projCtx(), db);
    if (g)
    {
        const char *c = proj_get_id_code(g, 0);
        out = c ? atoi(c) : 0;
        proj_destroy(g);
    }
    proj_destroy(db);
    return out;
}

std::string Srs::gtiffVerticalUnitName(int vertCode, int unitCode)
{
    if (vertCode > 0)
    {
        PJ *v = gtDbObject(PJ_CATEGORY_CRS, vertCode);
        if (v)
        {
            std::string r;
            PJ *cs = proj_crs_get_coordinate_system(projCtx(), v);
            if (cs)
            {
                const char *un = nullptr;
                proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr,
                                      nullptr, nullptr, &un, nullptr,
                                      nullptr);
                if (un)
                    r = un;
                proj_destroy(cs);
            }
            proj_destroy(v);
            if (!r.empty())
                return r;
        }
    }
    if (unitCode > 0)
    {
        const char *n = nullptr;
        if (proj_uom_get_info_from_database(
                projCtx(), "EPSG", strPrintf("%d", unitCode).c_str(), &n,
                nullptr, nullptr) &&
            n)
            return n;
    }
    return "metre";
}

bool Srs::pcsLinearUnitDiffers(int pcs, int code, std::string &name,
                               double &factor)
{
    if (code <= 0 || code == 32767)
        return false;
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, pcs);
    if (!db)
        return false;
    int dbCode = 0;
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), db);
    if (cs)
    {
        const char *uc = nullptr;
        proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, &uc);
        if (uc)
            dbCode = atoi(uc);
        proj_destroy(cs);
    }
    proj_destroy(db);
    if (dbCode == 0 || dbCode == code)
        return false;
    const char *n = nullptr;
    double f = 0.0;
    if (!proj_uom_get_info_from_database(projCtx(), "EPSG",
                                         strPrintf("%d", code).c_str(), &n,
                                         &f, nullptr) ||
        !n)
        return false;
    name = n;
    factor = f;
    return true;
}

bool Srs::pcsAngularUnitDiffers(int pcs, int code)
{
    if (code <= 0 || code == 32767)
        return false;
    double keyFactor = 0.0;
    if (!proj_uom_get_info_from_database(projCtx(), "EPSG",
                                         strPrintf("%d", code).c_str(),
                                         nullptr, &keyFactor, nullptr) ||
        keyFactor <= 0.0)
        return false;
    PJ *db = gtDbObject(PJ_CATEGORY_CRS, pcs);
    if (!db)
        return false;
    double dbFactor = 0.0;
    PJ *g = proj_crs_get_geodetic_crs(projCtx(), db);
    if (g)
    {
        PJ *cs = proj_crs_get_coordinate_system(projCtx(), g);
        if (cs)
        {
            proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr,
                                  nullptr, &dbFactor, nullptr, nullptr,
                                  nullptr);
            proj_destroy(cs);
        }
        proj_destroy(g);
    }
    proj_destroy(db);
    if (dbFactor <= 0.0)
        return false;
    return fabs(keyFactor - dbFactor) > 1e-15 * dbFactor;
}

std::string Srs::verticalUnitName() const
{
    if (!pj_ || proj_get_type(pj_) != PJ_TYPE_COMPOUND_CRS)
        return "";
    PJ *sub = proj_crs_get_sub_crs(projCtx(), pj_, 1);
    if (!sub)
        return "";
    std::string r;
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), sub);
    if (cs)
    {
        const char *unitName = nullptr;
        proj_cs_get_axis_info(projCtx(), cs, 0, nullptr, nullptr, nullptr,
                              nullptr, &unitName, nullptr, nullptr);
        if (unitName)
            r = unitName;
        proj_destroy(cs);
    }
    proj_destroy(sub);
    return r;
}

void Srs::setCustomAngularUnit(const std::string &name, double conv)
{
    if (!pj_)
        return;
    PJ *alt = proj_crs_alter_cs_angular_unit(projCtx(), pj_, name.c_str(),
                                             conv, nullptr, nullptr);
    if (alt)
    {
        proj_destroy(pj_);
        pj_ = alt;
    }
}

std::string Srs::name() const
{
    if (!pj_)
        return "";
    const char *n = proj_get_name(pj_);
    return n ? n : "";
}

std::string Srs::authName() const
{
    if (!pj_)
        return "";
    const char *n = proj_get_id_auth_name(pj_, 0);
    return n ? n : "";
}

std::string Srs::code() const
{
    if (!pj_)
        return "";
    const char *n = proj_get_id_code(pj_, 0);
    return n ? n : "";
}

std::string Srs::typeString() const
{
    if (!pj_)
        return "";
    switch (proj_get_type(pj_))
    {
        case PJ_TYPE_GEOGRAPHIC_2D_CRS:
            return "Geographic 2D";
        case PJ_TYPE_GEOGRAPHIC_3D_CRS:
            return "Geographic 3D";
        case PJ_TYPE_PROJECTED_CRS:
            return "Projected";
        case PJ_TYPE_GEOCENTRIC_CRS:
            return "Geocentric";
        case PJ_TYPE_COMPOUND_CRS:
        {
            std::string r = "Compound";
            PJ *sub = proj_crs_get_sub_crs(projCtx(), pj_, 0);
            if (sub)
            {
                switch (proj_get_type(sub))
                {
                    case PJ_TYPE_GEOGRAPHIC_2D_CRS:
                    case PJ_TYPE_GEOGRAPHIC_3D_CRS:
                    case PJ_TYPE_GEOGRAPHIC_CRS:
                        r += " of Geographic";
                        break;
                    case PJ_TYPE_PROJECTED_CRS:
                        r += " of Projected";
                        break;
                    default:
                        break;
                }
                proj_destroy(sub);
            }
            return r;
        }
        case PJ_TYPE_VERTICAL_CRS:
            return "Vertical";
        case PJ_TYPE_ENGINEERING_CRS:
            return "Engineering";
        case PJ_TYPE_BOUND_CRS:
            return "Bound";
        default:
            return "Other";
    }
}

bool Srs::areaOfUse(double &w, double &s, double &e, double &n,
                    std::string &areaName) const
{
    if (!pj_)
        return false;
    const char *nm = nullptr;
    if (!proj_get_area_of_use(projCtx(), pj_, &w, &s, &e, &n, &nm))
        return false;
    areaName = nm ? nm : "";
    return true;
}

std::string Srs::wkt2_2019() const
{
    if (!pj_)
        return "";
    const char *w = proj_as_wkt(projCtx(), pj_, PJ_WKT2_2019, nullptr);
    if (!w)
        return "";
    std::string out = w;
    if (strip3DUnitIds_)
        stripCs3DUnitIds(out);
    if (wgs84DatumSwap_)
        out = spliceEnsemble(out, forcedDatumWkt(pj_));
    return out;
}

static void injectWkt1Authority(std::string &w, const char *nodeName,
                                const std::string &code, bool last = false)
{
    if (code.empty())
        return;
    size_t p = last ? w.rfind(nodeName) : w.find(nodeName);
    if (p == std::string::npos)
        return;
    size_t open = w.find('[', p);
    if (open == std::string::npos)
        return;
    size_t close = matchBracket(w, open);
    if (close == std::string::npos)
        return;
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i <= close; ++i)
    {
        char c = w[i];
        if (inStr)
        {
            if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
            inStr = true;
        else if (c == '[')
            ++depth;
        else if (c == ']')
            --depth;
        else if (depth == 1 && c == 'A' &&
                 w.compare(i, 10, "AUTHORITY[") == 0)
            return;
    }
    w.insert(close, ",AUTHORITY[\"EPSG\",\"" + code + "\"]");
}

std::string Srs::wkt1Gdal() const
{
    if (!pj_)
        return "";
    const char *opts[] = {"MULTILINE=NO", "OUTPUT_AXIS=YES", nullptr};
    const char *w = proj_as_wkt(projCtx(), pj_, PJ_WKT1_GDAL, opts);
    if (!w)
        return "";
    std::string out = w;
    injectWkt1Authority(out, "SPHEROID[", wkt1EllpsCode_);
    injectWkt1Authority(out, "DATUM[", wkt1DatumCode_);
    injectWkt1Authority(out, "PRIMEM[", wkt1PmCode_);
    injectWkt1Authority(out, "UNIT[", wkt1LinUnitCode_, true);
    return out;
}

std::string Srs::wkt1GdalFull() const
{
    if (!pj_)
        return "";
    const char *opts[] = {"MULTILINE=NO", "OUTPUT_AXIS=YES", nullptr};
    const char *w = proj_as_wkt(projCtx(), pj_, PJ_WKT1_GDAL, opts);
    if (!w)
        return "";
    std::string out = w;
    std::string datumCode, ellCode, pmCode, baseCode;
    auto capture = [&](PJ *crs)
    {
        PJ *base = proj_crs_get_geodetic_crs(projCtx(), crs);
        PJ *geod = base ? base : crs;
        PJ *datum = proj_crs_get_datum(projCtx(), geod);
        if (!datum)
            datum = proj_crs_get_datum_forced(projCtx(), geod);
        datumCode = epsgIdOf(datum);
        if (datum)
            proj_destroy(datum);
        PJ *ell = proj_get_ellipsoid(projCtx(), crs);
        ellCode = epsgIdOf(ell);
        if (ell)
            proj_destroy(ell);
        PJ *pm = proj_get_prime_meridian(projCtx(), crs);
        pmCode = epsgIdOf(pm);
        if (pm)
            proj_destroy(pm);
        baseCode = epsgIdOf(base);
        if (base)
            proj_destroy(base);
    };
    capture(pj_);
    if (datumCode.empty())
    {
        // WKT-built objects carry no component ids; re-resolve through
        // the database when the CRS itself is catalogued
        const char *a = proj_get_id_auth_name(pj_, 0);
        const char *c = proj_get_id_code(pj_, 0);
        if (a && c)
        {
            PJ *db = proj_create_from_database(projCtx(), a, c,
                                               PJ_CATEGORY_CRS, 0,
                                               nullptr);
            if (db)
            {
                capture(db);
                proj_destroy(db);
            }
        }
    }
    injectWkt1Authority(out, "SPHEROID[", ellCode);
    injectWkt1Authority(out, "DATUM[", datumCode);
    injectWkt1Authority(out, "PRIMEM[", pmCode);
    injectWkt1Authority(out, "GEOGCS[", baseCode);
    return out;
}

std::string Srs::wkt1Esri() const
{
    if (!pj_)
        return "";
    const char *opts[] = {"MULTILINE=NO", nullptr};
    const char *w = proj_as_wkt(projCtx(), pj_, PJ_WKT1_ESRI, opts);
    return w ? w : "";
}

std::string Srs::wkt2SingleLine() const
{
    if (!pj_)
        return "";
    const char *opts[] = {"MULTILINE=NO", nullptr};
    const char *w = proj_as_wkt(projCtx(), pj_, PJ_WKT2_2019, opts);
    if (!w)
        return "";
    std::string out = w;
    if (strip3DUnitIds_)
        stripCs3DUnitIds(out);
    return out;
}

// keeps PROJ's own pretty formatting: number tokens must round-trip
// byte-exactly, so the ensemble→datum swap is done by text splicing
std::string Srs::projjson() const
{
    if (!pj_)
        return "";
    const char *w = proj_as_projjson(projCtx(), pj_, nullptr);
    if (!w)
        return "";
    if (!wgs84DatumSwap_)
        return w;

    std::string datumJson;
    PJ *base = proj_crs_get_geodetic_crs(projCtx(), pj_);
    if (base)
    {
        PJ *forced = proj_crs_get_datum_forced(projCtx(), base);
        proj_destroy(base);
        if (forced)
        {
            const char *dj = proj_as_projjson(projCtx(), forced, nullptr);
            if (dj)
                datumJson = dj;
            proj_destroy(forced);
        }
    }
    if (datumJson.empty())
        return w;

    // split the standalone datum document into top-level entries and drop
    // schema/identification keys
    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (pos < datumJson.size())
        {
            size_t nl = datumJson.find('\n', pos);
            if (nl == std::string::npos)
                nl = datumJson.size();
            lines.push_back(datumJson.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }
    std::vector<std::string> kept;
    bool dropping = false;
    for (size_t i = 1; i + 1 < lines.size(); ++i)
    {
        const std::string &l = lines[i];
        bool topEntry = l.rfind("  \"", 0) == 0;
        if (topEntry)
        {
            size_t q = l.find('"', 3);
            std::string key = l.substr(3, q - 3);
            dropping = key == "$schema" || key == "id" || key == "scope" ||
                       key == "area" || key == "bbox";
        }
        if (!dropping)
            kept.push_back(l);
    }
    if (kept.empty())
        return w;
    // last kept entry must not carry a trailing comma
    {
        std::string &last = kept.back();
        if (!last.empty() && last.back() == ',')
            last.pop_back();
    }

    std::string raw = w;
    size_t keyPos = raw.find("\"datum_ensemble\":");
    if (keyPos == std::string::npos)
        return w;
    size_t lineStart = raw.rfind('\n', keyPos);
    std::string keyIndent = raw.substr(lineStart + 1, keyPos - lineStart - 1);
    size_t open = raw.find('{', keyPos);
    if (open == std::string::npos)
        return w;
    int depth = 0;
    size_t close = open;
    bool inStr = false;
    for (size_t i = open; i < raw.size(); ++i)
    {
        char c = raw[i];
        if (inStr)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                inStr = false;
            continue;
        }
        if (c == '"')
            inStr = true;
        else if (c == '{')
            ++depth;
        else if (c == '}' && --depth == 0)
        {
            close = i;
            break;
        }
    }
    std::string repl = "\"datum\": {\n";
    for (const auto &l : kept)
        repl += keyIndent + l + "\n";
    repl += keyIndent + "}";
    return raw.substr(0, keyPos) + repl + raw.substr(close + 1);
}

namespace
{

bool pjTypeIsGeographic(PJ_TYPE t)
{
    return t == PJ_TYPE_GEOGRAPHIC_2D_CRS || t == PJ_TYPE_GEOGRAPHIC_3D_CRS ||
           t == PJ_TYPE_GEOGRAPHIC_CRS;
}

// like OGRSpatialReference, compound CRSs answer for their horizontal part
bool pjIs(const PJ *pj, bool (*pred)(PJ_TYPE))
{
    PJ_TYPE t = proj_get_type(pj);
    if (t == PJ_TYPE_COMPOUND_CRS)
    {
        PJ *sub = proj_crs_get_sub_crs(projCtx(), pj, 0);
        if (!sub)
            return false;
        bool r = pred(proj_get_type(sub));
        proj_destroy(sub);
        return r;
    }
    return pred(t);
}

}  // namespace

bool Srs::isGeographic() const
{
    return pj_ && pjIs(pj_, pjTypeIsGeographic);
}

bool Srs::isProjected() const
{
    return pj_ && pjIs(pj_, [](PJ_TYPE t)
                       { return t == PJ_TYPE_PROJECTED_CRS; });
}

bool Srs::isGeographic3D() const
{
    return pj_ && proj_get_type(pj_) == PJ_TYPE_GEOGRAPHIC_3D_CRS;
}

bool Srs::isVertical() const
{
    return pj_ && proj_get_type(pj_) == PJ_TYPE_VERTICAL_CRS;
}

Srs Srs::promotedTo3D(const Srs &geog3D) const
{
    Srs out;
    if (!pj_)
        return out;
    out.wgs84DatumSwap_ = wgs84DatumSwap_;
    out.wkt1DatumCode_ = wkt1DatumCode_;
    out.wkt1EllpsCode_ = wkt1EllpsCode_;
    out.wkt1PmCode_ = wkt1PmCode_;
    out.wkt1LinUnitCode_ = wkt1LinUnitCode_;
    PJ *p3 = proj_crs_create_projected_3D_crs_from_2D(projCtx(), nullptr,
                                                      pj_, geog3D.pj_);
    if (!p3)
    {
        out.pj_ = proj_clone(projCtx(), pj_);
        return out;
    }
    out.pj_ = p3;
    out.strip3DUnitIds_ = true;
    return out;
}

void Srs::alterLinearUnit(const std::string &name, double factor, int code)
{
    if (!pj_)
        return;
    PJ *p1 = proj_crs_alter_parameters_linear_unit(
        projCtx(), pj_, name.c_str(), factor, nullptr, nullptr, 1);
    if (!p1)
        return;
    PJ *p2 = proj_crs_alter_cs_linear_unit(
        projCtx(), p1, name.c_str(), factor,
        strPrintf("%d", code).c_str(), "EPSG");
    proj_destroy(p1);
    if (!p2)
        return;
    proj_destroy(pj_);
    pj_ = p2;
}

double Srs::semiMajor() const
{
    if (!pj_)
        return 0;
    PJ *ell = proj_get_ellipsoid(projCtx(), pj_);
    if (!ell)
        return 0;
    double a = 0, b = 0, invf = 0;
    int bComputed = 0;
    proj_ellipsoid_get_parameters(projCtx(), ell, &a, &b, &bComputed,
                                  &invf);
    proj_destroy(ell);
    return a;
}

bool Srs::idTypeMatchesDb() const
{
    if (!pj_)
        return true;
    const char *an = proj_get_id_auth_name(pj_, 0);
    const char *cd = proj_get_id_code(pj_, 0);
    if (!an || !cd)
        return true;
    PJ *db = proj_create_from_database(projCtx(), an, cd, PJ_CATEGORY_CRS,
                                       0, nullptr);
    if (!db)
        return true;
    bool same = proj_get_type(db) == proj_get_type(pj_);
    proj_destroy(db);
    return same;
}

bool Srs::matchesDbDefinition() const
{
    if (!pj_)
        return false;
    const char *an = proj_get_id_auth_name(pj_, 0);
    const char *cd = proj_get_id_code(pj_, 0);
    if (!an || !cd)
        return false;
    PJ *db = proj_create_from_database(projCtx(), an, cd, PJ_CATEGORY_CRS,
                                       0, nullptr);
    if (!db)
        return false;
    bool same = proj_is_equivalent_to_with_ctx(projCtx(), pj_, db, 1);
    proj_destroy(db);
    return same;
}

std::vector<int> Srs::dataAxisToSRSAxisMapping() const
{
    std::vector<int> mapping{1, 2};
    if (!pj_)
        return mapping;
    PJ *root = pj_;
    PJ *sub = nullptr;
    int extraAxes = 0;
    if (proj_get_type(pj_) == PJ_TYPE_COMPOUND_CRS)
    {
        sub = proj_crs_get_sub_crs(projCtx(), pj_, 0);
        if (!sub)
            return mapping;
        root = sub;
        extraAxes = 1;
    }
    PJ *cs = proj_crs_get_coordinate_system(projCtx(), root);
    if (!cs)
    {
        if (sub)
            proj_destroy(sub);
        return mapping;
    }
    // polar CRSs make direction alone ambiguous ("south" serves both
    // northing-first and easting-first orders); the axis names settle it,
    // and BOTH axes must agree (Northing/Westing pairs keep 1,2)
    const char *name0 = nullptr, *dir0 = nullptr;
    const char *name1 = nullptr, *dir1 = nullptr;
    bool ok0 = proj_cs_get_axis_info(projCtx(), cs, 0, &name0, nullptr,
                                     &dir0, nullptr, nullptr, nullptr,
                                     nullptr) != 0;
    bool ok1 = proj_cs_get_axis_info(projCtx(), cs, 1, &name1, nullptr,
                                     &dir1, nullptr, nullptr, nullptr,
                                     nullptr) != 0;
    auto northish = [](const char *n)
    {
        return n && (strncasecmp(n, "north", 5) == 0 ||
                     strncasecmp(n, "lat", 3) == 0 ||
                     strncasecmp(n, "geodetic lat", 12) == 0);
    };
    auto eastish = [](const char *n)
    {
        return n && (strncasecmp(n, "east", 4) == 0 ||
                     strncasecmp(n, "lon", 3) == 0 ||
                     strncasecmp(n, "geodetic lon", 12) == 0);
    };
    if (ok0 && ok1)
    {
        bool swap = false;
        if (northish(name0))
            swap = eastish(name1);
        else if (!eastish(name0) && dir0 && dir1 &&
                 strcmp(dir0, "north") == 0 && strcmp(dir1, "east") == 0)
            swap = true;
        if (swap)
            mapping = {2, 1};
    }
    int axisCount = proj_cs_get_axis_count(projCtx(), cs) + extraAxes;
    for (int i = 2; i < axisCount; ++i)
        mapping.push_back(i + 1);
    proj_destroy(cs);
    if (sub)
        proj_destroy(sub);
    return mapping;
}

int Srs::epsgCode() const
{
    if (authName() == "EPSG")
        return atoi(code().c_str());
    return -1;
}

bool Srs::isEquivalentTo(const Srs &o) const
{
    if (!pj_ || !o.pj_)
        return pj_ == o.pj_;
    bool same = false;
    PJ *a = proj_normalize_for_visualization(projCtx(), pj_);
    PJ *b = proj_normalize_for_visualization(projCtx(), o.pj_);
    if (a && b)
        same = proj_is_equivalent_to_with_ctx(projCtx(), a, b, 1);
    if (a)
        proj_destroy(a);
    if (b)
        proj_destroy(b);
    return same;
}

bool Srs::toGeodetic(double x, double y, double &lon, double &lat) const
{
    if (!pj_)
        return false;
    if (isGeographic())
    {
        lon = x;
        lat = y;
        double factor = geodeticAngularFactor(pj_);
        if (factor > 0 && !isDegreeFactor(factor))
        {
            double toDeg = factor * 180.0 / M_PI;
            lon = x * toDeg;
            lat = y * toDeg;
        }
        return true;
    }
    PJ *geod = proj_crs_get_geodetic_crs(projCtx(), pj_);
    if (!geod)
        return false;
    bool geocentric = proj_get_type(geod) == PJ_TYPE_GEOCENTRIC_CRS;
    if (geocentric)
    {
        // geocentric sources convert against a geographic CRS cloned from
        // their own datum
        PJ *datum = proj_crs_get_datum(projCtx(), geod);
        if (!datum)
            datum = proj_crs_get_datum_forced(projCtx(), geod);
        PJ *cs = proj_create_ellipsoidal_2D_cs(
            projCtx(), PJ_ELLPS2D_LONGITUDE_LATITUDE, nullptr, 0);
        PJ *geog = datum && cs
                       ? proj_create_geographic_crs_from_datum(
                             projCtx(), "unknown", datum, cs)
                       : nullptr;
        if (datum)
            proj_destroy(datum);
        if (cs)
            proj_destroy(cs);
        if (geog)
        {
            proj_destroy(geod);
            geod = geog;
        }
    }
    PJ *op = proj_create_crs_to_crs_from_pj(projCtx(), pj_, geod, nullptr,
                                            nullptr);
    proj_destroy(geod);
    if (!op)
        return false;
    PJ *norm = proj_normalize_for_visualization(projCtx(), op);
    proj_destroy(op);
    if (!norm)
        return false;
    PJ_COORD c = proj_coord(x, y, 0, 0);
    c = proj_trans(norm, PJ_FWD, c);
    proj_destroy(norm);
    lon = c.xyzt.x;
    lat = c.xyzt.y;
    // the normalized transform yields degrees, but the display conversion
    // applies the base CRS angular unit factor regardless
    double factor = geocentric ? 0.0 : geodeticAngularFactor(pj_);
    if (factor > 0 && !isDegreeFactor(factor))
    {
        double toDeg = factor * 180.0 / M_PI;
        lon *= toDeg;
        lat *= toDeg;
    }
    return std::isfinite(lon) && std::isfinite(lat);
}

bool Srs::transformBoundsTo(const Srs &dst, double &xmin, double &ymin,
                            double &xmax, double &ymax) const
{
    if (!pj_ || !dst.pj_)
        return false;
    PJ *op = proj_create_crs_to_crs_from_pj(projCtx(), pj_, dst.pj_,
                                            nullptr, nullptr);
    if (!op)
        return false;
    PJ *norm = proj_normalize_for_visualization(projCtx(), op);
    proj_destroy(op);
    if (!norm)
        return false;
    double xs[4] = {xmin, xmax, xmin, xmax};
    double ys[4] = {ymin, ymin, ymax, ymax};
    double oxmin = 0, oymin = 0, oxmax = 0, oymax = 0;
    bool any = false;
    for (int i = 0; i < 4; i++)
    {
        PJ_COORD c = proj_coord(xs[i], ys[i], 0, 0);
        c = proj_trans(norm, PJ_FWD, c);
        double ox = c.xyzt.x, oy = c.xyzt.y;
        if (!std::isfinite(ox) || !std::isfinite(oy))
            continue;
        if (!any || ox < oxmin)
            oxmin = ox;
        if (!any || ox > oxmax)
            oxmax = ox;
        if (!any || oy < oymin)
            oymin = oy;
        if (!any || oy > oymax)
            oymax = oy;
        any = true;
    }
    proj_destroy(norm);
    if (!any)
        return false;
    xmin = oxmin;
    ymin = oymin;
    xmax = oxmax;
    ymax = oymax;
    return true;
}

bool Srs::toWgs84(double x, double y, double &lon, double &lat) const
{
    if (!pj_)
        return false;
    bool ok = false;
    static Srs wgs84 = fromEpsg(4326, ok);
    PJ *op = proj_create_crs_to_crs_from_pj(projCtx(), pj_, wgs84.pj_,
                                            nullptr, nullptr);
    if (!op)
        return false;
    PJ *norm = proj_normalize_for_visualization(projCtx(), op);
    proj_destroy(op);
    if (!norm)
        return false;
    // an unset epoch: dynamic-frame transforms behave differently at t=0
    PJ_COORD c = proj_coord(x, y, 0, HUGE_VAL);
    c = proj_trans(norm, PJ_FWD, c);
    proj_destroy(norm);
    lon = c.xyzt.x;
    lat = c.xyzt.y;
    return std::isfinite(lon) && std::isfinite(lat);
}
