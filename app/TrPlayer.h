// =============================================================================
// SuVtitle - app/TrPlayer.h   (the player's core)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/TrPlayer.h at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md: S3 = parity first, internal names kept, only what the user or
// Windows sees renamed). Changed: the media/proxy hosts (suvtitle-*), the sentences
// that named AudioPB or its Translator card, the data paths in comments. The player files keep their format tags
// ("# AudioPB player v1"...): the first-run import copies AudioPB's files byte for byte.
// FIX ROUND 2026-10-23 (review RV4-1, SPLIT_PLAN ADDENDUM 2): Session.userSub + acceptSubGet / userSubFile -- the
// page's AUTOMATIC subtitle choice never replaces one the viewer made for the item.
// --- the original header follows; read "the tray" as SuVtitle.exe and "the helper" as SuVtitleEngine.exe ---
//
// THE PLAYER'S CORE. The user (11/10): watch a film after a SHORT load while the translation stays
// ahead of it and completes in the background; translation on/off (a plain player too); a real
// player around it -- a queue, folders, resume, repeat/shuffle, external subtitle files.
//
// This header is the whole STATE MACHINE behind that, and nothing else: the queue, the current
// item's session, the helper jobs (the current item's and the pre-analysis of the next ones), the
// media ranges already subtitled, the FFOCUS rule, the partial cache, the resume table, which
// folders the window must have mapped. It touches NOTHING itself -- no pipe, no disk, no WebView:
// every effect goes through the `Host` interface below, which bridge.cpp implements. That split is
// the point (WE_PLAN_v2 D1): v1's file session lived inside the bridge and was never executed by
// any test; this one runs end to end in scratchpad/ui_test/player_smoke.cpp against a fake Host
// that records every action -- the bridge in a box. Header-only (no CMake change), UI THREAD ONLY.
//
// THE COUNTER (as in v1): one counter mints every id -- a session's GENERATION (what the page keys
// its cue set on), a FILE job's wire id, an FPROXY's id -- so an id names exactly one thing and a
// message carrying an older one is stale everywhere. v2 DECOUPLES generation and job: a session can
// adopt a job minted before it (a pre-analysis of the next episode), and a job can outlive its
// session (cancelled when the user moves on, its FDONE still owed and its partial result saved).
//
// TRUSTED RANGES (the coordinator's rule: "only trust ranges whose cues you hold"). FPROG reports
// the ranges analysed so far. A report is VOUCHED FOR only once a LATER message of the same job has
// arrived (the next FPROG, or the helper's own FDONE): whatever ordering the helper uses between a
// window's cues and its FPROG, the pipe is ordered, so by then every cue of that window is here. A
// SYNTHESIZED FDONE (the helper died) vouches for nothing. What is saved -- and later sent back as
// FILE's `have` -- is the vouched ranges and exactly the cues inside them (pbtrf::cuesWithin): a
// crash costs at most the last window, re-analysed on resume; never a hole, never a duplicate.
//
// THE FILM'S OWN TRACKS (wave WH, 2026-10-11 -- WH_PLAN_pistes D1-D7). A container (mkv, webm, mp4, mov...) is PROBED
// on a worker (Host::probeTracks -> onTracks; MediaTracks.h, headers only, ~1 ms) before its job starts -- the job
// waits for it at most kProbeWaitMs -- and the core CHOOSES the sound track heard and analysed (D3): the user's own
// choice for this media (player-audio.txt), else the first DEFAULT track the WebView can play, else the first it can
// play, else the first. The page enforces it on the <video> and reports what it hears; a different choice with
// translation on starts a NEW GENERATION on that track (D5) -- its own cache record (key "\ta<ord>" for ord > 0
// only: every record made before WH is the first track's and keeps its key, D6), FILE's `audio` field
// "<ord>:<trackId>" ("" for ord 0: the wire as before WH). Embedded TEXT subtitle tracks get ids from the same counter
// as the sidecars and are extracted on a worker when asked for (Host::extractSub -> onSubExtracted; the last 8 kept).
//
// NOTE: ASCII only (the build has no /utf-8).
// =============================================================================
#pragma once

#include <windows.h>
#include <shlwapi.h>   // StrCmpLogicalW: Explorer's own "natural" order (Episode 2 < Episode 10)

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "TrPipe.h"
#include "TrFileStore.h"
#include "JsonW.h"
#include "MediaTracks.h"   // WH: the film's own tracks (probe + embedded subtitle extraction, on workers)

#pragma comment(lib, "shlwapi.lib")

namespace pbplayer {

namespace tp = pbt::trpipe;
using patchbus::JsonW;

const wchar_t* const kMediaHostSuffix = L".suvtitle-media.example";
const wchar_t* const kProxyHost       = L"suvtitle-proxy.example";
constexpr size_t   kQueueMax      = 2000;     // items in the queue
constexpr size_t   kMapMax        = 16;       // folders mapped per page load (measured: STEP0 M7)
constexpr size_t   kPreAhead      = 2;        // items pre-analysed ahead of the current one (D14)
constexpr size_t   kCueChunk      = 500;      // cues per bulk message
constexpr uint64_t kCheckpointMs  = 120000;   // a running job's partial result is saved this often
constexpr uint64_t kQueueSaveMs   = 2000;     // queue edits are written after this much quiet
constexpr uint64_t kResumeSaveMs  = 15000;    // positions are written at most this often while playing
constexpr uint64_t kPrevRestartMs = 3000;     // "previous" past this point restarts the item instead
constexpr size_t   kSummaryPeekMax = 500;     // badges: cache records peeked per refresh (the first N items)
constexpr uint64_t kTouchMs       = 1;        // two cues overlapping by this much TOUCH (TrPipe.h: end <= range.e + 1)
// WG (the user's first real test, 11/10): the per-language .srt files are written AS THE ANALYSIS GOES -- at most this
// long after the first cue they do not hold yet (and at once on pause, close, switch, the job's end, the tray's end).
constexpr uint64_t kSrtEveryMs    = 30000;
constexpr uint64_t kEditSaveMs    = 2000;     // WG editor: an edit reaches the cache and the files this long after the last one
// WH: the film's own tracks
constexpr uint64_t kProbeWaitMs   = 3000;     // a container's job waits at most this long for its track list (measured ~1 ms;
                                              // a sleeping disk or a share: the job then goes on the provisional track)
constexpr size_t   kProbeKeep     = 64;       // probes remembered per tray run (by mediaKey)
constexpr size_t   kAudioPickKeep = 500;      // the user's sound-track choices kept (player-audio.txt, newest)
constexpr uint64_t kAudioSaveMs   = 2000;     // ...written this long after the last change
constexpr size_t   kSubLruKeep    = 8;        // embedded subtitle extractions kept in memory (D7)
constexpr size_t   kSubChunk      = 1000;     // cues per playerSubs message (= bridge.cpp kTrSubChunk)

inline std::wstring w16(const std::string& s) { return pbtrf::fromUtf8(s); }
inline std::string  u8(const std::wstring& w) { return pbtrf::toUtf8(w); }
inline std::wstring lowerW(const std::wstring& s) { return pbtrf::lowerW(s); }

// ===========================================================================
// 1. THE RANGE RULES (pure). The page has twins of focusTarget/leadAt/coveredToEnd/readyAt
//    (translator.html) and BOTH sides run scratchpad/ui_test/we_vectors.txt.
// ===========================================================================
// Where the analysis would carry on for a playhead at `p`: p itself in a gap, the END of p's range
// inside one -- TrPipe.h: "a focus inside an analysed range means carry on from the end of it".
inline uint64_t focusTarget(const std::vector<tp::MsRange>& r, uint64_t p) {
    for (const auto& x : r) {
        if (p < x.s) break;
        if (p < x.e) return x.e;
    }
    return p;
}
// Subtitled time ahead of the playhead.
inline uint64_t leadAt(const std::vector<tp::MsRange>& r, uint64_t p) { return focusTarget(r, p) - p; }
// Nothing left to analyse from p to the end (d = 0: the duration is not known yet).
inline bool coveredToEnd(const std::vector<tp::MsRange>& r, uint64_t p, uint64_t d) {
    return d > 0 && focusTarget(r, p) + 500 >= d;
}
constexpr uint64_t kReadyLeadMs = 15000;     // play when ready: this much subtitled ahead of the start
inline bool readyAt(const std::vector<tp::MsRange>& r, uint64_t p, uint64_t d) {
    return leadAt(r, p) >= kReadyLeadMs || coveredToEnd(r, p, d);
}

// THE FFOCUS RULE (WE_PLAN_v2 section 4). The page reports the playhead on a seek, a play, a pause,
// and every 5 s while playing (tick); FFOCUS goes out only when the analysis would otherwise go on
// elsewhere: the target moved by more than a second, and the subtitled lead is short.
enum class PosReason { Tick, Seek, Play, Pause, Ended };
constexpr uint64_t kFocusSeekLeadMs = 60000, kFocusTickLeadMs = 30000, kFocusSameMs = 1000;
inline bool shouldFocus(const std::vector<tp::MsRange>& r, uint64_t d, uint64_t p, uint64_t lastFocus, PosReason why) {
    if (why == PosReason::Pause || why == PosReason::Ended) return false;
    if (coveredToEnd(r, p, d)) return false;
    const uint64_t t = focusTarget(r, p), lt = focusTarget(r, lastFocus);
    // A TICK never chases a playhead the job is already HEADING to: in a gap (t == p), past the last
    // focus -- the job analyses forward from there, towards it. Chasing it every 5 s restarted the run
    // each time on a machine that plays faster than it analyses, a hole behind every restart (review of
    // WE, P4). A seek or a play still refocuses: those are the user's own jumps. (The helper keeps its
    // own rule for an FFOCUS just ahead of its cursor: this one only spares it the burst.)
    if (why == PosReason::Tick && t == p && lastFocus <= p) return false;
    const uint64_t diff = t > lt ? t - lt : lt - t;
    if (diff <= kFocusSameMs) return false;
    const uint64_t lead = t - p;
    return why == PosReason::Tick ? lead < kFocusTickLeadMs : lead < kFocusSeekLeadMs;
}
inline PosReason reasonFrom(const std::string& s) {
    if (s == "seek") return PosReason::Seek;
    if (s == "play") return PosReason::Play;
    if (s == "pause") return PosReason::Pause;
    if (s == "ended") return PosReason::Ended;
    return PosReason::Tick;
}
inline std::vector<tp::MsRange> merged(std::vector<tp::MsRange> a, const std::vector<tp::MsRange>& b = {}) {
    a.insert(a.end(), b.begin(), b.end());
    tp::mergeRanges(a);
    return a;
}

// ===========================================================================
// 2. THE QUEUE'S PIECES (pure)
// ===========================================================================
enum class Repeat { Off, One, All };
inline const char* repeatName(Repeat r) { return r == Repeat::One ? "one" : r == Repeat::All ? "all" : "off"; }
inline Repeat repeatFrom(const std::string& s) { return s == "one" ? Repeat::One : s == "all" ? Repeat::All : Repeat::Off; }

struct Item {
    uint64_t     id = 0;                 // per run; the page names items by it and by nothing else
    std::wstring path, name, folder, dirName;
    uint64_t     size = 0, mtime = 0;    // identity when last seen (the cache key's half)
    uint64_t     durMs = 0;              // known once played (or from its cache record)
    bool         missing = false;
};
struct Sidecar { uint64_t id = 0; std::wstring name, hint, path; };

inline bool naturalLess(const std::wstring& a, const std::wstring& b) { return ::StrCmpLogicalW(a.c_str(), b.c_str()) < 0; }

// The resume table: where each file was left. Keyed by the file's identity, so a re-encoded file at
// the same path starts over.
struct ResumeEntry { uint64_t ms = 0, durMs = 0, epoch = 0; };
constexpr uint64_t kResumeMinMs = 10000, kResumeTailMs = 15000;
constexpr size_t   kResumeKeep = 1000;
inline std::string resumeKey(const std::wstring& path, uint64_t size, uint64_t mtime) {
    return u8(lowerW(path)) + "|" + std::to_string(size) + "|" + std::to_string(mtime);
}
inline bool resumeFinished(uint64_t ms, uint64_t durMs) {
    return durMs > 0 && (ms + kResumeTailMs >= durMs || ms * 100 >= durMs * 95);
}
// Where a file starts: where it was left, unless that was the very start or the very end.
inline uint64_t resumeStart(const ResumeEntry* e, uint64_t durMs) {
    if (!e || e->ms < kResumeMinMs) return 0;
    const uint64_t d = durMs ? durMs : e->durMs;
    return resumeFinished(e->ms, d) ? 0 : e->ms;
}
inline std::string packResume(const std::map<std::string, ResumeEntry>& m) {
    std::vector<std::pair<std::string, ResumeEntry>> v(m.begin(), m.end());
    std::stable_sort(v.begin(), v.end(), [](const std::pair<std::string, ResumeEntry>& a, const std::pair<std::string, ResumeEntry>& b) {
        return a.second.epoch < b.second.epoch;
    });
    const size_t from = v.size() > kResumeKeep ? v.size() - kResumeKeep : 0;
    std::string o = "# AudioPB player resume v1\n";
    for (size_t i = from; i < v.size(); ++i)
        o += "POS\t" + tp::esc(v[i].first) + "\t" + std::to_string(v[i].second.ms) + "\t" + std::to_string(v[i].second.durMs)
           + "\t" + std::to_string(v[i].second.epoch) + "\n";
    return o;
}
inline void parseResume(const std::string& text, std::map<std::string, ResumeEntry>& m) {
    m.clear();
    size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; if (line != "# AudioPB player resume v1") return; continue; }
        const std::vector<std::string> t = tp::split(line);
        if (t.size() < 5 || t[0] != "POS") continue;
        ResumeEntry e;
        e.ms = std::strtoull(t[2].c_str(), nullptr, 10);
        e.durMs = std::strtoull(t[3].c_str(), nullptr, 10);
        e.epoch = std::strtoull(t[4].c_str(), nullptr, 10);
        m[tp::unesc(t[1])] = e;
    }
}

// The queue file (%APPDATA%\SuVtitle\player.txt). Torn (no END, or a count that disagrees) = an
// empty queue, never half of one.
struct QueueFile {
    bool translate = true;
    Repeat repeat = Repeat::Off;
    bool shuffle = false;
    long long cur = -1;
    struct It { std::wstring path; uint64_t size = 0, mtime = 0, durMs = 0; };
    std::vector<It> items;
    std::vector<size_t> order;           // the shuffle order, as indexes into items
};
inline std::string packQueue(const QueueFile& q) {
    std::string o = "# AudioPB player v1\n";
    o += std::string("OPT\ttranslate\t") + (q.translate ? "1" : "0") + "\n";
    o += std::string("OPT\trepeat\t") + repeatName(q.repeat) + "\n";
    o += std::string("OPT\tshuffle\t") + (q.shuffle ? "1" : "0") + "\n";
    o += "CUR\t" + std::to_string(q.cur) + "\n";
    for (const auto& it : q.items)
        o += "ITEM\t" + tp::esc(u8(it.path)) + "\t" + std::to_string(it.size) + "\t" + std::to_string(it.mtime)
           + "\t" + std::to_string(it.durMs) + "\n";
    if (q.shuffle && !q.order.empty()) {
        std::string s;
        for (size_t i = 0; i < q.order.size(); ++i) { if (i) s += ","; s += std::to_string(q.order[i]); }
        o += "ORDER\t" + s + "\n";
    }
    o += "END\t" + std::to_string(q.items.size()) + "\n";
    return o;
}
inline bool parseQueue(const std::string& text, QueueFile& q) {
    q = QueueFile{};
    bool first = true, end = false;
    unsigned long long count = 0;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; if (line != "# AudioPB player v1") return false; continue; }
        if (line.empty()) continue;
        if (end) return false;
        const std::vector<std::string> t = tp::split(line);
        if (t[0] == "OPT" && t.size() >= 3) {
            if (t[1] == "translate") q.translate = t[2] == "1";
            else if (t[1] == "repeat") q.repeat = repeatFrom(t[2]);
            else if (t[1] == "shuffle") q.shuffle = t[2] == "1";
        } else if (t[0] == "CUR" && t.size() >= 2) {
            q.cur = std::strtoll(t[1].c_str(), nullptr, 10);
        } else if (t[0] == "ITEM" && t.size() >= 5) {
            QueueFile::It it;
            it.path = w16(tp::unesc(t[1]));
            it.size = std::strtoull(t[2].c_str(), nullptr, 10);
            it.mtime = std::strtoull(t[3].c_str(), nullptr, 10);
            it.durMs = std::strtoull(t[4].c_str(), nullptr, 10);
            if (!it.path.empty() && q.items.size() < kQueueMax) q.items.push_back(it);
        } else if (t[0] == "ORDER" && t.size() >= 2) {
            size_t p = 0;
            const std::string& s = t[1];
            while (p < s.size()) {
                const size_t c = s.find(',', p);
                q.order.push_back(static_cast<size_t>(std::strtoull(s.substr(p, c == std::string::npos ? std::string::npos : c - p).c_str(), nullptr, 10)));
                if (c == std::string::npos) break;
                p = c + 1;
            }
        } else if (t[0] == "END" && t.size() >= 2) {
            count = std::strtoull(t[1].c_str(), nullptr, 10);
            end = true;
        }
    }
    if (first || !end || count != q.items.size()) { q = QueueFile{}; return false; }
    if (q.cur >= static_cast<long long>(q.items.size())) q.cur = -1;
    // the order must be a permutation of the items, or it is dropped (a fresh one is drawn)
    std::vector<bool> seen(q.items.size(), false);
    bool okOrder = q.order.size() == q.items.size();
    for (size_t i : q.order) { if (i >= seen.size() || seen[i]) { okOrder = false; break; } seen[i] = true; }
    if (!okOrder) q.order.clear();
    return true;
}

// The PAIR the helper will run, as its own config says (SuVtitle: %APPDATA%\SuVtitle\engine.txt; MODE, LANGA,
// LANGB -- READ ONLY, the helper owns that file; its TrConfig::load reads the same lines). Raw codes:
// the caller normalizes them (bridge.cpp trLangCode, the helper's twin). The mode is migrated and
// checked like TrConfig::load does. False = none of the three lines is there.
// (Review of WE, P3: until the helper's first STATE echo -- or for good without the card -- the
// player keyed the cache with the default en/zh pair: the user's own analyses were missed.)
inline bool pairFromTranslatorCfg(const std::string& text, std::string& a, std::string& b, std::string& mode) {
    bool any = false;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string tag = line.substr(0, tab), val = line.substr(tab + 1);
        if (tag == "LANGA")      { a = val; any = true; }
        else if (tag == "LANGB") { b = val; any = true; }
        else if (tag == "MODE")  { mode = val; any = true; }
    }
    if (mode == "en2zh") mode = "a2b";
    else if (mode == "zh2en") mode = "b2a";
    if (mode != "auto" && mode != "a2b" && mode != "b2a") mode = "auto";
    return any;
}

// ===========================================================================
// 2b. WH: THE SOUND TRACK (pure)
// ===========================================================================
// The user's own choice of sound track for one media file (by mediaKey), persisted in %APPDATA%\SuVtitle\player-audio.txt
// -- the player's own file, never the app's or the helper's configuration. An AUTO choice (the page could not play
// the tray's) is never stored as his.
struct AudioPick { uint32_t ord = 0, trackId = 0; uint64_t epoch = 0; };
// D3: the track a file plays and is analysed on. The user's choice for it (still there: same ord, same track id), else
// the first DEFAULT track the WebView can play (Matroska FlagDefault -- absent = 1 -- and FlagEnabled; MP4 tkhd
// "enabled"), else the first it can play, else the first. -1 = no sound track at all. The TRAY decides because nothing
// else can: Chromium enables the first track it can DECODE and ignores FlagDefault (F3, measured: mkv_2a_def2 plays
// track 2 though track 3 is the default), Media Foundation analysed the first track whatever was heard (F5).
inline int initialAudioOrd(const pbmedia::Info& t, const AudioPick* pick) {
    if (t.audio.empty()) return -1;
    if (pick && pick->ord < t.audio.size() && (pick->trackId == 0 || t.audio[pick->ord].track == pick->trackId))
        return static_cast<int>(pick->ord);
    for (const auto& a : t.audio) if (a.play && a.def) return static_cast<int>(a.ord);
    for (const auto& a : t.audio) if (a.play) return static_cast<int>(a.ord);
    return 0;
}
inline std::string packAudioPicks(const std::map<std::string, AudioPick>& m) {
    std::vector<std::pair<std::string, AudioPick>> v(m.begin(), m.end());
    std::stable_sort(v.begin(), v.end(), [](const std::pair<std::string, AudioPick>& a, const std::pair<std::string, AudioPick>& b) {
        return a.second.epoch < b.second.epoch;
    });
    const size_t from = v.size() > kAudioPickKeep ? v.size() - kAudioPickKeep : 0;
    std::string o = "# AudioPB player audio v1\n";
    for (size_t i = from; i < v.size(); ++i)
        o += "AUD\t" + tp::esc(v[i].first) + "\t" + std::to_string(v[i].second.ord) + "\t" + std::to_string(v[i].second.trackId)
           + "\t" + std::to_string(v[i].second.epoch) + "\n";
    return o;
}
inline void parseAudioPicks(const std::string& text, std::map<std::string, AudioPick>& m) {
    m.clear();
    size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; if (line != "# AudioPB player audio v1") return; continue; }
        const std::vector<std::string> t = tp::split(line);
        if (t.size() < 5 || t[0] != "AUD" || m.size() >= kAudioPickKeep) continue;
        AudioPick p;
        p.ord = static_cast<uint32_t>(std::strtoul(t[2].c_str(), nullptr, 10));
        p.trackId = static_cast<uint32_t>(std::strtoul(t[3].c_str(), nullptr, 10));
        p.epoch = std::strtoull(t[4].c_str(), nullptr, 10);
        if (p.ord < pbmedia::kMaxTracks) m[tp::unesc(t[1])] = p;
    }
}

// ===========================================================================
// 3. FOLDERS: what a dropped or picked folder adds (runs on a WORKER thread in the bridge)
// ===========================================================================
struct ScanCaps { int maxDepth = 16; size_t maxEntries = 50000, maxMedia = kQueueMax; uint64_t maxMs = 10000; };
// `sizes`/`mtimes` go with `files`: read from the directory listing itself, on the worker -- the UI
// thread then needs no GetFileAttributesExW per file (review of WE, P2: seconds on a network share).
struct ScanResult {
    std::vector<std::wstring> files;
    std::vector<uint64_t> sizes, mtimes;
    bool truncated = false;
    size_t visited = 0;
};
// The media files under `dir`: each folder's FILES first (natural order), then its subfolders
// (natural order), recursively. Media extensions only (the Open dialog's list). Hidden and system
// entries are skipped ($RECYCLE.BIN, System Volume Information), and so are REPARSE POINTS --
// junctions and symlinks can loop, and a drop of one should not walk somewhere else entirely.
inline void scanFolder(const std::wstring& dir, const ScanCaps& caps, ScanResult& out,
                       const std::atomic<bool>* cancel = nullptr, uint64_t t0 = 0, int depth = 0) {
    if (depth == 0) t0 = ::GetTickCount64();
    if (out.truncated || depth > caps.maxDepth) return;
    struct F { std::wstring name; uint64_t size, mtime; };
    std::vector<F> files;
    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileExW(pbtrf::joinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                  nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (++out.visited > caps.maxEntries || (cancel && cancel->load()) || ::GetTickCount64() - t0 > caps.maxMs) {
            out.truncated = true;
            break;
        }
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(n);
        else if (pbtrf::isMediaExt(pbtrf::extOf(n)))
            files.push_back(F{ n, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,     // = fileIdentity()'s
                               ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime });
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return naturalLess(a.name, b.name); });
    std::sort(dirs.begin(), dirs.end(), naturalLess);
    for (const auto& f : files) {
        if (out.files.size() >= caps.maxMedia) { out.truncated = true; return; }
        out.files.push_back(pbtrf::joinPath(dir, f.name));
        out.sizes.push_back(f.size);
        out.mtimes.push_back(f.mtime);
    }
    for (const auto& d : dirs) {
        if (out.truncated) return;
        scanFolder(pbtrf::joinPath(dir, d), caps, out, cancel, t0, depth + 1);
    }
}

// ===========================================================================
// 4. THE CORE
// ===========================================================================
// One file's cue set under one pair/direction/quality: what the cache record holds, what a job adds
// to, what the window shows when it is the current item's. Shared (shared_ptr) by the session and
// the job working on it: either may go first.
struct CueSet {
    std::wstring path, name, folder;
    uint64_t     size = 0, mtime = 0;
    std::string  mediaKey, langA, langB, mode, quality, key;   // key = the cache key
    std::vector<tp::WireCue> cues;                             // arrival order; idx = position (session idx)
    std::map<uint64_t, uint64_t> spans;                        // start -> end: the overlap guard
    std::vector<tp::MsRange> ranges;                           // analysed, as last reported (display)
    std::vector<tp::MsRange> confirmed;                        // VOUCHED FOR (see TRUSTED RANGES)
    bool         complete = false, hasVideo = false, videoKnown = false, fromCache = false;
    uint64_t     totalMs = 0;
    std::wstring asr, mt;
    std::wstring srtPath, srtNote;                             // srtPath = the first of srtFiles (reveal)
    uint64_t     srtSize = 0, srtMtime = 0;
    std::vector<pbtrf::SrtFile> srtFiles;                      // WG: one file per language of the pair
    bool         srtDirty = false;                             // WG: cues the files do not hold yet...
    uint64_t     srtDirtySince = 0;                            // ...since this time (the 30 s rule)
    bool         edited = false;                               // WG: the user edited this set (record EDIT 1)
    bool         editDirty = false;                            // ...an edit not saved yet
    uint64_t     editAt = 0;                                   // ...the last one, at
    uint32_t     nextIdx = 0;                                  // WG: the next session idx (unique even after deletions)
    int          engine = tp::kFileEngineVersion;              // ENGINE: the cue engine that made these cues -- this
                                                               // build's for a new analysis, a record's when loaded from it
    bool         upgrade = false;                              // ENGINE: analysed AGAIN because the cached result came
                                                               // from an older engine (the strip says so)
    bool         dirty = false;                                // cues/ranges not saved yet
    uint64_t     savedAt = 0;
    tp::AudioSel audio;                                        // WH: the SOUND TRACK these cues are of (part of the key)
    bool         trackKnown = true;                            // WH: false = loaded from a record with no AUD line (made
                                                               // before WH; written back the same: never stamped as known)
};

struct Job {
    uint64_t wire = 0;
    std::shared_ptr<CueSet> set;
    bool     pre = false;              // a pre-analysis (not the current item's)
    uint32_t nextIdx = 0;              // the helper's next expected FCUE idx (dedupe)
    std::vector<tp::MsRange> lastProg; // the latest FPROG's ranges: vouched for by the NEXT message
    bool     cancelSent = false;
    std::string phase = "loading";     // loading | analyzing
};

struct Session {
    uint64_t gen = 0, seq = 0, itemId = 0;
    std::wstring path, folder, name, host, url;
    uint64_t size = 0, mtime = 0;
    std::string mediaKey;
    std::shared_ptr<CueSet> set;
    uint64_t wire = 0;                 // the job working on `set` for this session (0 = none)
    bool     cancelling = false, userCancelled = false, failed = false;
    std::wstring err;
    bool     autoplay = false, resumed = false;
    uint64_t startMs = 0, lastPosMs = 0, lastFocusMs = 0, durMs = 0;
    bool     playing = false;
    uint64_t proxyReq = 0;
    std::string proxyState;            // "" | working | ready | failed
    std::wstring proxyPath, proxyUrl, proxyErr;
    std::vector<Sidecar> sidecars;
    uint64_t extSubId = 0;
    std::vector<std::wstring> extSubChunks;   // the playerSubs messages, kept for a page reload
    // WH: the film's own tracks
    std::string tracksState = "none";         // none (not a container) | probing | ready | error
    std::shared_ptr<const pbmedia::Info> tracks;
    int      audioOrd = -1;                   // the track the page must make HEARD (-1 = none known: its default)
    bool     probeWait = false;               // the job waits for the probe (at most kProbeWaitMs)
    uint64_t probeAt = 0;
    bool     posSeen = false;                 // the page has reported a position (a re-track is then not an opening)
    struct Emb { uint64_t id = 0; uint32_t track = 0; };
    std::vector<Emb> embedded;                // embedded subtitle tracks: ids minted with the sidecars' counter
    uint64_t subReq = 0, subReqFor = 0;       // the extraction in flight, and the embedded id it is for
    // SuVtitle fix round (RV4-1): the VIEWER named or loaded a subtitle for this item -- a drop, --play with a
    // subtitle, Load subtitles..., or a playerSubGet without `auto`. From then on the page's automatic choice is
    // ignored for this item (acceptSubGet). A new item = a new Session = no mark.
    bool     userSub = false;
};

// Everything the core cannot do itself. bridge.cpp implements it; player_smoke.cpp fakes it.
class Host {
public:
    virtual ~Host() = default;
    // the helper (TranslatorManager behind it; whether one is running is told by setHelperAvailable)
    virtual void sendFile(const tp::WireFile& f) = 0;
    virtual void sendFocus(uint64_t wire, uint64_t ms) = 0;
    virtual void sendCancel(uint64_t id) = 0;
    virtual void sendProxy(const tp::WireProxy& p) = 0;
    // the pages
    virtual void postBoth(const std::wstring& json) = 0;       // both views: the card shows the progress
    virtual void postWindow(const std::wstring& json) = 0;     // the translator view only
    virtual void postMain(const std::wstring& json) = 0;       // the main view only (the window is reloading)
    virtual void openWindow() = 0;
    // the window's folder mappings: set exactly these (clearing the others); `failed` = could not be mapped
    virtual void mapFolders(const std::vector<std::wstring>& folders, std::vector<std::wstring>& failed) = 0;
    virtual void reloadWindow() = 0;
    // the disk
    virtual bool identity(const std::wstring& path, uint64_t& size, uint64_t& mtime) = 0;   // false: gone / a folder
    virtual bool readCache(const std::string& key, pbtrf::Record& r) = 0;
    virtual bool peekCache(const std::string& key, pbtrf::Summary& s) = 0;
    virtual void writeCache(const std::string& key, const pbtrf::Record& r) = 0;
    // WG: the file of ONE language of `set` (its folder, name, identity), holding `cues`' text in `lang` (the set's
    // own cues, or a more complete analysis of the same file). Written only when what is on disk differs; never over
    // a file that is not this media's (TrFileStore.h naming rules). Returns its path ("" = not written, `note` says
    // why) and its identity as written.
    // WH (D6): `fromKey` / `fromOrd` = the cache record whose cues these are (the set's own, or a more complete sibling's)
    // and its sound track -- the registry remembers them with the file.
    virtual std::wstring writeSrt(const CueSet& set, const std::string& lang, const std::vector<tp::WireCue>& cues,
                                  const std::string& fromKey, uint32_t fromOrd,
                                  std::wstring& note, uint64_t& size, uint64_t& mtime) = 0;
    // WH (D6): the cache key of the record whose cues the automatic file of `lang` for this media holds NOW (the
    // registry; "" = none known, or the file changed since it was written).
    virtual std::string srtWriter(const CueSet& set, const std::string& lang) = 0;
    virtual std::wstring proxyPath(const std::string& mediaKey, uint64_t id) = 0;   // "" = nowhere to write it
    virtual void deleteFile(const std::wstring& path) = 0;
    virtual std::vector<Sidecar> sidecars(const std::wstring& folder, const std::wstring& leaf) = 0;
    virtual void saveQueue(const std::string& bytes) = 0;
    virtual void saveResume(const std::string& bytes) = 0;
    virtual void saveAudio(const std::string& bytes) = 0;     // WH: %APPDATA%\SuVtitle\player-audio.txt
    // WH: the film's own tracks (MediaTracks.h), on WORKER threads. probeTracks -> Player::onTracks(req, info);
    // extractSub -> Player::onSubExtracted(req, result); cancelExtract = that request is no longer wanted (it stops
    // at its next cancel check; an answer that still comes is only kept in the cache).
    virtual void probeTracks(uint64_t req, const std::wstring& path) = 0;
    virtual void extractSub(uint64_t req, const std::wstring& path, std::shared_ptr<const pbmedia::Info> info, uint32_t track) = 0;
    virtual void cancelExtract(uint64_t req) = 0;
    // time
    virtual uint64_t nowMs() = 0;
    virtual uint64_t epochSec() = 0;
};

class Player {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    explicit Player(Host& h) : host_(h) {}

    // ---- what the engine runs (STATE echo), and the quality the window asks for ----------------
    void setPair(const std::string& a, const std::string& b, const std::string& mode) {
        if (a == langA_ && b == langB_ && mode == mode_) return;
        langA_ = a; langB_ = b; mode_ = mode;
        cancelPre();                   // a pre-analysis for the old pair is not what the user will want
        refreshSummaries();
        schedule();
        flush();
    }
    void setQuality(const std::string& q) {
        const std::string v = q == "max" ? "max" : "auto";
        if (v == quality_) return;
        quality_ = v;
        cancelPre();
        refreshSummaries();
        schedule();
        flush();
    }
    void setHelperAvailable(bool on) {
        if (on == helper_) return;
        helper_ = on;
        if (!on) {
            // The card was removed: no FDONE will come for what was running. Each job ends here as
            // if the helper had died: its VOUCHED part is saved, the rest is analysed again later.
            std::vector<Job> js;
            js.swap(jobs_);
            for (Job& j : js) {
                endJobSet(j, "error", true);
                if (loaded_ && s_.wire == j.wire) {
                    s_.wire = 0;
                    s_.cancelling = false;
                    s_.failed = true;
                    s_.err = L"The SuVtitle engine was stopped.";
                    stateDirty_ = true;
                }
            }
            if (loaded_ && s_.proxyState == "working") {     // its sound will not come either
                s_.proxyState = "failed";
                s_.proxyErr = L"the SuVtitle engine was stopped";
                stateDirty_ = true;
            }
        }
        schedule();
        flush();
    }

    // ---- persistence ---------------------------------------------------------------------------------
    // WH: player-audio.txt -- the user's sound-track choices (before load(): the badges' keys depend on them)
    void loadAudio(const std::string& audioText) { parseAudioPicks(audioText, picks_); }
    void load(const std::string& queueText, const std::string& resumeText) {
        parseResume(resumeText, resume_);
        QueueFile q;
        if (parseQueue(queueText, q)) {
            translate_ = q.translate;
            repeat_ = q.repeat;
            shuffle_ = q.shuffle;
            items_.clear();
            for (const auto& it : q.items) items_.push_back(itemOf(it.path, it.size, it.mtime, it.durMs));
            cur_ = (q.cur >= 0 && static_cast<size_t>(q.cur) < items_.size()) ? static_cast<size_t>(q.cur) : npos;
            order_.clear();
            if (shuffle_) {
                if (!q.order.empty()) for (size_t i : q.order) order_.push_back(items_[i].id);
                else reshuffle();
            }
        }
        refreshSummaries();
        queueDirty_ = true;
    }

    // ---- the queue ------------------------------------------------------------------------------------
    // A file's identity when the caller already HAS it: a folder scan reads size and write time from
    // its own directory listing, on its worker (review of WE, P2: one GetFileAttributesExW per file on
    // the UI thread froze the tray for seconds on a network share). `known[k]` goes with `paths[k]`.
    struct Known { uint64_t size = 0, mtime = 0; bool ok = false; };
    // Files to PLAY: inserted right after the current item, the first one starts now. A file already
    // in the queue is not added twice (dropping the same film again just plays it), nor a path given
    // twice in one call.
    void open(const std::vector<std::wstring>& paths, bool forceTranslate, const std::vector<Known>* known = nullptr) {
        if (forceTranslate && !translate_) { translate_ = true; queueSaveDirty(); }
        // The FIRST of `paths` plays -- remembered by its ID, never its index: the files inserted right
        // after the current item shift every later index (review of WE, C2: the WRONG item played).
        uint64_t firstId = 0;
        std::vector<Item> add;
        std::map<std::wstring, uint64_t> have = pathIndex();   // O(n log n), not a scan per path (P2)
        for (size_t k = 0; k < paths.size(); ++k) {
            const std::wstring l = lowerW(paths[k]);
            const auto ex = have.find(l);
            if (ex != have.end()) { if (!firstId && add.empty()) firstId = ex->second; continue; }
            Item it;
            if (!newItem(paths[k], it, known && k < known->size() ? &(*known)[k] : nullptr)) continue;
            have[l] = it.id;
            add.push_back(it);
        }
        if (!add.empty()) {
            const size_t at = (cur_ == npos) ? items_.size() : cur_ + 1;
            if (insertItems(at, add) && !firstId) firstId = add.front().id;
        }
        if (firstId) loadItem(indexOf(firstId), true, -1);
        flush();
    }
    // Files to QUEUE: appended. Nothing loaded yet: the first becomes current, ready, not playing.
    void add(const std::vector<std::wstring>& paths, const std::vector<Known>* known = nullptr) {
        std::vector<Item> add;
        std::map<std::wstring, uint64_t> have = pathIndex();
        for (size_t k = 0; k < paths.size(); ++k) {
            const std::wstring l = lowerW(paths[k]);
            if (have.count(l)) continue;
            Item it;
            if (!newItem(paths[k], it, known && k < known->size() ? &(*known)[k] : nullptr)) continue;
            have[l] = it.id;
            add.push_back(it);
        }
        const size_t at = items_.size();
        const size_t n = insertItems(at, add);
        if (n && !loaded_ && cur_ == npos) loadItem(at, false, -1);
        flush();
    }
    void queueOp(const std::string& op, uint64_t id, long long to, uint64_t ms) {
        const size_t idx = indexOf(id);
        if (op == "play" && idx != npos && loaded_ && s_.itemId == id) {
            postCmd("restart", 0);                         // the item already playing: from the top
        } else if (op == "play" && idx != npos) {
            loadItem(idx, true, -1);
        } else if (op == "remove" && idx != npos) {
            removeAt(idx);
        } else if (op == "move" && idx != npos) {
            Item it = items_[idx];
            const uint64_t curId = cur_ != npos ? items_[cur_].id : 0;
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(idx));
            size_t dst = to < 0 ? 0 : static_cast<size_t>(to);
            if (dst > items_.size()) dst = items_.size();
            items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(dst), it);
            cur_ = curId ? indexOf(curId) : npos;
            queueChanged();
        } else if (op == "clear") {
            const uint64_t keep = loaded_ ? s_.itemId : 0;
            std::vector<Item> left;
            for (const auto& it : items_) {
                if (it.id == keep) left.push_back(it);
                else cancelPreFor(it);
            }
            items_ = left;
            cur_ = keep ? 0 : npos;
            if (shuffle_) reshuffle();
            queueChanged();
        } else if (op == "next") {
            advance(+1);
        } else if (op == "prev") {
            if (loaded_ && ms > kPrevRestartMs) postCmd("restart", 0);
            else advance(-1);
        }
        schedule();
        flush();
    }
    void setTranslate(bool on, uint64_t ms) {
        if (on == translate_) return;
        translate_ = on;
        queueSaveDirty();
        if (!on) {
            for (Job& j : jobs_) cancelJob(j);   // their partial results are saved on their FDONE
        } else if (loaded_) {
            s_.userCancelled = false;
            s_.failed = false;
            s_.err.clear();
            if (ms) s_.lastPosMs = ms;
            // WH: the user switched the sound while translation was off -- the analysis goes to what he HEARS now
            if (s_.set && heardSel().ord != s_.set->audio.ord) retrack(heardSel());
            else ensureCurrentJob(s_.lastPosMs);
        }
        stateDirty_ = queueDirty_ = true;
        schedule();
        flush();
    }
    void setRepeat(Repeat r) { if (r != repeat_) { repeat_ = r; queueChanged(); schedule(); } flush(); }
    void setShuffle(bool on) {
        if (on != shuffle_) { shuffle_ = on; if (on) reshuffle(); else order_.clear(); queueChanged(); schedule(); }
        flush();
    }
    void seedShuffle(uint64_t seed) { rng_ = seed ? seed : 0x9E3779B97F4A7C15ULL; }

    // ---- the playhead -----------------------------------------------------------------------------------
    void pos(uint64_t gen, uint64_t ms, uint64_t durMs, bool playing, const std::string& reason) {
        if (!loaded_ || gen != s_.gen) return;
        const PosReason why = reasonFrom(reason);
        s_.lastPosMs = ms;
        s_.playing = playing;
        s_.posSeen = true;
        if (durMs && durMs != s_.durMs) {
            s_.durMs = durMs;
            const size_t i = indexOf(s_.itemId);
            if (i != npos && items_[i].durMs != durMs) { items_[i].durMs = durMs; queueSaveDirty(); queueDirty_ = true; }
        }
        noteResume(ms, why == PosReason::Ended);
        // WG: a pause is a moment the user may look at the files (VLC, another player): they hold what is analysed
        if ((why == PosReason::Pause || why == PosReason::Ended) && s_.set && s_.set->srtDirty) writeSrts(*s_.set);
        if (why == PosReason::Ended) { autoAdvance(); flush(); return; }
        if (s_.wire) {
            Job* j = job(s_.wire);
            if (j && shouldFocus(j->set->ranges, j->set->totalMs ? j->set->totalMs : s_.durMs, ms, s_.lastFocusMs, why)) {
                host_.sendFocus(s_.wire, ms);
                s_.lastFocusMs = ms;
            }
        } else {
            ensureCurrentJob(ms);
        }
        flush();
    }

    // ---- the strip's buttons ------------------------------------------------------------------------------
    // force = discard what this file has (complete or partial) and analyse it all again; else resume
    // from the vouched ranges -- or load the cache of the pair running NOW (the "languages changed" case).
    void analyse(bool force) {
        if (!loaded_) return;
        // the set going away first writes an edit not saved yet (the 2 s rule had not come: review of WG #15)
        if (s_.set && s_.set->editDirty) saveEdits(*s_.set);
        if (s_.wire) { if (Job* j = job(s_.wire)) cancelJob(*j); s_.wire = 0; }
        if (!translate_) { translate_ = true; queueSaveDirty(); }
        auto set = makeSet(s_.path, s_.name, s_.folder, s_.size, s_.mtime, heardSel());   // WH: the track HEARD now
        const auto keep = s_.set;
        if (!force) {
            // resume what is held -- as a COPY of its vouched part: the job just cancelled may still say
            // a last word into the old set, and none of it must land in the one being shown
            if (keep && keep->key == set->key && !keep->complete) set = vouchedCopy(*keep);
            else if (Job* pj = jobForKey(set->key)) set = pj->set;                 // a pre-analysis has it
            else loadCacheInto(*set);
        } else {
            set->hasVideo = keep ? keep->hasVideo : false;
            set->videoKnown = keep ? keep->videoKnown : false;
            set->totalMs = keep ? keep->totalMs : 0;
            // ENGINE: the user confirmed re-analysing an older engine's (edited) result -- the strip says why
            set->upgrade = keep && keep->key == set->key && pbtrf::olderEngine(keep->engine);
        }
        s_.set = set;
        s_.gen = next();
        s_.userCancelled = s_.failed = s_.cancelling = false;
        s_.err.clear();
        s_.autoplay = false;
        s_.startMs = s_.lastPosMs;
        if (Job* pj = jobForKey(set->key)) { pj->pre = false; s_.wire = pj->wire; }
        else ensureCurrentJob(s_.lastPosMs);
        postStateNow();
        postCuesNow();
        schedule();
        flush();
    }
    // ---- WG: THE SUBTITLE EDITOR (the user, 11/10: "editer le fichier sous-titre, en incluant modification du timing
    // des phrases"). The PAGE computes every edit -- a text, a start or an end, a split, a merge, a deletion, a new
    // line, a shift of everything after -- as "remove these cues, add these"; the core CHECKS it against what a cue
    // set must stay (no overlap; at least 0.1 s; inside the file; while the analysis runs, inside what is analysed:
    // the helper never writes there again) and applies it, or refuses it with a sentence. The answer goes back to
    // the window (translatorFileEdited); the set is marked EDITED, and its cache record and its per-language files
    // follow 2 s after the last edit. Undo/redo are the page's: an inverse edit, checked the same way.
    struct EditCue { uint64_t s = 0, e = 0; std::string lang, dstLang, src, dst; bool pass = false; };
    static constexpr uint64_t kMinCueMs = 100;           // the page's minimum for a cue the user makes (not checked here)
    static constexpr size_t   kMaxEditCues = 20000, kMaxEditText = 4000;
    bool editCues(uint64_t gen, uint64_t rev, const std::vector<uint32_t>& remove, const std::vector<EditCue>& add) {
        std::wstring err;
        std::vector<tp::WireCue> added;
        const bool ok = applyEdit(gen, remove, add, added, err);
        JsonW j;
        j.obj();
        j.kv(L"type", L"translatorFileEdited");
        j.kv(L"jobId", static_cast<long long>(gen));
        j.kv(L"rev", static_cast<long long>(rev));
        j.kv(L"ok", ok);
        if (!ok) j.kv(L"err", err);
        j.key(L"remove").arr();
        if (ok) for (uint32_t i : remove) j.num(static_cast<long long>(i));
        j.end();
        j.key(L"add").arr();
        for (const auto& c : added) { j.obj(); cueJson(j, c); j.end(); }
        j.end();
        j.kv(L"edited", loaded_ && s_.set ? s_.set->edited : false);
        j.end();
        if (!windowBusy()) host_.postWindow(j.take());
        if (ok) { stateDirty_ = true; flush(); }      // the strip says "edited"; the card's count follows
        return ok;
    }

    void cancel(uint64_t gen) {
        if (!loaded_ || gen != s_.gen || !s_.wire) return;
        if (Job* j = job(s_.wire)) cancelJob(*j);
        s_.cancelling = true;
        s_.userCancelled = true;
        stateDirty_ = true;
        flush();
    }
    // Stop and unload the current item (its job is cancelled, its partial result kept). The queue stays.
    void close() {
        if (!loaded_) return;
        endSession();
        s_ = Session{};
        s_.gen = next();
        loaded_ = false;
        stateDirty_ = queueDirty_ = true;
        schedule();
        flush();
    }
    void playFailed(uint64_t seq) {
        if (!loaded_ || seq != s_.seq) return;
        requestProxy();
        flush();
    }

    // ---- external subtitles (parsed by the bridge, SubtitleReader.h) ------------------------------------
    bool sidecarPath(uint64_t id, std::wstring& path) const {
        if (!loaded_) return false;
        for (const auto& s : s_.sidecars) if (s.id == id) { path = s.path; return true; }
        return false;
    }
    uint64_t newSubId() { return next(); }
    // playerSubGet's `auto` (SuVtitle fix round 2026-10-23, review RV4-1 = SPLIT_PLAN ADDENDUM 2). auto = the page's
    // AUTOMATIC default (a sidecar or an embedded track its default rule picked, never the viewer); without it = the
    // viewer's own pick, always honoured. False = IGNORE the request -- no answer, no notice: an automatic one for an
    // item whose subtitle the viewer already named or loaded. Before, the page's default arrived AFTER a subtitle
    // dropped with the film (open, then the file, then the page's answer to the new state) and replaced it. The mark
    // is taken when the viewer's request ARRIVES (userSubFile, or here), so no timing is involved: a file that is slow
    // to come, or an automatic request already on its way, cannot win over it. (id 0 = the viewer abandoning a
    // pending request: no pick, no mark.)
    bool acceptSubGet(uint64_t id, bool isAuto) {
        if (!loaded_) return true;              // (nothing to protect: the request fails as it always did)
        if (isAuto) return !s_.userSub;
        if (id) s_.userSub = true;
        return true;
    }
    // A subtitle FILE the viewer named for the current item (a drop, --play with a subtitle, Load subtitles...),
    // called BEFORE it is read: the item is marked, and an extraction still running (an automatic one, or an older
    // pick) is cancelled -- it must not land on top of the file, whether the file reads fine or not.
    void userSubFile() {
        if (!loaded_) return;
        s_.userSub = true;
        cancelSubExtract();
    }
    bool userSubChosen() const { return loaded_ && s_.userSub; }
    void setExtSub(uint64_t id, const std::vector<std::wstring>& chunks) {
        if (!loaded_) return;
        cancelSubExtract();                    // WH: a file loaded now wins over an embedded track still being read
        showExt(id, chunks);
    }

    // ---- WH: the film's own tracks ------------------------------------------------------------------------
    // A probe's answer (any item's: the current one, a pre-analysis candidate, one seen earlier). info = null: the file
    // could not be read at all (= error).
    void onTracks(uint64_t req, std::shared_ptr<const pbmedia::Info> info) {
        auto rq = probeReqs_.find(req);
        if (rq == probeReqs_.end()) return;                   // (never asked, or already answered)
        const std::string mk = rq->second;
        probeReqs_.erase(rq);
        ProbeEntry& p = probes_[mk];
        p.info = info;
        p.state = !info ? "error" : info->state == pbmedia::Info::State::Ready ? "ready"
                : info->state == pbmedia::Info::State::None ? "none" : "error";
        p.req = 0;
        trimProbes();
        // the badges of this file were judged without its tracks (its key and the pre-WH rule may change): again
        for (const auto& it : items_)
            if (pbtrf::mediaKey(it.path, it.size, it.mtime) == mk) { summaries_.erase(keyOf(it)); markItem(it.id); }
        if (loaded_ && s_.mediaKey == mk && s_.tracksState == "probing") applyTracks(probes_[mk]);
        schedule();
        flush();
    }
    // playerSet{audioOrd: k[, auto: true]} -- the user chose a sound track in the page (persisted for this file), or the
    // page could not play the tray's choice and plays k instead (`autoPick`: the analysis follows what is HEARD, the
    // choice is not the user's and is not kept). D5: with translation on, a track other than the analysed one starts a
    // NEW GENERATION on it -- its own cache record, resumed from what it has; with translation off only what is heard
    // changes (the analysis goes there when translation is turned back on).
    void setAudio(int ord, bool autoPick) {
        if (!loaded_ || s_.tracksState != "ready" || !s_.tracks || ord < 0 || static_cast<size_t>(ord) >= s_.tracks->audio.size()) return;
        if (proxied() && ord != 0) return;                     // (WHP-2: the proxy carries the first track only)
        const pbmedia::AudioTrack& t = s_.tracks->audio[static_cast<size_t>(ord)];
        if (!autoPick) {
            AudioPick& p = picks_[s_.mediaKey];
            p.ord = t.ord;
            p.trackId = t.track;
            p.epoch = host_.epochSec();
            audioDirty_ = true;
            audioChangedAt_ = host_.nowMs();
        }
        const bool moved = ord != s_.audioOrd;
        s_.audioOrd = ord;
        if (translate_ && s_.set && s_.set->audio.ord != t.ord) retrack(heardSel());
        else if (moved) stateDirty_ = true;
        flush();
    }
    // playerSubGet{id} for an EMBEDDED subtitle track: its extraction is asked for (or its text, already read, posted
    // at once). False = not one of this item's embedded ids (a sidecar's, a file's: the bridge reads those).
    bool subGet(uint64_t id) {
        if (!loaded_ || !s_.tracks) return false;
        const Session::Emb* e = nullptr;
        for (const auto& x : s_.embedded) if (x.id == id) e = &x;
        if (!e) return false;
        const pbmedia::SubTrack* st = s_.tracks->sub(e->track);
        if (!st) return true;
        if (!st->ok) {                                          // pictures, encrypted, unknown compression...: said
            notice(w16(st->why.empty() ? pbmedia::errFormat(st->codec) : st->why));
            return true;
        }
        const std::string key = s_.mediaKey + "#" + std::to_string(e->track);
        for (auto it = subLru_.begin(); it != subLru_.end(); ++it) {
            if (it->first != key) continue;
            auto hit = it->second;
            subLru_.splice(subLru_.begin(), subLru_, it);       // most recent first
            cancelSubExtract();
            showExt(id, embeddedChunks(id, *st, *hit));
            return true;
        }
        cancelSubExtract();                                     // a new request supersedes the one running (D7)
        s_.subReq = next();
        s_.subReqFor = id;
        subReqs_[s_.subReq] = key;
        host_.extractSub(s_.subReq, s_.path, s_.tracks, e->track);
        return true;
    }
    bool isEmbeddedSub(uint64_t id) const {
        if (!loaded_) return false;
        for (const auto& x : s_.embedded) if (x.id == id) return true;
        return false;
    }
    // playerSubGet{id:0} (the director, from the page's review WHP-1): the viewer ABANDONED a pending embedded request --
    // its extraction is cancelled and NOTHING is said about it: no subtitles, no failure sentence, extSubId untouched (a
    // result still coming is only kept in the LRU).
    void cancelSubRequest() { cancelSubExtract(); }
    void onSubExtracted(uint64_t req, std::shared_ptr<const pbmedia::SubResult> r) {
        auto rq = subReqs_.find(req);
        if (rq == subReqs_.end()) return;
        const std::string key = rq->second;
        subReqs_.erase(rq);
        if (r && r->ok) {                                       // kept even when no longer wanted: asked again, at once
            for (auto it = subLru_.begin(); it != subLru_.end(); ++it) if (it->first == key) { subLru_.erase(it); break; }
            subLru_.emplace_front(key, r);
            while (subLru_.size() > kSubLruKeep) subLru_.pop_back();
        }
        if (!loaded_ || req != s_.subReq) return;               // superseded, cancelled, another item: nothing to show
        s_.subReq = 0;
        const uint64_t id = s_.subReqFor;
        uint32_t track = 0;
        for (const auto& x : s_.embedded) if (x.id == id) track = x.track;
        const pbmedia::SubTrack* st = s_.tracks ? s_.tracks->sub(track) : nullptr;
        if (!r || !r->ok || !st) {
            const std::string err = r ? r->err : std::string();
            if (r && err.empty()) return;                       // (cancelled by its own worker: nobody waits for it)
            notice(w16(err.empty() ? std::string(pbmedia::errDamaged()) : err));
            return;
        }
        showExt(id, embeddedChunks(id, *st, *r));
    }
    // WG editor: an edited subtitle FILE's parts, kept for a page reload (not posted: the page has the edit already)
    void keepExtSub(uint64_t id, const std::vector<std::wstring>& chunks) {
        if (loaded_ && s_.extSubId == id) s_.extSubChunks = chunks;
    }
    void notice(const std::wstring& text) {
        JsonW j;
        j.obj().kv(L"type", L"translatorFileNotice").kv(L"text", text).end();
        if (windowBusy()) { if (heldNotices_.size() < 8) heldNotices_.push_back(j.take()); return; }
        host_.postWindow(j.take());
    }

    // ---- the window ------------------------------------------------------------------------------------------
    // Before the view's first navigation: its mappings, so the page that loads sees them.
    void windowAttached() {
        attached_ = true;
        viewLoaded_ = viewReady_ = false;
        committed_.clear();
        mapNow();
    }
    void windowDetached() {
        attached_ = viewLoaded_ = viewReady_ = false;
        committed_.clear();
        mapped_.clear();
        heldNotices_.clear();
    }
    // The translator page said `ready`: what was mapped before it navigated is what it can load, and it
    // gets everything back -- the session, every cue, the queue, the external subtitle, and the notices
    // said while it was (re)loading.
    void windowReady() {
        viewLoaded_ = viewReady_ = true;
        committed_ = mapped_;
        replay();
        for (const auto& n : heldNotices_) host_.postWindow(n);
        heldNotices_.clear();
    }
    void replay() {
        postStateNow();
        postCuesNow();
        for (const auto& c : s_.extSubChunks) host_.postWindow(c);
        queueDirty_ = true;
        flush();
    }
    void mainReady() { stateDirty_ = true; flush(); }   // the main page only needs the state (the card)
    void windowShown(bool on) { shown_ = on; schedule(); flush(); }
    void view(const std::string& mode) { view_ = mode == "file" ? "file" : "live"; schedule(); flush(); }

    // ---- the helper -----------------------------------------------------------------------------------------
    void onProg(const tp::WireFileProg& p) {
        Job* j = job(p.jobId);
        if (!j) return;
        CueSet& S = *j->set;
        j->phase = p.phase == "analyzing" ? "analyzing" : "loading";
        if (p.totalMs) S.totalMs = p.totalMs;
        if (!p.asrModel.empty()) S.asr = w16(p.asrModel);
        if (!p.mtModel.empty()) S.mt = w16(p.mtModel);
        if (p.totalMs || p.phase == "analyzing") { S.hasVideo = p.hasVideo; S.videoKnown = true; }
        std::vector<tp::MsRange> rg;
        if (tp::parseRanges(p.ranges, rg)) {
            // TRUSTED RANGES: this message vouches for the PREVIOUS report of the same job
            S.confirmed = merged(S.confirmed, j->lastProg);
            j->lastProg = rg;
            S.ranges = merged(rg, S.confirmed);
            S.dirty = true;
        }
        if (isCurrent(*j)) stateDirty_ = true;
        else markKey(S.key);
    }
    void onCue(const tp::WireCue& c) {
        Job* j = job(c.jobId);
        if (!j) return;
        if (c.idx < j->nextIdx) return;                    // a duplicate
        j->nextIdx = c.idx + 1;
        CueSet& S = *j->set;
        if (overlapsHeld(S, c.startMs, c.endMs)) return;   // never two cues at one moment (TrPipe.h promises it)
        tp::WireCue k = c;
        touchUp(S, k);                                     // ...a 1 ms TOUCH is not an overlap: it is clamped away
        k.idx = S.nextIdx++;                               // WG: a counter, not the size (an edit may delete cues)
        S.cues.push_back(k);
        S.spans[k.startMs] = k.endMs;
        S.dirty = true;
        markSrt(S);                                        // WG: the per-language files follow, within 30 s
        if (isCurrent(*j)) postCue(k);
    }
    void onDone(const tp::WireFileDone& d, bool synthesized) {
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& x) { return x.wire == d.jobId; });
        if (it == jobs_.end()) return;
        Job j = *it;
        jobs_.erase(it);
        if (!synthesized) j.set->confirmed = merged(j.set->confirmed, j.lastProg);   // the helper's own last word
        const bool current = loaded_ && s_.wire == j.wire;
        endJobSet(j, d.result, synthesized);
        if (current) {
            s_.wire = 0;
            s_.cancelling = false;
            if (d.result == "error") {
                s_.failed = true;
                s_.err = d.err.empty() ? std::wstring(L"The analysis failed.") : w16(d.err);
            }
            stateDirty_ = true;
        } else if (j.pre && d.result == "error") {
            failedKeys_.insert(j.set->key);
        }
        markKey(j.set->key);
        schedule();
        flush();
    }
    void onProxyDone(const tp::WireProxyDone& d) {
        if (!loaded_ || !s_.proxyReq || d.jobId != s_.proxyReq) return;
        if (d.ok) {
            s_.proxyState = "ready";
            s_.proxyUrl = std::wstring(L"https://") + kProxyHost + L"/" + w16(pbtrf::pctEncode(u8(pbtrf::leafOf(s_.proxyPath))));
        } else if (d.err == "superseded") {
            return;
        } else {
            s_.proxyState = "failed";
            s_.proxyErr = w16(d.err);
        }
        stateDirty_ = true;
        flush();
    }

    // ---- time -----------------------------------------------------------------------------------------------
    void tick() {
        const uint64_t now = host_.nowMs();
        for (Job& j : jobs_)
            if (j.set->dirty && now - j.set->savedAt >= kCheckpointMs) savePartial(*j.set);
        // WG: the edits of the set on screen (2 s after the last one), and every set's per-language files (30 s
        // after the first cue they lack) -- the job's and the session's, which may be the same set
        if (loaded_ && s_.set && s_.set->editDirty && now - s_.set->editAt >= kEditSaveMs) saveEdits(*s_.set);
        for (CueSet* S : liveSets())
            if (S->srtDirty && now - S->srtDirtySince >= kSrtEveryMs) writeSrts(*S);
        if (queueSave_ && now - queueChangedAt_ >= kQueueSaveMs) saveQueueNow();
        if (resumeDirty_ && now - resumeSavedAt_ >= kResumeSaveMs) saveResumeNow();
        if (audioDirty_ && now - audioChangedAt_ >= kAudioSaveMs) saveAudioNow();
        // WH: a probe that does not answer (a sleeping disk, a slow share) does not hold the analysis for long: the job
        // starts on the provisional track; a late answer is still applied (and may re-track)
        if (loaded_ && s_.probeWait && now - s_.probeAt >= kProbeWaitMs) {
            s_.probeWait = false;
            ensureCurrentJob(s_.lastPosMs);
        }
        schedule();
        flush();
    }
    // The tray is quitting: every running job's VOUCHED part is saved (FDONE will not come), and the
    // queue and the positions are written now.
    void shutdown() {
        flushFiles();
        for (Job& j : jobs_) savePartial(*j.set);
        saveQueueNow();
        saveResumeNow();
        saveAudioNow();
        cancelSubExtract();
        if (loaded_ && !s_.proxyPath.empty()) host_.deleteFile(s_.proxyPath);   // kept only while played
    }
    // The Windows SESSION is ending (logoff, shutdown -- WM_QUERYENDSESSION / WM_ENDSESSION): the process
    // may be killed without a WM_DESTROY, so what shutdown() would write is written NOW -- the vouched part
    // of every running job, the queue, the positions (review of WE, P8: up to 2 min of analysis and 15 s
    // of positions were lost). Nothing is stopped: the session's end may still be cancelled.
    void persist() {
        flushFiles();
        for (Job& j : jobs_) savePartial(*j.set);
        saveQueueNow();
        saveResumeNow();
        saveAudioNow();
    }

    // ---- read-only views (export, reveal, the bridge's own needs) -------------------------------------------
    bool loaded() const { return loaded_; }
    const Session& session() const { return s_; }
    bool translate() const { return translate_; }
    Repeat repeat() const { return repeat_; }
    bool shuffle() const { return shuffle_; }
    const std::vector<Item>& items() const { return items_; }
    size_t current() const { return cur_; }
    const std::vector<Job>& jobs() const { return jobs_; }
    const std::string& quality() const { return quality_; }
    std::vector<uint64_t> playOrderIds() const { std::vector<uint64_t> v; for (size_t i : playOrder()) v.push_back(items_[i].id); return v; }
    const std::map<std::string, AudioPick>& audioPicks() const { return picks_; }   // WH (tests)

    // Coalesced posts: the state and the queue at most once per call site.
    void flush() {
        if (stateDirty_) postStateNow();
        if (queueDirty_) postQueueNow();
        for (uint64_t id : itemDirty_) postItem(id);
        itemDirty_.clear();
    }

private:
    Host& host_;
    // what the engine runs / what the user chose
    std::string langA_ = "en", langB_ = "zh", mode_ = "auto", quality_ = "auto";
    bool translate_ = true, helper_ = false;
    Repeat repeat_ = Repeat::Off;
    bool shuffle_ = false;
    // the queue
    std::vector<Item> items_;
    size_t cur_ = npos;
    std::vector<uint64_t> order_;      // the shuffle order (item ids); empty when not shuffling
    uint64_t nextItemId_ = 0, rng_ = 0x9E3779B97F4A7C15ULL;
    // the current item, the jobs
    Session s_;
    bool loaded_ = false;
    std::vector<Job> jobs_;
    uint64_t counter_ = 0, seqCounter_ = 0;
    // the window
    bool attached_ = false, viewLoaded_ = false, viewReady_ = false, shown_ = false;
    std::string view_ = "live";
    std::set<std::wstring> committed_, mapped_;   // lower-cased folders: seen by the page / mapped now
    uint64_t windowSaw_ = 0;                      // the generation the window page has been told about
    std::vector<std::wstring> heldNotices_;       // said while the page was (re)loading: posted on its `ready`
    // The window's page is (re)loading: NOTHING is posted to it -- during a Reload() the message would
    // reach the OLD document, which would try the new item's URL on a host its mapping does not have,
    // fail, and ask for the sound-only proxy that later replaces the picture (review of WE, P1). Its
    // `ready` brings everything back (windowReady -> replay).
    bool windowBusy() const { return attached_ && !viewReady_; }
    // bookkeeping
    std::set<std::string> failedKeys_;            // pre-analyses that failed this run (not retried)
    std::map<std::string, pbtrf::Summary> summaries_;
    std::map<std::string, ResumeEntry> resume_;
    bool stateDirty_ = false, queueDirty_ = false, queueSave_ = false, resumeDirty_ = false;
    std::set<uint64_t> itemDirty_;
    uint64_t queueChangedAt_ = 0, resumeSavedAt_ = 0;
    // WH: the film's own tracks
    struct ProbeEntry { std::string state; std::shared_ptr<const pbmedia::Info> info; uint64_t req = 0, seq = 0; };
    std::map<std::string, ProbeEntry> probes_;            // mediaKey -> its probe (kProbeKeep, the oldest go)
    uint64_t probeSeq_ = 0;
    std::map<uint64_t, std::string> probeReqs_;           // a probe in flight -> its mediaKey
    std::map<std::string, AudioPick> picks_;              // mediaKey -> the user's sound track (player-audio.txt)
    bool audioDirty_ = false;
    uint64_t audioChangedAt_ = 0;
    std::list<std::pair<std::string, std::shared_ptr<const pbmedia::SubResult>>> subLru_;   // "mediaKey#track", newest first
    std::map<uint64_t, std::string> subReqs_;             // an extraction in flight -> "mediaKey#track"

    uint64_t next() { return ++counter_; }
    uint64_t rand64() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17; return rng_; }

    // ---- items ----------------------------------------------------------------------------------------------
    Item itemOf(const std::wstring& path, uint64_t size, uint64_t mtime, uint64_t durMs) {
        Item it;
        it.id = ++nextItemId_;
        it.path = path;
        it.name = pbtrf::leafOf(path);
        it.folder = pbtrf::dirOf(path);
        it.dirName = pbtrf::leafOf(it.folder);
        it.size = size;
        it.mtime = mtime;
        it.durMs = durMs;
        return it;
    }
    bool newItem(const std::wstring& path, Item& out, const Known* known) {
        uint64_t size = 0, mtime = 0;
        if (known && known->ok) { size = known->size; mtime = known->mtime; }
        else if (!host_.identity(path, size, mtime)) return false;
        out = itemOf(path, size, mtime, 0);
        return true;
    }
    size_t indexOf(uint64_t id) const {
        for (size_t i = 0; i < items_.size(); ++i) if (items_[i].id == id) return i;
        return npos;
    }
    // every queued path (lower-cased) -> its item id: one pass, then O(log n) per lookup
    std::map<std::wstring, uint64_t> pathIndex() const {
        std::map<std::wstring, uint64_t> m;
        for (const auto& it : items_) m.emplace(lowerW(it.path), it.id);
        return m;
    }
    size_t insertItems(size_t at, std::vector<Item>& add) {
        if (add.empty()) return 0;
        size_t room = items_.size() < kQueueMax ? kQueueMax - items_.size() : 0;
        if (add.size() > room) {
            notice(L"The queue holds " + std::to_wstring(kQueueMax) + L" files at most: "
                   + std::to_wstring(add.size() - room) + L" were not added.");
            add.resize(room);
        }
        if (add.empty()) return 0;
        const uint64_t curId = cur_ != npos ? items_[cur_].id : 0;
        items_.insert(items_.begin() + static_cast<std::ptrdiff_t>(at), add.begin(), add.end());
        if (curId) cur_ = indexOf(curId);
        if (shuffle_) {
            // each new item lands at a random place AFTER the current one in the play order
            size_t after = 0;
            for (size_t k = 0; k < order_.size(); ++k) if (order_[k] == curId) { after = k + 1; break; }
            for (const auto& it : add) {
                const size_t span = order_.size() - after + 1;
                const size_t p = after + static_cast<size_t>(rand64() % span);
                order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(p), it.id);
            }
        }
        for (const auto& it : add) summaryFor(it);
        queueChanged();
        return add.size();
    }
    void removeAt(size_t idx) {
        const Item it = items_[idx];
        if (loaded_ && s_.itemId == it.id) {
            endSession();
            s_ = Session{};
            s_.gen = next();
            loaded_ = false;
            stateDirty_ = true;
        }
        cancelPreFor(it);
        const uint64_t curId = (cur_ != npos && cur_ != idx) ? items_[cur_].id : 0;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(idx));
        order_.erase(std::remove(order_.begin(), order_.end(), it.id), order_.end());
        if (curId) cur_ = indexOf(curId);
        else cur_ = items_.empty() ? npos : std::min(idx, items_.size() - 1);   // the next one, not loaded
        queueChanged();
    }
    void queueChanged() { queueDirty_ = true; queueSaveDirty(); }
    void queueSaveDirty() { queueSave_ = true; queueChangedAt_ = host_.nowMs(); }
    void reshuffle() {
        order_.clear();
        const uint64_t curId = cur_ != npos ? items_[cur_].id : 0;
        std::vector<uint64_t> rest;
        for (const auto& it : items_) if (it.id != curId) rest.push_back(it.id);
        for (size_t i = rest.size(); i > 1; --i) std::swap(rest[i - 1], rest[static_cast<size_t>(rand64() % i)]);
        if (curId) order_.push_back(curId);
        order_.insert(order_.end(), rest.begin(), rest.end());
    }
    // The play order as item indexes: the queue's own order, or the shuffle's.
    std::vector<size_t> playOrder() const {
        std::vector<size_t> v;
        if (!shuffle_) { for (size_t i = 0; i < items_.size(); ++i) v.push_back(i); return v; }
        std::vector<bool> in(items_.size(), false);
        for (uint64_t id : order_) { const size_t i = indexOf(id); if (i != npos && !in[i]) { v.push_back(i); in[i] = true; } }
        for (size_t i = 0; i < items_.size(); ++i) if (!in[i]) v.push_back(i);
        return v;
    }
    // The item `dir` steps away from `from` in the play order (wrap: repeat all, or a user's next/prev).
    size_t step(size_t from, int dir, bool wrap) const {
        const std::vector<size_t> o = playOrder();
        if (o.empty()) return npos;
        if (from == npos) return dir > 0 ? o.front() : o.back();
        size_t p = 0;
        while (p < o.size() && o[p] != from) ++p;
        if (p == o.size()) return o.front();
        if (dir > 0) return p + 1 < o.size() ? o[p + 1] : (wrap ? o.front() : npos);
        return p > 0 ? o[p - 1] : (wrap ? o.back() : npos);
    }
    void advance(int dir) {
        const bool wrap = repeat_ == Repeat::All;
        size_t nx = step(cur_, dir, wrap);
        for (size_t guard = 0; nx != npos && guard < items_.size(); ++guard) {
            if (loadItem(nx, true, -1)) return;
            nx = step(nx, dir, wrap);                      // a file that is gone is skipped
        }
    }
    void autoAdvance() {
        if (repeat_ == Repeat::One) { postCmd("restart", 0); return; }
        advance(+1);
    }

    // ---- the session ----------------------------------------------------------------------------------------------
    std::shared_ptr<CueSet> makeSet(const std::wstring& path, const std::wstring& name, const std::wstring& folder,
                                    uint64_t size, uint64_t mtime, const tp::AudioSel& audio = tp::AudioSel{}) const {
        auto s = std::make_shared<CueSet>();
        s->path = path; s->name = name; s->folder = folder; s->size = size; s->mtime = mtime;
        s->mediaKey = pbtrf::mediaKey(path, size, mtime);
        s->langA = langA_; s->langB = langB_; s->mode = mode_; s->quality = quality_;
        s->audio = audio;
        s->key = pbtrf::cacheKey(path, size, mtime, langA_, langB_, mode_, quality_, audio.ord);
        return s;
    }
    std::string keyOf(const Item& it) const {
        return pbtrf::cacheKey(it.path, it.size, it.mtime, langA_, langB_, mode_, quality_, audioFor(it).ord);
    }

    // ---- WH: the sound track ---------------------------------------------------------------------------------
    // The track an ITEM is analysed on: the current item's set's; else what its probe (or the user's choice) says;
    // else the first.
    tp::AudioSel audioFor(const Item& it) const {
        if (loaded_ && s_.itemId == it.id && s_.set) return s_.set->audio;
        const std::string mk = pbtrf::mediaKey(it.path, it.size, it.mtime);
        const auto pk = picks_.find(mk);
        const AudioPick* pick = pk == picks_.end() ? nullptr : &pk->second;
        const auto p = probes_.find(mk);
        if (p != probes_.end() && p->second.state == "ready" && p->second.info) {
            const int o = initialAudioOrd(*p->second.info, pick);
            return o < 0 ? tp::AudioSel{} : tp::AudioSel{ static_cast<uint32_t>(o), p->second.info->audio[static_cast<size_t>(o)].track };
        }
        // read and NOT a container we know (or unreadable): the page can enforce nothing -- the default track plays and
        // the first is analysed ("" on the wire), whatever was chosen once
        if (p != probes_.end() && (p->second.state == "none" || p->second.state == "error")) return tp::AudioSel{};
        if (pick && probeWorthy(it.path)) return tp::AudioSel{ pick->ord, pick->trackId };   // (provisional, until the probe)
        return tp::AudioSel{};
    }
    // The track HEARD now (the page enforces s_.audioOrd), as the analysis takes it. While the probe runs: the provisional
    // set's track; without a track list (not a container, unreadable, no sound track): the first -- the page can enforce
    // nothing then, the WebView's default plays.
    tp::AudioSel heardSel() const {
        if (s_.tracksState == "probing") return s_.set ? s_.set->audio : tp::AudioSel{};
        if (s_.audioOrd >= 0 && s_.tracks && static_cast<size_t>(s_.audioOrd) < s_.tracks->audio.size())
            return tp::AudioSel{ static_cast<uint32_t>(s_.audioOrd), s_.tracks->audio[static_cast<size_t>(s_.audioOrd)].track };
        return tp::AudioSel{};
    }
    static bool probeWorthy(const std::wstring& path) { return pbmedia::probeWorthyExt(pbtrf::extOf(pbtrf::leafOf(path))); }
    // An mp4/mov (ISO-BMFF) file whose probe lists 2+ sound tracks: where a record made before WH may be another track's
    // (TrFileStore.h unknownTrackMiss). Unknown (no probe, or not answered yet) = no: as before WH.
    bool multiTrackIsoBmff(const std::string& mk) const {
        const auto p = probes_.find(mk);
        if (p == probes_.end() || p->second.state != "ready" || !p->second.info) return false;
        const pbmedia::Info& i = *p->second.info;
        return (i.container == "mp4" || i.container == "mov") && i.audio.size() >= 2;
    }
    // One probe in flight per file; the answer is remembered (kProbeKeep) for the queue's next turns.
    void requestProbe(const std::string& mk, const std::wstring& path) {
        ProbeEntry& p = probes_[mk];
        if (p.state == "probing" || p.state == "ready" || p.state == "none" || p.state == "error") return;
        p.state = "probing";
        p.req = next();
        p.seq = ++probeSeq_;
        probeReqs_[p.req] = mk;
        host_.probeTracks(p.req, path);
    }
    void trimProbes() {
        while (probes_.size() > kProbeKeep) {
            auto oldest = probes_.end();
            for (auto it = probes_.begin(); it != probes_.end(); ++it) {
                if (it->second.state == "probing" || (loaded_ && it->first == s_.mediaKey)) continue;
                if (oldest == probes_.end() || it->second.seq < oldest->second.seq) oldest = it;
            }
            if (oldest == probes_.end()) break;
            probes_.erase(oldest);
        }
    }
    // The current item's probe is in: its tracks are listed (embedded subtitles get their ids), the track to hear is
    // chosen (D3), and the job -- held for it -- starts on that track. A choice other than the provisional one (the set
    // loaded at the opening: the user's choice, else the first track) is a re-track.
    void applyTracks(const ProbeEntry& p) {
        s_.tracksState = p.state == "probing" ? "probing" : p.state;
        s_.tracks = p.state == "ready" ? p.info : nullptr;
        s_.probeWait = false;
        s_.embedded.clear();
        if (s_.tracks) for (const auto& st : s_.tracks->subs) s_.embedded.push_back(Session::Emb{ next(), st.track });
        const auto pk = picks_.find(s_.mediaKey);
        int o = s_.tracks ? initialAudioOrd(*s_.tracks, pk == picks_.end() ? nullptr : &pk->second) : -1;
        if (o > 0 && proxied()) o = 0;                         // (WHP-2: played through the proxy = the first track)
        s_.audioOrd = o;
        stateDirty_ = true;
        const tp::AudioSel want = heardSel();
        // (also when the set shown is a pre-WH record that the probe now says may be another track's: analysed again)
        const bool unsure = s_.set && s_.set->fromCache && pbtrf::unknownTrackMiss(s_.set->trackKnown, s_.set->edited, multiTrackIsoBmff(s_.mediaKey));
        if (s_.set && (want.ord != s_.set->audio.ord || unsure)) { retrack(want, unsure); return; }
        if (s_.set && want.trackId && s_.set->audio.trackId != want.trackId && !s_.wire) s_.set->audio.trackId = want.trackId;
        ensureCurrentJob(s_.lastPosMs);
    }
    // A NEW GENERATION on another sound track of the same item (D5): the set going away writes what it owes (an edit, its
    // files), its job is cancelled (its FDONE saves its partial), and the track's own set comes in -- a pre-analysis'
    // job adopted, a cancelled job's vouched part, its cache record, or nothing yet; then its job (when translating).
    // An opening (the page has reported nothing yet) keeps its start and autoplay; later it is the same file going on
    // where the user has it (review of WE, C5). `fresh` = the set shown was a pre-WH record the probe now distrusts: none of
    // its cues come back (not through the cancelled job's set either) -- analysed from nothing.
    void retrack(const tp::AudioSel& want, bool fresh = false) {
        if (s_.set) {
            if (s_.set->editDirty) saveEdits(*s_.set);
            else if (s_.set->srtDirty) writeSrts(*s_.set);
        }
        if (s_.wire) { if (Job* j = job(s_.wire)) cancelJob(*j); s_.wire = 0; }
        auto set = makeSet(s_.path, s_.name, s_.folder, s_.size, s_.mtime, want);
        if (Job* pj = jobForKey(set->key)) { set = pj->set; pj->pre = false; s_.wire = pj->wire; }
        else if (fresh) { /* nothing: a new analysis */ }
        else if (const CueSet* held = heldSet(set->key)) set = vouchedCopy(*held);
        else loadCacheInto(*set);
        set->audio = want;
        s_.set = set;
        s_.gen = next();
        s_.userCancelled = s_.failed = s_.cancelling = false;
        s_.err.clear();
        if (s_.posSeen) {                                    // (the same rule as a re-post in ensureCurrentJob)
            s_.autoplay = s_.resumed = false;
            s_.startMs = s_.lastPosMs;
        }
        if (!s_.wire) ensureCurrentJob(s_.lastPosMs);
        stateDirty_ = queueDirty_ = true;
        markItem(s_.itemId);
        postStateNow();
        postCuesNow();
    }
    // A set a job still holds though it was cancelled (its FDONE is owed): its vouched part is what the cache will hold.
    const CueSet* heldSet(const std::string& key) const {
        for (const Job& j : jobs_) if (j.set->key == key) return j.set.get();
        return nullptr;
    }
    void cancelSubExtract() {
        if (!s_.subReq) return;
        host_.cancelExtract(s_.subReq);
        s_.subReq = 0;
    }
    void showExt(uint64_t id, const std::vector<std::wstring>& chunks) {
        s_.extSubId = id;
        s_.extSubChunks = chunks;
        if (!windowBusy()) for (const auto& c : chunks) host_.postWindow(c);   // (a reloading page: on its `ready`)
        stateDirty_ = true;
        flush();
    }
    // An embedded track's cues as playerSubs messages -- the sidecars' shape (bridge.cpp extDocChunks) + embedded/track.
    // Read only: each cue's `i` is its position (the editor does not edit these: the bridge refuses).
    std::vector<std::wstring> embeddedChunks(uint64_t id, const pbmedia::SubTrack& st, const pbmedia::SubResult& r) const {
        std::vector<std::wstring> chunks;
        const auto& cues = r.parsed.cues;
        const size_t parts = cues.empty() ? 1 : (cues.size() + kSubChunk - 1) / kSubChunk;
        std::wstring name = w16(st.name);
        if (name.empty()) name = L"Track " + std::to_wstring(st.track) + (st.lang.empty() ? std::wstring() : L" (" + w16(st.lang) + L")");
        for (size_t part = 0; part < parts; ++part) {
            JsonW j;
            j.obj();
            j.kv(L"type", L"playerSubs");
            j.kv(L"id", static_cast<long long>(id));
            j.kv(L"name", name);
            j.kv(L"enc", static_cast<long long>(pbsubr::kCpUtf8));
            j.kv(L"encName", L"UTF-8");
            j.kv(L"forced", false);
            j.kv(L"truncated", r.parsed.truncated || r.ex.truncated);
            j.kv(L"embedded", true);
            j.kv(L"track", static_cast<long long>(st.track));
            j.kv(L"part", static_cast<long long>(part));
            j.kv(L"parts", static_cast<long long>(parts));
            j.key(L"cues").arr();
            for (size_t i = part * kSubChunk; i < cues.size() && i < (part + 1) * kSubChunk; ++i) {
                j.obj();
                j.kv(L"i", static_cast<long long>(i));
                j.kv(L"s", static_cast<long long>(cues[i].startMs));
                j.kv(L"e", static_cast<long long>(cues[i].endMs));
                j.key(L"r").arr();
                for (const auto& run : cues[i].runs) { j.arr(); j.str(w16(run.text)); j.num(run.flags); j.end(); }
                j.end();
                j.end();
            }
            j.end();
            j.end();
            chunks.push_back(j.take());
        }
        return chunks;
    }
    void saveAudioNow() {
        if (!audioDirty_) return;
        host_.saveAudio(packAudioPicks(picks_));
        audioDirty_ = false;
    }
    // A set's vouched part, as a new set (same file and pair): its confirmed ranges and the cues in them.
    static std::shared_ptr<CueSet> vouchedCopy(const CueSet& S) {
        auto c = std::make_shared<CueSet>(S);
        c->cues.clear();
        c->spans.clear();
        trimInto(S, *c);
        c->ranges = c->confirmed = merged(S.confirmed);
        c->dirty = false;
        c->srtDirty = c->editDirty = false;
        return c;
    }
    static void trimInto(const CueSet& from, CueSet& to) {
        to.cues = pbtrf::cuesWithin(from.cues, merged(from.confirmed));
        pbtrf::canonicalCues(to.cues);               // start order, idx 0..n-1 (a new generation shows them)
        to.nextIdx = static_cast<uint32_t>(to.cues.size());
        to.spans.clear();
        for (const auto& c : to.cues) to.spans[c.startMs] = c.endMs;
    }
    // Before a job starts on a set, the set holds exactly what it can vouch for: a cue outside its
    // confirmed ranges would come back from the job a second time. True = cues were dropped.
    static bool trimToConfirmed(CueSet& S) {
        const size_t before = S.cues.size();
        const std::vector<tp::WireCue> kept = pbtrf::cuesWithin(S.cues, merged(S.confirmed));
        if (kept.size() == before) return false;
        CueSet tmp;
        tmp.cues = S.cues;
        tmp.confirmed = S.confirmed;
        trimInto(tmp, S);
        return true;
    }
    // The cache record of this file and pair, if any: complete or partial (its vouched ranges and cues; the job
    // resumes from them). `files` (WG): its per-language files are confirmed now -- written when missing, stale, or
    // still the single mixed file of 1.0.6 (the writer compares bytes: an up-to-date file costs one read). Not for a
    // pre-analysis candidate that is only looked at.
    bool loadCacheInto(CueSet& S, bool files = true) {
        pbtrf::Record r;
        if (!host_.readCache(S.key, r) || !pbtrf::samePath(r.path, S.path) || r.size != S.size || r.mtime != S.mtime
            || r.audioOrd != S.audio.ord)                  // (WH: the same sound track -- the key says so; this guards it)
            return false;
        // WH: a record from before WH of an mp4/mov with 2+ sound tracks may be ANOTHER track's (TrFileStore.h
        // unknownTrackMiss): a miss, analysed again -- unless the user edited it
        if (pbtrf::unknownTrackMiss(r.trackKnown, r.edited, multiTrackIsoBmff(S.mediaKey))) return false;
        // ENGINE (TrFileStore.h olderEngine): a result from an OLDER cue engine is a MISS -- the file is analysed again,
        // progressively, as a first analysis is, and its files rewritten from the new cues (the strip: "Improved subtitles
        // -- re-analysing") -- UNLESS the user edited it: then it loads, and the window asks before anything discards it
        if (pbtrf::staleRecord(r.engine, r.edited)) { S.upgrade = true; return false; }
        S.engine = r.engine;
        S.cues = r.cues;                                   // canonical: start order, idx 0..n-1
        S.nextIdx = static_cast<uint32_t>(S.cues.size());
        S.spans.clear();
        for (const auto& c : S.cues) S.spans[c.startMs] = c.endMs;
        S.ranges = S.confirmed = merged(r.ranges);
        S.complete = r.complete;
        S.totalMs = r.totalMs;
        S.hasVideo = r.hasVideo;
        S.videoKnown = true;
        S.asr = w16(r.asrModel);
        S.mt = w16(r.mtModel);
        S.fromCache = true;
        S.edited = r.edited;
        S.trackKnown = r.trackKnown;
        S.srtFiles = r.srtFiles;
        S.srtPath = S.srtFiles.empty() ? std::wstring() : w16(S.srtFiles.front().path);
        if (!files) return true;
        if (S.cues.empty()) { if (S.complete) S.srtNote = L"No speech was found in this file."; return true; }
        if (writeSrts(S)) host_.writeCache(S.key, recordOf(S));   // the record names the files it has now
        return true;
    }
    pbtrf::Record recordOf(const CueSet& S) const {
        pbtrf::Record r;
        r.path = u8(S.path); r.size = S.size; r.mtime = S.mtime;
        r.langA = S.langA; r.langB = S.langB; r.mode = S.mode; r.quality = S.quality;
        r.asrModel = u8(S.asr); r.mtModel = u8(S.mt);
        r.hasVideo = S.hasVideo; r.totalMs = S.totalMs;
        r.srtFiles = S.srtFiles;
        if (!S.srtFiles.empty()) { r.srtPath = S.srtFiles.front().path; r.srtSize = S.srtFiles.front().size; r.srtMtime = S.srtFiles.front().mtime; }
        r.edited = S.edited;
        r.engine = S.engine;           // the engine that made THESE cues -- an edited older set is never stamped current
        r.audioOrd = S.audio.ord;      // WH: the AUD line -- on every record, except one read without it (kept so)
        r.audioTrack = S.audio.trackId;
        r.trackKnown = S.trackKnown;

        r.complete = S.complete;
        if (S.complete) {
            r.ranges.clear();
            if (S.totalMs) r.ranges.push_back(tp::MsRange{ 0, S.totalMs });
            r.cues = S.cues;
        } else {
            r.ranges = merged(S.confirmed);
            r.cues = pbtrf::cuesWithin(S.cues, r.ranges);   // exactly what the ranges vouch for
        }
        return r;
    }
    void savePartial(CueSet& S) {
        if (S.complete) return;
        if (S.confirmed.empty() && S.cues.empty()) { S.dirty = false; return; }
        host_.writeCache(S.key, recordOf(S));
        S.dirty = false;
        S.savedAt = host_.nowMs();
        pbtrf::Summary sm;
        sm.ok = true; sm.complete = false; sm.totalMs = S.totalMs; sm.ranges = merged(S.confirmed);
        sm.langA = S.langA; sm.langB = S.langB; sm.edited = S.edited; sm.engine = S.engine; sm.audioOrd = S.audio.ord;
        summaries_[S.key] = sm;
    }
    void saveComplete(CueSet& S) {
        if (S.cues.empty()) { S.srtNote = L"No speech was found in this file."; S.srtDirty = false; }
        else writeSrts(S);                               // WG: the per-language files, then the record naming them
        host_.writeCache(S.key, recordOf(S));
        S.dirty = false;
        S.editDirty = false;
        S.savedAt = host_.nowMs();
        pbtrf::Summary sm;
        sm.ok = true; sm.complete = true; sm.totalMs = S.totalMs;
        if (S.totalMs) sm.ranges.assign(1, tp::MsRange{ 0, S.totalMs });
        sm.langA = S.langA; sm.langB = S.langB; sm.edited = S.edited; sm.engine = S.engine; sm.audioOrd = S.audio.ord;
        summaries_[S.key] = sm;
    }

    // ---- WG: the per-language subtitle files -------------------------------------------------------------------
    void markSrt(CueSet& S) {
        if (S.srtDirty) return;
        S.srtDirty = true;
        S.srtDirtySince = host_.nowMs();
    }
    // Every set with cues the files may lack: the one on screen, and each running job's (often the same set).
    std::vector<CueSet*> liveSets() {
        std::vector<CueSet*> v;
        if (loaded_ && s_.set) v.push_back(s_.set.get());
        for (Job& j : jobs_) if (std::find(v.begin(), v.end(), j.set.get()) == v.end()) v.push_back(j.set.get());
        return v;
    }
    // The pending writes, now (the tray's end, the session's end).
    void flushFiles() {
        if (loaded_ && s_.set && s_.set->editDirty) saveEdits(*s_.set);
        for (CueSet* S : liveSets()) if (S->srtDirty) writeSrts(*S);
    }
    // THE SHARED ORIGINAL (TrFileStore.h siblingKeys): another analysis of this file whose pair holds `lang` and that
    // covers MORE of it than this set -- its record, to write that language's file from. The ranking: COMPLETE first (a
    // partial never shrinks a complete file), then EDITED (the user's own text -- another pair's unedited analysis
    // finishing must never put back what he corrected in the shared file: review of WG #4), then coverage; a tie keeps
    // the set being worked on.
    static std::tuple<int, int, uint64_t> siblingRank(bool complete, bool edited, uint64_t coverage) {
        return std::make_tuple(complete ? 1 : 0, edited ? 1 : 0, coverage);
    }
    // WH (D6): the candidates are the 144 analyses of THIS sound track (TrFileStore.h siblingKeys) plus ONE of any track:
    // the record that wrote the file on disk now (Host::srtWriter, the registry) -- so a switch of sound track never
    // shrinks a file (a complete French file made from track 0 stays until track k's set is at least as complete), and
    // the search stays bounded (no 144 x n peeks on the UI thread). `outKey` = the record chosen.
    bool bestSibling(const CueSet& S, const std::string& lang, pbtrf::Record& out, std::string* outKey = nullptr) {
        auto best = siblingRank(S.complete, S.edited, pbtrf::coverage(S.complete, merged(S.confirmed, S.ranges)));
        std::string bestKey;
        std::vector<std::string> keys;
        for (const auto& k : pbtrf::siblingKeys(S.path, S.size, S.mtime, lang, { S.langA, S.langB }, S.audio.ord)) keys.push_back(k.key);
        const std::string writer = host_.srtWriter(S, lang);
        const bool writerExtra = !writer.empty() && std::find(keys.begin(), keys.end(), writer) == keys.end();
        if (writerExtra) keys.push_back(writer);
        for (const auto& k : keys) {
            if (k == S.key) continue;
            pbtrf::Summary sm;
            if (!host_.peekCache(k, sm) || !sm.ok || !pbtrf::samePath(sm.path, S.path) || sm.size != S.size || sm.mtime != S.mtime)
                continue;
            if (pbtrf::staleRecord(sm.engine, sm.edited)) continue;   // ENGINE: an older engine's result is a miss here too
            if (pbtrf::unknownTrackMiss(sm.trackKnown, sm.edited, multiTrackIsoBmff(S.mediaKey))) continue;   // WH: and a track-less one
            // (the writer of ANOTHER track: its pair must hold this language -- it wrote this language's file)
            if (k == writer && writerExtra && pbtrf::lowerAscii(sm.langA) != lang && pbtrf::lowerAscii(sm.langB) != lang) continue;
            const auto r = siblingRank(sm.complete, sm.edited, pbtrf::coverage(sm.complete, sm.ranges));
            if (r > best) { best = r; bestKey = k; }
        }
        if (bestKey.empty()) return false;
        if (outKey) *outKey = bestKey;
        return host_.readCache(bestKey, out) && pbtrf::samePath(out.path, S.path) && out.size == S.size && out.mtime == S.mtime;
    }
    // Write (or confirm) one file per language of the set's pair, each from the most complete set holding that
    // language; a language with no line yet keeps the file it had. True = what the set knows of its files changed.
    bool writeSrts(CueSet& S) {
        S.srtDirty = false;
        std::vector<pbtrf::SrtFile> files;
        std::wstring notes;
        for (const std::string& L : pbtrf::pairLangs(S.langA, S.langB)) {
            pbtrf::Record sib;
            std::string sibKey;
            const bool fromSib = bestSibling(S, L, sib, &sibKey);
            const std::vector<tp::WireCue>& cues = fromSib ? sib.cues : S.cues;
            if (pbtrf::countInLang(cues, L) == 0) {      // nothing in L yet: never an empty file, never one shrunk to it
                for (const auto& f : S.srtFiles) if (f.lang == L) files.push_back(f);
                continue;
            }
            std::wstring note;
            uint64_t size = 0, mtime = 0;
            const std::wstring p = host_.writeSrt(S, L, cues, fromSib ? sibKey : S.key, fromSib ? sib.audioOrd : S.audio.ord,
                                                  note, size, mtime);
            if (!note.empty()) notes += (notes.empty() ? L"" : L" ") + note;
            if (p.empty()) continue;
            pbtrf::SrtFile f;
            f.lang = L; f.path = u8(p); f.size = size; f.mtime = mtime;
            files.push_back(f);
        }
        bool changed = files.size() != S.srtFiles.size();
        for (size_t i = 0; !changed && i < files.size(); ++i)
            changed = files[i].lang != S.srtFiles[i].lang || files[i].path != S.srtFiles[i].path
                   || files[i].size != S.srtFiles[i].size || files[i].mtime != S.srtFiles[i].mtime;
        S.srtFiles = files;
        S.srtPath = files.empty() ? std::wstring() : w16(files.front().path);
        if (notes != S.srtNote) { S.srtNote = notes; changed = true; }
        if (changed && loaded_ && s_.set.get() == &S) stateDirty_ = true;
        return changed;
    }
    // ---- WG editor: the checks of an edit, then the edit -------------------------------------------------------------
    static std::wstring clockOf(uint64_t ms) {
        const unsigned long long s = ms / 1000ULL;
        wchar_t b[40];
        if (s >= 3600) std::swprintf(b, 40, L"%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
        else           std::swprintf(b, 40, L"%llu:%02llu", s / 60, s % 60);
        return b;
    }
    // a language code as the helper writes them: 2-12 of [a-z-]
    static bool langOk(const std::string& l) {
        if (l.size() < 2 || l.size() > 12) return false;
        for (char c : l) if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
        return true;
    }
    // a line of text as a cue holds it: no control character but a line break, no blank at either end
    static std::string cleanEditText(const std::string& in) {
        std::string o;
        o.reserve(in.size());
        for (char ch : in) {
            const unsigned char u = static_cast<unsigned char>(ch);
            if (ch == '\r') continue;
            if (ch == '\t') { o += ' '; continue; }
            if (u < 0x20 && ch != '\n') continue;
            o += ch;
        }
        while (!o.empty() && (o.back() == ' ' || o.back() == '\n')) o.pop_back();
        size_t k = 0;
        while (k < o.size() && (o[k] == ' ' || o[k] == '\n')) ++k;
        return o.substr(k);
    }
    bool applyEdit(uint64_t gen, const std::vector<uint32_t>& remove, const std::vector<EditCue>& add,
                   std::vector<tp::WireCue>& added, std::wstring& err) {
        if (!loaded_ || gen != s_.gen || !s_.set) { err = L"This file changed meanwhile: nothing was edited."; return false; }
        CueSet& S = *s_.set;
        if (remove.size() > kMaxEditCues || add.size() > kMaxEditCues) { err = L"Too many subtitles in one edit."; return false; }
        if (remove.empty() && add.empty()) { err = L"Nothing to edit."; return false; }
        const std::set<uint32_t> rm(remove.begin(), remove.end());
        size_t found = 0;
        for (const auto& c : S.cues) if (rm.count(c.idx)) ++found;
        if (rm.size() != remove.size() || found != rm.size()) { err = L"A subtitle changed meanwhile: nothing was edited."; return false; }
        // Where an edit may land: anywhere in a file analysed to its end; while it is still being analysed, only
        // inside what is analysed AND VOUCHED FOR (S.confirmed: a later message confirmed every cue of it) -- the
        // helper never writes there again (TrPipe.h), so no cue of the analysis can ever collide with an edited one,
        // and the partial record holds exactly the vouched part, so an edit there survives the tray's exit (one the
        // record did not hold would be analysed over at the resume) -- and never across a gap. The last report
        // (one stretch, at the analysis' front, far ahead of the playhead) is vouched for by the next one.
        std::vector<tp::MsRange> zone;
        if (!S.complete) zone = merged(S.confirmed);
        auto notYet = [&](uint64_t at) {
            err = L"That part of the file is not analysed yet (" + clockOf(at)
                + L"): edit inside the green stretches, or wait for the analysis.";
            return false;
        };
        // (a line taken out must be one the record holds -- by its start, as the record keeps them -- or the resume
        // would bring it back)
        if (!S.complete)
            for (const auto& c : S.cues)
                if (rm.count(c.idx) && !pbtrf::inRanges(zone, c.startMs)) return notYet(c.startMs);
        std::vector<tp::WireCue> nw;
        for (const auto& a : add) {
            // (> 0 only: the 0.1 s minimum is the PAGE's rule for what the user makes -- an UNDO must be able to put back
            // a 50 ms cue whisper made, and the core would refuse it)
            if (a.e <= a.s) { err = L"A subtitle must end after it starts."; return false; }
            if (S.totalMs && a.e > S.totalMs) { err = L"That goes past the end of the file (" + clockOf(S.totalMs) + L")."; return false; }
            if (a.src.size() > kMaxEditText || a.dst.size() > kMaxEditText) { err = L"That text is too long."; return false; }
            if (!langOk(a.lang) || !langOk(a.dstLang)) { err = L"That language is not one SuVtitle knows."; return false; }
            if (!S.complete) {
                bool inside = false;
                for (const auto& r : zone) if (a.s >= r.s && a.e <= r.e + kTouchMs) { inside = true; break; }
                if (!inside) return notYet(a.s);
            }
            tp::WireCue c;
            c.startMs = a.s; c.endMs = a.e; c.lang = a.lang; c.dstLang = a.dstLang; c.pass = a.pass;
            c.src = cleanEditText(a.src); c.dst = cleanEditText(a.dst);
            if (c.pass) c.dst = c.src;                     // a passthrough shows its one text, in both places
            nw.push_back(c);
        }
        // NEVER AN OVERLAP: the cues that stay and the new ones, in start order, each ending by the next one's start
        // (touching is fine: one ends where the next starts).
        struct Span { uint64_t s, e; bool fresh; };
        std::vector<Span> all;
        all.reserve(S.cues.size() + nw.size());
        for (const auto& c : S.cues) if (!rm.count(c.idx)) all.push_back(Span{ c.startMs, c.endMs, false });
        for (const auto& c : nw) all.push_back(Span{ c.startMs, c.endMs, true });
        std::sort(all.begin(), all.end(), [](const Span& x, const Span& y) { return x.s < y.s || (x.s == y.s && x.e < y.e); });
        for (size_t i = 1; i < all.size(); ++i) {
            if (all[i].s >= all[i - 1].e) continue;
            if (!all[i].fresh && !all[i - 1].fresh) continue;       // (not ours: the set's own business)
            const Span& other = all[i].fresh ? all[i - 1] : all[i];
            err = L"That would overlap the subtitle at " + clockOf(other.s) + L": not applied.";
            return false;
        }
        // apply
        std::vector<tp::WireCue> keep;
        keep.reserve(S.cues.size() + nw.size());
        for (const auto& c : S.cues) if (!rm.count(c.idx)) keep.push_back(c);
        S.cues.swap(keep);
        S.spans.clear();
        for (const auto& c : S.cues) S.spans[c.startMs] = c.endMs;
        for (auto c : nw) {
            c.idx = S.nextIdx++;
            S.cues.push_back(c);
            S.spans[c.startMs] = c.endMs;
            added.push_back(c);
        }
        S.edited = true;
        S.dirty = true;
        S.editDirty = true;
        S.editAt = host_.nowMs();
        markSrt(S);
        markKey(S.key);
        return true;
    }

    // WG editor: an edited set reaches the cache (EDIT 1) and its files.
    void saveEdits(CueSet& S) {
        S.editDirty = false;
        if (S.complete) { saveComplete(S); return; }
        writeSrts(S);
        host_.writeCache(S.key, recordOf(S));
        S.dirty = false;
        S.savedAt = host_.nowMs();
    }
    // A job is over (FDONE, or the helper went away): the set keeps what it can vouch for.
    void endJobSet(Job& j, const std::string& result, bool synthesized) {
        CueSet& S = *j.set;
        (void)synthesized;
        if (result == "ok") {
            S.complete = true;
            S.confirmed = S.totalMs ? std::vector<tp::MsRange>{ tp::MsRange{ 0, S.totalMs } } : merged(S.confirmed, S.ranges);
            S.ranges = S.confirmed;
            saveComplete(S);
        } else {
            S.ranges = merged(S.confirmed);              // shown as analysed = what is vouched for
            if (S.srtDirty || S.editDirty) writeSrts(S); // WG: the files hold what was analysed, at once
            S.editDirty = false;
            savePartial(S);
        }
    }

    bool loadItem(size_t index, bool autoplay, long long startOverride) {
        if (index >= items_.size()) return false;
        Item& it = items_[index];
        uint64_t size = 0, mtime = 0;
        if (!host_.identity(it.path, size, mtime)) {
            it.missing = true;
            markItem(it.id);
            notice(L"Not found: " + it.name);
            return false;
        }
        if (it.missing) { it.missing = false; markItem(it.id); }
        if (size != it.size || mtime != it.mtime) { it.size = size; it.mtime = mtime; queueSaveDirty(); }
        endSession();
        cur_ = index;
        Session n;
        n.gen = next();
        n.seq = ++seqCounter_;
        n.itemId = it.id;
        n.path = it.path; n.folder = it.folder; n.name = it.name;
        n.size = size; n.mtime = mtime;
        n.mediaKey = pbtrf::mediaKey(it.path, size, mtime);
        n.host = L"f" + w16(pbtrf::folderKey(it.folder)) + kMediaHostSuffix;
        n.url = L"https://" + n.host + L"/" + w16(pbtrf::pctEncode(u8(it.name))) + L"?v=" + w16(n.mediaKey);
        n.autoplay = autoplay;
        const auto re = resume_.find(resumeKey(it.path, size, mtime));
        n.startMs = startOverride >= 0 ? static_cast<uint64_t>(startOverride)
                  : resumeStart(re == resume_.end() ? nullptr : &re->second, it.durMs);
        n.resumed = startOverride < 0 && n.startMs > 0;
        n.lastPosMs = n.lastFocusMs = n.startMs;
        n.durMs = it.durMs;
        for (Sidecar sc : host_.sidecars(it.folder, it.name)) { sc.id = next(); n.sidecars.push_back(sc); }
        // WH: the container's own tracks. Known already (probed for the queue or an earlier opening): applied below.
        // Else the probe is asked for and the JOB WAITS for it (<= kProbeWaitMs): analysing the wrong sound track is
        // the one mistake the wire refuses to make (TrPipe.h WH block). The set loaded meanwhile is the PROVISIONAL
        // track's -- the user's choice for this file, else the first -- so a cached result shows at once as before.
        const bool worthy = probeWorthy(it.path);
        const auto pr = worthy ? probes_.find(n.mediaKey) : probes_.end();
        const bool known = pr != probes_.end() && pr->second.state != "probing" && !pr->second.state.empty();
        n.tracksState = !worthy ? "none" : known ? pr->second.state : "probing";
        n.probeWait = worthy && !known;
        n.probeAt = host_.nowMs();
        // the cue set: a pre-analysis of THIS file, pair and track is ADOPTED (no model reload); else the
        // cache; else nothing yet
        const std::string key = keyOf(it);
        Job* pj = jobForKey(key);
        if (pj) { n.set = pj->set; n.wire = pj->wire; pj->pre = false; }
        else { n.set = makeSet(it.path, it.name, it.folder, size, mtime, audioFor(it)); loadCacheInto(*n.set); }
        s_ = std::move(n);
        loaded_ = true;
        if (known) {                                             // (the same choice as audioFor: no re-track here)
            s_.tracks = pr->second.state == "ready" ? pr->second.info : nullptr;
            if (s_.tracks) for (const auto& st : s_.tracks->subs) s_.embedded.push_back(Session::Emb{ next(), st.track });
            const auto pk = picks_.find(s_.mediaKey);
            s_.audioOrd = s_.tracks ? initialAudioOrd(*s_.tracks, pk == picks_.end() ? nullptr : &pk->second) : -1;
        }
        if (s_.wire) {
            Job* j = job(s_.wire);
            if (j && shouldFocus(j->set->ranges, j->set->totalMs, s_.startMs, 0, PosReason::Play) && s_.startMs > 0) {
                host_.sendFocus(s_.wire, s_.startMs);
            }
        } else {
            ensureCurrentJob(s_.startMs);
        }
        const bool reloading = ensureMapped();
        queueDirty_ = true;
        markItem(it.id);
        if (!reloading) { postStateNow(); postCuesNow(); }   // a reloading page gets it all on its `ready`
        else stateDirty_ = true;                             // the main view (the card) still wants it
        if (s_.probeWait) requestProbe(s_.mediaKey, s_.path); // (last: its answer may come back at once)
        schedule();
        return true;
    }
    // The current item goes: its job is cancelled (its FDONE saves the partial), its proxy deleted.
    void endSession() {
        if (!loaded_) return;
        // WG: an item switched away from (or closed) leaves its files and its edits up to date
        if (s_.set && s_.set->editDirty) saveEdits(*s_.set);
        else if (s_.set && s_.set->srtDirty) writeSrts(*s_.set);
        if (s_.wire) { if (Job* j = job(s_.wire)) cancelJob(*j); }
        endProxy();
        cancelSubExtract();                                  // WH: an embedded track still being read is for this item only
        s_.extSubChunks.clear();
    }
    void ensureCurrentJob(uint64_t focus) {
        if (!loaded_ || s_.wire || !s_.set || s_.set->complete || !translate_) return;
        if (s_.userCancelled || s_.failed) return;
        if (s_.probeWait) return;                            // WH: which sound track is not known yet (<= kProbeWaitMs)
        if (!helper_) {
            if (s_.err.empty()) { s_.err = L"The SuVtitle engine is not running: files cannot be analysed."; stateDirty_ = true; }
            return;
        }
        if (s_.err == L"The SuVtitle engine is not running: files cannot be analysed.") s_.err.clear();
        for (Job& j : jobs_) if (j.pre) cancelJob(j);     // the helper goes to the current item
        // A set that kept cues it cannot vouch for (its last job died) shows them no more: the page
        // gets a new generation with exactly what the new job will not redo.
        const bool regen = trimToConfirmed(*s_.set);
        if (regen) {
            s_.gen = next();
            // The SAME file, re-posted: not a file being opened. It is wherever the user has it -- no
            // start of its own, no autoplay, no "Resumed at" (review of WE, C5: translation Off->On re-
            // armed play-when-ready, and a film the user had PAUSED started playing by itself).
            s_.startMs = s_.lastPosMs;
            s_.autoplay = s_.resumed = false;
        }
        s_.wire = startJob(s_.set, focus, false);
        s_.lastFocusMs = focus;
        stateDirty_ = true;
        if (regen) { postStateNow(); postCuesNow(); }
    }
    uint64_t startJob(const std::shared_ptr<CueSet>& set, uint64_t focus, bool pre) {
        Job j;
        j.wire = next();
        j.set = set;
        j.pre = pre;
        trimToConfirmed(*set);
        tp::WireFile w;
        w.jobId = j.wire;
        w.path = u8(set->path);
        w.langA = set->langA; w.langB = set->langB; w.mode = set->mode; w.quality = set->quality;
        w.have = tp::packRanges(merged(set->confirmed));
        w.focusMs = focus;
        // WH: the sound track -- "" for the first one, so the wire and every cache record are exactly what they were
        w.audio = set->audio.ord > 0 ? tp::packAudioSel(set->audio) : std::string();
        set->savedAt = host_.nowMs();
        host_.sendFile(w);
        jobs_.push_back(j);
        markKey(set->key);
        return j.wire;
    }
    void cancelJob(Job& j) {
        if (j.cancelSent) return;
        j.cancelSent = true;
        host_.sendCancel(j.wire);
    }
    Job* job(uint64_t wire) {
        for (Job& j : jobs_) if (j.wire == wire) return &j;
        return nullptr;
    }
    Job* jobForKey(const std::string& key) {
        for (Job& j : jobs_) if (j.set->key == key && !j.cancelSent) return &j;
        return nullptr;
    }
    bool isCurrent(const Job& j) const { return loaded_ && s_.wire == j.wire; }
    // Would [start, end) REALLY overlap a cue already held? (the one starting at or before it, or the
    // next one). TrPipe.h (PRECISIONS): a cue's end may run 1 ms past its range's end, and a resumed
    // run's first cue may start exactly at `have`'s end -- so two neighbours TOUCHING by up to 1 ms are
    // neighbours, not an overlap (review of WE, C3: that cue was refused for good while the next FPROG
    // vouched its range, a permanent hole in the cache and the .srt). touchUp() then clamps the 1 ms.
    static bool overlapsHeld(const CueSet& S, uint64_t start, uint64_t end) {
        auto it = S.spans.upper_bound(start);
        if (it != S.spans.end() && it->first + kTouchMs < std::max(end, start + 1)) return true;   // the next one starts inside
        if (it == S.spans.begin()) return false;
        --it;
        return it->first == start || start + kTouchMs < it->second;
    }
    // A cue accepted against a neighbour it TOUCHES: the earlier one ends where the later one starts.
    static void touchUp(CueSet& S, tp::WireCue& k) {
        auto nx = S.spans.upper_bound(k.startMs);
        if (nx != S.spans.end() && k.endMs > nx->first) k.endMs = nx->first;
        if (nx == S.spans.begin()) return;
        auto pv = std::prev(nx);
        if (pv->second <= k.startMs) return;
        pv->second = k.startMs;
        for (auto r = S.cues.rbegin(); r != S.cues.rend(); ++r)
            if (r->startMs == pv->first) { r->endMs = k.startMs; break; }
    }

    // ---- pre-analysis (D14) ----------------------------------------------------------------------------------------
    void cancelPre() { for (Job& j : jobs_) if (j.pre) cancelJob(j); }
    void cancelPreFor(const Item& it) {
        const std::string k = keyOf(it);
        for (Job& j : jobs_) if (j.pre && j.set->key == k) cancelJob(j);
    }
    // At most ONE job runs: the current item's first. Then -- translation on, the window shown on its
    // File tab, the current item complete -- the next kPreAhead items in play order, one at a time.
    void schedule() {
        if (!translate_ || !helper_) { cancelPre(); return; }
        ensureCurrentJob(loaded_ ? s_.lastPosMs : 0);
        for (const Job& j : jobs_) if (!j.pre && !j.cancelSent) return;   // the current item's job runs
        // only while something is being WATCHED: shown, on the File tab, the current item complete
        if (!shown_ || view_ != "file" || !loaded_ || !s_.set || !s_.set->complete) { cancelPre(); return; }
        // the candidates
        std::vector<size_t> cands;
        const bool wrap = repeat_ == Repeat::All;
        if (repeat_ != Repeat::One && cur_ != npos) {
            size_t nx = step(cur_, +1, wrap);
            for (size_t guard = 0; nx != npos && nx != cur_ && cands.size() < kPreAhead && guard < items_.size(); ++guard) {
                cands.push_back(nx);
                nx = step(nx, +1, wrap);
            }
        }
        std::set<std::string> wantKeys;
        for (size_t i : cands) wantKeys.insert(keyOf(items_[i]));
        for (Job& j : jobs_) if (j.pre && !wantKeys.count(j.set->key)) cancelJob(j);   // no longer next
        for (const Job& j : jobs_) if (j.pre && !j.cancelSent) return;                 // one at a time
        if (jobs_.size() >= 2) return;              // two cancelled jobs still dying: let them go first
        for (size_t i : cands) {
            const Item& it = items_[i];
            if (it.missing) continue;
            // WH: a container's sound track must be known before it is analysed ahead -- its probe first (on a worker;
            // its answer schedules again). One at a time, in play order: the next item is never overtaken.
            if (probeWorthy(it.path)) {
                const std::string mk = pbtrf::mediaKey(it.path, it.size, it.mtime);
                const auto p = probes_.find(mk);
                if (p == probes_.end() || p->second.state.empty()) { requestProbe(mk, it.path); return; }
                if (p->second.state == "probing") return;
            }
            const std::string k = keyOf(it);
            if (failedKeys_.count(k)) continue;
            const pbtrf::Summary& sm = summaryFor(it);
            if (sm.ok && sm.complete) continue;
            auto set = makeSet(it.path, it.name, it.folder, it.size, it.mtime, audioFor(it));
            loadCacheInto(*set, false);
            if (set->complete) continue;
            startJob(set, 0, true);
            return;
        }
    }

    // ---- the window's mappings ---------------------------------------------------------------------------------------
    std::vector<std::wstring> wantFolders() const {
        std::vector<std::wstring> v;
        std::set<std::wstring> seen;
        auto addF = [&](const std::wstring& f) {
            const std::wstring l = lowerW(f);
            if (!f.empty() && !seen.count(l) && v.size() < kMapMax) { seen.insert(l); v.push_back(f); }
        };
        if (loaded_) addF(s_.folder);
        const std::vector<size_t> o = playOrder();
        size_t p = 0;
        if (cur_ != npos) while (p < o.size() && o[p] != cur_) ++p;
        if (p >= o.size()) p = 0;
        for (size_t k = 0; k < o.size() && v.size() < kMapMax; ++k) addF(items_[o[(p + k) % o.size()]].folder);
        return v;
    }
    void mapNow() {
        if (!attached_) return;
        std::vector<std::wstring> want = wantFolders(), failed;
        host_.mapFolders(want, failed);
        mapped_.clear();
        for (const auto& f : want) {
            bool bad = false;
            for (const auto& x : failed) if (lowerW(x) == lowerW(f)) bad = true;
            if (!bad) mapped_.insert(lowerW(f));
        }
        if (loaded_ && !mapped_.count(lowerW(s_.folder)) && !s_.url.empty()) {
            // This folder cannot be served (an odd UNC share, a vanished drive): no URL -- and the
            // sound is asked for at once (v1, review of WC: a page with nothing to load waits forever).
            s_.url.clear();
            requestProxy();
        }
    }
    // True when the page is being RELOADED to see the current item's folder (MEASURED, STEP0 M1: a
    // mapping set after a page loaded is invisible to it, and a re-pointed host keeps its old folder).
    bool ensureMapped() {
        if (!attached_ || !loaded_) return false;
        if (committed_.count(lowerW(s_.folder))) return false;
        mapNow();
        if (s_.url.empty() || !viewLoaded_) return false;
        viewReady_ = false;
        // What the RELOADING page will see: a further switch before its `ready` is judged against
        // this set, never the old page's -- whose hosts mapNow() may just have cleared (review of
        // WE, C4: a folder of the old set was then loaded with no reload, and played as sound only).
        committed_ = mapped_;
        host_.reloadWindow();
        return true;
    }

    // ---- the proxy (v1) ---------------------------------------------------------------------------------------------
    void requestProxy() {
        if (!loaded_ || !s_.proxyState.empty()) return;    // asked once per item
        if (!helper_) {
            s_.proxyState = "failed";
            s_.proxyErr = L"the SuVtitle engine is not running, so its sound cannot be extracted";
            stateDirty_ = true;
            proxyToFirstTrack();
            return;
        }
        const uint64_t id = next();
        const std::wstring p = host_.proxyPath(s_.mediaKey, id);
        if (p.empty()) {
            s_.proxyState = "failed";
            s_.proxyErr = L"no folder to write its sound to";
            stateDirty_ = true;
            proxyToFirstTrack();
            return;
        }
        s_.proxyReq = id;
        s_.proxyPath = p;
        s_.proxyState = "working";
        tp::WireProxy w;
        w.jobId = id; w.src = u8(s_.path); w.out = u8(p);
        host_.sendProxy(w);
        stateDirty_ = true;
        proxyToFirstTrack();
    }
    // WH (the director, from the page's review WHP-2): an item played through the FPROXY copy hears ONE sound track -- the
    // FIRST, the one the helper's proxy carries (WH-H) -- so its audioOrd is 0 (forced, never stored as the user's) and,
    // with translation on, an analysis on another track moves to it (a new generation, like any track switch). A later
    // playerSet for another track is ignored (the page shows the Audio section disabled for such an item).
    bool proxied() const { return loaded_ && !s_.proxyState.empty(); }
    void proxyToFirstTrack() {
        if (!s_.tracks || s_.tracks->audio.empty()) return;    // (no track list: -1, nothing to say)
        if (s_.audioOrd != 0) { s_.audioOrd = 0; stateDirty_ = true; }
        if (translate_ && s_.set && s_.set->audio.ord != 0) retrack(heardSel());
    }
    void endProxy() {
        if (s_.proxyReq && s_.proxyState == "working") host_.sendCancel(s_.proxyReq);
        if (!s_.proxyPath.empty()) host_.deleteFile(s_.proxyPath);
        s_.proxyReq = 0;
        s_.proxyState.clear();
        s_.proxyPath.clear(); s_.proxyUrl.clear(); s_.proxyErr.clear();
    }

    // ---- the resume table -----------------------------------------------------------------------------------------
    void noteResume(uint64_t ms, bool ended) {
        const std::string k = resumeKey(s_.path, s_.size, s_.mtime);
        const uint64_t d = s_.durMs;
        if (ended || resumeFinished(ms, d) || ms < kResumeMinMs) {
            if (resume_.erase(k)) { resumeDirty_ = true; markItem(s_.itemId); }
            if (ended) saveResumeNow();
            return;
        }
        ResumeEntry& e = resume_[k];
        if (e.ms / 1000 != ms / 1000) { e.ms = ms; e.durMs = d; e.epoch = host_.epochSec(); resumeDirty_ = true; }
    }
    void saveQueueNow() {
        QueueFile q;
        q.translate = translate_; q.repeat = repeat_; q.shuffle = shuffle_;
        q.cur = cur_ == npos ? -1 : static_cast<long long>(cur_);
        for (const auto& it : items_) { QueueFile::It x; x.path = it.path; x.size = it.size; x.mtime = it.mtime; x.durMs = it.durMs; q.items.push_back(x); }
        if (shuffle_) for (size_t i : playOrder()) q.order.push_back(i);
        host_.saveQueue(packQueue(q));
        queueSave_ = false;
    }
    void saveResumeNow() {
        if (!resumeDirty_) return;
        host_.saveResume(packResume(resume_));
        resumeDirty_ = false;
        resumeSavedAt_ = host_.nowMs();
    }

    // ---- badges ---------------------------------------------------------------------------------------------------------
    const pbtrf::Summary& summaryFor(const Item& it) {
        const std::string k = keyOf(it);
        auto f = summaries_.find(k);
        if (f != summaries_.end()) return f->second;
        pbtrf::Summary s;
        if (!host_.peekCache(k, s) || !pbtrf::samePath(s.path, it.path) || s.size != it.size || s.mtime != it.mtime) s = pbtrf::Summary{};
        else if (pbtrf::staleRecord(s.engine, s.edited)) s = pbtrf::Summary{};   // ENGINE: an older engine's = none (no
                                                                                 // "ready" badge; the pre-analysis redoes it)
        else if (pbtrf::unknownTrackMiss(s.trackKnown, s.edited, multiTrackIsoBmff(pbtrf::mediaKey(it.path, it.size, it.mtime))))
            s = pbtrf::Summary{};                                                // WH: the same for a track-less record
        return summaries_[k] = s;
    }
    void refreshSummaries() {
        summaries_.clear();
        for (size_t i = 0; i < items_.size() && i < kSummaryPeekMax; ++i) summaryFor(items_[i]);
        queueDirty_ = true;
    }
    void badge(const Item& it, std::string& state, int& pct) {
        state = "none";
        pct = 0;
        if (it.missing) { state = "missing"; return; }
        const std::string k = keyOf(it);
        auto pctOf = [](const std::vector<tp::MsRange>& r, uint64_t total) {
            return total ? static_cast<int>(std::min<uint64_t>(100, tp::rangesTotal(r) * 100 / total)) : 0;
        };
        for (const Job& j : jobs_)
            if (j.set->key == k && !j.cancelSent) { state = "analysing"; pct = pctOf(j.set->ranges, j.set->totalMs); return; }
        if (loaded_ && s_.itemId == it.id && s_.set) {
            if (s_.set->complete) { state = "ready"; pct = 100; return; }
            if (!s_.set->confirmed.empty()) { state = "partial"; pct = pctOf(s_.set->confirmed, s_.set->totalMs); }
            if (s_.failed) state = "error";
            return;
        }
        if (failedKeys_.count(k)) { state = "error"; return; }
        if (indexOf(it.id) < kSummaryPeekMax || summaries_.count(k)) {
            const pbtrf::Summary& s = summaryFor(it);
            if (s.ok && s.complete) { state = "ready"; pct = 100; }
            else if (s.ok && !s.ranges.empty()) { state = "partial"; pct = pctOf(s.ranges, s.totalMs); }
        }
    }
    void markItem(uint64_t id) { if (id) itemDirty_.insert(id); }
    void markKey(const std::string& key) {
        for (const auto& it : items_) if (keyOf(it) == key) itemDirty_.insert(it.id);
    }

    // ---- the posts ------------------------------------------------------------------------------------------------------
    std::string phaseOf() const {
        if (!loaded_) return "idle";
        if (s_.wire) {
            for (const Job& j : jobs_) if (j.wire == s_.wire) return j.phase;
            return "loading";
        }
        if (s_.set && s_.set->complete) return "done";
        if (s_.cancelling || s_.userCancelled) return "cancelled";
        if (s_.failed || !s_.err.empty()) return translate_ ? "error" : "off";
        return translate_ ? "loading" : "off";
    }
    void postStateNow() {
        stateDirty_ = false;
        const bool fresh = s_.gen != windowSaw_;
        if (attached_ && viewReady_) windowSaw_ = s_.gen;
        const CueSet* S = s_.set.get();
        JsonW j;
        j.obj();
        j.kv(L"type", L"translatorFile");
        j.kv(L"jobId", static_cast<long long>(s_.gen));
        j.kv(L"seq", static_cast<long long>(s_.seq));
        j.kv(L"fresh", fresh);
        j.kv(L"phase", w16(phaseOf()));
        j.kv(L"itemId", static_cast<long long>(loaded_ ? s_.itemId : 0));
        j.kv(L"name", s_.name);
        j.kv(L"url", s_.url);
        j.kv(L"mediaKey", w16(s_.mediaKey));
        j.kv(L"proxyUrl", s_.proxyUrl);
        j.kv(L"proxyState", w16(s_.proxyState));
        j.kv(L"proxyErr", s_.proxyErr);
        j.kv(L"hasVideo", S ? S->hasVideo : false);
        j.kv(L"videoKnown", S ? S->videoKnown : false);
        const uint64_t total = S ? (S->totalMs ? S->totalMs : 0) : 0;
        j.kv(L"doneMs", static_cast<long long>(S ? (S->complete ? total : tp::rangesTotal(S->ranges)) : 0));
        j.kv(L"totalMs", static_cast<long long>(total));
        j.key(L"ranges").arr();
        if (S) for (const auto& r : S->ranges) { j.arr(); j.num(static_cast<long long>(r.s)); j.num(static_cast<long long>(r.e)); j.end(); }
        j.end();
        j.kv(L"complete", S ? S->complete : false);
        j.kv(L"asrModel", S ? S->asr : std::wstring());
        j.kv(L"mtModel", S ? S->mt : std::wstring());
        j.kv(L"langA", w16(S ? S->langA : langA_));
        j.kv(L"langB", w16(S ? S->langB : langB_));
        j.kv(L"mode", w16(S ? S->mode : mode_));
        j.kv(L"quality", w16(S ? S->quality : quality_));
        j.kv(L"cached", S ? (S->fromCache && S->complete) : false);
        j.kv(L"count", static_cast<long long>(S ? S->cues.size() : 0));
        j.kv(L"srtPath", S ? S->srtPath : std::wstring());
        // WG: one file per language -- srtName names them all ("film.fr.srt + film.en.srt"), srtFiles one by one
        std::wstring names;
        if (S) for (const auto& f : S->srtFiles) names += (names.empty() ? L"" : L" + ") + pbtrf::leafOf(w16(f.path));
        j.kv(L"srtName", names);
        j.key(L"srtFiles").arr();
        if (S) for (const auto& f : S->srtFiles) { j.obj(); j.kv(L"lang", w16(f.lang)); j.kv(L"name", pbtrf::leafOf(w16(f.path))); j.end(); }
        j.end();
        j.kv(L"srtNote", S ? S->srtNote : std::wstring());
        j.kv(L"edited", S ? S->edited : false);
        // ENGINE: upgrade = analysed again for a better engine ("Improved subtitles -- re-analysing"); oldEngine = these
        // cues ARE an older engine's (only ever an edited result: the window asks before discarding the edits)
        j.kv(L"upgrade", S ? S->upgrade : false);
        j.kv(L"oldEngine", S ? pbtrf::olderEngine(S->engine) : false);
        j.kv(L"err", s_.err);
        j.kv(L"cancelling", s_.cancelling);
        j.kv(L"translate", translate_);
        j.kv(L"autoplay", s_.autoplay);
        j.kv(L"startMs", static_cast<long long>(fresh ? s_.startMs : s_.lastPosMs));
        j.kv(L"resumed", s_.resumed);
        j.key(L"sidecars").arr();
        for (const auto& sc : s_.sidecars) { j.obj(); j.kv(L"id", static_cast<long long>(sc.id)); j.kv(L"name", sc.name); j.kv(L"lang", sc.hint); j.end(); }
        j.end();
        j.kv(L"extSubId", static_cast<long long>(s_.extSubId));
        j.end();
        std::wstring msg = j.take();
        appendTracks(msg);
        if (windowBusy()) host_.postMain(msg);        // the card only: the reloading page gets it on `ready`
        else host_.postBoth(msg);
    }
    // WH (the contract, WH_PLAN_pistes section 2): translatorFile gains
    //   tracks: {state, container, video: {codec, w, h} | null, audio: [{ord, track, name, lang, codec, ch, def, play}],
    //            subs: [{id, track, name, lang, codec, kind, def, forced, ok}]},  audioOrd: <int>
    // JsonW writes no null: the two members are spliced in before the message's closing brace.
    void appendTracks(std::wstring& msg) const {
        const pbmedia::Info* T = (loaded_ && s_.tracks) ? s_.tracks.get() : nullptr;
        JsonW t;
        t.obj();
        t.kv(L"state", w16(loaded_ ? s_.tracksState : std::string("none")));
        t.kv(L"container", w16(T ? T->container : std::string()));
        t.key(L"audio").arr();
        if (T) for (const auto& a : T->audio) {
            t.obj();
            t.kv(L"ord", static_cast<long long>(a.ord));
            t.kv(L"track", static_cast<long long>(a.track));
            t.kv(L"name", w16(a.name));
            t.kv(L"lang", w16(a.lang));
            t.kv(L"codec", w16(a.codec));
            t.kv(L"ch", static_cast<long long>(a.ch));
            t.kv(L"def", a.def);
            t.kv(L"play", a.play);
            t.end();
        }
        t.end();
        t.key(L"subs").arr();
        if (T) for (const auto& e : s_.embedded) {
            const pbmedia::SubTrack* st = T->sub(e.track);
            if (!st) continue;
            t.obj();
            t.kv(L"id", static_cast<long long>(e.id));
            t.kv(L"track", static_cast<long long>(st->track));
            t.kv(L"name", w16(st->name));
            t.kv(L"lang", w16(st->lang));
            t.kv(L"codec", w16(st->codec));
            t.kv(L"kind", st->image ? L"image" : L"text");
            t.kv(L"def", st->def);
            t.kv(L"forced", st->forced);
            t.kv(L"ok", st->ok);
            t.end();
        }
        t.end();
        t.end();
        std::wstring tracks = t.take();
        std::wstring video = L"null";
        if (T && T->hasVideo) {
            JsonW v;
            v.obj().kv(L"codec", w16(T->video.codec)).kv(L"w", static_cast<long long>(T->video.w))
                   .kv(L"h", static_cast<long long>(T->video.h)).end();
            video = v.take();
        }
        tracks.insert(1, L"\"video\":" + video + L",");          // (the object always has members after it)
        if (msg.empty() || msg.back() != L'}') return;
        msg.pop_back();
        msg += L",\"tracks\":" + tracks + L",\"audioOrd\":" + std::to_wstring(loaded_ ? s_.audioOrd : -1) + L"}";
    }
    static void cueJson(JsonW& j, const tp::WireCue& c) {
        j.kv(L"idx", static_cast<long long>(c.idx));
        j.kv(L"s", static_cast<long long>(c.startMs));
        j.kv(L"e", static_cast<long long>(c.endMs));
        j.kv(L"lang", w16(c.lang));
        j.kv(L"dstLang", w16(c.dstLang));
        j.kv(L"pass", c.pass);
        j.kv(L"src", w16(c.src));
        j.kv(L"dst", w16(c.dst));
    }
    void postCue(const tp::WireCue& c) {
        if (windowBusy()) return;                      // the page's `ready` replays every cue
        JsonW j;
        j.obj();
        j.kv(L"type", L"translatorFileCue");
        j.kv(L"jobId", static_cast<long long>(s_.gen));
        cueJson(j, c);
        j.end();
        host_.postWindow(j.take());
    }
    void postCuesNow() {
        if (!loaded_ || !s_.set || windowBusy()) return;
        const auto& cues = s_.set->cues;
        for (size_t at = 0; at < cues.size(); at += kCueChunk) {
            JsonW j;
            j.obj();
            j.kv(L"type", L"translatorFileCues");
            j.kv(L"jobId", static_cast<long long>(s_.gen));
            j.key(L"cues").arr();
            for (size_t i = at; i < cues.size() && i < at + kCueChunk; ++i) { j.obj(); cueJson(j, cues[i]); j.end(); }
            j.end();
            j.end();
            host_.postWindow(j.take());
        }
    }
    void postQueueNow() {
        queueDirty_ = false;
        if (windowBusy()) { itemDirty_.clear(); return; }   // replayed whole on the page's `ready`
        JsonW j;
        j.obj();
        j.kv(L"type", L"playerQueue");
        j.key(L"items").arr();
        for (const auto& it : items_) {
            std::string st;
            int pct = 0;
            badge(it, st, pct);
            const auto re = resume_.find(resumeKey(it.path, it.size, it.mtime));
            j.obj();
            j.kv(L"id", static_cast<long long>(it.id));
            j.kv(L"name", it.name);
            j.kv(L"dir", it.dirName);
            j.kv(L"state", w16(st));
            j.kv(L"pct", pct);
            j.kv(L"durMs", static_cast<long long>(it.durMs));
            j.kv(L"resumeMs", static_cast<long long>(re == resume_.end() ? 0 : resumeStart(&re->second, it.durMs)));
            j.end();
        }
        j.end();
        j.kv(L"cur", static_cast<long long>(cur_ != npos ? items_[cur_].id : 0));
        j.kv(L"loaded", loaded_);
        j.kv(L"repeat", w16(repeatName(repeat_)));
        j.kv(L"shuffle", shuffle_);
        j.kv(L"translate", translate_);
        j.end();
        host_.postWindow(j.take());
        itemDirty_.clear();                        // the whole list carried every badge
    }
    void postItem(uint64_t id) {
        if (windowBusy()) return;
        const size_t i = indexOf(id);
        if (i == npos) return;
        std::string st;
        int pct = 0;
        badge(items_[i], st, pct);
        JsonW j;
        j.obj().kv(L"type", L"playerItem").kv(L"id", static_cast<long long>(id)).kv(L"state", w16(st)).kv(L"pct", pct).end();
        host_.postWindow(j.take());
    }
    void postCmd(const char* op, uint64_t ms) {
        if (windowBusy()) return;
        JsonW j;
        j.obj().kv(L"type", L"playerCmd").kv(L"gen", static_cast<long long>(s_.gen)).kv(L"op", w16(op))
               .kv(L"ms", static_cast<long long>(ms)).end();
        host_.postWindow(j.take());
    }
};

}   // namespace pbplayer
