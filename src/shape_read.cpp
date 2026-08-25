// ESRI Shapefile reader (.shp/.shx/.dbf/.prj) for the vector commands
#include "cpl.h"
#include "ogr.h"
#include "recode.h"
#include "util.h"
#include "vsi.h"

#include <cmath>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{

uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

uint32_t le32(const unsigned char *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

double leDouble(const unsigned char *p)
{
    double d;
    memcpy(&d, p, 8);
    return d;
}

std::string shpRecodeToUtf8(const std::string &in, const std::string &enc)
{
    if (enc.empty())
        return in;
    // read-side iconv diagnostics stay quiet (GDAL emits them lazily at
    // feature materialization; our eager parse would misplace them)
    return cplRecodeSilent(in, enc, "UTF-8");
}

struct DbfField
{
    std::string name;
    char type = 'C';
    int width = 0;
    int decimals = 0;
};

struct DbfFile
{
    int year = 0, month = 0, day = 0;
    uint32_t nRecords = 0;
    int ldid = 0;
    std::vector<DbfField> fields;
    std::vector<size_t> offsets;  // per-field offset inside a record
    size_t recordSize = 0;
    std::string data;
    size_t headerSize = 0;
    bool valid = false;
};

bool readDbf(const std::string &path, DbfFile &dbf)
{
    if (!readFileToString(path, dbf.data))
        return false;
    const std::string &d = dbf.data;
    if (d.size() < 32)
        return false;
    const unsigned char *h = (const unsigned char *)d.data();
    dbf.year = 1900 + h[1];
    dbf.month = h[2];
    dbf.day = h[3];
    dbf.nRecords = le32(h + 4);
    dbf.headerSize = le16(h + 8);
    dbf.recordSize = le16(h + 10);
    dbf.ldid = h[29];
    size_t off = 32;
    size_t fieldOff = 1;  // record byte 0 is the deletion flag
    while (off + 32 <= d.size() && (unsigned char)d[off] != 0x0D)
    {
        DbfField f;
        size_t nameLen = 0;
        while (nameLen < 11 && d[off + nameLen] != '\0')
            nameLen++;
        f.name = d.substr(off, nameLen);
        f.type = d[off + 11];
        f.width = (unsigned char)d[off + 16];
        f.decimals = (unsigned char)d[off + 17];
        dbf.offsets.push_back(fieldOff);
        fieldOff += f.width;
        dbf.fields.push_back(std::move(f));
        off += 32;
    }
    dbf.valid = true;
    return true;
}

void dbfFieldDefn(const DbfField &f, const std::string &enc,
                  OgrFieldDefn &fd)
{
    fd.name = shpRecodeToUtf8(f.name, enc);
    fd.width = f.width;
    fd.precision = f.decimals;
    switch (f.type)
    {
        case 'N':
            if (f.decimals > 0)
                fd.type = OFTReal;
            else if (f.width < 10)
                fd.type = OFTInteger;
            else if (f.width < 19)
                fd.type = OFTInteger64;
            else
                fd.type = OFTReal;
            break;
        case 'F':
            fd.type = OFTReal;
            break;
        case 'L':
            fd.type = OFTInteger;
            fd.subType = OFSTBoolean;
            break;
        case 'D':
            fd.type = OFTDate;
            fd.width = 10;
            fd.precision = 0;
            break;
        default:
            fd.type = OFTString;
            break;
    }
}

void dbfValue(const DbfFile &dbf, size_t rec, size_t fld,
              const std::string &enc, const OgrFieldDefn &fd,
              OgrFieldValue &out)
{
    size_t base = dbf.headerSize + rec * dbf.recordSize;
    const DbfField &f = dbf.fields[fld];
    if (base + dbf.offsets[fld] + f.width > dbf.data.size())
        return;
    std::string raw = dbf.data.substr(base + dbf.offsets[fld], f.width);
    size_t b = raw.find_first_not_of(' ');
    if (b == std::string::npos)
        raw.clear();
    else
    {
        size_t e = raw.find_last_not_of(' ');
        raw = raw.substr(b, e - b + 1);
    }
    switch (f.type)
    {
        case 'N':
        case 'F':
        {
            if (raw.empty() || raw[0] == '*')
                out.v.type = JVal::NUL;
            else if (fd.type == OFTReal)
            {
                out.v.type = JVal::DOUBLE;
                out.v.d = atof(raw.c_str());
                out.v.s = ogrJsonDouble(out.v.d);
            }
            else
            {
                out.v.type = JVal::INT;
                out.v.i = atoll(raw.c_str());
            }
            break;
        }
        case 'L':
        {
            char c = raw.empty() ? '?' : raw[0];
            if (c == 'T' || c == 't' || c == 'Y' || c == 'y')
            {
                out.v.type = JVal::BOOL;
                out.v.b = true;
            }
            else if (c == 'F' || c == 'f' || c == 'N' || c == 'n')
            {
                out.v.type = JVal::BOOL;
                out.v.b = false;
            }
            else
                out.v.type = JVal::NUL;
            break;
        }
        case 'D':
        {
            if (raw.size() < 8 || raw == "00000000" || raw[0] == '*')
                out.v.type = JVal::NUL;
            else
            {
                out.v.type = JVal::STRING;
                out.v.s = raw.substr(0, 4) + "/" + raw.substr(4, 2) + "/" +
                          raw.substr(6, 2);
            }
            break;
        }
        default:
        {
            std::string full =
                dbf.data.substr(base + dbf.offsets[fld], f.width);
            size_t e = full.find_last_not_of(' ');
            if (e == std::string::npos)
            {
                out.v.type = JVal::NUL;
                break;
            }
            full = full.substr(0, e + 1);
            out.v.type = JVal::STRING;
            out.v.s = shpRecodeToUtf8(full, enc);
            break;
        }
    }
    out.set = true;
}

// ---- shp geometry

double ringArea2(const std::vector<double> &c)
{
    double a = 0;
    size_t n = c.size() / 3;
    for (size_t i = 0; i + 1 < n; i++)
        a += c[i * 3] * c[(i + 1) * 3 + 1] - c[(i + 1) * 3] * c[i * 3 + 1];
    return a;
}

bool pointInRing(const std::vector<double> &c, double x, double y)
{
    bool in = false;
    size_t n = c.size() / 3;
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        double xi = c[i * 3], yi = c[i * 3 + 1];
        double xj = c[j * 3], yj = c[j * 3 + 1];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            in = !in;
    }
    return in;
}

bool parseShpRecord(const unsigned char *p, size_t len, OgrGeometry &g)
{
    if (len < 4)
        return false;
    int type = (int)le32(p);
    if (type == 0)
        return false;
    bool hasZ = type == 11 || type == 13 || type == 15 || type == 18;
    int base = type % 10;
    if (type == 21 || type == 23 || type == 25 || type == 28)
        base = type - 20 == 8 ? 8 : type - 20;
    if (type == 28)
        base = 8;
    switch (base)
    {
        case 1:  // point
        {
            if (len < 4 + 16)
                return false;
            g.type = 1;
            g.hasZ = hasZ;
            double z = 0;
            if (hasZ && len >= 4 + 24)
                z = leDouble(p + 20);
            g.coords = {leDouble(p + 4), leDouble(p + 12), z};
            if (type == 21 && len >= 4 + 24)
            {
                g.hasM = true;
                g.m = {leDouble(p + 20)};
            }
            else if (type == 11 && len >= 4 + 32)
            {
                g.hasM = true;
                g.m = {leDouble(p + 28)};
            }
            return true;
        }
        case 8:  // multipoint
        {
            if (len < 4 + 32 + 4)
                return false;
            uint32_t n = le32(p + 36);
            size_t need = 40 + (size_t)n * 16;
            if (len < need)
                return false;
            g.type = 4;
            g.hasZ = hasZ;
            size_t zOff = need + 16;  // skip zmin/zmax
            bool zAvail = hasZ && len >= zOff + (size_t)n * 8;
            size_t mSec = need + (hasZ ? 16 + (size_t)n * 8 : 0);
            bool mAvail = (type == 18 || type == 28) &&
                          len >= mSec + 16 + (size_t)n * 8;
            for (uint32_t i = 0; i < n; i++)
            {
                OgrGeometry pt;
                pt.type = 1;
                pt.hasZ = hasZ;
                double z = zAvail ? leDouble(p + zOff + i * 8) : 0;
                pt.coords = {leDouble(p + 40 + i * 16),
                             leDouble(p + 40 + i * 16 + 8), z};
                if (mAvail)
                {
                    pt.hasM = true;
                    pt.m = {leDouble(p + mSec + 16 + i * 8)};
                }
                g.parts.push_back(std::move(pt));
            }
            if (mAvail)
                g.hasM = true;
            return true;
        }
        case 3:  // arc
        case 5:  // polygon
        {
            if (len < 4 + 32 + 8)
                return false;
            uint32_t nParts = le32(p + 36);
            uint32_t nPoints = le32(p + 40);
            size_t ptOff = 44 + (size_t)nParts * 4;
            size_t need = ptOff + (size_t)nPoints * 16;
            if (len < need)
                return false;
            std::vector<uint32_t> starts(nParts);
            for (uint32_t i = 0; i < nParts; i++)
                starts[i] = le32(p + 44 + i * 4);
            size_t zOff = need + 16;
            bool zAvail = hasZ && len >= zOff + (size_t)nPoints * 8;
            size_t mSec = need + (hasZ ? 16 + (size_t)nPoints * 8 : 0);
            bool mAvail = (type == 13 || type == 15 || type == 23 ||
                           type == 25) &&
                          len >= mSec + 16 + (size_t)nPoints * 8;
            std::vector<OgrGeometry> lines;
            for (uint32_t i = 0; i < nParts; i++)
            {
                uint32_t s = starts[i];
                uint32_t e = i + 1 < nParts ? starts[i + 1] : nPoints;
                OgrGeometry ln;
                ln.type = 2;
                ln.hasZ = hasZ;
                ln.hasM = mAvail;
                for (uint32_t k = s; k < e && k < nPoints; k++)
                {
                    ln.coords.push_back(leDouble(p + ptOff + k * 16));
                    ln.coords.push_back(leDouble(p + ptOff + k * 16 + 8));
                    ln.coords.push_back(
                        zAvail ? leDouble(p + zOff + k * 8) : 0);
                    if (mAvail)
                        ln.m.push_back(leDouble(p + mSec + 16 + k * 8));
                }
                lines.push_back(std::move(ln));
            }
            if (base == 3)
            {
                if (lines.size() == 1)
                    g = std::move(lines[0]);
                else
                {
                    g.type = 5;
                    g.hasZ = hasZ;
                    g.hasM = mAvail;
                    g.parts = std::move(lines);
                }
                return true;
            }
            // polygon assembly: clockwise rings are shells, counter-
            // clockwise rings are holes attached to the first shell that
            // contains them (organizePolygons ONLY_CCW)
            if (lines.size() == 1)
            {
                g.type = 3;
                g.hasZ = hasZ;
                g.hasM = mAvail;
                g.parts = std::move(lines);
                return true;
            }
            std::vector<OgrGeometry> polys;
            std::vector<OgrGeometry> orphanHoles;
            for (OgrGeometry &ring : lines)
            {
                bool shell = ringArea2(ring.coords) <= 0;
                if (shell)
                {
                    OgrGeometry poly;
                    poly.type = 3;
                    poly.hasZ = hasZ;
                    poly.hasM = mAvail;
                    poly.parts.push_back(std::move(ring));
                    polys.push_back(std::move(poly));
                }
                else
                {
                    bool placed = false;
                    for (OgrGeometry &poly : polys)
                    {
                        if (pointInRing(poly.parts[0].coords,
                                        ring.coords[0], ring.coords[1]))
                        {
                            poly.parts.push_back(std::move(ring));
                            placed = true;
                            break;
                        }
                    }
                    if (!placed)
                        orphanHoles.push_back(std::move(ring));
                }
            }
            for (OgrGeometry &ring : orphanHoles)
            {
                OgrGeometry poly;
                poly.type = 3;
                poly.hasZ = hasZ;
                poly.hasM = mAvail;
                poly.parts.push_back(std::move(ring));
                polys.push_back(std::move(poly));
            }
            if (polys.size() == 1)
                g = std::move(polys[0]);
            else
            {
                g.type = 6;
                g.hasZ = hasZ;
                g.hasM = mAvail;
                g.parts = std::move(polys);
            }
            return true;
        }
        default:
            return false;
    }
}

bool geomMValid(const OgrGeometry &g)
{
    for (double v : g.m)
        if (v > -1e38)
            return true;
    for (const OgrGeometry &p : g.parts)
        if (geomMValid(p))
            return true;
    return false;
}

// once the layer measured-ness is settled, features either lose M
// entirely or gain zero-filled M for records that lacked the section
void applyM(OgrGeometry &g, bool keep)
{
    if (!keep)
    {
        g.hasM = false;
        g.m.clear();
    }
    else
    {
        if (!g.hasM && (g.type == 1 || g.type == 2))
            g.m.assign(g.coords.size() / 3, 0.0);
        g.hasM = true;
    }
    for (OgrGeometry &p : g.parts)
        applyM(p, keep);
}

bool fileExists(const std::string &p)
{
    if (vsiIsVirtual(p))
        return vsiExists(p) && !vsiIsDir(p);
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string findSibling(const std::string &base, const char *extLower,
                        const char *extUpper)
{
    if (fileExists(base + extLower))
        return base + extLower;
    if (fileExists(base + extUpper))
        return base + extUpper;
    return "";
}

bool readShapeLayer(const std::string &anyPath, OgrLayer &lyr,
                    const std::vector<std::string> &openOptions,
                    bool *openFailed = nullptr)
{
    std::string base = anyPath;
    size_t dot = base.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos)
    {
        ext = base.substr(dot + 1);
        base = base.substr(0, dot);
    }
    std::string name = base;
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    lyr.name = name;

    std::string shpPath = findSibling(base, ".shp", ".SHP");
    std::string shxPath = findSibling(base, ".shx", ".SHX");
    std::string dbfPath = findSibling(base, ".dbf", ".DBF");
    std::string prjPath = findSibling(base, ".prj", ".PRJ");
    std::string cpgPath = findSibling(base, ".cpg", ".CPG");

    DbfFile dbf;
    bool haveDbf = !dbfPath.empty() && readDbf(dbfPath, dbf) && dbf.valid;

    std::string shp;
    bool haveShpFile = !shpPath.empty() && readFileToString(shpPath, shp);
    bool haveShp = haveShpFile && shp.size() >= 100;

    if (!haveShpFile && !haveDbf)
        return false;

    // SHPOpen requires a readable .shx whenever the .shp file exists
    std::string shx;
    bool haveShx = false;
    if (haveShpFile)
    {
        bool shxOk = !shxPath.empty() && readFileToString(shxPath, shx) &&
                     shx.size() >= 100;
        if (shxOk)
            haveShx = true;
        else
        {
            if (configTestBool("SHAPE_RESTORE_SHX", false) && haveShp)
            {
                const unsigned char *sb = (const unsigned char *)shp.data();
                std::string entries;
                size_t off = 100;
                while (off + 8 <= shp.size())
                {
                    uint32_t cw = be32(sb + off + 4);
                    uint32_t ow = (uint32_t)(off / 2);
                    char e[8] = {(char)(ow >> 24), (char)(ow >> 16),
                                 (char)(ow >> 8),  (char)ow,
                                 (char)(cw >> 24), (char)(cw >> 16),
                                 (char)(cw >> 8),  (char)cw};
                    entries.append(e, 8);
                    off += 8 + (size_t)cw * 2;
                }
                shx = shp.substr(0, 100);
                uint32_t lenWords =
                    (uint32_t)((100 + entries.size()) / 2);
                shx[24] = (char)(lenWords >> 24);
                shx[25] = (char)(lenWords >> 16);
                shx[26] = (char)(lenWords >> 8);
                shx[27] = (char)lenWords;
                shx += entries;
                writeStringToFile(
                    shxPath.empty() ? base + ".shx" : shxPath, shx);
                haveShx = true;
            }
            else if (shxPath.empty())
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "Unable to open " + base + ".shx or " + base +
                                ".SHX. Set SHAPE_RESTORE_SHX config "
                                "option to YES to restore or create it.");
                if (openFailed)
                    *openFailed = true;
                return false;
            }
            else
            {
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            ".shx file is unreadable, or corrupt.");
                if (openFailed)
                    *openFailed = true;
                return false;
            }
        }
    }

    // encoding model (OGRShapeLayer::ConvertCodePage replica)
    std::string codePage;
    if (haveDbf)
    {
        std::string cpg;
        if (!cpgPath.empty() && readFileToString(cpgPath, cpg))
        {
            // dbfopen reads at most 31 bytes of the .cpg
            if (cpg.size() > 31)
                cpg.resize(31);
            codePage = cpg.substr(0, cpg.find_first_of("\r\n"));
        }
        else if (dbf.ldid != 0)
            codePage = strPrintf("LDID/%d", dbf.ldid);
        if (!codePage.empty())
            lyr.debugNotes.emplace_back(
                "Shape", "DBF Codepage = " + codePage + " for " + anyPath);
    }
    std::string encoding;
    std::vector<std::pair<std::string, std::string>> shpDom;
    if (haveDbf)
    {
        lyr.metadata.emplace_back(
            "DBF_DATE_LAST_UPDATE",
            strPrintf("%04d-%02d-%02d", dbf.year, dbf.month, dbf.day));
        if (!codePage.empty())
        {
            std::string eLdid, eCpg;
            if (dbf.ldid != 0)
            {
                shpDom.emplace_back("LDID_VALUE",
                                    strPrintf("%d", dbf.ldid));
                eLdid = recodeFromLdid(dbf.ldid);
                if (!eLdid.empty())
                    shpDom.emplace_back("ENCODING_FROM_LDID", eLdid);
            }
            if (!(codePage.size() >= 5 &&
                  strEqualNoCase(codePage.substr(0, 5), "LDID/")))
            {
                shpDom.emplace_back("CPG_VALUE", codePage);
                eCpg = recodeFromCpg(codePage);
                if (!eCpg.empty())
                    shpDom.emplace_back("ENCODING_FROM_CPG", eCpg);
            }
            encoding = !eCpg.empty() ? eCpg : eLdid;
        }
    }
    std::string ooEnc;
    bool haveOoEnc = false;
    for (const std::string &kv : openOptions)
    {
        size_t eq = kv.find('=');
        if (eq != std::string::npos &&
            strEqualNoCase(kv.substr(0, eq), "ENCODING"))
        {
            ooEnc = kv.substr(eq + 1);
            haveOoEnc = true;
            break;
        }
    }
    if (haveOoEnc)
        encoding = ooEnc;
    else if (configIsSet("SHAPE_ENCODING"))
        encoding = configGet("SHAPE_ENCODING");
    if (!encoding.empty())
    {
        lyr.debugNotes.emplace_back(
            "Shape", "Treating as encoding '" + encoding + "'.");
        if (!recodeSupported(encoding))
        {
            lyr.debugNotes.emplace_back("Shape", "Cannot recode from '" +
                                                     encoding +
                                                     "'. Disabling recoding");
            encoding.clear();
        }
    }
    shpDom.emplace_back("SOURCE_ENCODING", encoding);
    lyr.extraMdDomains.emplace_back("SHAPEFILE", std::move(shpDom));
    if (haveDbf)
    {
        for (const DbfField &f : dbf.fields)
        {
            OgrFieldDefn fd;
            dbfFieldDefn(f, encoding, fd);
            lyr.fields.push_back(std::move(fd));
        }
    }

    long long count = 0;
    bool mCandidate = false;
    const unsigned char *sp = (const unsigned char *)shp.data();
    if (haveShp)
    {
        int shpType = (int)le32(sp + 32);
        bool hasZ = shpType == 11 || shpType == 13 || shpType == 15 ||
                    shpType == 18;
        mCandidate = hasZ || shpType == 21 || shpType == 23 ||
                     shpType == 25 || shpType == 28;
        int base10 = shpType % 10;
        if (shpType >= 21 && shpType <= 28)
            base10 = shpType == 28 ? 8 : shpType - 20;
        switch (base10)
        {
            case 1:
                lyr.geomType = 1;
                break;
            case 3:
                lyr.geomType = 2;
                break;
            case 5:
                lyr.geomType = 3;
                break;
            case 8:
                lyr.geomType = 4;
                break;
            default:
                lyr.geomType = 0;
                break;
        }
        lyr.geomHasZ = hasZ && lyr.geomType != 0;
        lyr.hasGeomField = true;
        lyr.extent[0] = leDouble(sp + 36);
        lyr.extent[1] = leDouble(sp + 44);
        lyr.extent[2] = leDouble(sp + 52);
        lyr.extent[3] = leDouble(sp + 60);
        lyr.hasExtent = true;

        if (haveShx && shx.size() >= 100)
            count = ((long long)shx.size() - 100) / 8;
        else
            count = haveDbf ? (long long)dbf.nRecords : 0;
    }
    else
    {
        lyr.geomType = 101;
        lyr.hasGeomField = false;
        count = (long long)dbf.nRecords;
    }
    if (!haveShp && haveDbf)
        count = (long long)dbf.nRecords;

    size_t shpOff = 100;
    for (long long rec = 0; rec < count; rec++)
    {
        OgrFeature feat;
        feat.fid = rec;
        feat.values.resize(lyr.fields.size());
        if (haveDbf && (size_t)rec < dbf.nRecords)
            for (size_t i = 0; i < dbf.fields.size(); i++)
                dbfValue(dbf, (size_t)rec, i, encoding, lyr.fields[i],
                         feat.values[i]);
        if (haveShp && shpOff + 8 <= shp.size())
        {
            uint32_t contentWords = be32(sp + shpOff + 4);
            size_t contentLen = (size_t)contentWords * 2;
            if (shpOff + 8 + contentLen <= shp.size())
            {
                OgrGeometry g;
                if (parseShpRecord(sp + shpOff + 8, contentLen, g))
                {
                    feat.hasGeom = true;
                    feat.geom = std::move(g);
                }
            }
            shpOff += 8 + contentLen;
        }
        lyr.features.push_back(std::move(feat));
    }

    if (mCandidate && lyr.geomType != 0)
    {
        // measured-ness of the declared type gets adjusted from the
        // shapes themselves (all-nodata M demotes to the plain type)
        std::string adjust = "FIRST_SHAPE";
        for (const std::string &kv : openOptions)
        {
            size_t eq = kv.find('=');
            if (eq != std::string::npos &&
                strEqualNoCase(kv.substr(0, eq), "ADJUST_GEOM_TYPE"))
                adjust = kv.substr(eq + 1);
        }
        bool layerM = true;
        if (count > 0 && (strEqualNoCase(adjust, "FIRST_SHAPE") ||
                          strEqualNoCase(adjust, "ALL_SHAPES")))
        {
            bool firstOnly = strEqualNoCase(adjust, "FIRST_SHAPE");
            layerM = false;
            for (const OgrFeature &feat : lyr.features)
            {
                if (feat.hasGeom && geomMValid(feat.geom))
                {
                    layerM = true;
                    break;
                }
                if (firstOnly)
                    break;
            }
        }
        lyr.geomHasM = layerM;
        for (OgrFeature &feat : lyr.features)
            if (feat.hasGeom)
                applyM(feat.geom, layerM);
    }

    if (!prjPath.empty())
    {
        std::string prj;
        if (readFileToString(prjPath, prj))
        {
            while (!prj.empty() &&
                   (prj.back() == '\n' || prj.back() == '\r' ||
                    prj.back() == ' '))
                prj.pop_back();
            if (!prj.empty())
            {
                bool sok = false;
                Srs s = Srs::fromEsriPrj(prj, sok);
                if (sok)
                {
                    lyr.srs = std::move(s);
                    lyr.hasSrs = true;
                }
            }
        }
    }
    lyr.emitNullFields = true;
    lyr.directFidRange = true;
    return true;
}

}  // namespace

std::unique_ptr<OgrDataset> openShapefile(
    const std::string &path, std::string &err,
    const std::vector<std::string> &openOptions)
{
    bool virt = vsiIsVirtual(path);
    bool isDir = false;
    struct stat st;
    if (virt)
    {
        if (!vsiExists(path))
        {
            err = "";
            return nullptr;
        }
        isDir = vsiIsDir(path);
    }
    else
    {
        if (stat(path.c_str(), &st) != 0)
        {
            err = "";
            return nullptr;
        }
        isDir = S_ISDIR(st.st_mode);
    }
    auto ds = std::unique_ptr<OgrDataset>(new OgrDataset());
    ds->path = path;
    ds->driverShort = "ESRI Shapefile";
    ds->driverLong = "ESRI Shapefile";
    if (isDir)
    {
        std::vector<std::string> names;
        if (virt)
        {
            if (!vsiListDir(path, names))
            {
                err = "";
                return nullptr;
            }
        }
        else
        {
            DIR *dir = opendir(path.c_str());
            if (!dir)
            {
                err = "";
                return nullptr;
            }
            struct dirent *de;
            while ((de = readdir(dir)) != nullptr)
            {
                std::string n = de->d_name;
                if (n != "." && n != "..")
                    names.push_back(n);
            }
            closedir(dir);
        }
        std::set<std::string> used;
        bool anyOpenFailed = false;
        auto addLayer = [&](const std::string &fname) {
            std::string base = fname.substr(0, fname.find_last_of('.'));
            if (used.count(base))
                return;
            OgrLayer lyr;
            bool openFailed = false;
            std::string member = path + "/" + fname;
            if (readShapeLayer(member, lyr, openOptions, &openFailed))
            {
                used.insert(base);
                ds->layers.push_back(std::move(lyr));
            }
            else if (openFailed)
            {
                // the member stays claimed so its .dbf is not retried
                used.insert(base);
                anyOpenFailed = true;
                cplErrorStr(CE_Failure, CPLE_OpenFailed,
                            "Failed to open file " + member +
                                ".It may be corrupt or read-only file "
                                "accessed in update mode.");
            }
        };
        for (const std::string &n : names)
            if (n.size() > 4 &&
                (strEqualNoCase(n.substr(n.size() - 4), ".shp")))
                addLayer(n);
        for (const std::string &n : names)
            if (n.size() > 4 &&
                (strEqualNoCase(n.substr(n.size() - 4), ".dbf")))
                addLayer(n);
        if (ds->layers.empty() && !anyOpenFailed)
        {
            err = "";
            return nullptr;
        }
        ds->debugDeferred = true;
        for (const OgrLayer &l : ds->layers)
            for (const auto &n : l.debugNotes)
                ds->pendingDebug.push_back(n);
        return ds;
    }

    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos)
        ext = path.substr(dot + 1);
    if (!(strEqualNoCase(ext, "shp") || strEqualNoCase(ext, "dbf")))
    {
        err = "";
        return nullptr;
    }
    OgrLayer lyr;
    bool openFailed = false;
    if (!readShapeLayer(path, lyr, openOptions, &openFailed))
    {
        err = openFailed ? "reported" : "";
        return nullptr;
    }
    ds->layers.push_back(std::move(lyr));
    for (const auto &n : ds->layers[0].debugNotes)
        ds->pendingDebug.push_back(n);
    return ds;
}
