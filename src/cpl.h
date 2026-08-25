#pragma once
#include <map>
#include <string>

enum CPLErrClass
{
    CE_None = 0,
    CE_Debug = 1,
    CE_Warning = 2,
    CE_Failure = 3,
    CE_Fatal = 4
};

// CPLE_* error numbers
enum
{
    CPLE_None = 0,
    CPLE_AppDefined = 1,
    CPLE_OutOfMemory = 2,
    CPLE_FileIO = 3,
    CPLE_OpenFailed = 4,
    CPLE_IllegalArg = 5,
    CPLE_NotSupported = 6,
    CPLE_AssertionFailed = 7,
    CPLE_NoWriteAccess = 8,
    CPLE_UserInterrupt = 9,
    CPLE_ObjectNull = 10,
    CPLE_HttpResponse = 11,
    CPLE_AWSBucketNotFound = 12,
    CPLE_AWSObjectNotFound = 13,
    CPLE_AWSAccessDenied = 14,
    CPLE_AWSInvalidCredentials = 15,
    CPLE_AWSSignatureDoesNotMatch = 16,
    CPLE_AWSError = 17
};

void cplPushQuietHandler(bool muteDebug = true);
void cplPopHandler();
bool cplQuietActive();
// diagnostics that outlive quiet-wrapped opens (lazy SRS decode replays):
// suspend the whole quiet stack, emit, then restore
int cplSuspendQuiet();
void cplRestoreQuiet(int depth);

void cplError(CPLErrClass eclass, int num, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void cplErrorStr(CPLErrClass eclass, int num, const std::string &msg);

std::string cplGetLastErrorMsg();
int cplGetLastErrorNo();
void cplResetLastError();
CPLErrClass cplGetLastErrorType();
long cplErrorSeq();

// Config options (from --config and environment)
void configSet(const std::string &key, const std::string &value);
std::string configGet(const std::string &key, const std::string &def = "");
bool configIsSet(const std::string &key);
bool configTestBool(const std::string &key, bool def);

// CPL_DEBUG-driven traces ("CATEGORY: message" on stderr); suppressed
// under quiet handlers like every other handler-routed message
bool cplDebugEnabled(const std::string &category);
void cplDebug(const std::string &category, const std::string &msg);
// stand-in for the heap addresses the reference prints in debug traces
std::string cplDebugPtr();

// GDAL_SKIP driver unregistration
bool gdalSkipHas(const std::string &driverName);
void gdalSkipStartupWarnings();

// 5% of usable physical RAM (cgroup/rlimit aware), floored like
// GDALGetCacheMax64's percentage math
long long gdalDefaultCacheMax();
// "GDAL: GDAL_CACHEMAX = N MB" on the first block-cache use
void gdalDebugCacheMaxOnce();

// GDAL_PAM_ENABLED gate; emits the once-per-process disabled debug note
bool gdalPamEnabled();
