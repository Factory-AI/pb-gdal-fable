#include "cpl.h"
#include "engine.h"
#include "jsonc.h"
#include "ogr.h"
#include "spec.h"
#include "util.h"
#include "vectorverbs.h"
#include "vsi.h"

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// vector set-field-type: two distinct conversion engines share the verb.
// The leaf (and any step that does not directly follow a GeoJSON read)
// translates features SetFrom-style: declared source type drives the
// coercion, with the reference's warning set. A step sitting directly
// after a GeoJSON read is absorbed into the read as a schema override:
// the raw JSON value is reinterpreted under the new declared type, with
// silent clamps and a different string form.

long long g_vectorInfoFid = -1;

namespace
{

struct TypeName
{
    const char *name;
    int type;
    int sub;
};

const TypeName kTypeNames[] = {
    {"Integer", OFTInteger, OFSTNone},
    {"Integer64", OFTInteger64, OFSTNone},
    {"Real", OFTReal, OFSTNone},
    {"String", OFTString, OFSTNone},
    {"Date", OFTDate, OFSTNone},
    {"DateTime", OFTDateTime, OFSTNone},
    {"Time", OFTTime, OFSTNone},
    {"Binary", OFTBinary, OFSTNone},
    {"IntegerList", OFTIntegerList, OFSTNone},
    {"Integer64List", OFTInteger64List, OFSTNone},
    {"RealList", OFTRealList, OFSTNone},
    {"StringList", OFTStringList, OFSTNone},
    {"Boolean", OFTInteger, OFSTBoolean},
    {"Int16", OFTInteger, OFSTInt16},
    {"Float32", OFTReal, OFSTFloat32},
    {"JSON", OFTString, OFSTJSON},
    {"UUID", OFTString, OFSTUUID},
};

JVal jvI(long long n)
{
    JVal j;
    j.type = JVal::INT;
    j.i = n;
    return j;
}

JVal jvD(double d)
{
    JVal j;
    j.type = JVal::DOUBLE;
    j.d = d;
    return j;
}

JVal jvS(std::string s)
{
    JVal j;
    j.type = JVal::STRING;
    j.s = std::move(s);
    return j;
}

std::string rawText(const JVal &v)
{
    if (v.type == JVal::STRING)
        return v.s;
    return jsoncSerialize(v, false);
}

long long sat32(long long n)
{
    if (n > 2147483647LL)
        return 2147483647LL;
    if (n < -2147483648LL)
        return -2147483648LL;
    return n;
}

// SetField(double) saturation: out-of-range values pin to the type
// bounds (a later comparison decides the lossy warning)
long long satD32(double d)
{
    if (d >= 2147483647.0)
        return 2147483647LL;
    if (d <= -2147483648.0)
        return -2147483648LL;
    return (long long)d;
}

long long satD64(double d)
{
    if (d >= 9223372036854775807.0)
        return 9223372036854775807LL;
    if (d <= -9223372036854775808.0)
        return -9223372036854775807LL - 1;
    return (long long)d;
}

// raw hardware conversion used by list-element casts: out-of-range and
// NaN produce the sign-bit value (cvttsd2si)
long long castD32(double d)
{
    if (d >= -2147483648.0 && d < 2147483648.0)
        return (long long)(int)d;
    return -2147483648LL;
}

long long castD64(double d)
{
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0)
        return (long long)d;
    return -9223372036854775807LL - 1;
}

// OGRFeatureGetIntegerValue: Boolean/Int16 range folding with warning
long long applyIntSubtype(WarnLog &log, const std::string &ref, int sub,
                          long long n)
{
    if (sub == OFSTBoolean && n != 0 && n != 1)
    {
        log.add(strPrintf("Field %s: Only 0 or 1 should be passed for a "
                          "OFSTBoolean subtype. Considering non-zero "
                          "value %lld as 1.",
                          ref.c_str(), n));
        return 1;
    }
    if (sub == OFSTInt16)
    {
        if (n < -32768)
        {
            log.add(strPrintf("Field %s: Out-of-range value for a "
                              "OFSTInt16 subtype. Considering value %lld "
                              "as -32768.",
                              ref.c_str(), n));
            return -32768;
        }
        if (n > 32767)
        {
            log.add(strPrintf("Field %s: Out-of-range value for a "
                              "OFSTInt16 subtype. Considering value %lld "
                              "as 32767.",
                              ref.c_str(), n));
            return 32767;
        }
    }
    return n;
}

std::string sftIso(const OgrDateTime &dt, int fieldType)
{
    std::string s;
    if (fieldType == OFTDate)
        return strPrintf("%04d-%02d-%02d", dt.year, dt.month, dt.day);
    if (fieldType == OFTTime)
    {
        s = strPrintf("%02d:%02d:%02d", dt.hour, dt.minute, (int)dt.sec);
        double frac = dt.sec - (int)dt.sec;
        if (frac > 0)
            s += strPrintf(".%03d", (int)(frac * 1000 + 0.5));
        return s;
    }
    s = strPrintf("%04d-%02d-%02dT%02d:%02d:%02d", dt.year, dt.month,
                  dt.day, dt.hour, dt.minute, (int)dt.sec);
    double frac = dt.sec - (int)dt.sec;
    if (frac > 0)
        s += strPrintf(".%03d", (int)(frac * 1000 + 0.5));
    if (dt.tzFlag == 100)
        s += "Z";
    else if (dt.tzFlag > 1)
    {
        int off = (dt.tzFlag - 100) * 15;
        s += strPrintf("%c%02d:%02d", off < 0 ? '-' : '+', abs(off) / 60,
                       abs(off) % 60);
    }
    return s;
}

bool subCompatible(int sub, int type)
{
    switch (sub)
    {
        case OFSTBoolean:
        case OFSTInt16:
            return type == OFTInteger || type == OFTIntegerList;
        case OFSTFloat32:
            return type == OFTReal || type == OFTRealList;
        case OFSTJSON:
            return type == OFTString || type == OFTStringList;
        case OFSTUUID:
            return type == OFTString;
        default:
            return true;
    }
}

bool isDateType(int t)
{
    return t == OFTDate || t == OFTTime || t == OFTDateTime;
}

// -------------------------------------------------------------------
// cast engine: raw JSON value reinterpreted under the new declared type

void castConvert(WarnLog &log, const std::string &ref, int dstType,
                 int dstSub, OgrFieldValue &fv)
{
    if (!fv.set || fv.v.type == JVal::NUL)
        return;
    const JVal v = fv.v;
    auto unset = [&] {
        fv.set = false;
        fv.v = JVal();
    };
    switch (dstType)
    {
        case OFTInteger:
        case OFTInteger64:
        {
            bool is32 = dstType == OFTInteger;
            long long n = 0;
            switch (v.type)
            {
                case JVal::INT:
                    n = is32 ? sat32(v.i) : v.i;
                    break;
                case JVal::BOOL:
                    n = v.b ? 1 : 0;
                    break;
                case JVal::DOUBLE:
                    n = is32 ? satD32(v.d) : satD64(v.d);
                    break;
                case JVal::STRING:
                {
                    long long ll = strtoll(v.s.c_str(), nullptr, 10);
                    n = is32 ? sat32(ll) : ll;
                    break;
                }
                default:
                    n = 0;
                    break;
            }
            if (is32)
                n = applyIntSubtype(log, ref, dstSub, n);
            fv.v = jvI(n);
            break;
        }
        case OFTReal:
        {
            double d = 0;
            switch (v.type)
            {
                case JVal::INT:
                    d = (double)v.i;
                    break;
                case JVal::BOOL:
                    d = v.b ? 1 : 0;
                    break;
                case JVal::DOUBLE:
                    d = v.d;
                    break;
                case JVal::STRING:
                    d = cplValueType(v.s) != 0
                            ? strtod(v.s.c_str(), nullptr)
                            : 0.0;
                    break;
                default:
                    d = 0.0;
                    break;
            }
            // Float32 stays stored at double precision; each output
            // channel clamps (or not) at its own boundary
            fv.v = jvD(d);
            break;
        }
        case OFTString:
            switch (v.type)
            {
                case JVal::INT:
                    fv.v = jvS(strPrintf("%lld", v.i));
                    break;
                case JVal::DOUBLE:
                    fv.v = jvS(strPrintf("%.17g", v.d));
                    break;
                case JVal::BOOL:
                    fv.v = jvS(v.b ? "true" : "false");
                    break;
                default:
                    break;  // strings, arrays and objects stay raw
            }
            break;
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
        {
            OgrDateTime dt;
            if (v.type == JVal::STRING && ogrParseDate(v.s, dt))
                fv.v = jvS(sftIso(dt, dstType));
            else
                unset();
            break;
        }
        case OFTIntegerList:
        case OFTInteger64List:
        {
            bool is32 = dstType == OFTIntegerList;
            if (v.type == JVal::ARRAY)
            {
                JVal a;
                a.type = JVal::ARRAY;
                for (const JVal &e : v.arr)
                {
                    long long n = 0;
                    switch (e.type)
                    {
                        case JVal::INT:
                            n = is32 ? sat32(e.i) : e.i;
                            break;
                        case JVal::BOOL:
                            n = e.b ? 1 : 0;
                            break;
                        case JVal::DOUBLE:
                            n = is32 ? castD32(e.d) : castD64(e.d);
                            break;
                        case JVal::STRING:
                        {
                            long long ll =
                                strtoll(e.s.c_str(), nullptr, 10);
                            n = is32 ? sat32(ll) : ll;
                            break;
                        }
                        default:
                            break;
                    }
                    a.arr.push_back(jvI(n));
                }
                fv.v = std::move(a);
            }
            else if (v.type == JVal::INT || v.type == JVal::BOOL)
            {
                long long n = v.type == JVal::BOOL
                                  ? (v.b ? 1 : 0)
                                  : (is32 ? sat32(v.i) : v.i);
                JVal a;
                a.type = JVal::ARRAY;
                a.arr.push_back(jvI(n));
                fv.v = std::move(a);
            }
            else
                unset();
            break;
        }
        case OFTRealList:
            if (v.type == JVal::ARRAY)
            {
                JVal a;
                a.type = JVal::ARRAY;
                for (const JVal &e : v.arr)
                {
                    double d = 0.0;
                    if (e.type == JVal::INT)
                        d = (double)e.i;
                    else if (e.type == JVal::DOUBLE)
                        d = e.d;
                    else if (e.type == JVal::BOOL)
                        d = e.b ? 1 : 0;
                    else if (e.type == JVal::STRING)
                        d = cplValueType(e.s) != 0
                                ? strtod(e.s.c_str(), nullptr)
                                : 0.0;
                    a.arr.push_back(jvD(d));
                }
                fv.v = std::move(a);
            }
            else if (v.type == JVal::INT || v.type == JVal::DOUBLE ||
                     v.type == JVal::BOOL)
            {
                double d = v.type == JVal::INT      ? (double)v.i
                           : v.type == JVal::DOUBLE ? v.d
                                                    : (v.b ? 1 : 0);
                JVal a;
                a.type = JVal::ARRAY;
                a.arr.push_back(jvD(d));
                fv.v = std::move(a);
            }
            else
                unset();
            break;
        case OFTStringList:
        {
            auto strForm = [](const JVal &e) -> std::string {
                switch (e.type)
                {
                    case JVal::INT:
                        return strPrintf("%lld", e.i);
                    case JVal::DOUBLE:
                        return strPrintf("%.17g", e.d);
                    case JVal::BOOL:
                        return e.b ? "true" : "false";
                    default:
                        return rawText(e);
                }
            };
            JVal a;
            a.type = JVal::ARRAY;
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    a.arr.push_back(jvS(strForm(e)));
            else
                a.arr.push_back(jvS(strForm(v)));
            fv.v = std::move(a);
            break;
        }
        case OFTBinary:
        default:
            unset();
            break;
    }
}

// -------------------------------------------------------------------
// SetFrom engine (leaf and non-absorbed steps)

void leafFromInt(WarnLog &log, const std::string &ref, long long n,
                 int dstType, int dstSub, OgrFieldValue &fv)
{
    auto unset = [&] {
        fv.set = false;
        fv.v = JVal();
    };
    switch (dstType)
    {
        case OFTInteger:
        {
            long long c = n;
            if (n > 2147483647LL || n < -2147483648LL)
            {
                c = sat32(n);
                log.add(strPrintf("Field %s: integer overflow occurred "
                                  "when trying to set %lld as 32 bit "
                                  "integer.",
                                  ref.c_str(), n));
            }
            c = applyIntSubtype(log, ref, dstSub, c);
            fv.v = jvI(c);
            break;
        }
        case OFTInteger64:
            fv.v = jvI(n);
            break;
        case OFTReal:
            fv.v = jvD((double)n);
            break;
        case OFTString:
            fv.v = jvS(strPrintf("%lld", n));
            break;
        case OFTIntegerList:
        {
            long long c = n;
            if (n > 2147483647LL || n < -2147483648LL)
            {
                c = sat32(n);
                log.add(strPrintf("Field %s: Integer overflow occurred "
                                  "when trying to set %lld as 32 bit "
                                  "value.",
                                  ref.c_str(), n));
            }
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvI(c));
            fv.v = std::move(a);
            break;
        }
        case OFTInteger64List:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvI(n));
            fv.v = std::move(a);
            break;
        }
        case OFTRealList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvD((double)n));
            fv.v = std::move(a);
            break;
        }
        case OFTStringList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvS(strPrintf("%lld", n)));
            fv.v = std::move(a);
            break;
        }
        default:  // dates and binary
            unset();
            break;
    }
}

void leafFromReal(WarnLog &log, const std::string &ref, double d,
                  int dstType, int dstSub, OgrFieldValue &fv)
{
    auto unset = [&] {
        fv.set = false;
        fv.v = JVal();
    };
    switch (dstType)
    {
        case OFTInteger:
        case OFTInteger64:
        {
            bool is32 = dstType == OFTInteger;
            long long c = is32 ? satD32(d) : satD64(d);
            long long stored =
                is32 ? applyIntSubtype(log, ref, dstSub, c) : c;
            if ((double)c != d)
                log.add(strPrintf("Field %s: Lossy conversion occurred "
                                  "when trying to set %s bit integer "
                                  "field from real value %.17g.",
                                  ref.c_str(), is32 ? "32" : "64", d));
            fv.v = jvI(stored);
            break;
        }
        case OFTReal:
            fv.v = jvD(d);
            break;
        case OFTString:
            fv.v = jvS(strPrintf("%.15g", d));
            break;
        case OFTIntegerList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvI(castD32(d)));
            fv.v = std::move(a);
            break;
        }
        case OFTInteger64List:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvI(castD64(d)));
            fv.v = std::move(a);
            break;
        }
        case OFTRealList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvD(d));
            fv.v = std::move(a);
            break;
        }
        case OFTStringList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvS(strPrintf("%.15g", d)));
            fv.v = std::move(a);
            break;
        }
        default:
            unset();
            break;
    }
}

void leafFromString(WarnLog &log, const std::string &ref,
                    const std::string &text, int dstType, int dstSub,
                    OgrFieldValue &fv)
{
    auto unset = [&] {
        fv.set = false;
        fv.v = JVal();
    };
    switch (dstType)
    {
        case OFTInteger:
        {
            if (dstSub == OFSTBoolean)
            {
                if (strEqualNoCase(text, "1") ||
                    strEqualNoCase(text, "true") ||
                    strEqualNoCase(text, "yes") ||
                    strEqualNoCase(text, "on"))
                {
                    fv.v = jvI(1);
                    break;
                }
                if (strEqualNoCase(text, "0") ||
                    strEqualNoCase(text, "false") ||
                    strEqualNoCase(text, "no") ||
                    strEqualNoCase(text, "off"))
                {
                    fv.v = jvI(0);
                    break;
                }
                bool b = cplValueType(text) != 0 &&
                         strtod(text.c_str(), nullptr) != 0;
                log.add(strPrintf("Invalid value '%s' for boolean field "
                                  "%s. Assuming it to be %s.",
                                  text.c_str(), ref.c_str(),
                                  b ? "true" : "false"));
                fv.v = jvI(b ? 1 : 0);
                break;
            }
            long long c = sat32(strtoll(text.c_str(), nullptr, 10));
            if (strPrintf("%lld", c) != text)
                log.add(strPrintf("Value '%s' of field %s parsed "
                                  "incompletely to integer %lld.",
                                  text.c_str(), ref.c_str(), c));
            c = applyIntSubtype(log, ref, dstSub, c);
            fv.v = jvI(c);
            break;
        }
        case OFTInteger64:
        {
            errno = 0;
            long long ll = strtoll(text.c_str(), nullptr, 10);
            if (errno == ERANGE)
                log.add("64 bit integer overflow when converting " +
                        text);
            fv.v = jvI(ll);
            break;
        }
        case OFTReal:
        {
            char *end = nullptr;
            double d = strtod(text.c_str(), &end);
            if (!(end && end != text.c_str() && *end == '\0'))
                log.add(strPrintf("Value '%s' of field %s parsed "
                                  "incompletely to real %g.",
                                  text.c_str(), ref.c_str(), d));
            fv.v = jvD(d);
            break;
        }
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
        {
            OgrDateTime dt;
            if (ogrParseDate(text, dt))
                fv.v = jvS(sftIso(dt, dstType));
            else
                unset();
            break;
        }
        case OFTStringList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvS(text));
            fv.v = std::move(a);
            break;
        }
        default:  // numeric lists and binary
            unset();
            break;
    }
}

void leafFromDate(WarnLog &log, const std::string &fname, int srcType,
                  const JVal &v, int dstType, OgrFieldValue &fv)
{
    auto unset = [&] {
        fv.set = false;
        fv.v = JVal();
    };
    auto cannot = [&] {
        log.addCannot("Cannot convert field '" + fname +
                      "' to new type, setting it to NULL");
        unset();
    };
    OgrDateTime dt;
    bool ok = v.type == JVal::STRING && ogrParseDate(v.s, dt);
    if (dstType == OFTString || dstType == OFTStringList)
    {
        // Date and Time render GetFieldAsString-style; DateTime keeps
        // the ISO form
        std::string s;
        if (!ok)
            s = rawText(v);
        else if (srcType == OFTDateTime)
            s = sftIso(dt, OFTDateTime);
        else
            s = ogrDateTimeToString(dt, srcType);
        if (dstType == OFTString)
            fv.v = jvS(s);
        else
        {
            JVal a;
            a.type = JVal::ARRAY;
            a.arr.push_back(jvS(s));
            fv.v = std::move(a);
        }
        return;
    }
    if (isDateType(dstType))
    {
        if (ok)
            fv.v = jvS(sftIso(dt, dstType));
        else
            unset();
        return;
    }
    cannot();
}

void leafFromNumList(WarnLog &log, const std::string &ref, int srcType,
                     const JVal &v, int dstType, int dstSub,
                     OgrFieldValue &fv)
{
    auto unset = [&] {
        fv.set = false;
        fv.v = JVal();
    };
    bool srcReal = srcType == OFTRealList;
    bool src32 = srcType == OFTIntegerList;
    auto elemInt = [&](const JVal &e) -> long long {
        return e.type == JVal::INT      ? e.i
               : e.type == JVal::BOOL   ? (e.b ? 1 : 0)
               : e.type == JVal::DOUBLE ? (long long)e.d
                                        : 0;
    };
    auto elemReal = [&](const JVal &e) -> double {
        return e.type == JVal::DOUBLE ? e.d
               : e.type == JVal::INT  ? (double)e.i
               : e.type == JVal::BOOL ? (e.b ? 1 : 0)
                                      : 0.0;
    };
    size_t count = v.type == JVal::ARRAY ? v.arr.size() : 1;
    const JVal *single =
        v.type == JVal::ARRAY ? (count == 1 ? &v.arr[0] : nullptr) : &v;
    switch (dstType)
    {
        case OFTInteger:
        case OFTInteger64:
        case OFTReal:
            if (!single)
            {
                unset();
                return;
            }
            if (srcReal)
                leafFromReal(log, ref, elemReal(*single), dstType, dstSub,
                             fv);
            else
                leafFromInt(log, ref, elemInt(*single), dstType, dstSub,
                            fv);
            return;
        case OFTString:
        {
            std::string s = strPrintf("(%d:", (int)count);
            if (v.type == JVal::ARRAY)
                for (size_t k = 0; k < v.arr.size(); ++k)
                {
                    if (k)
                        s += ',';
                    s += srcReal
                             ? strPrintf("%.16g", elemReal(v.arr[k]))
                             : strPrintf("%lld", elemInt(v.arr[k]));
                }
            s += ')';
            if (dstSub == OFSTJSON)
                fv.v = v;  // serialized JSON form survives the writer raw
            else
                fv.v = jvS(s);
            return;
        }
        case OFTIntegerList:
        case OFTInteger64List:
        case OFTRealList:
        {
            if (v.type != JVal::ARRAY || v.arr.empty())
            {
                unset();
                return;
            }
            JVal a;
            a.type = JVal::ARRAY;
            for (const JVal &e : v.arr)
            {
                if (dstType == OFTRealList)
                {
                    a.arr.push_back(jvD(elemReal(e)));
                    continue;
                }
                if (dstType == OFTInteger64List)
                {
                    a.arr.push_back(jvI(srcReal ? castD64(elemReal(e))
                                                : elemInt(e)));
                    continue;
                }
                if (srcReal)
                {
                    a.arr.push_back(jvI(castD32(elemReal(e))));
                    continue;
                }
                long long n = elemInt(e);
                if (!src32 &&
                    (n > 2147483647LL || n < -2147483648LL))
                {
                    log.add(strPrintf(
                        "Field %s: Integer overflow occurred when "
                        "trying to set %lld as 32 bit value.",
                        ref.c_str(), n));
                    n = sat32(n);
                }
                a.arr.push_back(jvI(n));
            }
            fv.v = std::move(a);
            return;
        }
        case OFTStringList:
        {
            JVal a;
            a.type = JVal::ARRAY;
            if (v.type == JVal::ARRAY)
                for (const JVal &e : v.arr)
                    a.arr.push_back(
                        jvS(srcReal
                                ? strPrintf("%.16g", elemReal(e))
                                : strPrintf("%lld", elemInt(e))));
            fv.v = std::move(a);
            return;
        }
        default:
            unset();
            return;
    }
}

}  // namespace

void leafConvert(WarnLog &log, const std::string &lyrName,
                 const OgrFieldDefn &src, int dstType, int dstSub,
                 OgrFieldValue &fv)
{
    if (!fv.set || fv.v.type == JVal::NUL)
        return;
    std::string ref = lyrName + "." + src.name;
    // same base type copies the raw union unchanged; Float32 keeps
    // double precision in storage and clamps only at output boundaries
    if (src.type == dstType)
        return;
    const JVal v = fv.v;
    auto cannot = [&] {
        log.addCannot("Cannot convert field '" + src.name +
                      "' to new type, setting it to NULL");
        fv.set = false;
        fv.v = JVal();
    };
    switch (src.type)
    {
        case OFTInteger:
        case OFTInteger64:
        {
            long long n = v.type == JVal::INT      ? v.i
                          : v.type == JVal::BOOL   ? (v.b ? 1 : 0)
                          : v.type == JVal::DOUBLE ? (long long)v.d
                                                   : atoll(v.s.c_str());
            leafFromInt(log, ref, n, dstType, dstSub, fv);
            return;
        }
        case OFTReal:
        {
            double d = v.type == JVal::DOUBLE ? v.d
                       : v.type == JVal::INT  ? (double)v.i
                       : v.type == JVal::BOOL ? (v.b ? 1 : 0)
                                              : strtod(v.s.c_str(),
                                                       nullptr);
            leafFromReal(log, ref, d, dstType, dstSub, fv);
            return;
        }
        case OFTString:
            leafFromString(log, ref, rawText(v), dstType, dstSub, fv);
            return;
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
            leafFromDate(log, src.name, src.type, v, dstType, fv);
            return;
        case OFTIntegerList:
        case OFTInteger64List:
        case OFTRealList:
            leafFromNumList(log, ref, src.type, v, dstType, dstSub, fv);
            return;
        case OFTStringList:
            if (dstType == OFTString)
            {
                if (dstSub == OFSTJSON)
                {
                    fv.v = v;
                    return;
                }
                std::string s = strPrintf(
                    "(%d:",
                    (int)(v.type == JVal::ARRAY ? v.arr.size() : 0));
                if (v.type == JVal::ARRAY)
                    for (size_t k = 0; k < v.arr.size(); ++k)
                    {
                        if (k)
                            s += ',';
                        s += rawText(v.arr[k]);
                    }
                s += ')';
                fv.v = jvS(s);
                return;
            }
            cannot();
            return;
        default:
            cannot();
            return;
    }
}

namespace
{

void recomputeTzAggr(OgrLayer &l, size_t idx)
{
    OgrFieldDefn &f = l.fields[idx];
    f.tzAggr = -1;
    for (const OgrFeature &ft : l.features)
    {
        if (idx >= ft.values.size() || !ft.values[idx].set ||
            ft.values[idx].v.type != JVal::STRING)
            continue;
        OgrDateTime dt;
        if (!ogrParseDate(ft.values[idx].v.s, dt))
            continue;
        int tz = dt.tzFlag >= 100 ? dt.tzFlag : 0;
        if (tz == 0)
            f.tzAggr = 0;  // a naive value locks the field to unknown
        else if (f.tzAggr == -1)
            f.tzAggr = tz;
        else if (f.tzAggr != 0 && f.tzAggr != tz)
            f.tzAggr = -2;
        if (f.tzAggr == 0)
            break;
    }
}

}  // namespace

bool vectorFieldTypeNameParse(const std::string &name, int &type, int &sub)
{
    for (const auto &e : kTypeNames)
        if (strEqualNoCase(name, e.name))
        {
            type = e.type;
            sub = e.sub;
            return true;
        }
    return false;
}

int vectorSetFieldTypeValidate(OgrDataset &d,
                               const VectorSetFieldTypeOpts &o,
                               bool castMode)
{
    if (!o.hasFieldName && !o.hasDstType)
        return 0;
    const OgrLayer *target = nullptr;
    if (o.hasActiveLayer)
    {
        for (const auto &l : d.layers)
            if (l.name == o.activeLayer)
            {
                target = &l;
                break;
            }
        if (!target)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        castMode
                            ? "Layer " + o.activeLayer + " not found"
                            : "Cannot find layer '" + o.activeLayer +
                                  "'");
            return 1;
        }
    }
    if (o.hasFieldName)
    {
        auto lacksField = [&](const OgrLayer &l) {
            for (const auto &f : l.fields)
                if (f.name == o.fieldName)
                    return false;
            if (!castMode)
                for (const auto &f : l.fields)
                    if (strEqualNoCase(f.name, o.fieldName))
                        return false;
            return true;
        };
        const OgrLayer *bad = nullptr;
        if (target)
        {
            if (lacksField(*target))
                bad = target;
        }
        else
            for (const auto &l : d.layers)
                if (lacksField(l))
                {
                    bad = &l;
                    break;
                }
        if (bad)
        {
            cplErrorStr(
                CE_Failure, CPLE_AppDefined,
                castMode
                    ? "Field " + o.fieldName + " not found in layer " +
                          (o.hasActiveLayer ? o.activeLayer : "*")
                    : "Cannot find field '" + o.fieldName +
                          "' in layer '" + bad->name + "'");
            return 1;
        }
        if (castMode && !o.hasDstType)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Field " + o.fieldName +
                            " has no valid overrides and schemaType is "
                            "not \"Full\"");
            return 1;
        }
    }
    else if (castMode && o.hasSrcType && o.hasDstType)
    {
        int t = 0, s = 0;
        if (vectorFieldTypeNameParse(o.srcTypeName, t, s) &&
            s != OFSTNone)
        {
            cplErrorStr(CE_Failure, CPLE_AppDefined,
                        "Unsupported source field subType: none");
            return 1;
        }
    }
    return 0;
}

void vectorSetFieldTypeConvert(OgrDataset &d,
                               const VectorSetFieldTypeOpts &o,
                               bool castMode, int emitCount,
                               long long extraFid)
{
    // the SetFrom translation rebuilds every layer, even when nothing
    // matches: default-domain metadata drops, the extent is rescanned
    struct Rebuild
    {
        OgrDataset *d = nullptr;
        ~Rebuild()
        {
            if (!d)
                return;
            for (OgrLayer &l : d->layers)
            {
                l.metadata.clear();
                vectorLayerRecomputeExtent(l);
            }
        }
    } rebuild;
    if (!castMode)
        rebuild.d = &d;
    if (!o.hasDstType)
        return;
    // the SetFrom form validates --active-layer but converts nothing;
    // the read-absorbed cast applies to the selected layer
    if (o.hasActiveLayer && !castMode)
        return;
    int dstType = 0, dstSub = 0;
    if (!vectorFieldTypeNameParse(o.dstTypeName, dstType, dstSub))
        return;
    int srcType = -1, srcSub = 0;
    bool srcMode = false;
    if (!o.hasFieldName)
    {
        if (!o.hasSrcType ||
            !vectorFieldTypeNameParse(o.srcTypeName, srcType, srcSub))
            return;
        srcMode = true;
    }
    WarnLog log;
    for (OgrLayer &l : d.layers)
    {
        if (castMode && o.hasActiveLayer && l.name != o.activeLayer)
            continue;
        std::vector<size_t> idxs;
        if (srcMode)
        {
            for (size_t i = 0; i < l.fields.size(); ++i)
                if (l.fields[i].type == srcType &&
                    l.fields[i].subType == srcSub)
                    idxs.push_back(i);
        }
        else
        {
            int idx = -1;
            for (size_t i = 0; i < l.fields.size(); ++i)
                if (l.fields[i].name == o.fieldName)
                {
                    idx = (int)i;
                    break;
                }
            if (idx < 0 && !castMode)
                for (size_t i = 0; i < l.fields.size(); ++i)
                    if (strEqualNoCase(l.fields[i].name, o.fieldName))
                    {
                        idx = (int)i;
                        break;
                    }
            if (idx < 0)
                continue;
            idxs.push_back((size_t)idx);
        }
        if (idxs.empty())
            continue;
        size_t kept = 0;
        for (size_t fi = 0; fi < l.features.size(); ++fi)
        {
            OgrFeature &ft = l.features[fi];
            log.fid = ft.fid;
            log.drop = false;
            for (size_t idx : idxs)
            {
                if (idx >= ft.values.size())
                    continue;
                std::string ref = l.name + "." + l.fields[idx].name;
                if (castMode)
                    castConvert(log, ref, dstType, dstSub,
                                ft.values[idx]);
                else
                    leafConvert(log, l.name, l.fields[idx], dstType,
                                dstSub, ft.values[idx]);
            }
            if (!log.drop)
            {
                if (kept != fi)
                    l.features[kept] = std::move(ft);
                ++kept;
            }
        }
        if (kept != l.features.size())
            l.features.resize(kept);
        for (size_t idx : idxs)
        {
            OgrFieldDefn &f = l.fields[idx];
            bool changed = f.type != dstType;
            if (castMode && f.subType != OFSTNone && changed &&
                !subCompatible(f.subType, dstType))
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Type and subtype of field definition are "
                            "not compatible. Resetting to OFSTNone");
            // the SetFrom translation clones the definition (width and
            // precision survive the retype)
            if (changed)
                f.tzAggr = -1;
            f.type = dstType;
            f.subType = dstSub;
            if (changed && dstType == OFTDateTime)
                recomputeTzAggr(l, idx);
        }
    }
    for (int k = 0; k < emitCount; ++k)
        for (const auto &e : log.msgs)
            cplErrorStr(CE_Warning, CPLE_AppDefined, e.second);
    if (extraFid >= 0)
        for (const auto &e : log.msgs)
            if (e.first == extraFid)
                cplErrorStr(CE_Warning, CPLE_AppDefined, e.second);
}

void vectorSetFieldTypeCastGroup(
    OgrDataset &d, const std::vector<VectorSetFieldTypeOpts> &group,
    int emitCount, long long extraFid)
{
    WarnLog log;
    for (OgrLayer &l : d.layers)
    {
        size_t nf = l.fields.size();
        std::vector<int> curT(nf), curS(nf);
        std::vector<char> ov(nf, 0);
        for (size_t i = 0; i < nf; ++i)
        {
            curT[i] = l.fields[i].type;
            curS[i] = l.fields[i].subType;
        }
        // each step matches against the types as overridden so far;
        // the final type per field wins over the raw value
        for (const auto &o : group)
        {
            if (!o.hasDstType)
                continue;
            if (o.hasActiveLayer && l.name != o.activeLayer)
                continue;
            int dstType = 0, dstSub = 0;
            if (!vectorFieldTypeNameParse(o.dstTypeName, dstType,
                                          dstSub))
                continue;
            if (o.hasFieldName)
            {
                for (size_t i = 0; i < nf; ++i)
                    if (l.fields[i].name == o.fieldName)
                    {
                        curT[i] = dstType;
                        curS[i] = dstSub;
                        ov[i] = 1;
                        break;
                    }
            }
            else if (o.hasSrcType)
            {
                int st = 0, ss = 0;
                if (!vectorFieldTypeNameParse(o.srcTypeName, st, ss))
                    continue;
                for (size_t i = 0; i < nf; ++i)
                    if (curT[i] == st && curS[i] == ss)
                    {
                        curT[i] = dstType;
                        curS[i] = dstSub;
                        ov[i] = 1;
                    }
            }
        }
        std::vector<size_t> idxs;
        for (size_t i = 0; i < nf; ++i)
            if (ov[i])
                idxs.push_back(i);
        if (idxs.empty())
            continue;
        for (OgrFeature &ft : l.features)
        {
            log.fid = ft.fid;
            for (size_t i : idxs)
            {
                if (i >= ft.values.size())
                    continue;
                std::string ref = l.name + "." + l.fields[i].name;
                castConvert(log, ref, curT[i], curS[i], ft.values[i]);
            }
        }
        for (size_t i : idxs)
        {
            OgrFieldDefn &f = l.fields[i];
            bool changed = f.type != curT[i];
            if (f.subType != OFSTNone && changed &&
                !subCompatible(f.subType, curT[i]))
                cplErrorStr(CE_Warning, CPLE_AppDefined,
                            "Type and subtype of field definition are "
                            "not compatible. Resetting to OFSTNone");
            if (changed)
            {
                f.width = 0;
                f.precision = 0;
                f.tzAggr = -1;
            }
            f.type = curT[i];
            f.subType = curS[i];
            if (changed && curT[i] == OFTDateTime)
                recomputeTzAggr(l, i);
        }
    }
    for (int k = 0; k < emitCount; ++k)
        for (const auto &e : log.msgs)
            cplErrorStr(CE_Warning, CPLE_AppDefined, e.second);
    if (extraFid >= 0)
        for (const auto &e : log.msgs)
            if (e.first == extraFid)
                cplErrorStr(CE_Warning, CPLE_AppDefined, e.second);
}

bool g_vectorSftAllCast = false;

namespace
{

int vectorSetFieldTypeHandler(const CmdSpec &cmd, ParseResult &r)
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
    if (vvOpenInputDsNoUsage(cmd, r, input, ds))
    {
        // the reference's set-field-type leaf dereferences the failed
        // open: error text without usage, then SIGSEGV
        fflush(stdout);
        fflush(stderr);
        raise(SIGSEGV);
        return 1;
    }

    VectorSetFieldTypeOpts o;
    const ArgValue *a;
    o.hasFieldName = (a = r.get("field-name")) && a->set;
    o.fieldName = r.str("field-name");
    o.hasSrcType = (a = r.get("src-field-type")) && a->set;
    o.srcTypeName = r.str("src-field-type");
    o.hasDstType = (a = r.get("field-type")) && a->set;
    o.dstTypeName = r.str("field-type");
    o.activeLayer = r.str("active-layer");
    o.hasActiveLayer = !o.activeLayer.empty();

    std::string cli = vvGdalgHead(r, input, true, true);
    if (o.hasActiveLayer)
        cli += " --active-layer " + vvGq(o.activeLayer);
    if (o.hasFieldName)
        cli += " --field-name " + vvGq(o.fieldName);
    else if (o.hasSrcType)
        cli += " --src-field-type " + vvGq(o.srcTypeName);
    if (o.hasDstType)
        cli += " --field-type " + vvGq(o.dstTypeName);
    cli += " --output-format stream --output streamed_dataset";

    g_convertDatasetPreCheck = [o](OgrDataset &dd) -> int {
        return vectorSetFieldTypeValidate(dd, o, false);
    };
    auto mutate = [o](OgrDataset &dd) -> int {
        vectorSetFieldTypeConvert(dd, o, false, g_convertWritePulls, -1);
        return 0;
    };
    int rc = vvDelegateVerb(r, cmd.name, std::move(ds), cli, driver, true,
                            mutate);
    g_convertDatasetPreCheck = nullptr;
    return rc;
}

}  // namespace

void registerVectorSetFieldTypeHandler()
{
    registerHandler("vector_set-field-type", vectorSetFieldTypeHandler);
}
