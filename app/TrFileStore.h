// =============================================================================
// SuVtitle - app/TrFileStore.h   (the cache, the .srt naming rule, the registry, the AV-safe writes)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TrFileStore.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: cacheDir() = %LOCALAPPDATA%\SuVtitle\Files, the
// fallback .srt name ".suvtitle.srt", the registry's comment line. KEPT: the record format tags ("# AudioPB
// Translator file cache v2"): AudioPB's records are imported as they are (AudioPBImport.h) and must still read.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// Everything the tray keeps ON DISK for the file mode, in one header-only place so the logic is
// proved by a smoke test (scratchpad/ui_test/filestore_smoke.cpp) rather than only through the
// bridge, and so adding it needed no CMake change:
//
//   * THE CACHE. A recording analysed once plays instantly the next time it is opened -- even
//     with the helper not running -- because its subtitles are kept in
//     %LOCALAPPDATA%\SuVtitle\Files\<key>.cues. The key is a hash of what the result
//     depends on: the file (path, size, write time) and the job (pair, direction, quality).
//     The record is written in the WIRE's own line format (TrPipe.h's packCue/unpackCue,
//     round-trip tested, escape-safe), closed by an END line carrying the cue count: a record
//     torn by a crash or a full disk fails that count and is simply a cache MISS.
//     v2 (wave WE): a record can be PARTIAL (RNG = the media ranges its cues cover, DONE 0/1), so a
//     film closed half way resumes where its analysis stopped; peekRecord() reads only its head.
//   * THE SRT NAME. The subtitle file lands next to the video as "<base>.<lang>.srt", which is
//     the name VLC, MPC-HC, Plex and Jellyfin pick up by themselves. If a file of that name is
//     already there and is NOT one we wrote FOR THIS MEDIA (a downloaded subtitle, ours edited
//     by hand since, or ours for another file), it is left alone and ours goes to
//     "<base>.<lang>.suvtitle.srt", then "-2" ... "-9" -- each tested the same way. When another
//     MEDIA file with the same base sits in the folder (Interview_01.wav + Interview_01.mp4), the
//     base carries the extension ("Interview_01.wav.fr.srt"): two media never share one .srt.
//   * THE REGISTRY that answers "did we write this .srt, and for which media?": one small file
//     listing the path, size, write time AND media of every SRT we produced, automatic and
//     exported alike. One registry and not a line per cache record, because two different jobs
//     (en->fr and de->fr) are two cache keys that both target the same film.fr.srt.
//   * The URL path encoding for the WebView's virtual host, and the house's AV-safe write
//     (temp file + rename with Bitdefender's retry window -- PB.md section 8).
//
// UI THREAD ONLY where it touches the disk; the pure functions are thread-agnostic.
// =============================================================================
#pragma once

#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "TrPipe.h"

namespace pbtrf {

namespace tp = pbt::trpipe;

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
inline std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
inline std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
// Windows paths compare case-insensitively; so do our keys and our registry.
inline std::wstring lowerW(std::wstring s) {
    if (!s.empty()) ::CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
inline std::wstring leafOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}
// The folder a file sits in. "C:\film.mp4" -> "C:\" (a drive root keeps its backslash: "C:"
// alone means "the current directory of drive C", which is not the same folder at all).
inline std::wstring dirOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    if (s == std::wstring::npos) return std::wstring();
    if (s == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, s);
}
// "film.2024.mkv" -> "film.2024": only the LAST extension goes. A leading dot is a name, not
// an extension (".hidden" stays ".hidden").
inline std::wstring stripExt(const std::wstring& leaf) {
    const size_t dot = leaf.find_last_of(L'.');
    return (dot == std::wstring::npos || dot == 0) ? leaf : leaf.substr(0, dot);
}
inline std::wstring extOf(const std::wstring& leaf) {
    const size_t dot = leaf.find_last_of(L'.');
    return (dot == std::wstring::npos || dot == 0) ? std::wstring() : lowerW(leaf.substr(dot + 1));
}
inline std::wstring joinPath(const std::wstring& dir, const std::wstring& leaf) {
    if (dir.empty()) return leaf;
    const wchar_t last = dir[dir.size() - 1];
    return (last == L'\\' || last == L'/') ? dir + leaf : dir + L"\\" + leaf;
}
// The same file? Windows paths compare case-insensitively (a UTF-8 path from a record against
// the wide path in hand).
inline bool samePath(const std::string& utf8, const std::wstring& w) {
    return lowerW(fromUtf8(utf8)) == lowerW(w);
}

// ---------------------------------------------------------------------------
// What counts as a VIDEO OR AUDIO file: the Open dialog's own list (bridge.cpp pickMediaFile),
// twinned in translator.html (MEDIA_EXTS, the harness compares the two). A drop of anything
// else -- an .srt, a .txt with no MIME type -- is refused before the current session is touched.
// ---------------------------------------------------------------------------
inline const wchar_t* const* mediaExts() {
    static const wchar_t* const k[] = {
        L"mp4", L"m4v", L"mkv", L"webm", L"mov", L"avi", L"wmv", L"mpg", L"mpeg", L"ts", L"m2ts",
        L"mts", L"flv", L"3gp", L"ogv", L"mxf", L"mp3", L"wav", L"m4a", L"aac", L"flac", L"ogg",
        L"oga", L"opus", L"mka", L"wma", L"aif", L"aiff", L"aifc", L"ac3", L"eac3", L"amr", nullptr };
    return k;
}
inline bool isMediaExt(const std::wstring& ext) {
    const std::wstring e = lowerW(ext);
    for (const wchar_t* const* p = mediaExts(); *p; ++p) if (e == *p) return true;
    return false;
}
// Another MEDIA file with the same base sits next to this one (Interview_01.wav when this is
// Interview_01.mp4): their subtitles must not compete for "<base>.<lang>.srt".
inline bool hasSiblingMedia(const std::vector<std::wstring>& leavesInFolder, const std::wstring& thisLeaf) {
    const std::wstring me = lowerW(thisLeaf), base = lowerW(stripExt(thisLeaf));
    for (const auto& l : leavesInFolder) {
        if (lowerW(l) == me) continue;
        if (lowerW(stripExt(l)) == base && isMediaExt(extOf(l))) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Hashing and the cache key
// ---------------------------------------------------------------------------
inline uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
inline std::string hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
// What the subtitles DEPEND on, and nothing else: the file as it is (a re-encoded file at the
// same path changes size or write time and is analysed again), and the job (the pair, the
// direction, the quality the user asked for).
// WH (2026-10-11, D6): ...and the SOUND TRACK analysed (TrPipe.h AudioSel.ord) -- "\ta<ord>" appended for ord > 0
// ONLY: every record made before WH analysed the first sound track (F5: Media Foundation's FIRST_AUDIO_STREAM), so
// ord 0 keeps the key it always had and none of them is lost.
inline std::string cacheKey(const std::wstring& path, uint64_t size, uint64_t mtime,
                            const std::string& langA, const std::string& langB,
                            const std::string& mode, const std::string& quality, uint32_t audioOrd = 0) {
    std::string k = toUtf8(lowerW(path));
    k += "\t" + std::to_string(size) + "\t" + std::to_string(mtime);
    k += "\t" + langA + "\t" + langB + "\t" + mode + "\t" + quality;
    if (audioOrd > 0) k += "\ta" + std::to_string(audioOrd);
    return hex16(fnv1a64(k));
}
// The MEDIA's own identity (the file, not the job): what names its virtual host and its audio
// proxy. One identity = one URL, across tray runs too -- a counter restarting at 1 every run
// would hand a URL the WebView's HTTP cache already holds for ANOTHER file (PB.md section 8,
// 2026-07-20: mapped-host responses served stale). A re-encoded file changes size or write
// time, so it gets a new URL as well.
inline std::string mediaKey(const std::wstring& path, uint64_t size, uint64_t mtime) {
    return hex16(fnv1a64(toUtf8(lowerW(path)) + "\t" + std::to_string(size) + "\t" + std::to_string(mtime)));
}
// WE (player v2): a FOLDER's identity -- its virtual host is f<folderKey>.suvtitle-media.example, so a
// playlist that stays in one folder never needs a new mapping (= a page reload). A host is NEVER
// re-pointed to another folder: measured (we_e2e STEP0 M1), a page keeps serving the folder its host
// had when it navigated, so a re-pointed host would hand it the WRONG files until the next reload.
inline std::string folderKey(const std::wstring& folder) {
    return hex16(fnv1a64("dir\t" + toUtf8(lowerW(folder))));
}

// ---------------------------------------------------------------------------
// The cache record.
// v2 (wave WE, the player): a record may be PARTIAL -- the user closed the film half way, a job was
// cancelled or failed -- and says which media RANGES its cues cover (RNG, TrPipe.h's packRanges) and
// whether the whole file is covered (DONE). The tray resumes such a job by sending those ranges as
// FILE's `have`. The header changed on purpose: a v1 reader (an older tray, 1.0.5) MISSES a v2
// record instead of reading a partial one as a finished result. A v1 record reads as complete.
// ---------------------------------------------------------------------------
inline const char* recordHeader()   { return "# AudioPB Translator file cache v2"; }
inline const char* recordHeaderV1() { return "# AudioPB Translator file cache v1"; }

// WG (the user's first real test, 11/10): ONE .srt PER LANGUAGE of the pair, written as the analysis goes. A record
// remembers each file it wrote (SRTL lines; v2 readers ignore a line they do not know).
struct SrtFile { std::string lang, path; uint64_t size = 0, mtime = 0; };

struct Record {
    std::string path;                  // UTF-8, as analysed (verified on read: guards a collision)
    uint64_t    size = 0, mtime = 0;
    std::string langA, langB, mode, quality;
    std::string asrModel, mtModel;
    bool        hasVideo = false;
    uint64_t    totalMs = 0;
    std::string srtPath;               // the automatic SRT written for this result ("" = none) -- WC/WE, one file;
    uint64_t    srtSize = 0, srtMtime = 0;   // since WG the first of srtFiles (kept for the record's SRT line)
    std::vector<SrtFile> srtFiles;     // WG: one per language of the pair (SRTL lines)
    bool        edited = false;        // WG: the user EDITED these subtitles (EDIT 1): a re-analysis asks first
    int         engine = 0;            // ENGINE: the cue engine that made the cues (TrPipe.h kFileEngineVersion); 0 = no
                                       // ENGINE line (every record written before it existed) -- see olderEngine()
    std::vector<tp::MsRange> ranges;   // v2: what the cues cover (merged); complete => [0, totalMs]
    bool        complete = true;       // v2: the WHOLE file was analysed (FDONE ok)
    std::vector<tp::WireCue> cues;     // sorted by start, idx 0..n-1 (canonicalCues)
    // WH: the sound track analysed -- the "AUD <ord> <trackId>" line, in EVERY record this tray writes (older readers
    // ignore a line they do not know). trackKnown = the line was there: a record written BEFORE WH has none, and for an
    // ISO-BMFF file with several sound tracks it may hold ANOTHER track's cues (measured by WH-H, M2/Q1: Media
    // Foundation then analysed ITS default pick, often not the first trak) -- see unknownTrackMiss().
    uint32_t    audioOrd = 0;
    uint32_t    audioTrack = 0;
    bool        trackKnown = true;
};

// THE CUE ENGINE'S VERSION (2026-10-11, TrPipe.h kFileEngineVersion; an ENGINE line, after JOB -- inside the 16 KB a
// peek reads; a v2 reader of 1.0.6 ignores a line it does not know, so it still reads these records). A record from an
// OLDER engine -- or with no ENGINE line at all: every record written before the line existed -- is a cache MISS: the
// file is analysed again (progressively, as a first analysis is), so an improved engine reaches the films analysed
// before it. EXCEPT a record the user EDITED by hand: never discarded silently -- it loads, and the window asks
// ("Subtitles from an older engine -- re-analyse? (your edits would be lost)"). A NEWER engine's record (a downgraded
// tray) reads as it is: its cues are not worse.
inline int engineOf(const std::string& s) {        // the ENGINE value: 1-6 digits, anything else = 0 (= older)
    if (s.empty() || s.size() > 6) return 0;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    return v;
}
inline bool olderEngine(int engine) { return engine < tp::kFileEngineVersion; }
inline bool staleRecord(int engine, bool edited) { return olderEngine(engine) && !edited; }
// WH (the director, from WH-H's measurements M2/Q1, 2026-10-11): a record WITHOUT the AUD line was made before WH, when
// the helper analysed Media Foundation's own pick of sound track. For Matroska/WebM and for one-track files that pick is
// the first track (= ord 0: the record is right). For an ISO-BMFF file (mp4 m4v mov 3gp m4a) with 2+ sound tracks it
// is often NOT the first trak: such a record is a MISS (analysed again) -- unless the user EDITED it: edited records
// are never discarded silently.
inline bool unknownTrackMiss(bool trackKnown, bool edited, bool multiTrackIsoBmff) {
    return !trackKnown && !edited && multiTrackIsoBmff;
}

// How many PAIRS of these spans overlap (one starts before the other ends; touching is not overlapping): a sweep over
// the starts with a heap of the ends still open -- O(n log n + pairs), counted, never listed. The subtitle editor's
// rule for a subtitle FILE (bridge.cpp extDocEdit, review of WG #5): an edit may not make this number grow.
inline size_t overlapPairs(std::vector<std::pair<uint64_t, uint64_t>> v) {
    std::sort(v.begin(), v.end());
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> open;
    size_t n = 0;
    for (const auto& x : v) {
        while (!open.empty() && open.top() <= x.first) open.pop();
        n += open.size();
        open.push(x.second);
    }
    return n;
}

// The canonical order of a cue set: by start time (v2's runs interleave), idx renumbered 0..n-1.
inline void canonicalCues(std::vector<tp::WireCue>& cues) {
    std::stable_sort(cues.begin(), cues.end(),
                     [](const tp::WireCue& a, const tp::WireCue& b) { return a.startMs < b.startMs; });
    for (size_t i = 0; i < cues.size(); ++i) cues[i].idx = static_cast<uint32_t>(i);
}
// Is `ms` inside one of the (merged, ascending) ranges?
inline bool inRanges(const std::vector<tp::MsRange>& r, uint64_t ms) {
    size_t lo = 0, hi = r.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (r[mid].e <= ms) lo = mid + 1;
        else if (r[mid].s > ms) hi = mid;
        else return true;
    }
    return false;
}
// The cues a set of ranges VOUCHES for: those that start inside them. What a record keeps (and a
// `have` claims) must be exactly what the tray holds -- a cue outside its ranges would come back
// twice when the job resumes, and a range without its cues would stay empty forever.
inline std::vector<tp::WireCue> cuesWithin(const std::vector<tp::WireCue>& cues, const std::vector<tp::MsRange>& r) {
    std::vector<tp::WireCue> out;
    for (const auto& c : cues) if (inRanges(r, c.startMs)) out.push_back(c);
    return out;
}

inline std::string packRecord(const Record& r) {
    std::string o = std::string(recordHeader()) + "\n";
    o += "SRC\t" + tp::esc(r.path) + "\t" + std::to_string(r.size) + "\t" + std::to_string(r.mtime) + "\n";
    o += "JOB\t" + tp::esc(r.langA) + "\t" + tp::esc(r.langB) + "\t" + tp::esc(r.mode)
       + "\t" + tp::esc(r.quality) + "\t" + tp::esc(r.asrModel) + "\t" + tp::esc(r.mtModel)
       + "\t" + (r.hasVideo ? "1" : "0") + "\t" + std::to_string(r.totalMs) + "\n";
    o += "ENGINE\t" + std::to_string(r.engine) + "\n";
    // WH: the track marker (in the peek's head) -- absent only from a record that never had one (kept as it was read)
    if (r.trackKnown) o += "AUD\t" + std::to_string(r.audioOrd) + "\t" + std::to_string(r.audioTrack) + "\n";
    o += "SRT\t" + tp::esc(r.srtPath) + "\t" + std::to_string(r.srtSize) + "\t" + std::to_string(r.srtMtime) + "\n";
    for (const auto& f : r.srtFiles)
        o += "SRTL\t" + tp::esc(f.lang) + "\t" + tp::esc(f.path) + "\t" + std::to_string(f.size) + "\t" + std::to_string(f.mtime) + "\n";
    if (r.edited) o += "EDIT\t1\n";
    std::vector<tp::MsRange> rg = r.ranges;
    tp::mergeRanges(rg);
    o += "RNG\t" + tp::packRanges(rg) + "\n";
    o += std::string("DONE\t") + (r.complete ? "1" : "0") + "\n";
    std::vector<tp::WireCue> cues = r.cues;
    canonicalCues(cues);
    for (tp::WireCue c : cues) {
        c.jobId = 1;                   // unpackCue refuses 0; the record names no job
        o += tp::packCue(c) + "\n";
    }
    o += "END\t" + std::to_string(cues.size()) + "\n";
    return o;
}

// False = not a usable record (wrong header, missing part, torn, a malformed RNG, a cue no range
// vouches for, or anything after END). A false here is a cache MISS, never an error the user sees.
inline bool parseRecord(const std::string& text, Record& r) {
    r = Record{};
    r.trackKnown = false;                              // (until its AUD line says)
    bool first = true, v1 = false, haveSrc = false, haveJob = false, haveEnd = false, haveRng = false, haveDone = false;
    unsigned long long endCount = 0;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            if (line == recordHeaderV1()) v1 = true;
            else if (line != recordHeader()) return false;
            first = false;
            continue;
        }
        if (line.empty()) continue;
        if (haveEnd) return false;
        const std::vector<std::string> t = tp::split(line);
        if (t[0] == "SRC" && t.size() >= 4) {
            r.path  = tp::unesc(t[1]);
            r.size  = std::strtoull(t[2].c_str(), nullptr, 10);
            r.mtime = std::strtoull(t[3].c_str(), nullptr, 10);
            haveSrc = true;
        } else if (t[0] == "JOB" && t.size() >= 9) {
            r.langA    = tp::unesc(t[1]);
            r.langB    = tp::unesc(t[2]);
            r.mode     = tp::unesc(t[3]);
            r.quality  = tp::unesc(t[4]);
            r.asrModel = tp::unesc(t[5]);
            r.mtModel  = tp::unesc(t[6]);
            r.hasVideo = (t[7] == "1");
            r.totalMs  = std::strtoull(t[8].c_str(), nullptr, 10);
            haveJob = true;
        } else if (t[0] == "SRT" && t.size() >= 2) {
            r.srtPath = tp::unesc(t[1]);
            if (t.size() >= 4) {               // an identity-less SRT line never matches a file
                r.srtSize  = std::strtoull(t[2].c_str(), nullptr, 10);
                r.srtMtime = std::strtoull(t[3].c_str(), nullptr, 10);
            }
        } else if (t[0] == "SRTL" && t.size() >= 5) {
            SrtFile f;
            f.lang = tp::unesc(t[1]);
            f.path = tp::unesc(t[2]);
            f.size = std::strtoull(t[3].c_str(), nullptr, 10);
            f.mtime = std::strtoull(t[4].c_str(), nullptr, 10);
            if (!f.lang.empty() && !f.path.empty()) r.srtFiles.push_back(f);
        } else if (t[0] == "EDIT" && t.size() >= 2) {
            r.edited = (t[1] == "1");
        } else if (t[0] == "ENGINE" && t.size() >= 2) {
            r.engine = engineOf(t[1]);
        } else if (t[0] == "AUD" && t.size() >= 3) {
            r.audioOrd = static_cast<uint32_t>(engineOf(t[1]));          // (1-6 digits, else 0)
            r.audioTrack = static_cast<uint32_t>(std::strtoul(t[2].c_str(), nullptr, 10));
            r.trackKnown = true;
        } else if (t[0] == "RNG" && !v1) {
            // STRICT, like the wire: a range list that does not parse is a refusal of the whole record
            // -- a wrong range is a stretch of film that silently never gets subtitles.
            if (!tp::parseRanges(t.size() >= 2 ? t[1] : std::string(), r.ranges)) return false;
            haveRng = true;
        } else if (t[0] == "DONE" && !v1 && t.size() >= 2) {
            r.complete = (t[1] == "1");
            haveDone = true;
        } else if (t[0] == "FCUE") {
            tp::WireCue c;
            if (!tp::unpackCue(t, c)) return false;
            r.cues.push_back(c);
        } else if (t[0] == "END" && t.size() >= 2) {
            endCount = std::strtoull(t[1].c_str(), nullptr, 10);
            haveEnd = true;
        }
        // Any other line is ignored: a later version may add one, and this one must still read
        // the cues it understands.
    }
    if (first || !haveSrc || !haveJob || !haveEnd || endCount != (unsigned long long)r.cues.size()) return false;
    if (v1) {
        // v1 kept finished results only: the whole file, in idx (= start) order.
        r.complete = true;
        r.ranges.clear();
        if (r.totalMs) r.ranges.push_back(tp::MsRange{ 0, r.totalMs });
    } else {
        if (!haveRng || !haveDone) return false;
        if (!r.complete)
            for (const auto& c : r.cues) if (!inRanges(r.ranges, c.startMs)) return false;
    }
    canonicalCues(r.cues);
    return true;
}

// WE: what the queue's badges need, WITHOUT reading the cues -- the header lines come first, so at
// most 16 KB of a record is read. ok = a record of either version, for that file (the caller still
// checks SRC against the file it has in hand).
struct Summary {
    bool        ok = false, complete = false, hasVideo = false;
    std::string path;
    uint64_t    size = 0, mtime = 0, totalMs = 0;
    std::vector<tp::MsRange> ranges;
    std::string langA, langB;          // WG: the pair (a language's file is written from the most complete set)
    bool        edited = false;        // WG: EDIT 1
    int         engine = 0;            // ENGINE (0 = none: older)
    uint32_t    audioOrd = 0;          // WH: AUD (0 = none: the first sound track)
    bool        trackKnown = false;    // WH: an AUD line was there (see unknownTrackMiss)
};
inline bool peekRecordText(const std::string& head, Summary& s) {
    s = Summary{};
    bool first = true, v1 = false, src = false, job = false, rng = false, done = false;
    size_t at = 0;
    while (at < head.size()) {
        const size_t nl = head.find('\n', at);
        if (nl == std::string::npos) break;            // a line cut by the 16 KB window is not read
        std::string line = head.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            if (line == recordHeaderV1()) v1 = true;
            else if (line != recordHeader()) return false;
            first = false;
            continue;
        }
        const std::vector<std::string> t = tp::split(line);
        if (t[0] == "FCUE" || t[0] == "END") break;
        if (t[0] == "SRC" && t.size() >= 4) {
            s.path = tp::unesc(t[1]);
            s.size = std::strtoull(t[2].c_str(), nullptr, 10);
            s.mtime = std::strtoull(t[3].c_str(), nullptr, 10);
            src = true;
        } else if (t[0] == "JOB" && t.size() >= 9) {
            s.langA = tp::unesc(t[1]);
            s.langB = tp::unesc(t[2]);
            s.hasVideo = (t[7] == "1");
            s.totalMs = std::strtoull(t[8].c_str(), nullptr, 10);
            job = true;
        } else if (t[0] == "EDIT" && t.size() >= 2) {
            s.edited = (t[1] == "1");
        } else if (t[0] == "ENGINE" && t.size() >= 2) {
            s.engine = engineOf(t[1]);
        } else if (t[0] == "AUD" && t.size() >= 2) {
            s.audioOrd = static_cast<uint32_t>(engineOf(t[1]));
            s.trackKnown = true;
        } else if (t[0] == "RNG" && !v1) {
            if (!tp::parseRanges(t.size() >= 2 ? t[1] : std::string(), s.ranges)) return false;
            rng = true;
        } else if (t[0] == "DONE" && !v1 && t.size() >= 2) {
            s.complete = (t[1] == "1");
            done = true;
        }
    }
    if (first || !src || !job) return false;
    if (v1) {
        s.complete = true;
        if (s.totalMs) s.ranges.assign(1, tp::MsRange{ 0, s.totalMs });
    } else if (!rng || !done) {
        return false;
    }
    s.ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// Which language a result's subtitles are IN -- the "<lang>" of film.<lang>.srt. A locked
// direction says it outright; AUTO translates each sentence into "the other half" of the pair,
// so the answer is the language most of the subtitles came out in (a bilingual recording gets
// the majority; a tie goes to B, the half the user named second -- the usual target).
// ---------------------------------------------------------------------------
inline std::string majority(const std::vector<std::string>& langs, const std::string& prefer) {
    std::vector<std::pair<std::string, size_t>> n;
    for (const auto& l : langs) {
        if (l.empty()) continue;
        bool hit = false;
        for (auto& e : n) if (e.first == l) { ++e.second; hit = true; break; }
        if (!hit) n.emplace_back(l, 1);
    }
    std::string best = prefer;
    size_t bestN = 0;
    for (const auto& e : n)
        if (e.second > bestN || (e.second == bestN && e.first == prefer)) { best = e.first; bestN = e.second; }
    return best;
}
inline std::string targetLang(const std::string& mode, const std::string& langA,
                              const std::string& langB, const std::vector<tp::WireCue>& cues) {
    if (mode == "a2b") return langB;
    if (mode == "b2a") return langA;
    std::vector<std::string> d;
    d.reserve(cues.size());
    for (const auto& c : cues) d.push_back(c.dstLang);
    return majority(d, langB);
}
// ...and the language that was SPOKEN, for the "original language" export's name.
inline std::string spokenLang(const std::vector<tp::WireCue>& cues, const std::string& fallback) {
    std::vector<std::string> s;
    s.reserve(cues.size());
    for (const auto& c : cues) s.push_back(c.lang);
    return majority(s, fallback);
}

// ---------------------------------------------------------------------------
// WG: ONE SUBTITLE FILE PER LANGUAGE (the user's first real test, 11/10: "il faut creer un fichier sous-titre par
// langue, avec le nom du langage dans le nom du fichier" -- his film.en.srt held 109 French blocks: lines detected as
// English were translated INTO French, and the old rule put every cue's TRANSLATION in one file named after the
// majority target). File L holds, for each cue, its text IN L: the SOURCE when it was spoken in L, the TRANSLATION
// when it was translated into L (a passthrough -- spoken and shown in L -- once); a cue with no text in L is not in
// file L. Both halves of the pair get a file, whatever the direction.
// ---------------------------------------------------------------------------
inline std::string lowerAscii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
inline bool textInLang(const tp::WireCue& c, const std::string& lang, std::string& out) {
    const std::string L = lowerAscii(lang);
    if (L.empty()) return false;
    if (lowerAscii(c.lang) == L && !c.src.empty()) { out = c.src; return true; }
    if (lowerAscii(c.dstLang) == L && !c.dst.empty()) { out = c.dst; return true; }
    return false;
}
inline size_t countInLang(const std::vector<tp::WireCue>& cues, const std::string& lang) {
    size_t n = 0;
    std::string t;
    for (const auto& c : cues) if (textInLang(c, lang, t)) ++n;
    return n;
}
// The <tag> of file L, as players read it: zh -> zh-CN (Simplified), zh-tw -> zh-TW (Traditional), any other code as
// it is, lower case (en, fr, de, es, it, pt, ru, ja, ko, ar, nl).
inline std::string fileLangTag(const std::string& code) {
    const std::string c = lowerAscii(code);
    if (c == "zh") return "zh-CN";
    if (c == "zh-tw") return "zh-TW";
    return c;
}
// The languages a set's files are written in: both halves of its pair, once each.
inline std::vector<std::string> pairLangs(const std::string& a, const std::string& b) {
    std::vector<std::string> v;
    if (!a.empty()) v.push_back(lowerAscii(a));
    if (!b.empty() && lowerAscii(b) != lowerAscii(a)) v.push_back(lowerAscii(b));
    return v;
}
// The codes a pair is built from -- TWIN of translator.html TR_LANGS (the harness compares the two).
inline const char* const* knownLangCodes() {
    static const char* const k[] = { "en", "zh", "zh-tw", "fr", "de", "es", "it", "pt", "ja", "ko", "ru", "nl", "ar", nullptr };
    return k;
}
// THE SHARED ORIGINAL. A film's original language (fr, for a French film) is in EVERY pair the user tries (fr->zh,
// then fr->en), and so is its file: written from the most complete cue set that holds the language -- never shrunk
// by the first minutes of the next pair. These are the cache keys of every analysis of THIS file whose pair holds
// `lang` (both orders, every direction and quality): the candidates. `extra` = codes beyond the table (the pair in
// use, should it be another).
// WH: `audioOrd` = the sound track of the set asking -- its siblings analysed THE SAME track (the 144 keys stay 144:
// another track's analysis comes in through the registry's writer key instead -- see registryWriter).
struct KeyOf { std::string key, langA, langB, mode, quality; };
inline std::vector<KeyOf> siblingKeys(const std::wstring& path, uint64_t size, uint64_t mtime, const std::string& lang,
                                      const std::vector<std::string>& extra = {}, uint32_t audioOrd = 0) {
    std::vector<std::string> codes;
    for (const char* const* p = knownLangCodes(); *p; ++p) codes.push_back(*p);
    for (const auto& x : extra) {
        const std::string l = lowerAscii(x);
        if (!l.empty() && std::find(codes.begin(), codes.end(), l) == codes.end()) codes.push_back(l);
    }
    const std::string L = lowerAscii(lang);
    static const char* const modes[] = { "auto", "a2b", "b2a" };
    static const char* const quals[] = { "auto", "max" };
    std::vector<KeyOf> out;
    for (const auto& o : codes) {
        if (o == L) continue;
        for (int order = 0; order < 2; ++order) {
            const std::string a = order ? o : L, b = order ? L : o;
            for (const char* m : modes)
                for (const char* q : quals) {
                    KeyOf k;
                    k.langA = a; k.langB = b; k.mode = m; k.quality = q;
                    k.key = cacheKey(path, size, mtime, a, b, m, q, audioOrd);
                    out.push_back(k);
                }
        }
    }
    return out;
}
// How much of the file a set covers: complete beats any partial; a partial is the total of its analysed ranges.
inline uint64_t coverage(bool complete, const std::vector<tp::MsRange>& r) {
    return complete ? UINT64_MAX : tp::rangesTotal(r);
}

// ---------------------------------------------------------------------------
// The registry of SRTs we wrote, and FOR WHICH MEDIA. Paths are kept LOWER-CASED UTF-8 (Windows
// compares them that way); newest LAST; capped. An entry without a media (an older registry) owns
// nothing: no media can claim it.
// ---------------------------------------------------------------------------
// WH (2026-10-11, D6): an automatic per-language file also records WHICH ANALYSIS wrote it -- its language, the cache
// key of the record whose cues it holds and that record's sound track. The never-shrink rule (TrPlayer.h bestSibling)
// then compares against the right record even after the user switched the sound track: a complete French file made
// from track 0 stays until track k's analysis is at least as complete. Optional fields (an export writes none); an
// older tray reads the first four and rewrites without them (the file then just has no known writer).
struct RegEntry { std::string path; uint64_t size = 0, mtime = 0; std::string media, lang, key; uint32_t ord = 0; };

inline std::vector<RegEntry> parseRegistry(const std::string& text) {
    std::vector<RegEntry> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> t = tp::split(line);
        if (t.size() < 3) continue;
        RegEntry e;
        e.path  = tp::unesc(t[0]);
        e.size  = std::strtoull(t[1].c_str(), nullptr, 10);
        e.mtime = std::strtoull(t[2].c_str(), nullptr, 10);
        if (t.size() >= 4) e.media = tp::unesc(t[3]);
        if (t.size() >= 7) {                           // WH: language, writer key, its sound track
            e.lang = tp::unesc(t[4]);
            e.key = tp::unesc(t[5]);
            e.ord = static_cast<uint32_t>(engineOf(t[6]));
        }
        if (!e.path.empty()) out.push_back(e);
    }
    return out;
}
inline std::string packRegistry(const std::vector<RegEntry>& e, size_t cap = 1000) {
    std::string o = "# SuVtitle: subtitle files written by SuVtitle (path, size, write time, media[, language, "
                    "writer record, its sound track])\n";
    const size_t from = e.size() > cap ? e.size() - cap : 0;
    for (size_t i = from; i < e.size(); ++i) {
        o += tp::esc(e[i].path) + "\t" + std::to_string(e[i].size) + "\t" + std::to_string(e[i].mtime)
           + "\t" + tp::esc(e[i].media);
        if (!e[i].key.empty()) o += "\t" + tp::esc(e[i].lang) + "\t" + tp::esc(e[i].key) + "\t" + std::to_string(e[i].ord);
        o += "\n";
    }
    return o;
}
// OURS FOR THIS MEDIA = we wrote a file at this path, FOR this media, AND it is still exactly what
// we wrote. A user who edited it changed its size or its write time, and their edit is then theirs
// to keep; one we wrote for ANOTHER file (same base, other folder's fallback...) is that file's.
inline bool registryOwns(const std::vector<RegEntry>& e, const std::wstring& path,
                         uint64_t size, uint64_t mtime, const std::wstring& media) {
    const std::string k = toUtf8(lowerW(path)), m = toUtf8(lowerW(media));
    for (const auto& r : e)
        if (r.path == k && r.size == size && r.mtime == mtime && !r.media.empty() && r.media == m) return true;
    return false;
}
inline void registryPut(std::vector<RegEntry>& e, const std::wstring& path, uint64_t size, uint64_t mtime,
                        const std::wstring& media, const std::string& lang = std::string(),
                        const std::string& key = std::string(), uint32_t ord = 0) {
    const std::string k = toUtf8(lowerW(path));
    for (size_t i = 0; i < e.size(); ++i)
        if (e[i].path == k) { e.erase(e.begin() + (std::ptrdiff_t)i); break; }
    RegEntry r;
    r.path = k; r.size = size; r.mtime = mtime; r.media = toUtf8(lowerW(media));
    r.lang = lang; r.key = key; r.ord = ord;
    e.push_back(r);
}
// WH (D6): the newest entry naming a writer for (media, language) -- the automatic file of that language and the
// record whose cues it holds. The caller checks the file is still exactly what was written (size, write time): a
// file the user edited since has no writer any more.
inline const RegEntry* registryWriter(const std::vector<RegEntry>& e, const std::wstring& media, const std::string& lang) {
    const std::string m = toUtf8(lowerW(media));
    for (size_t i = e.size(); i-- > 0;)
        if (!e[i].key.empty() && e[i].media == m && e[i].lang == lang) return &e[i];
    return nullptr;
}

// ---------------------------------------------------------------------------
// The SRT name, as a PURE decision (the questions about the disk are handed in). The candidates,
// in order: "<stem>.<tag>.srt", "<stem>.<tag>.suvtitle.srt", then "-2" .. "-9"; the first that is
// ABSENT or OURS FOR THIS MEDIA wins. <stem> is the base, or "<base>.<ext>" when another media
// file with the same base sits in the folder. "" = no usable name (the caller says so, it never
// overwrites a file that is not this media's).
// ---------------------------------------------------------------------------
inline std::vector<std::wstring> srtCandidates(const std::wstring& dir, const std::wstring& base,
                                               const std::wstring& ext, const std::wstring& tag, bool sibling) {
    const std::wstring stem = (sibling && !ext.empty()) ? base + L"." + ext : base;
    std::vector<std::wstring> v;
    v.push_back(joinPath(dir, stem + L"." + tag + L".srt"));
    v.push_back(joinPath(dir, stem + L"." + tag + L".suvtitle.srt"));
    for (int n = 2; n <= 9; ++n)
        v.push_back(joinPath(dir, stem + L"." + tag + L".suvtitle-" + std::to_wstring(n) + L".srt"));
    return v;
}
inline std::wstring chooseSrtPath(const std::wstring& dir, const std::wstring& base, const std::wstring& ext,
                                  const std::wstring& tag, bool sibling,
                                  const std::function<bool(const std::wstring&)>& exists,
                                  const std::function<bool(const std::wstring&)>& ownsForThisMedia) {
    for (const auto& p : srtCandidates(dir, base, ext, tag, sibling))
        if (!exists(p) || ownsForThisMedia(p)) return p;
    return std::wstring();
}

// ---------------------------------------------------------------------------
// The path of a virtual-host URL: UTF-8 bytes, everything but the unreserved set percent-encoded
// (spaces, '#', '?', '%', accents, CJK...). A '#' left bare would end the URL at the fragment.
// ---------------------------------------------------------------------------
inline std::string pctEncode(const std::string& utf8) {
    static const char* const kHex = "0123456789ABCDEF";
    std::string o;
    o.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~') {
            o += (char)c;
        } else {
            o += '%';
            o += kHex[c >> 4];
            o += kHex[c & 15];
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// The disk
// ---------------------------------------------------------------------------
// Size and last-write time (FILETIME, 100 ns ticks) -- the file's identity for the cache key and
// for the registry. False = it is not there (or not readable).
inline bool fileIdentity(const std::wstring& path, uint64_t& size, uint64_t& mtime, bool* isDir = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    size  = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    mtime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    if (isDir) *isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}
inline bool fileExists(const std::wstring& path) {
    const DWORD a = path.empty() ? INVALID_FILE_ATTRIBUTES : ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool ensureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const DWORD a = ::GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Whole file, with a ceiling (a cache record of a ten-hour recording is ~3 MB; anything near the
// ceiling is not one of ours).
inline bool readFileAll(const std::wstring& path, std::string& out, uint64_t maxBytes = 64ULL << 20) {
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || (uint64_t)sz.QuadPart > maxBytes) {
        ::CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    const BOOL ok = out.empty() ? TRUE : ::ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    ::CloseHandle(h);
    if (!ok) { out.clear(); return false; }
    out.resize(rd);
    return true;
}

// The first `maxBytes` of a file (the queue's badges peek at cache records' header lines).
inline bool readFileHead(const std::wstring& path, std::string& out, size_t maxBytes = 16384) {
    out.clear();
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize(maxBytes);
    DWORD rd = 0;
    const BOOL ok = ::ReadFile(h, &out[0], static_cast<DWORD>(maxBytes), &rd, nullptr);
    ::CloseHandle(h);
    if (!ok) { out.clear(); return false; }
    out.resize(rd);
    return true;
}
inline bool peekRecord(const std::wstring& path, Summary& s) {
    std::string head;
    s = Summary{};
    return readFileHead(path, head) && peekRecordText(head, s);
}

// The identity of a file WE just wrote, known without asking the disk again: its size is what we
// wrote and its write time is one we SET on the handle (see stampHandle). Asking the path after
// the rename can be answered from an SMB client's cache -- the registry would then record a
// stale identity and never recognise the file as ours again.
struct WrittenId { uint64_t size = 0, mtime = 0; bool valid = false; };

// Sets the handle's last-write time to "now" rounded DOWN to an even second, and returns it. Set
// explicitly, the file system no longer moves it when the handle closes; even seconds are exactly
// representable on NTFS (100 ns), exFAT (10 ms) and FAT32 (2 s) alike -- a USB stick of films
// would otherwise round our time and the file would stop being "ours" the moment it was written.
inline uint64_t stampHandle(HANDLE h) {
    FILETIME ft{};
    ::GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= t % 20000000ULL;
    ft.dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(t >> 32);
    return ::SetFileTime(h, nullptr, nullptr, &ft) ? t : 0;
}

// THE HOUSE'S AV-SAFE WRITE (PB.md section 8, TrConfig::save, the W5 export): write a .tmp, then
// rename it over the target -- retried ~5 x 20 ms, because a rename issued inside Bitdefender's
// on-write scan window fails on this machine -- and, if it never lands, write the target in place
// (a complete file written directly beats an orphaned .tmp next to a file that never appeared).
// replace == false: the target was ABSENT when its name was chosen, and it must still be: the
//   rename does not replace, and the in-place fallback does not create over an existing file, so
//   a file that appeared meanwhile (another tool, another run) is never clobbered -- the write
//   just fails and the caller tries its next name.
// id (optional): the identity of what was written (see WrittenId); valid only on success.
// False = nothing usable was written (the folder refused the .tmp, the target appeared, or both
// writes failed).
inline bool writeFileAvSafe(const std::wstring& path, const std::string& bytes, bool replace = true,
                            WrittenId* id = nullptr) {
    if (id) *id = WrittenId{};
    const std::wstring tmp = path + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const BOOL ok = bytes.empty() ? TRUE : ::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    const uint64_t stamp = (ok && wr == (DWORD)bytes.size()) ? stampHandle(h) : 0;
    ::CloseHandle(h);
    if (!ok || wr != (DWORD)bytes.size()) { ::DeleteFileW(tmp.c_str()); return false; }
    auto done = [&](uint64_t t) {
        if (id) { id->size = (uint64_t)bytes.size(); id->mtime = t; id->valid = t != 0; }
        return true;
    };
    for (int t = 0; t < 5; ++t) {
        if (::MoveFileExW(tmp.c_str(), path.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) return done(stamp);
        const DWORD e = ::GetLastError();
        if (!replace && (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS)) {   // it appeared: not ours
            ::DeleteFileW(tmp.c_str());
            return false;
        }
        ::Sleep(20);
    }
    ::DeleteFileW(tmp.c_str());
    HANDLE hd = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              replace ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hd == INVALID_HANDLE_VALUE) return false;
    DWORD wr2 = 0;
    const BOOL ok2 = bytes.empty() ? TRUE : ::WriteFile(hd, bytes.data(), (DWORD)bytes.size(), &wr2, nullptr);
    const uint64_t stamp2 = (ok2 && wr2 == (DWORD)bytes.size()) ? stampHandle(hd) : 0;
    ::CloseHandle(hd);
    return (ok2 && wr2 == (DWORD)bytes.size()) ? done(stamp2) : false;
}

// %LOCALAPPDATA%\SuVtitle\Files (created; AudioPB's was %LOCALAPPDATA%\AudioPB\Translator files -- copied over once
// by AudioPBImport.h). LOCAL, not roaming: a cache of subtitles for
// files on THIS machine has no business following the user to another one.
inline std::wstring cacheDir() {
    wchar_t la[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return std::wstring();
    std::wstring d = std::wstring(la) + L"\\SuVtitle";
    ::CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\Files";
    return ensureDir(d) ? d : std::wstring();
}

// Files matching `pattern` in `dir`, oldest write first.
inline std::vector<std::pair<uint64_t, std::wstring>> listByAge(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<std::pair<uint64_t, std::wstring>> v;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        v.emplace_back(t, joinPath(dir, fd.cFileName));
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    for (size_t i = 1; i < v.size(); ++i)            // insertion sort: a few hundred entries at most
        for (size_t j = i; j > 0 && v[j - 1].first > v[j].first; --j) std::swap(v[j - 1], v[j]);
    return v;
}
// Keep the `keep` newest files matching `pattern`; delete the rest. Returns how many went.
inline size_t keepNewest(const std::wstring& dir, const wchar_t* pattern, size_t keep) {
    const auto v = listByAge(dir, pattern);
    size_t gone = 0;
    for (size_t i = 0; i + keep < v.size(); ++i)
        if (::DeleteFileW(v[i].second.c_str())) ++gone;
    return gone;
}

}   // namespace pbtrf
