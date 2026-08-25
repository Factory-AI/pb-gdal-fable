#pragma once

#include <ctime>
#include <string>
#include <vector>

// Virtual file system layer: /vsimem/, /vsizip/, /vsitar/, /vsigzip/,
// /vsisubfile/, /vsistdin/, /vsistdout/ with recursive chaining.

bool vsiIsVirtual(const std::string &path);

// whole-file read; errKind: "missing", "reported", "disabled", ""
bool vsiReadWhole(const std::string &path, std::string &out,
                  std::string &errKind);
bool vsiWriteWhole(const std::string &path, const std::string &content,
                   time_t srcMtime = 0, bool zipStreaming = true);
void vsiWriteGzipProperties(const std::string &path);
bool vsiExists(const std::string &path);
bool vsiIsDir(const std::string &path);
// immediate children of a virtual directory (archive root or subdir)
bool vsiListDir(const std::string &path, std::vector<std::string> &out);
void vsiMemRemove(const std::string &path);

struct VsiPathInfo
{
    bool exists = false;
    bool isDir = false;
    unsigned long long size = 0;
    long long mtime = 0;
    unsigned mode = 0;  // permission bits when known (gzip underlying file)
};
bool vsiStat(const std::string &path, VsiPathInfo &out);

struct VsiDirEntry
{
    std::string name;
    bool isDir = false;
    unsigned long long size = 0;
    long long mtime = 0;
};
bool vsiListDirInfo(const std::string &path, std::vector<VsiDirEntry> &out);

// engine-facing wording for a dataset path that does not exist
std::string datasetMissingMessage(const std::string &path);
bool vsiRegisteredFs(const std::string &path);
// emits filesystem-specific errors for a missing input path; returns
// false when the standard missing-dataset error must be suppressed
bool vsiMissingPrelude(const std::string &path);
std::string vsiCredText(const std::string &path);
void vsiStatPrelude(const std::string &path);
void vsiCreatePrelude(const std::string &path);
std::string vsiWebhdfsHost(const std::string &path);

// ensures the underlying archive of a /vsizip/ path exists (bare EOCD)
void vsiZipTouchArchive(const std::string &vsizipPath);

// /vsizip/ write-side name check: true when the name splits (brace or
// recognized extension) so the zip write handler would engage
bool vsiZipWriteNameOk(const std::string &path);
// /vsizip/ write-name probe: 0 = unsplittable name (silent
// nonexistence), 1 = archive write path engages, 2 = archive is
// /vsistdin/ (probe consumes stdin and emits the handler's ERROR 6,
// matching the reference's read-then-refuse ordering)
int vsiZipWriteProbe(const std::string &path);
// /vsitar/ write side: true when the name splits and the underlying
// archive file is absent (drives the strerror text of Failed-to-create)
bool vsiTarWriteArchiveMissing(const std::string &path);

std::string vsiCurlHost(const std::string &path);
