#include "cpl.h"
#include "engine.h"
#include "jsonwriter.h"
#include "progress.h"
#include "util.h"
#include "vsi.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace
{

struct Entry
{
    std::string display;
    struct stat st;
    bool statOk = true;
    bool zeroMtime = false;
    int depth = 1;
    std::vector<Entry> children;  // used for tree output
};

bool statPath(const std::string &p, struct stat &st)
{
    vsiStatPrelude(p);
    if (vsiIsVirtual(p))
    {
        // the network handler never answers here, unlike the dataset
        // open path where the empty read yields "not recognized"
        if (strStartsWith(p, "/vsiwebhdfs/"))
            return false;
        VsiPathInfo pi;
        if (!vsiStat(p, pi))
            return false;
        memset(&st, 0, sizeof(st));
        st.st_mode = (pi.isDir ? S_IFDIR : S_IFREG) | pi.mode;
        st.st_size = (off_t)pi.size;
        st.st_mtime = (time_t)pi.mtime;
        return true;
    }
    return stat(p.c_str(), &st) == 0;
}

std::string modeString(const struct stat &st)
{
    char m[11];
    m[0] = S_ISDIR(st.st_mode) ? 'd'
           : S_ISLNK(st.st_mode) ? 'l'
           : S_ISCHR(st.st_mode) ? 'c'
           : S_ISBLK(st.st_mode) ? 'b'
           : S_ISFIFO(st.st_mode) ? 'p'
           : S_ISSOCK(st.st_mode) ? 's'
                                   : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i)
        m[1 + i] = (st.st_mode & (1 << (8 - i))) ? rwx[i] : '-';
    m[10] = 0;
    return m;
}

bool isCurlPath(const std::string &p)
{
    return strncmp(p.c_str(), "/vsicurl/", 9) == 0;
}

std::string curlAccessMsg(const char *verb, const std::string &p)
{
    return std::string(verb) + ": '" + p +
           "' cannot be accessed. HttpError: CURL error: Could not "
           "resolve host: " +
           vsiCurlHost(p);
}

// composed failure line for filesystems that surface an access detail;
// empty when the generic wording applies
std::string accessFailMsg(const char *verb, const std::string &p,
                          bool readAttempt)
{
    std::string det = vsiCredText(p);
    if (det.empty() && readAttempt && strStartsWith(p, "/vsiwebhdfs/"))
        det = "HttpError: CURL error: Could not resolve host: " +
              vsiWebhdfsHost(p);
    if (det.empty())
        return "";
    return std::string(verb) + ": '" + p + "' cannot be accessed. " + det;
}

std::string absolutize(const std::string &p)
{
    if (!p.empty() && p[0] == '/')
        return p;
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
        return p;
    if (p == "." || p.empty())
        return cwd;
    return std::string(cwd) + "/" + p;
}

void listDir(const std::string &dir, const std::string &prefix, int depth,
             bool recursive, long long maxDepth, std::vector<Entry> &out)
{
    if (vsiIsVirtual(dir))
    {
        std::vector<VsiDirEntry> es;
        if (!vsiListDirInfo(dir, es))
            return;
        for (const auto &ve : es)
        {
            Entry e;
            e.display = prefix + ve.name;
            e.depth = depth;
            memset(&e.st, 0, sizeof(e.st));
            e.st.st_mode = ve.isDir ? S_IFDIR : S_IFREG;
            e.st.st_size = (off_t)ve.size;
            e.st.st_mtime = (time_t)ve.mtime;
            out.push_back(e);
            if (recursive && ve.isDir && (maxDepth <= 0 || depth < maxDepth))
                listDir(dir + "/" + ve.name, e.display + "/", depth + 1,
                        recursive, maxDepth, out);
        }
        return;
    }
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr)
    {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        Entry e;
        e.display = prefix + de->d_name;
        e.depth = depth;
        std::string full = dir + "/" + de->d_name;
        e.statOk = statPath(full, e.st);
        out.push_back(e);
        if (recursive && out.back().statOk && S_ISDIR(out.back().st.st_mode) &&
            (maxDepth <= 0 || depth < maxDepth))
        {
            listDir(full, e.display + "/", depth + 1, recursive, maxDepth,
                    out);
        }
    }
    closedir(d);
}

std::string fmtDateText(time_t t)
{
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return strPrintf("%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900,
                     tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

std::string fmtDateJson(time_t t)
{
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return strPrintf("%04d-%02d-%02d %02d:%02d:%02dZ", tmv.tm_year + 1900,
                     tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
                     tmv.tm_sec);
}

void writeLongEntry(JsonStreamWriter &w, const Entry &e, bool withEntries,
                    const std::vector<Entry> *children);

void writeTreeChildren(JsonStreamWriter &w, const std::vector<Entry> &entries,
                       bool longListing)
{
    for (const auto &e : entries)
    {
        bool isDir = e.statOk && S_ISDIR(e.st.st_mode);
        if (longListing)
            writeLongEntry(w, e, isDir, &e.children);
        else if (isDir)
        {
            w.startObject();
            w.addKey("name");
            w.addString(e.display);
            w.addKey("entries");
            w.startArray();
            writeTreeChildren(w, e.children, longListing);
            w.endArray();
            w.endObject();
        }
        else
            w.addString(e.display);
    }
}

void writeLongEntry(JsonStreamWriter &w, const Entry &e, bool withEntries,
                    const std::vector<Entry> *children)
{
    w.startObject();
    w.addKey("name");
    w.addString(e.display);
    w.addKey("type");
    bool isDir = e.statOk && S_ISDIR(e.st.st_mode);
    w.addString(isDir ? "directory" : "file");
    w.addKey("size");
    w.addInt(e.statOk ? static_cast<long long>(e.st.st_size) : 0);
    time_t mt = e.zeroMtime || !e.statOk ? 0 : e.st.st_mtime;
    if (mt != 0)
    {
        w.addKey("last_modification_date");
        w.addString(fmtDateJson(mt));
    }
    w.addKey("permissions");
    w.addString(e.statOk ? modeString(e.st) : "");
    if (withEntries && children)
    {
        w.addKey("entries");
        w.startArray();
        writeTreeChildren(w, *children, true);
        w.endArray();
    }
    w.endObject();
}

// Rebuild flat entries into a nested tree (children by path prefix).
std::vector<Entry> nestEntries(const std::vector<Entry> &flat)
{
    std::vector<Entry> roots;
    std::vector<Entry *> stack;
    for (const auto &e : flat)
    {
        Entry copy = e;
        size_t slash = copy.display.rfind('/');
        std::string shortName = slash == std::string::npos
                                    ? copy.display
                                    : copy.display.substr(slash + 1);
        copy.display = shortName;
        while (static_cast<int>(stack.size()) >= e.depth)
            stack.pop_back();
        Entry *inserted;
        if (stack.empty())
        {
            roots.push_back(copy);
            inserted = &roots.back();
        }
        else
        {
            stack.back()->children.push_back(copy);
            inserted = &stack.back()->children.back();
        }
        if (e.statOk && S_ISDIR(e.st.st_mode))
            stack.push_back(inserted);
    }
    return roots;
}

int vsiList(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string filename = r.str("filename");
    std::string format = r.str("output-format", "text");
    bool longListing = r.flag("long-listing");
    bool recursive = r.flag("recursive");
    bool absPath = r.flag("absolute-path");
    bool tree = r.flag("tree");
    long long depth = 0;
    if (r.get("depth"))
        depth = strtoll(r.str("depth").c_str(), nullptr, 10);

    struct stat st;
    if (!statPath(filename, st))
    {
        std::string m = accessFailMsg("list", filename, true);
        if (isCurlPath(filename))
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        curlAccessMsg("list", filename));
        else if (!m.empty())
            cplErrorStr(CE_Failure, CPLE_FileIO, m);
        else
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "list: '" + filename +
                            "' does not exist or cannot be accessed");
        return 1;
    }

    vsiWriteGzipProperties(filename);
    std::vector<Entry> entries;
    bool singleFile = !S_ISDIR(st.st_mode);
    if (singleFile)
    {
        Entry e;
        e.display = filename;
        e.st = st;
        e.zeroMtime = true;
        entries.push_back(e);
    }
    else
    {
        listDir(filename, "", 1, recursive, depth, entries);
    }

    if (absPath)
    {
        std::string base = absolutize(filename);
        for (auto &e : entries)
            e.display = base + "/" + e.display;
    }

    std::string out;
    if (format == "text")
    {
        for (const auto &e : entries)
        {
            if (longListing)
            {
                time_t mt = e.zeroMtime ? 0 : e.st.st_mtime;
                out += strPrintf(
                    "%s 1 unknown unknown %12llu %s %s\n",
                    modeString(e.st).c_str(),
                    static_cast<unsigned long long>(e.st.st_size),
                    fmtDateText(mt).c_str(), e.display.c_str());
            }
            else
                out += e.display + "\n";
        }
        fwrite(out.data(), 1, out.size(), stdout);
    }
    else
    {
        JsonStreamWriter w;
        if (singleFile)
        {
            if (longListing)
                writeLongEntry(w, entries[0], false, nullptr);
            else
                w.addString(entries[0].display);
        }
        else
        {
            w.startArray();
            if (tree)
            {
                std::vector<Entry> nested = nestEntries(entries);
                writeTreeChildren(w, nested, longListing);
            }
            else
            {
                for (const auto &e : entries)
                {
                    if (longListing)
                        writeLongEntry(w, e, false, nullptr);
                    else
                        w.addString(e.display);
                }
            }
            w.endArray();
        }
        std::string s = w.result();
        fwrite(s.data(), 1, s.size(), stdout);
    }
    fflush(stdout);
    return 0;
}

std::string baseName(const std::string &p)
{
    std::string q = p;
    while (q.size() > 1 && q.back() == '/')
        q.pop_back();
    size_t slash = q.rfind('/');
    return slash == std::string::npos ? q : q.substr(slash + 1);
}

bool copyFileRaw(const std::string &src, const std::string &dst,
                 unsigned long long expected, unsigned long long &copied,
                 bool *createFailed = nullptr)
{
    if (vsiIsVirtual(src) || vsiIsVirtual(dst))
    {
        copied = 0;
        std::string content, errKind;
        if (vsiIsVirtual(src))
        {
            if (!vsiReadWhole(src, content, errKind))
                return false;
        }
        else if (!readFileToString(src, content))
            return false;
        if (vsiIsVirtual(dst))
        {
            time_t srcMtime = 0;
            struct stat sst;
            if (!vsiIsVirtual(src) && stat(src.c_str(), &sst) == 0)
                srcMtime = sst.st_mtime;
            if (!vsiWriteWhole(dst, content, srcMtime, false))
            {
                if (createFailed)
                    *createFailed = true;
                return false;
            }
        }
        else if (!writeStringToFile(dst, content))
        {
            if (createFailed)
                *createFailed = true;
            return false;
        }
        copied = content.size();
        return copied == expected;
    }
    FILE *in = fopen(src.c_str(), "rb");
    if (!in)
        return false;
    FILE *outF = fopen(dst.c_str(), "wb");
    if (!outF)
    {
        if (createFailed)
            *createFailed = true;
        fclose(in);
        return false;
    }
    char buf[262144];
    size_t n;
    copied = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
        if (fwrite(buf, 1, n, outF) != n)
            break;
        copied += n;
    }
    fclose(in);
    fclose(outF);
    return copied == expected;
}

struct RecCopyProgress
{
    TermProgress *tp = nullptr;
    unsigned long long total = 0;
    unsigned long long acc = 0;
    void tick()
    {
        if (tp)
            tp->update(total ? static_cast<double>(acc) /
                                   static_cast<double>(total)
                             : 0.0);
    }
};

// files are charged size+1 and directories their raw stat size into the
// denominator, while a processed directory only advances the numerator by
// 1 (and an empty file never emits a callback), so any subdirectory keeps
// the bar from completing while a flat all-nonempty tree ends at exactly
// 100%
void recCopyScanTotal(const std::string &src, unsigned long long &total,
                      bool &anyEntry)
{
    std::vector<Entry> entries;
    listDir(src, "", 1, true, 0, entries);
    for (const auto &e : entries)
    {
        anyEntry = true;
        if (!e.statOk)
        {
            total += 1;
            continue;
        }
        total += static_cast<unsigned long long>(e.st.st_size);
        if (!S_ISDIR(e.st.st_mode))
            total += 1;
    }
}

bool copyRecursive(const std::string &src, const std::string &dst,
                   RecCopyProgress *prog = nullptr, bool root = true)
{
    struct stat st;
    if (!statPath(src, st))
        return false;
    if (S_ISDIR(st.st_mode))
    {
        if (mkdir(dst.c_str(), 0755) != 0 && errno != EEXIST)
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "copy: Cannot create directory " + dst);
            return false;
        }
        if (!root && prog)
            prog->acc += 1;
        DIR *d = opendir(src.c_str());
        if (!d)
            return false;
        struct dirent *de;
        bool ok = true;
        while (ok && (de = readdir(d)) != nullptr)
        {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            ok = copyRecursive(src + "/" + de->d_name,
                               dst + "/" + de->d_name, prog, false);
        }
        closedir(d);
        return ok;
    }
    unsigned long long copied = 0;
    if (!copyFileRaw(src, dst, static_cast<unsigned long long>(st.st_size),
                     copied))
    {
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    strPrintf("Copying of %s to %s failed: %llu bytes were "
                              "copied whereas %llu were expected",
                              src.c_str(), dst.c_str(), copied,
                              static_cast<unsigned long long>(st.st_size)));
        unlink(dst.c_str());
        return false;
    }
    if (prog)
    {
        prog->acc += static_cast<unsigned long long>(st.st_size) + 1;
        if (st.st_size > 0)
            prog->tick();
    }
    return true;
}

int vsiCopy(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string src = r.str("source");
    std::string dst = r.str("destination");
    bool recursive = r.flag("recursive");
    bool quiet = r.flag("quiet");

    struct stat st;
    if (!statPath(src, st))
    {
        std::string m = accessFailMsg("copy", src, true);
        if (isCurlPath(src))
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        curlAccessMsg("copy", src));
        else if (!m.empty())
            cplErrorStr(CE_Failure, CPLE_FileIO, m);
        else
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "copy: '" + src + "' cannot be accessed.");
        return 1;
    }
    vsiWriteGzipProperties(src);
    if (S_ISDIR(st.st_mode) && !recursive)
    {
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    "copy: " + src + " is a directory. Use -r/--recursive "
                                     "option");
        return 1;
    }

    struct stat dstSt;
    bool dstIsDir = statPath(dst, dstSt) && S_ISDIR(dstSt.st_mode);

    if (S_ISDIR(st.st_mode))
    {
        TermProgress progress;
        RecCopyProgress prog;
        bool anyEntry = false;
        recCopyScanTotal(src, prog.total, anyEntry);
        if (!quiet && anyEntry)
        {
            prog.tp = &progress;
            progress.update(0.0);
        }
        std::string target = dstIsDir ? dst + "/" + baseName(src) : dst;
        if (!copyRecursive(src, target, &prog))
            return 1;
        return 0;
    }

    std::string target = dstIsDir ? dst + "/" + baseName(src) : dst;
    TermProgress progress;
    unsigned long long copied = 0;
    bool createFailed = false;
    if (!copyFileRaw(src, target,
                     static_cast<unsigned long long>(st.st_size), copied,
                     &createFailed))
    {
        if (createFailed)
        {
            vsiCreatePrelude(target);
            // the zip handler fails write-opens silently (unsplittable
            // name or underlying refusal already reported)
            if (!(target.size() >= 8 &&
                  strEqualNoCase(target.substr(0, 8), "/vsizip/")))
                cplErrorStr(CE_Failure, CPLE_FileIO,
                            "Cannot create " + target);
            return 1;
        }
        if (!quiet && copied > 0 && st.st_size > 0)
            progress.update(static_cast<double>(copied) /
                            static_cast<double>(st.st_size));
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    strPrintf("Copying of %s to %s failed: %llu bytes were "
                              "copied whereas %llu were expected",
                              src.c_str(), target.c_str(), copied,
                              static_cast<unsigned long long>(st.st_size)));
        unlink(target.c_str());
        return 1;
    }
    if (!quiet && st.st_size > 0)
        progress.update(1.0);
    return 0;
}

bool deleteRecursive(const std::string &p)
{
    struct stat st;
    if (lstat(p.c_str(), &st) != 0)
        return false;
    if (S_ISDIR(st.st_mode))
    {
        DIR *d = opendir(p.c_str());
        if (!d)
            return false;
        struct dirent *de;
        bool ok = true;
        while (ok && (de = readdir(d)) != nullptr)
        {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            ok = deleteRecursive(p + "/" + de->d_name);
        }
        closedir(d);
        return ok && rmdir(p.c_str()) == 0;
    }
    return unlink(p.c_str()) == 0;
}

int vsiDelete(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string filename = r.str("filename");
    bool recursive = r.flag("recursive");

    struct stat st;
    if (!statPath(filename, st))
    {
        std::string m = accessFailMsg("delete", filename, true);
        if (isCurlPath(filename))
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        curlAccessMsg("delete", filename));
        else if (!m.empty())
            cplErrorStr(CE_Failure, CPLE_FileIO, m);
        else
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "delete: '" + filename +
                            "' does not exist or cannot be accessed");
        return 1;
    }
    bool ok;
    if (vsiIsVirtual(filename))
    {
        if (strEqualNoCase(filename.substr(0, 8), "/vsimem/"))
        {
            if (S_ISDIR(st.st_mode))
            {
                std::vector<std::string> kids;
                vsiListDir(filename, kids);
                std::string pfx = filename;
                if (pfx.back() != '/')
                    pfx += '/';
                if (recursive)
                    for (const auto &k : kids)
                        vsiMemRemove(pfx + k);
            }
            else
                vsiMemRemove(filename);
            return 0;
        }
        ok = false;
    }
    else if (S_ISDIR(st.st_mode))
    {
        if (recursive)
            ok = deleteRecursive(filename);
        else
            ok = rmdir(filename.c_str()) == 0;
    }
    else
        ok = unlink(filename.c_str()) == 0;
    if (!ok)
    {
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    "delete: Cannot delete " + filename);
        return 1;
    }
    return 0;
}

int vsiMove(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string src = r.str("source");
    std::string dst = r.str("destination");
    bool quiet = r.flag("quiet");

    struct stat st;
    if (!statPath(src, st))
    {
        struct stat st2;
        statPath(src, st2);
        std::string m = accessFailMsg("move", src, false);
        if (isCurlPath(src))
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        curlAccessMsg("move", src));
        else if (!m.empty())
            cplErrorStr(CE_Failure, CPLE_FileIO, m);
        else
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "move: '" + src +
                            "' does not exist or cannot be accessed");
        return 1;
    }
    struct stat dstSt;
    bool dstIsDir = statPath(dst, dstSt) && S_ISDIR(dstSt.st_mode);
    std::string target = dstIsDir ? dst + "/" + baseName(src) : dst;

    vsiWriteGzipProperties(src);
    TermProgress progress;
    if (vsiIsVirtual(src) || vsiIsVirtual(target))
    {
        // cross-filesystem move: copy, then remove the source
        unsigned long long copied = 0;
        bool createFailed = false;
        if (!copyFileRaw(src, target,
                         static_cast<unsigned long long>(st.st_size),
                         copied, &createFailed))
        {
            if (createFailed)
            {
                vsiCreatePrelude(target);
                if (!(target.size() >= 8 &&
                      strEqualNoCase(target.substr(0, 8), "/vsizip/")))
                    cplErrorStr(CE_Failure, CPLE_FileIO,
                                "Cannot create " + target);
            }
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "move: " + src + " could not be moved to " + target);
            return 1;
        }
        if (!quiet)
            progress.update(1.0);
        bool removed;
        if (!vsiIsVirtual(src))
            removed = unlink(src.c_str()) == 0;
        else if (strEqualNoCase(src.substr(0, 8), "/vsimem/"))
        {
            vsiMemRemove(src);
            removed = true;
        }
        else
            removed = false;
        if (!removed)
        {
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "move: " + src + " could not be moved to " + target);
            return 1;
        }
        return 0;
    }
    if (rename(src.c_str(), target.c_str()) != 0)
    {
        cplErrorStr(CE_Failure, CPLE_FileIO,
                    "move: " + src + " could not be moved to " + target);
        return 1;
    }
    if (!quiet)
        progress.update(1.0);
    return 0;
}

int vsiSync(const CmdSpec &cmd, ParseResult &r)
{
    (void)cmd;
    std::string src = r.str("source");
    std::string dst = r.str("destination");
    bool recursive = r.flag("recursive");
    bool quiet = r.flag("quiet");

    struct stat st;
    if (!statPath(src, st))
    {
        cplErrorStr(CE_Failure, CPLE_FileIO, src + " does not exist");
        struct stat st2;
        statPath(src, st2);
        std::string m = accessFailMsg("sync", src, false);
        if (isCurlPath(src))
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        curlAccessMsg("sync", src));
        else if (!m.empty())
            cplErrorStr(CE_Failure, CPLE_FileIO, m);
        else
            cplErrorStr(CE_Failure, CPLE_FileIO,
                        "sync: '" + src +
                            "' does not exist or cannot be accessed");
        return 1;
    }

    vsiWriteGzipProperties(src);
    TermProgress progress;
    if (S_ISDIR(st.st_mode))
    {
        bool srcTrailingSlash = !src.empty() && src.back() == '/';
        std::string base = dst;
        struct stat dstSt;
        if (!statPath(dst, dstSt))
            mkdir(dst.c_str(), 0755);
        if (!srcTrailingSlash)
        {
            base = dst + "/" + baseName(src);
            if (!statPath(base, dstSt))
                mkdir(base.c_str(), 0755);
        }

        // without --recursive only one level is synchronized;
        // subdirectories are created but their contents are not copied.
        // The bar ticks after every entry whether or not it was copied.
        // breadth-first enumeration: a directory's children land at the
        // END of the list (pinned by the bar's final tick position)
        std::vector<std::string> names;
        auto listInto = [&](const std::string &rel)
        {
            std::string dir = rel.empty() ? src : src + "/" + rel;
            DIR *d = opendir(dir.c_str());
            if (!d)
                return;
            struct dirent *de;
            while ((de = readdir(d)) != nullptr)
            {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                names.push_back(rel.empty() ? de->d_name
                                            : rel + "/" + de->d_name);
            }
            closedir(d);
        };
        listInto("");
        if (recursive)
            for (size_t i = 0; i < names.size(); ++i)
            {
                struct stat cs;
                if (statPath(src + "/" + names[i], cs) &&
                    S_ISDIR(cs.st_mode))
                    listInto(names[i]);
            }
        for (size_t i = 0; i < names.size(); ++i)
        {
            std::string s = src + "/" + names[i];
            std::string t = base + "/" + names[i];
            struct stat ss;
            if (!statPath(s, ss))
                continue;
            struct stat ts;
            bool exists = statPath(t, ts);
            if (S_ISDIR(ss.st_mode))
            {
                if (!exists)
                    mkdir(t.c_str(), 0755);
                continue;  // directory entries never tick the bar
            }
            // skipped only when size AND mtime match exactly; a copied
            // file ticks the bar at its enumeration index
            if (!exists || ts.st_size != ss.st_size ||
                ts.st_mtime != ss.st_mtime)
            {
                unsigned long long copied = 0;
                copyFileRaw(s, t,
                            static_cast<unsigned long long>(ss.st_size),
                            copied);
                if (!quiet)
                    progress.update(static_cast<double>(i + 1) /
                                    static_cast<double>(names.size()));
            }
        }
    }
    else
    {
        struct stat dstSt;
        bool dstStatOk = statPath(dst, dstSt);
        bool dstIsDir = dstStatOk && S_ISDIR(dstSt.st_mode);
        std::string t = dstIsDir ? dst + "/" + baseName(src) : dst;
        struct stat ts;
        bool exists;
        if (dstIsDir)
            exists = statPath(t, ts);
        else
        {
            ts = dstSt;
            exists = dstStatOk;
        }
        if (!exists || ts.st_size != st.st_size ||
            ts.st_mtime != st.st_mtime)
        {
            unsigned long long copied = 0;
            bool createFailed = false;
            if (!copyFileRaw(src, t,
                             static_cast<unsigned long long>(st.st_size),
                             copied, &createFailed))
            {
                if (createFailed)
                {
                    vsiCreatePrelude(t);
                    cplErrorStr(CE_Failure, CPLE_FileIO,
                                "Cannot create " + t);
                }
                cplErrorStr(CE_Failure, CPLE_FileIO,
                            "sync: " + src +
                                " could not be synchronised with " + t);
                return 1;
            }
            if (!quiet)
                progress.update(1.0);
        }
    }
    return 0;
}

}  // namespace

void registerVsiHandlers()
{
    registerHandler("vsi_list", vsiList);
    registerHandler("vsi_copy", vsiCopy);
    registerHandler("vsi_delete", vsiDelete);
    registerHandler("vsi_move", vsiMove);
    registerHandler("vsi_sync", vsiSync);
}
