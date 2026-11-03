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
// SUVTITLE LIVE (1.1.0, 2026-10-25 -- docs/LIVE_PLAN.md section 2, the study section 2.8): THE LIVE GATE (setLive, live_,
// Job::liveHit, the phase "live"): no FILE while the live mode listens, the running jobs cancelled when it starts (their
// vouched part kept), the current item resumed with `have` when it stops, never `failed` because of it. Proved by
// tests/cpp/player_smoke.cpp [18].
// U1 (1.1.0, 02/11/2026 -- the user: "on ne lancera pas automatiquement la traduction au chargement d'une video"; docs/
// PROGRESS_U1.md): NOTHING IS ANALYSED UNLESS ASKED. The global translation switch (translate_) is gone: a file is analysed
// when it is WANTED -- the viewer's press of Translate (Session::wanted: THIS file, while it plays) or "Translate all" (batchQ_:
// every file of the queue not complete yet, one FILE job at a time, in LIST order, the playing one first). Loading a file
// reads its cache and SHOWS it (complete or partial) and starts nothing; there is no pre-analysis of the next items any more
// (only the batch analyses files that are not playing). The live gate holds the batch too (resumed after). Badges gain
// "queued" and "cancelled". player.txt's 1.0.x `OPT translate` is still parsed and IGNORED (never written again).
// U2 (1.1.0, 02/11/2026 -- "Import model..."; docs/PROGRESS_U2.md): THE USER'S OWN MODELS. setModels() = the models the next
// analyses run on (an imported model's path + identity per half, {} = the default); a CueSet remembers its models, its FILE
// names them, its cache key carries their tag ONLY when one is imported (the defaults' keys are the pre-U2 keys); an FDONE error
// of an imported model (TrPipe.h fileModelErrKind) sends THAT kind back to the default and tells the host (Host::modelFailed).
// And the user's rule for CANCEL ALL: only what "Translate all" launched (cancelAll).
// U3 (1.1.0, 02/11/2026 -- the user: "un deuxieme bouton pour la Transcription", "un fichier propre utilisable par des
// transcripteurs professionnels"; docs/PROGRESS_U3.md): THE TRANSCRIPT. Translate and Transcribe are two WISHES on ONE
// analysis (whisper runs once whatever is pressed): the analysis runs while either wants it (analysisWanted), each has its own
// Cancel (cancel / transcriptCancel: cancelling one never stops the other's work), and "Transcribe all" (tbatchQ_) is the
// twin of "Translate all". A transcript = the analysis' original text (TrTranscript.h pieces) + the VOICE PASS (FVOICE, a job
// of the engine's one slot -- Job::voice, Session::vwire) run on the COMPLETE analysis; its result is the CueSet's `voices`
// (<cache>\<key>.voices: the speakers, the user's names, the Word file written). At its completion the Word file is written
// next to the media (Host::writeTranscript; Q2), by Re-transcribe, and by Transcribe all when missing or older and still
// ours -- never silently on a rename or an edit ("older" in the post). Speakers are renamed here (renameSpeaker: every
// speaker sharing the name; the same name twice = one person). Stamps: the file's SOURCE timecode when it carries one
// (Host::probeTimecode on a worker -> onTimecode; SourceTimecode.h), else file-relative. The page gets translatorTranscript
// (coalesced) and translatorFile.transcribe / tphase / tpct; the rows a second badge (tstate / tpct).
// U2b (1.1.0, 02/11/2026 -- docs/PROGRESS_U2b.md): THE BATCH'S ONE RETRY -- a file "Translate all" / "Transcribe all" asked for whose
// IMPORTED model failed is redone ONCE with the default model (redone_; translatorFile.redo, the rows' `redo`), a single press keeps
// U2's rule; the host's notice naming the failing model ONCE (Host::modelFailed's `say`); the models the running jobs use
// (modelsInUse, Host::modelsInUseChanged: a model in use cannot be removed). G2 (the user's answer): a rename -- or a subtitle
// edit -- rewrites the transcript's Word file IN PLACE by itself while it is SuVtitle's and unchanged (docxSoon / flushDocxDue,
// Host::rewriteTranscript; coalesced, never lost); "open in another program" said (docxErr) and retried by the next change.
// FX (1.1.0, 02/11/2026 -- the adversarial review's fix round, docs/PROGRESS_FIX.md): F1 the stamps' timecode is the one this run
// READ, else the one the Word file was written with (stampTc; its record's DOCXTC) -- an unknown or failed read never makes a
// Word file "older"; F2 the user's rule, ONE Word file per media AND sound track: another analysis of the same track (another
// pair or model) never writes it (docxState "other", Host::docxWriter); F4 a refusal of the Word file written before is said with
// "older" (docxErr), never as a refusing folder; F5 a row says "redone" once the redo STARTED (redoRan_); F6 each row says where
// its Word file is, or why there is none (`tdoc`), and Transcribe all counts them in ONE notice; F7 the rows say which ones a
// batch has (`batch`: what Cancel all acts on); F8 a rename names a speaker of the numbering it was typed against (CueSet::vrev).
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
#include "TrTranscript.h"  // U3: the transcript (pieces, speakers, turns; SourceTimecode.h's stamps)

#pragma comment(lib, "shlwapi.lib")

namespace pbplayer {

namespace tp = pbt::trpipe;
using patchbus::JsonW;

const wchar_t* const kMediaHostSuffix = L".suvtitle-media.example";
const wchar_t* const kProxyHost       = L"suvtitle-proxy.example";
constexpr size_t   kQueueMax      = 2000;     // items in the queue
constexpr size_t   kMapMax        = 16;       // folders mapped per page load (measured: STEP0 M7)
                                              // (U1: kPreAhead went with the pre-analysis -- only "Translate all" analyses
                                              //  a file that is not playing)
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
// U3: the transcript
constexpr uint64_t kTranscriptGrowMs = 2000;  // a transcript that GROWS (an analysis running) is posted at most this often
constexpr uint64_t kDocxRenameMs  = 1000;     // U2b (G2): a rename's (or an edit's) Word file rewrite: this long after the LAST one
constexpr size_t   kTcKeep        = 64;       // source timecodes remembered per tray run (by mediaKey)
constexpr uint64_t kTcWaitMs      = 3000;     // a batch file's Word file waits at most this long for its timecode
constexpr size_t   kTurnsPostMax  = 6000;     // turns in one translatorTranscript (a longer one says `cut`)
constexpr int      kUnmatchedSuggestPct = 10; // more of the speech than this unheard by the voice pass: Re-transcribe suggested
constexpr uint64_t kDocxWatchSlowMs = 250;    // FX2-B: a watch read of the playing file's Word file this slow stops that watch

inline std::wstring w16(const std::string& s) { return pbtrf::fromUtf8(s); }
inline std::string  u8(const std::wstring& w) { return pbtrf::toUtf8(w); }
inline std::wstring lowerW(const std::wstring& s) { return pbtrf::lowerW(s); }

// FX2-B (03/11/2026, docs/PROGRESS_FX2.md): a path's FORM, for Host::onNetwork -- a UNC path ("\\server\share\...", or the long
// "\\?\UNC\server\share\...", either slash), never the long-path prefix "\\?\C:\..." nor the device namespace "\\.\..."; and the
// drive letter a path names ("C:\...", "\\?\C:\...", "\\.\C:\..."; 0 = none)
inline bool slashW(wchar_t c) { return c == L'\\' || c == L'/'; }
inline bool uncPath(const std::wstring& p) {
    if (p.size() < 3 || !slashW(p[0]) || !slashW(p[1])) return false;
    if (p.size() >= 4 && (p[2] == L'?' || p[2] == L'.') && slashW(p[3]))       // a prefix, not a server
        return p[2] == L'?' && p.size() >= 8 && (p[4] | 0x20) == L'u' && (p[5] | 0x20) == L'n' && (p[6] | 0x20) == L'c' && slashW(p[7]);
    return true;
}
inline wchar_t driveOf(const std::wstring& p) {
    const size_t at = p.size() >= 4 && slashW(p[0]) && slashW(p[1]) && (p[2] == L'?' || p[2] == L'.') && slashW(p[3]) ? 4 : 0;
    if (p.size() < at + 2 || p[at + 1] != L':') return 0;
    const wchar_t c = p[at];
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ? c : 0;
}

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
// (U1: a 1.0.x file's `OPT translate` line -- the old global switch -- is read and ignored; it is never written again.)
struct QueueFile {
    Repeat repeat = Repeat::Off;
    bool shuffle = false;
    long long cur = -1;
    struct It { std::wstring path; uint64_t size = 0, mtime = 0, durMs = 0; };
    std::vector<It> items;
    std::vector<size_t> order;           // the shuffle order, as indexes into items
};
inline std::string packQueue(const QueueFile& q) {
    std::string o = "# AudioPB player v1\n";
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
            if (t[1] == "translate") { /* U1: 1.0.x's global switch -- read, ignored (nothing starts by itself) */ }
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
    // U2: the MODELS these cues are made with -- an imported model's path ("" = the default) for each half, and the key's
    // model tag (pbtrf::modelsTag: "" when both are the defaults, the pre-U2 key). Its FILE names the paths.
    std::string  asrPath, mtPath, models;
    // U3: THE TRANSCRIPT of this analysis -- the voice pass' result, <cache>\<key>.voices (read once: voicesRead), or made by
    // FVOICE: the speakers of its pieces, the user's names (NAME), the Word file written for it (DOCX). Kept through edits and
    // a re-analysis (the new cues are named by overlap: TrTranscript.h speakersOf).
    bool         voicesRead = false;
    bool         hasVoices = false;
    pbtrf::VoiceRec voices;
    std::wstring tnote;                                        // the Word file could not be written: why (said once)
    std::wstring docxErr;                                      // U2b (G2): its automatic rewrite failed -- why ("open in another
                                                               // program"); cleared by the next one that lands, or a Save
    // FX (F8): the NUMBERING of its speakers -- bumped each time a voice pass' result REPLACES its voices (a pass that could not run
    // leaves no names: nothing to rename); a rename keeps it. A rename names a speaker of the numbering it was typed against
    // (translatorTranscript.vrev).
    uint64_t     vrev = 0;
    // FX2-B (03/11/2026): its record's Word file is on the NETWORK (Host::onNetwork) -- asked where that path is SET (ensureVoices,
    // writeDocx, transcriptSaved: docxPathSet), read by the 1 Hz watch, which never stats such a file
    bool         docxRemote = false;
};

struct Job {
    uint64_t wire = 0;
    std::shared_ptr<CueSet> set;
    bool     pre = false;              // not the current item's: a job of "Translate all" (U1; it was the pre-analysis)
    uint32_t nextIdx = 0;              // the helper's next expected FCUE idx (dedupe)
    std::vector<tp::MsRange> lastProg; // the latest FPROG's ranges: vouched for by the NEXT message
    bool     cancelSent = false;
    std::string phase = "loading";     // loading | analyzing (U3: voices)
    bool     liveHit = false;          // SUVTITLE LIVE: it was running when the live gate closed (cancelled by it)
    // U3: an FVOICE -- the transcript's VOICE PASS on the complete analysis `set` -- not a FILE. Its pieces as sent (FSPK's
    // labels go with them in order), its FSPK, its progress.
    bool     voice = false;
    std::vector<tp::MsRange> vspans;
    bool     spkGot = false;
    tp::WireSpk spk;
    uint64_t vdone = 0, vtotal = 0;
};

struct Session {
    uint64_t gen = 0, seq = 0, itemId = 0;
    std::wstring path, folder, name, host, url;
    uint64_t size = 0, mtime = 0;
    std::string mediaKey;
    std::shared_ptr<CueSet> set;
    uint64_t wire = 0;                 // the job working on `set` for this session (0 = none)
    bool     wanted = false;           // U1: the viewer pressed Translate for THIS file (it ends with the session)
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
    // 1.1.0 (fix round, L6 -- DECIDED): the sound tracks as MPV lists them (MpvPlayer's audioList, offered by the bridge for
    // THIS opening), the item's track list where MediaTracks has none (a container it does not read: AVI, TS, MPG, WMV,
    // FLV...; or a probe that failed). mpvOrd = the track mpv plays (aid=auto's pick, or the one asked); tracksFromMpv =
    // `tracks` IS mpv's list (adopted), never a MediaTracks one.
    std::shared_ptr<const pbmedia::Info> mpvTracks;
    int      mpvOrd = -1;
    bool     tracksFromMpv = false;
    bool     mpvFinal = false;                // mpv's choice of track is final (its first restart), or it let the item go
    // U3: THE TRANSCRIPT's wish for THIS file (the viewer pressed Transcribe; it ends with the session, like `wanted`), the
    // voice job working on `set` for it (0 = none), its own cancelled / failed marks (Transcribe's Cancel: never Translate's),
    // and a Re-transcribe waiting for the engine (the voice pass runs again although the transcript has voices)
    bool     twanted = false;
    uint64_t vwire = 0;
    bool     tUserCancelled = false, tFailed = false, tForce = false;
    std::wstring terr;
    // U2b: a batch ("Translate all" / "Transcribe all") REDOES this file with the default model -- its imported model failed:
    // the strip says it (translatorFile.redo, the whole sentence; "" = not redone). Cleared by the viewer's own press.
    std::wstring redo;
    // FX2-B (03/11/2026): the media is on the NETWORK (Host::onNetwork, asked at the load) -- its Word file is never watched; and
    // the Word file (analysis key + path) whose watch read was SLOW (>= kDocxWatchSlowMs): not watched again until the next load
    bool         mediaRemote = false;
    std::string  watchSlowKey, watchSlowPath;
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
    // 1.1.0 (fix round, L2): the native video engine (mpv) plays the CURRENT item -- or is about to: its opening is asked
    // right after the core loads it. Then the SOUND COPY (FPROXY) is never asked: it exists for the page's own <video>
    // only (an item that falls back asks for it then: Player::mpvDropped). Not pure: a host without the engine (the
    // smoke's) answers false, as before 1.1.0.
    virtual bool mpvPlays() { return false; }
    // U2: an analysis ended because an IMPORTED model failed (missing, wrong kind, would not load: TrPipe.h
    // fileModelErrKind) -- `kind` "asr" | "mt", `path` the model, `sentence` the engine's words; the core has ALREADY gone
    // back to the default for that kind (when it still was the choice). The host persists it and updates the menu. Never
    // calls the core. U2b (02/11/2026): `say` = the host says it in ONE notice -- only when THIS failure made the choice fall
    // back (a second job failing on a model that is no longer chosen says nothing again: the message ONCE, not once per file),
    // and the strip does not say it (a file not on screen, or one a batch redoes at once: the strip then shows the redo);
    // `redone` = the name of the file a batch redoes with the default model ("" = none).
    virtual void modelFailed(const std::string& kind, const std::string& path, const std::wstring& sentence, bool say,
                             const std::wstring& redone) {
        (void)kind; (void)path; (void)sentence; (void)say; (void)redone;
    }
    // U2b: the imported models the analyses use NOW changed (a FILE job naming one started, or ended): the host's menu disables the
    // "x" (remove) of a model in use (its removal is refused meanwhile: modelInUse). Never calls the core back.
    virtual void modelsInUseChanged() {}
    // ---- U3: THE TRANSCRIPT (no-op defaults: a host without it -- an older fake -- answers "nothing") -----------------
    // the voice pass (TranslatorManager::startVoice: exactly one FDONE per FVOICE, FSPK before an FDONE ok)
    virtual void sendVoice(const tp::WireVoice& v) { (void)v; }
    // <cache>\<key>.voices (TrFileStore.h; key = the analysis' cache key): read whole / its head only (the badges) / written
    virtual bool readVoices(const std::string& key, pbtrf::VoiceRec& v) { (void)key; (void)v; return false; }
    virtual bool peekVoices(const std::string& key, pbtrf::VoiceRec& v) { (void)key; (void)v; return false; }
    virtual void writeVoices(const std::string& key, const pbtrf::VoiceRec& v) { (void)key; (void)v; }
    // The transcript's Word file for `set` (the media's folder; TrFileStore.h transcriptCandidates / chooseTranscriptPath: never
    // over a file that is not ours for this media; `t.prefer`, still ours, is replaced in place; a folder that refuses ->
    // Documents\SuVtitle Transcripts outside a test instance). Returns its path and identity as written ("" = not written,
    // `note` says why).
    virtual std::wstring writeTranscript(const CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& note,
                                         uint64_t& size, uint64_t& mtime) {
        (void)set; (void)t; (void)size; (void)mtime;
        note = L"The Word file cannot be written here.";
        return std::wstring();
    }
    // The file's SOURCE timecode (SourceTimecode.h readFileTimecode), on a WORKER -> Player::onTimecode(req, tc).
    virtual void probeTimecode(uint64_t req, const std::wstring& path) { (void)req; (void)path; }
    // U2b (G2, the user's answer 02/11): the Word file written for this transcript REWRITTEN IN PLACE -- `t.prefer`, only while
    // it is still OURS for this media (the registry); never a new name, never another folder. True = written (its identity
    // as written). False = not written: `why` says it in one sentence ("open in another program" -- the next change retries),
    // or is empty when the file is simply not SuVtitle's to touch any more (nothing said).
    virtual bool rewriteTranscript(const CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& why, uint64_t& size,
                                   uint64_t& mtime) {
        (void)set; (void)t; (void)size; (void)mtime;
        why.clear();
        return false;
    }
    // FX (F2, the user's rule 02/11: ONE Word file per media AND sound track): WHICH analysis' transcript a Word file holds now, as
    // the registry says -- `path` given: the entry for THAT file; `path` empty: this set's SOUND TRACK's Word file (the newest one
    // SuVtitle wrote for this media and track that is still there). Its analysis key; "" = none known (a host without the registry).
    virtual std::string docxWriter(const CueSet& set, const std::wstring& path) { (void)set; (void)path; return std::string(); }
    // FX2-B (03/11/2026, docs/PROGRESS_FX2.md): is this path on the NETWORK? -- a UNC path (uncPath) or a drive letter whose type is
    // remote (a mapped letter: GetDriveTypeW, the redirector's own answer, no round trip to the share). The core never stats such
    // a file once a second (a share that drops blocks a stat up to the SMB timeout, on the UI thread) -- asked where a path is SET
    // (the load, the record's Word file), never in a tick. The bridge's host keeps this default; a test host says it per path.
    virtual bool onNetwork(const std::wstring& path) {
        if (uncPath(path)) return true;
        const wchar_t d = driveOf(path);
        if (!d) return false;
        const wchar_t root[4] = { d, L':', L'\\', 0 };
        return ::GetDriveTypeW(root) == DRIVE_REMOTE;
    }
};

class Player {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    explicit Player(Host& h) : host_(h) {}

    // ---- what the engine runs (STATE echo), and the quality the window asks for ----------------
    void setPair(const std::string& a, const std::string& b, const std::string& mode) {
        if (a == langA_ && b == langB_ && mode == mode_) return;
        langA_ = a; langB_ = b; mode_ = mode;
        cancelPre();                   // a batch job for the old pair stops: "Translate all" goes on in the new one (U1)
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
    // U2 ("Import model..."): the MODELS the next analyses run on -- each an imported model's absolute path and its identity
    // (pbtrf::modelIdent), or {"", ""} = SuVtitle's default. Like a quality change: the batch's job re-starts on the new choice,
    // the summaries (badges) follow the new keys; the playing file's set stays on screen, said "made with another model"
    // (translatorFile.modelsStale) until Translate / Re-analyse asks for the new one.
    struct ModelPick {
        std::string path, ident;
        bool operator==(const ModelPick& o) const { return path == o.path && ident == o.ident; }
    };
    void setModels(const ModelPick& asr, const ModelPick& mt) {
        const ModelPick a = asr.path.empty() ? ModelPick{} : asr, m = mt.path.empty() ? ModelPick{} : mt;
        if (a == asrPick_ && m == mtPick_) return;
        asrPick_ = a;
        mtPick_ = m;
        modelsTag_ = pbtrf::modelsTag(a.ident, m.ident);
        cancelPre();
        refreshSummaries();
        stateDirty_ = true;
        queueDirty_ = true;
        schedule();
        flush();
    }
    const ModelPick& asrModel() const { return asrPick_; }
    const ModelPick& mtModel() const { return mtPick_; }
    const std::string& modelsTag() const { return modelsTag_; }
    // U2b: the imported models an analysis USES now -- every FILE job still in jobs_ (running, or cancelled with its FDONE still
    // owed: the engine may still have it in hand), never a voice pass (it loads no whisper / GGUF model). A model in use cannot
    // be removed (the bridge's double guard behind the page's disabled "x").
    std::vector<std::string> modelsInUse() const {
        std::vector<std::string> v;
        auto add = [&](const std::string& p) {
            if (p.empty()) return;
            for (const auto& x : v) if (sameModelPath(x, p)) return;
            v.push_back(p);
        };
        for (const Job& j : jobs_) if (!j.voice) { add(j.set->asrPath); add(j.set->mtPath); }
        return v;
    }
    bool modelInUse(const std::string& path) const {
        for (const auto& p : modelsInUse()) if (sameModelPath(p, path)) return true;
        return false;
    }
    // (Windows compares file names without case: ASCII letters folded, as AppConfig.h samePathCi does for app.txt)
    static bool sameModelPath(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
            if (x != y) return false;
        }
        return true;
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
                if (j.voice) {                               // U3: a voice pass: nothing to save, its transcript says why
                    if (loaded_ && s_.vwire == j.wire) {
                        s_.vwire = 0;
                        s_.tFailed = true;
                        s_.terr = L"The SuVtitle engine was stopped.";
                        stateDirty_ = true;
                        transcriptNow();
                    }
                    continue;
                }
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
            noteModelsInUse();                               // U2b: no job left, no model in use
        }
        schedule();
        flush();
    }

    // ---- SUVTITLE LIVE (1.1.0): THE LIVE GATE -----------------------------------------------------------------------
    // Live translation and a file analysis never run at the same time, and the live one wins (the user, 24/10: "on ne
    // fait pas tourner les deux ensemble"). The bridge closes the gate while the app WANTS to listen or the engine SAYS it
    // does (bridge.cpp), and opens it once both are over. Closed: no FILE leaves (ensureCurrentJob, schedule: "Translate
    // all" too -- U1), every running job is cancelled -- its FDONE keeps its vouched part, the TRUSTED RANGES rule --
    // and the strip says why (phase "live": a notice, not an error; the film plays with the subtitles already made).
    // Opened: schedule() starts the current item again, its FILE carrying `have` = what is vouched -- nothing is
    // analysed twice. A FILE that crossed the LIVE on the wire is refused by the engine with an FDONE "error": an FDONE
    // received while the gate is closed, or for a job the gate cancelled, never marks anything failed (onDone).
    void setLive(bool on) {
        if (on == live_) return;
        live_ = on;
        if (on) for (Job& j : jobs_) { j.liveHit = true; cancelJob(j); }
        stateDirty_ = true;
        schedule();
        flush();
    }
    bool live() const { return live_; }

    // ---- persistence ---------------------------------------------------------------------------------
    // WH: player-audio.txt -- the user's sound-track choices (before load(): the badges' keys depend on them)
    void loadAudio(const std::string& audioText) { parseAudioPicks(audioText, picks_); }
    void load(const std::string& queueText, const std::string& resumeText) {
        parseResume(resumeText, resume_);
        QueueFile q;
        if (parseQueue(queueText, q)) {
            // (U1: a 1.0.x file's `OPT translate 1` starts nothing -- parseQueue reads it and drops it)
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
    // twice in one call. U1: `analyseFirst` = the file that plays is WANTED at once, as if Translate were pressed on it
    // (an "open and translate" door; the bridge passes false everywhere: a file opened is played, never analysed by itself).
    void open(const std::vector<std::wstring>& paths, bool analyseFirst, const std::vector<Known>* known = nullptr) {
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
        if (firstId) loadItem(indexOf(firstId), true, -1, analyseFirst);
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
                else { cancelPreFor(it); forgetBatch(it.id); }
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
    // (U1: setTranslate -- 1.0.x's global On/Off -- is gone: playerSet{translate} is no longer read. A file is analysed when
    //  Translate is pressed on it (analyse) or by "Translate all" (analyseAll), and stopped by Cancel / Cancel all.)
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
    // TRANSLATE -- the press (U1: translatorFileAnalyse{force:false}) -- and the re-analyses. The playing file becomes
    // WANTED (Session::wanted: this file only, while it plays); a cancelled or failed one is asked again.
    // force = discard what this file has (complete or partial) and analyse it all again; else resume
    // from the vouched ranges -- or load the cache of the pair running NOW (the "languages changed" case).
    void analyse(bool force) {
        if (!loaded_) return;
        s_.wanted = true;
        s_.redo.clear();                                     // (U2b: the viewer's own press -- a batch's redo is not said any more)
        batchCancelled_.erase(s_.itemId);
        if (s_.set) failedKeys_.erase(s_.set->key);
        // U3: Translate pressed while the analysis already runs for the TRANSCRIPT (the same key): it JOINS it -- whisper runs
        // once whatever is pressed
        if (!force && transcribeWantedNow() && runningHeard()) { stateDirty_ = true; markItem(s_.itemId); schedule(); flush(); return; }
        runAnalysis(force);
    }
    // U3: the press of Translate (analyse) and of Transcribe (transcribe) share this -- the analysis of the playing file, for
    // whichever wish asked; its body is U1's analyse unchanged.
    void runAnalysis(bool force) {
        flushDocxDue();                                      // (U2b G2: a rename's Word file first -- its record before a new set reads it)
        // THE PRESS on what is on screen (U1): no job, the set shown IS the heard track's in the pair running now, and it is
        // not complete -- the job simply starts from what is vouched (ensureCurrentJob), with NO new generation: the
        // subtitles already shown stay as they are (a cached partial, a cancelled run's part)
        if (!force && !s_.wire && s_.set && !s_.set->complete && s_.set->key == heardKey()) {
            s_.userCancelled = s_.failed = s_.cancelling = false;
            s_.err.clear();
            stateDirty_ = true;
            markItem(s_.itemId);
            ensureCurrentJob(s_.lastPosMs);
            schedule();
            flush();
            return;
        }
        // the set going away first writes an edit not saved yet (the 2 s rule had not come: review of WG #15)
        if (s_.set && s_.set->editDirty) saveEdits(*s_.set);
        if (s_.wire) { if (Job* j = job(s_.wire)) cancelJob(*j); s_.wire = 0; }
        auto set = makeSet(s_.path, s_.name, s_.folder, s_.size, s_.mtime, heardSel());   // WH: the track HEARD now
        const auto keep = s_.set;
        // U3: a voice pass running on another set (another pair, or a re-analysis: force) stops -- the engine's one slot
        // goes to the analysis; one running on THIS analysis (a batch's, or the playing file's) goes on (adopted below)
        if (s_.vwire) {
            Job* vj = job(s_.vwire);
            if (vj && (force || vj->set->key != set->key)) { cancelJob(*vj); s_.vwire = 0; }
        }
        if (!force) {
            // resume what is held -- as a COPY of its vouched part: the job just cancelled may still say
            // a last word into the old set, and none of it must land in the one being shown
            if (keep && keep->key == set->key && !keep->complete) set = vouchedCopy(*keep);
            else if (Job* pj = jobForKey(set->key)) set = pj->set;                 // a batch job has it
            else if (Job* vj = voiceJobForKey(set->key)) set = vj->set;            // U3: its voice pass runs (a complete set)
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
        ensureVoices(*set);                                  // U3: the transcript of this analysis, if it has one
        if (Job* vj = voiceJobForKey(set->key)) { vj->pre = false; s_.vwire = vj->wire; }
        if (Job* pj = jobForKey(set->key)) { pj->pre = false; s_.wire = pj->wire; }
        else ensureCurrentJob(s_.lastPosMs);
        markItem(s_.itemId);
        transcriptNow();
        postStateNow();
        postCuesNow();
        schedule();
        flush();
    }
    // ---- U3: TRANSCRIBE (translatorTranscribe{force}) ---------------------------------------------------------------------
    // The playing file's TRANSCRIPT is wished (Session::twanted: this file, while it plays). Its analysis first when it has
    // none in the pair running now -- Translate's press path (runAnalysis), or JOINED when it already runs -- then the voice
    // pass on the complete analysis (ensureCurrentVoice); its Word file at the end. force = RE-TRANSCRIBE: the voice pass again
    // on the analysis kept (names carried over by time), then the Word file again. A transcript complete already: its Word
    // file is (re)written when missing or older and still ours (Transcribe all's rule), nothing runs.
    void transcribe(bool force) {
        if (!loaded_) return;
        s_.twanted = true;
        s_.redo.clear();                                     // (U2b: the viewer's own press)
        tbatchCancelled_.erase(s_.itemId);
        s_.tUserCancelled = s_.tFailed = false;
        s_.terr.clear();
        if (s_.set) { tfailedKeys_.erase(s_.set->key); failedKeys_.erase(s_.set->key); }
        const bool analysed = s_.set && s_.set->complete && s_.set->key == heardKey();
        if (analysed) {
            ensureVoices(*s_.set);
            if (force || !s_.set->hasVoices) {
                s_.tForce = force && s_.set->hasVoices;
                if (s_.vwire) { if (Job* vj = job(s_.vwire)) { if (force) cancelJob(*vj); } if (force) s_.vwire = 0; }
                ensureCurrentVoice();
            } else {
                docxIfNeeded(*s_.set);
            }
        } else if (!runningHeard()) {
            s_.userCancelled = s_.failed = s_.cancelling = false;
            s_.err.clear();
            transcriptNow();                                 // (the tab: waiting / analysing, posted by runAnalysis)
            runAnalysis(false);
            return;
        }
        stateDirty_ = true;
        transcriptNow();
        markItem(s_.itemId);
        schedule();
        flush();
    }
    // TRANSCRIBE ALL (translatorTranscribeAll{quality}): every file of the list (not missing) is QUEUED unless this run already
    // KNOWS its transcript complete with an up-to-date Word file; one job at a time (FILE or FVOICE), the playing file first,
    // then the LIST's order; per file, at its turn, all its work: its analysis if incomplete, its voice pass if none, its Word
    // file if missing or older (still ours) -- a file found complete and up to date leaves with nothing done (schedule).
    void transcribeAll() {
        for (const Item& it : items_) {
            if (it.missing || knownTranscribed(it)) continue;
            tbatchQ_.insert(it.id);
            tbatchCancelled_.erase(it.id);
            redone_.erase(it.id);                            // (U2b: asked again = a new batch: its one retry again)
            redoRan_.erase(it.id);                           // (FX F5: and what its row says of it)
            const std::string k = keyOf(it);
            tfailedKeys_.erase(k);
            failedKeys_.erase(k);
        }
        if (loaded_ && tbatchQ_.count(s_.itemId)) {
            s_.tUserCancelled = s_.tFailed = false;
            s_.terr.clear();
            if (!s_.set || !s_.set->complete) { s_.userCancelled = s_.failed = false; s_.err.clear(); }
            stateDirty_ = true;
            transcriptNow();
        }
        queueDirty_ = true;
        schedule();
        flush();
    }
    // TRANSCRIBE's CANCEL (translatorTranscriptCancel{jobId}): the playing file's transcript wish ends -- its voice pass stops;
    // its ANALYSIS goes on when Translate still wants it, else it is cancelled (its vouched part kept, FDONE). In "Transcribe
    // all" the file is marked cancelled and the batch goes on. Never touches Translate's wish.
    void transcriptCancel(uint64_t gen) {
        if (!loaded_ || gen != s_.gen) return;
        const bool queued = tbatchQ_.erase(s_.itemId) > 0;
        if (!s_.twanted && !queued && !s_.vwire) return;
        s_.twanted = false;
        s_.tForce = false;
        if (queued) tbatchCancelled_.insert(s_.itemId);
        if (s_.vwire) { if (Job* vj = job(s_.vwire)) cancelJob(*vj); s_.vwire = 0; }
        s_.tUserCancelled = true;
        if (!wantedNow() && s_.wire) {                       // nobody wants the analysis any more
            if (Job* j = job(s_.wire)) cancelJob(*j);
            s_.cancelling = true;
            s_.userCancelled = true;
        }
        stateDirty_ = true;
        transcriptNow();
        markItem(s_.itemId);
        schedule();
        flush();
    }
    // RENAME A SPEAKER (translatorSpeakerName{jobId, id, label, vrev}): the speaker's name for EVERY turn -- and for every speaker
    // that shares its name now (two speakers given one name are one person: renaming that person renames both; "" sends each
    // back to its own "Speaker <n>", which undoes a merge). Kept in the voices record (NAME lines); the Word file follows (U2b G2:
    // docxSoon). False (nothing changed) when refused.
    // FX (F8, the review 02/11): `gen` and `vrev` name the transcript the name was TYPED for (the page takes both when its field
    // opens) -- another file, a re-analysis or a re-track (the generation), or a Re-transcribe that ended meanwhile and numbered
    // the speakers again (vrev, the same generation): refused, said in one sentence; nobody else is renamed.
    bool renameSpeaker(uint64_t gen, uint64_t vrev, uint32_t id, const std::string& label, std::wstring* why = nullptr) {
        if (!loaded_ || !s_.set) return false;
        if (gen != s_.gen || vrev != s_.set->vrev) {
            if (why) *why = L"The speaker was not renamed: the transcript changed while the name was being typed.";
            return false;
        }
        if (!s_.set->hasVoices || !s_.set->voices.on) return false;
        pbtrf::VoiceRec& V = s_.set->voices;
        if (id == 0 || id > V.speakers) return false;
        std::string clean, err;
        if (!pbtrs::nameOk(label, clean, err)) { if (why) *why = w16(err); transcriptNow(); flush(); return false; }
        std::map<uint32_t, std::string> names(V.names.begin(), V.names.end());
        const std::string was = pbtrs::foldName(pbtrs::speakerName(id, names));
        for (uint32_t k = 1; k <= V.speakers; ++k) {
            if (pbtrs::foldName(pbtrs::speakerName(k, names)) != was) continue;
            if (clean.empty()) names.erase(k);
            else names[k] = clean;
        }
        V.names.assign(names.begin(), names.end());
        host_.writeVoices(s_.set->key, V);
        docxSoon(s_.set);                                    // U2b (G2): its Word file follows, kDocxRenameMs after the LAST rename
        transcriptNow();
        flush();
        return true;
    }
    // TRANSLATE ALL (U1 -- the user: "un bouton Transcription/Traduction qui le lance sur TOUS les fichiers de la liste";
    // translatorFileAnalyseAll{quality}): every file of the queue that is not complete yet (and not missing) is QUEUED; they
    // are analysed one after another -- one FILE job at a time, the playing file first, then in the LIST's order (schedule).
    // Its failed and cancelled marks are cleared: asked again. A file whose cache record has not been read yet is queued too:
    // the scheduler takes it out when it turns out complete. The live gate holds it all, as everything (resumed after).
    void analyseAll() {
        for (const Item& it : items_) {
            if (it.missing || knownComplete(it)) continue;
            batchQ_.insert(it.id);
            batchCancelled_.erase(it.id);
            failedKeys_.erase(keyOf(it));
            redone_.erase(it.id);                            // (U2b: asked again = a new batch: its one retry again)
            redoRan_.erase(it.id);                           // (FX F5: and what its row says of it)
        }
        if (loaded_ && batchQ_.count(s_.itemId)) {
            s_.userCancelled = s_.failed = false;
            s_.err.clear();
            stateDirty_ = true;
        }
        queueDirty_ = true;
        schedule();
        flush();
    }
    // CANCEL ALL (U1, the queue panel; translatorFileCancelAll{}) -- U2, THE USER'S RULE (02/11): it cancels ONLY what the
    // LIST launched ("Translate all"): the files still queued are marked cancelled, the batch's jobs for files not playing
    // are cancelled (the vouched part of each is kept, FDONE) -- and the PLAYING file's job only when the batch launched it
    // (queued, not pressed). A playing file the user PRESSED Translate on is the strip's Cancel's: its job goes on (it only
    // leaves the batch, unmarked). Nothing starts again by itself.
    void cancelAll() {
        const bool curBatch = loaded_ && batchQ_.count(s_.itemId) > 0;
        const bool curT = loaded_ && tbatchQ_.count(s_.itemId) > 0;      // U3: "Transcribe all" had it too
        for (uint64_t id : batchQ_) batchCancelled_.insert(id);
        batchQ_.clear();
        for (uint64_t id : tbatchQ_) tbatchCancelled_.insert(id);        // U3: both batches, the same rule
        tbatchQ_.clear();
        for (Job& j : jobs_) if (j.pre) cancelJob(j);          // the batches' jobs (FILE and FVOICE) for files that are not playing
        if (curBatch && s_.wanted) {
            batchCancelled_.erase(s_.itemId);                   // pressed too: the press stands (the strip's Cancel stops it)
        } else if (curBatch && s_.twanted) {
            batchCancelled_.erase(s_.itemId);                   // U3: Transcribe pressed on it: that press keeps the analysis
        } else if ((curBatch || (curT && !s_.wanted && !s_.twanted)) && (s_.wire || (s_.set && !s_.set->complete))) {
            // launched by a batch (a complete file: nothing to stop)
            if (s_.wire) {
                if (Job* j = job(s_.wire)) cancelJob(*j);
                s_.cancelling = true;
            }
            s_.userCancelled = true;
            stateDirty_ = true;
        }
        // U3: the playing file's VOICE PASS, when only "Transcribe all" asked for it (a press of Transcribe keeps it)
        if (curT && s_.twanted) {
            tbatchCancelled_.erase(s_.itemId);
        } else if (curT) {
            if (s_.vwire) { if (Job* vj = job(s_.vwire)) cancelJob(*vj); s_.vwire = 0; }
            s_.tUserCancelled = true;
            stateDirty_ = true;
            transcriptNow();
        }
        queueDirty_ = true;
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
        if (ok) {                                                   // the strip says "edited"; the card's count follows (U3: the
            stateDirty_ = true;                                     // transcript too -- its Word file then follows it, U2b G2)
            if (loaded_ && s_.set) docxSoon(s_.set);
            transcriptNow();
            flush();
        }
        return ok;
    }

    // CANCEL (the strip; translatorFileCancel{jobId}): the playing file's analysis stops -- its job (its vouched part is kept:
    // FDONE), or (U1) the wish the live gate holds (phase "live": it will not start when Live stops). Its press is over; in
    // "Translate all" it is marked cancelled and the batch goes on with the next file.
    void cancel(uint64_t gen) {
        if (!loaded_ || gen != s_.gen) return;
        const bool queued = batchQ_.erase(s_.itemId) > 0;
        if (!s_.wire && !s_.wanted && !queued) return;
        s_.wanted = false;
        if (queued) batchCancelled_.insert(s_.itemId);
        // U3: the TRANSCRIPT still wants the analysis (Transcribe pressed, or "Transcribe all"): it goes on -- cancelling one
        // wish never stops the other's work; only the translation's press is over
        if (transcribeWantedNow()) {
            stateDirty_ = true;
            markItem(s_.itemId);
            schedule();
            flush();
            return;
        }
        if (s_.wire) {
            if (Job* j = job(s_.wire)) cancelJob(*j);
            s_.cancelling = true;
        }
        s_.userCancelled = true;
        stateDirty_ = true;
        markItem(s_.itemId);
        schedule();
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
        if (host_.mpvPlays()) return;                  // 1.1.0 (L2): mpv plays it -- a stale <video> failure (the copy is the <video>'s)
        requestProxy();
        flush();
    }

    // 1.1.0 (fix round, L6 -- DECIDED by the director, 26/10): the bridge offers MPV's sound-track list for the CURRENT
    // opening (seq; at mpv's Tracks / Loaded). It becomes the item's list where MediaTracks has none -- a container it does not
    // read (AVI, TS / M2TS, MPG / VOB, WMV, FLV, OGG...), or a probe that failed -- so the right-click menu, the CC panel and
    // setAudio work as for a Matroska file, and the analysis follows the same ord (the engine maps ord exactly as MpvPlayer
    // does). A MediaTracks list is NEVER overridden (a probe still running: its answer first). The analysis that already
    // runs on the track aid=auto picked is NOT restarted: with no pick of the viewer's, the track heard IS mpv's choice.
    // `info` = the list as the bridge built it from mpv (its strings capped there: the FILE's, untrusted; empty = mpv lists
    // no sound track); mpvOrd = the track mpv plays now (-1 = none / not known yet); final = mpv's choice will not change
    // by itself any more (its first restart).
    void offerMpvTracks(uint64_t seq, std::shared_ptr<const pbmedia::Info> info, int mpvOrd, bool final) {
        if (!loaded_ || !seq || s_.seq != seq || !info) return;
        s_.mpvOrd = mpvOrd;
        s_.mpvFinal = s_.mpvFinal || final;
        if (!info->audio.empty()) s_.mpvTracks = std::move(info);
        adoptMpvTracks();
        mpvWaitDone();
        flush();
    }
    // fix round (L2 / L3): mpv will not play this opening after all (it refused the file, the page gave up waiting, the
    // engine went away): it is the page's <video>'s -- an item with NO URL has its sound copy asked now (none was while mpv
    // was to play it: never an item left on "Preparing its sound..."), and a job held for mpv's track list starts.
    void mpvDropped(uint64_t seq) {
        if (!loaded_ || !seq || s_.seq != seq) return;
        s_.mpvFinal = true;
        mpvWaitDone();
        if (s_.url.empty()) requestProxy();            // (asked once per item: requestProxy's own guard)
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
    // choice is not the user's and is not kept). D5: for a file being analysed (U1: WANTED), a track other than the analysed
    // one starts a NEW GENERATION on it -- its own cache record, resumed from what it has. A file not being analysed (U1):
    // the subtitles follow the sound only when that track HAS an analysis (it is shown; nothing starts) -- else the ones on
    // screen stay, and Translate analyses the track heard.
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
        if (s_.set && s_.set->audio.ord != t.ord && followsSound(heardSel())) retrack(heardSel());
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
    // A sentence for the page's toast: an error by default (red); `info` = news, not a failure (FX2, 03/11/2026: Transcribe all's
    // one notice) -- `info: true` on the wire, as the bridge's own postTranslatorFileNotice says it (the page also keeps a subtitle
    // track the viewer waits for on an `info` notice). Held while the page reloads, flag and all.
    void notice(const std::wstring& text, bool info = false) {
        JsonW j;
        j.obj().kv(L"type", L"translatorFileNotice").kv(L"text", text);
        if (info) j.kv(L"info", true);
        j.end();
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
        transcriptNow();                                     // U3: the Transcript tab's content
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
        if (j->voice) {                                      // U3: a voice pass' progress -- the analysis is not touched
            j->phase = "voices";
            j->vdone = p.doneMs;
            if (p.totalMs) j->vtotal = p.totalMs;
            if (loaded_ && s_.vwire == j->wire) { stateDirty_ = true; transcriptGrow(); }
            else markKey(S.key);
            return;
        }
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
        if (!j || j->voice) return;                        // (U3: a voice pass sends no FCUE)
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
        if (isCurrent(*j)) { postCue(k); transcriptGrow(); }   // (U3: the Transcript tab's text grows with it)
    }
    // U3: FSPK -- the voice pass' names, once, just before its FDONE ok: one label per piece SENT (0 = none heard).
    void onSpk(const tp::WireSpk& s) {
        Job* j = job(s.jobId);
        if (!j || !j->voice) return;
        if (s.speakers > 0 && s.labels.size() != j->vspans.size()) return;   // not this pass' pieces: no names from it
        j->spk = s;
        j->spkGot = true;
    }
    // U3: a file's SOURCE timecode (Host::probeTimecode's answer, from a worker): kept per media (kTcKeep), the stamps follow.
    void onTimecode(uint64_t req, const pbtc::SourceTc& tc) {
        const auto r = tcReqs_.find(req);
        if (r == tcReqs_.end()) return;
        const std::string mk = r->second;
        tcReqs_.erase(r);
        TcEntry& e = tcs_[mk];
        e.known = true;
        e.tc = tc;
        trimTcs();
        if (loaded_ && s_.mediaKey == mk) { stateDirty_ = true; transcriptNow(); }
        schedule();
        flush();
    }
    void onDone(const tp::WireFileDone& d, bool synthesized) {
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& x) { return x.wire == d.jobId; });
        if (it == jobs_.end()) return;
        Job j = *it;
        jobs_.erase(it);
        noteModelsInUse();                                   // U2b: the models it used are free again (the menu's x follows)
        if (j.voice) { voiceDone(j, d); return; }           // U3: a voice pass ended
        if (!synthesized) j.set->confirmed = merged(j.set->confirmed, j.lastProg);   // the helper's own last word
        const bool current = loaded_ && s_.wire == j.wire;
        endJobSet(j, d.result, synthesized);
        // SUVTITLE LIVE: an "error" the LIVE MODE caused -- received while the gate is closed, or for a job the gate
        // cancelled (a FILE that crossed the LIVE on the wire) -- fails nothing: the item waits for the gate to open
        const bool liveErr = live_ || j.liveHit;
        const bool failed = d.result == "error" && !liveErr;
        // U2: an IMPORTED model failed (TrPipe.h fileModelErrKind: missing, of the wrong kind, would not load) -- which kind, which
        // file (a failure that is not an imported model's changes no choice)
        std::string fellKind, fellPath;
        if (failed) {
            const std::string k = tp::fileModelErrKind(d.err);
            if (k == "asr" && !j.set->asrPath.empty()) { fellKind = k; fellPath = j.set->asrPath; }
            else if (k == "mt" && !j.set->mtPath.empty()) { fellKind = k; fellPath = j.set->mtPath; }
        }
        // U2b (02/11/2026, the user's rule): a file a BATCH asked for ("Translate all", "Transcribe all", or both: ONE analysis)
        // whose imported model failed is REDONE ONCE with the default model -- it stays queued, no failed mark, and the default
        // runs it next (its key then has no "\tm" tag: the normal cache first); a second failure of it is a normal failure
        // (redone_). A single press (not in a batch) keeps U2's rule: the strip says it, no redo.
        const std::vector<uint64_t> batchIds = fellKind.empty() ? std::vector<uint64_t>() : batchItemsOf(*j.set);
        const bool redo = !batchIds.empty() && redoAllowed(fellKind, fellPath, batchIds);
        if (current) {
            s_.wire = 0;
            s_.cancelling = false;
            if (failed && !redo) {
                s_.failed = true;
                s_.err = d.err.empty() ? std::wstring(L"The analysis failed.") : w16(d.err);
                s_.redo.clear();                             // (U2b: a redo that failed in its turn is a failure, said as one)
                if (batchQ_.count(s_.itemId)) failedKeys_.insert(j.set->key);   // (U1: "Translate all" does not retry it)
                if (tbatchQ_.count(s_.itemId)) tfailedKeys_.insert(j.set->key); // (U3: nor "Transcribe all")
            }
            stateDirty_ = true;
            transcriptNow();                                 // U3: the transcript's text is complete (or stopped)
        } else if (j.pre && failed && !redo) {
            failedKeys_.insert(j.set->key);
            if (inTBatch(j.set->key)) tfailedKeys_.insert(j.set->key);
        }
        // U1: "Translate all" -- a file done (complete) or failed leaves the batch; a cancelled job's file stays queued (taken
        // over by the playing file, a new pair: resumed later) unless Cancel / Cancel all said otherwise (U2b: a file the batch
        // redoes stays queued too)
        if (d.result == "ok" || (failed && !redo)) batchFinished(j.set->key, failed);
        // U3: "Transcribe all" -- a file whose ANALYSIS failed leaves it too (its transcript cannot be made); one done stays
        // queued: its voice pass is next (schedule)
        if (failed && !redo) tbatchFinished(j.set->key, true);
        // U2: that kind goes back to the default for what comes next, when it still is the choice (the user may have moved on
        // since); the strip says so, the host persists it and updates the menu (Host::modelFailed, after this). The file's own
        // failure stands (asked again: Translate); "Translate all" goes on with the next files, on the default.
        bool fellBack = false;
        if (!fellKind.empty()) {
            ModelPick& pick = fellKind == "asr" ? asrPick_ : mtPick_;
            if (pick.path == fellPath) {
                pick = ModelPick{};
                modelsTag_ = pbtrf::modelsTag(asrPick_.ident, mtPick_.ident);
                refreshSummaries();
                queueDirty_ = true;
                fellBack = true;
            }
            if (current && !redo)
                s_.err += fellKind == "asr" ? L" SuVtitle goes back to its own transcription model (large-v3-turbo)."
                                            : L" SuVtitle goes back to its own translation model (Qwen3-4B).";
        }
        std::wstring redoneName;
        if (redo) {
            for (uint64_t id : batchIds) { redone_.insert(id); markItem(id); }   // (the rows say it: playerQueue / playerItem `redo`)
            redoneName = j.set->name;
            queueDirty_ = true;
            if (current) {
                // the PLAYING file: its analysis again at once, on the default (runAnalysis: a set of the default's key, its cache
                // first) -- the strip says why, the engine's sentence whole in its tooltip (translatorFile.redo)
                s_.redo = (d.err.empty() ? std::wstring() : w16(d.err) + L" ") + redoSentence(fellKind);
                runAnalysis(false);
            }
        }
        markKey(j.set->key);
        schedule();
        flush();
        // the host's ONE notice: only when THIS failure made the choice fall back, and the strip does not say it
        if (!fellKind.empty()) host_.modelFailed(fellKind, fellPath, w16(d.err), fellBack && (!current || redo), redoneName);
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
        // U3: a source timecode that does not come (a sleeping disk, a slow share) holds a batch's Word file kTcWaitMs at most:
        // it is then written with file-relative stamps (a late answer still updates the tab)
        for (auto& kv : tcs_)
            if (!kv.second.known && !kv.second.timedOut && now - kv.second.askedAt >= kTcWaitMs) kv.second.timedOut = true;
        // U2b (G2): the Word file of a renamed (or edited) transcript, once the burst is over -- ONE write for many renames
        if (docxDue_ && now >= docxDueAt_) flushDocxDue();
        // FX2-A: the playing file's Word file, once a second -- deleted, re-created, edited in Word: the tab and the row follow
        watchDocx();
        schedule();
        flush();
    }
    // The tray is quitting: every running job's VOUCHED part is saved (FDONE will not come), and the
    // queue and the positions are written now.
    void shutdown() {
        flushFiles();
        flushDocxDue();                                      // U2b (G2): the last rename never lost
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
        flushDocxDue();                                      // U2b (G2): the last rename never lost
        for (Job& j : jobs_) savePartial(*j.set);
        saveQueueNow();
        saveResumeNow();
        saveAudioNow();
    }

    // ---- read-only views (export, reveal, the bridge's own needs) -------------------------------------------
    bool loaded() const { return loaded_; }
    const Session& session() const { return s_; }
    // U1: the playing file is being analysed or asked to be (the press, or "Translate all"); the batch's queued files
    bool wanted() const { return wantedNow(); }
    const std::set<uint64_t>& batchQueued() const { return batchQ_; }
    Repeat repeat() const { return repeat_; }
    bool shuffle() const { return shuffle_; }
    const std::vector<Item>& items() const { return items_; }
    size_t current() const { return cur_; }
    const std::vector<Job>& jobs() const { return jobs_; }
    const std::string& quality() const { return quality_; }
    std::vector<uint64_t> playOrderIds() const { std::vector<uint64_t> v; for (size_t i : playOrder()) v.push_back(items_[i].id); return v; }
    const std::map<std::string, AudioPick>& audioPicks() const { return picks_; }   // WH (tests)
    // U3: the transcript's wish for the playing file (the press, or "Transcribe all"); the files "Transcribe all" still has
    bool transcribeWanted() const { return transcribeWantedNow(); }
    const std::set<uint64_t>& tbatchQueued() const { return tbatchQ_; }
    // U3: the playing file's transcript as the Word file would hold it NOW (the tab's Save .docx..., the Export menu's
    // "Transcript (.docx)"): false when there is no text yet. A partial one (its analysis not complete) says so (partial).
    // `prefer` = the Word file written for it, while still ours: the dialog offers that same file (it may replace it).
    bool transcriptForSave(pbtrs::TranscriptOut& out) {
        if (!loaded_ || !s_.set) return false;
        out = transcriptOutOf(*s_.set);
        if (out.turns.empty()) return false;
        out.partial = !s_.set->complete;
        out.doneMs = s_.set->complete ? s_.set->totalMs : tp::rangesTotal(merged(s_.set->confirmed));
        return true;
    }
    // ...and the file the user saved through it (SuVtitle registered it as ours for this media): a COMPLETE transcript keeps
    // it as its Word file (its identity and what it holds), so "older" is said against it from now on.
    // FX (F1): `tc` = the timecode of the stamps the Save wrote (TranscriptOut.tc): kept with the file, as an automatic write does.
    void transcriptSaved(const std::wstring& path, uint64_t size, uint64_t mtime, uint64_t sig, const pbtc::SourceTc& tc) {
        if (!loaded_ || !s_.set || !s_.set->complete || !s_.set->hasVoices || path.empty()) return;
        pbtrf::VoiceRec& V = s_.set->voices;
        V.docxPath = u8(path); V.docxSize = size; V.docxMtime = mtime; V.docxSig = sig;
        V.docxTc = voiceTcOf(tc);                            // (FX F1: what its stamps were made with)
        docxPathSet(*s_.set);                                // (FX2-B: saved to a share?)
        s_.set->tnote.clear();
        s_.set->docxErr.clear();                             // (U2b G2: the Save wrote it -- a failed automatic update is over)
        tdocWhy_.erase(s_.set->key);                         // (FX F6: its row has nothing to excuse any more)
        vdocx_[s_.set->key] = V.docxPath;
        docxSeen_.erase(s_.set->key);                        // (FX2: nor what it read of the file before -- saved over it, maybe)
        host_.writeVoices(s_.set->key, V);
        markKey(s_.set->key);
        transcriptNow();
        flush();
    }

    // Coalesced posts: the state and the queue at most once per call site. (U3: the transcript at once when it CHANGED --
    // a state, a name, an edit -- and at most every kTranscriptGrowMs while it only GROWS with the analysis.)
    void flush() {
        if (stateDirty_) postStateNow();
        if (queueDirty_) postQueueNow();
        for (uint64_t id : itemDirty_) postItem(id);
        itemDirty_.clear();
        if (transcriptDirty_ && (transcriptUrgent_ || host_.nowMs() - transcriptPostedAt_ >= kTranscriptGrowMs)) postTranscriptNow();
        // FX (F6): "Transcribe all" is over (nothing left in it) and some of its files have no up-to-date Word file next to them:
        // ONE notice counts them -- each row says where its Word file is, or why there is none
        if (!tdocMiss_.empty() && tbatchQ_.empty()) postTdocNotice();
    }

private:
    Host& host_;
    // what the engine runs / what the user chose
    std::string langA_ = "en", langB_ = "zh", mode_ = "auto", quality_ = "auto";
    ModelPick asrPick_, mtPick_;       // U2: the models the next analyses run on ({} = the default)
    std::string modelsTag_;            // ...their part of every cache key ("" = both defaults: the pre-U2 key)
    bool helper_ = false;
    bool live_ = false;                // SUVTITLE LIVE: the live gate (setLive) -- closed while the live mode listens
    // U1: "Translate all" -- the files it still has to analyse (item ids; LIST order decides, the playing one first), and the
    // ones whose analysis was cancelled (Cancel, Cancel all: the badge says so until they are asked again)
    std::set<uint64_t> batchQ_, batchCancelled_;
    // U3: "Transcribe all" -- its twin (item ids), the voice passes that failed this run (by analysis key), the source
    // timecodes (by mediaKey: asked on a worker, kTcKeep kept), the transcript post's coalescing, the voices' peeks (badges)
    std::set<uint64_t> tbatchQ_, tbatchCancelled_;
    std::set<std::string> tfailedKeys_;
    struct TcEntry { bool known = false, timedOut = false; pbtc::SourceTc tc; uint64_t askedAt = 0, req = 0; };
    std::map<std::string, TcEntry> tcs_;
    std::map<uint64_t, std::string> tcReqs_;
    bool transcriptDirty_ = false, transcriptUrgent_ = false;
    uint64_t transcriptPostedAt_ = 0;
    std::map<std::string, int> vsummaries_;              // analysis key -> 0 none / 1 named / 2 no names (a head peek)
    // U2b: the files a batch REDID with the default model after their imported model failed (item ids: once per file and batch;
    // asked again by Translate all / Transcribe all = a new batch), the imported models the running jobs use (their signature),
    // and G2's Word file rewrite waiting for the end of a burst of renames / edits (the set, and when)
    std::set<uint64_t> redone_;
    std::string inUseSig_;
    std::shared_ptr<CueSet> docxDue_;
    uint64_t docxDueAt_ = 0;
    // FX (02/11/2026, docs/PROGRESS_FIX.md): what the rows say of a redo -- it STARTED, or ended (F5; `redone_` stays the grant: the
    // once-per-press guard); each analysis' Word file as its record names it (F6: the head peek's DOCX, kept up to date by every
    // write -- vsummaries_' twin); this run's reason when an analysis has no Word file next to it, or could not update it (F6); the
    // analyses of "Transcribe all" whose Word file is not next to them (F6: its ONE notice, when nothing is left in it -- presses
    // made while it runs add to the same list and the same notice)
    std::set<uint64_t> redoRan_;
    std::map<std::string, std::string> vdocx_;
    std::map<std::string, std::wstring> tdocWhy_;
    std::set<std::string> tdocMiss_;
    // FX2 (03/11/2026, docs/PROGRESS_FX2.md): what the PLAYING file's row read of its analysis' Word file when it was another
    // analysis' (n7) or gone (A4) -- kept for that row once the file stops playing (a row not playing never reads the disk):
    // analysis key -> the reading. Forgotten by a reading back to normal (docxIdentity), by a write that lands (writeDocx) and by
    // a Save (transcriptSaved) -- every change of an analysis' Word file goes through one of them, so no path is kept to compare
    // (it could never differ).
    std::map<std::string, std::string> docxSeen_;
    // FX2-A (03/11/2026): the playing file's Word file as the last tick read it (watchDocx) -- its raw identity, with the analysis
    // key and the path it was read for
    struct DocxWatch { std::string key, path; bool there = false; uint64_t size = 0, mtime = 0; };
    DocxWatch docxWatch_;
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
    std::set<std::string> failedKeys_;            // batch analyses that failed this run (not retried until asked again)
    std::map<std::string, pbtrf::Summary> summaries_;
    std::map<std::string, ResumeEntry> resume_;
    bool stateDirty_ = false, queueDirty_ = false, queueSave_ = false, resumeDirty_ = false;
    std::set<uint64_t> itemDirty_;
    uint64_t queueChangedAt_ = 0, resumeSavedAt_ = 0;
    // WH: the film's own tracks
    // autoOrd (1.1.0 fix round, L6): for a list MPV gave, the track mpv chose by itself at its first opening in this run
    // (aid=auto: the default flag, then the system's language) -- a later opening with no pick plays it again (chooseOrd)
    struct ProbeEntry { std::string state; std::shared_ptr<const pbmedia::Info> info; uint64_t req = 0, seq = 0; int autoOrd = -1; };
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

    // ---- U1: WHAT IS ASKED FOR ----------------------------------------------------------------------------------------
    // The playing file is analysed only when it is WANTED: Translate pressed on it, or queued by "Translate all".
    bool wantedNow() const { return loaded_ && (s_.wanted || batchQ_.count(s_.itemId) > 0); }
    // The cache key of the playing file's analysis of the sound track HEARD, in the pair (and quality) running now.
    std::string heardKey() const {
        return pbtrf::cacheKey(s_.path, s_.size, s_.mtime, langA_, langB_, mode_, quality_, heardSel().ord, modelsTag_);
    }
    // Complete as far as this run already KNOWS (the playing file's set, or a summary already read): never a disk read --
    // "Translate all" over 2000 files must not read 2000 records on the UI thread (the scheduler reads them one at a time).
    bool knownComplete(const Item& it) const {
        if (loaded_ && s_.itemId == it.id && s_.set) return s_.set->complete;
        const auto f = summaries_.find(keyOf(it));
        return f != summaries_.end() && f->second.ok && f->second.complete;
    }
    // Does the playing file have an analysis of THIS sound track (complete or partial, the pair running now)? A file that
    // is not being analysed shows it when its sound moves there; else the subtitles on screen stay (nothing starts).
    bool hasAnalysis(const tp::AudioSel& sel) {
        const std::string k = pbtrf::cacheKey(s_.path, s_.size, s_.mtime, langA_, langB_, mode_, quality_, sel.ord, modelsTag_);
        if (jobForKey(k) || heldSet(k)) return true;
        pbtrf::Summary s;
        if (!host_.peekCache(k, s) || !pbtrf::samePath(s.path, s_.path) || s.size != s_.size || s.mtime != s_.mtime) return false;
        if (pbtrf::staleRecord(s.engine, s.edited)) return false;
        return s.ok && (s.complete || !s.ranges.empty());
    }
    // The subtitles follow a new sound track: always for a file being analysed (D5: the analysis moves there), else only
    // to a track that has an analysis of its own.
    bool followsSound(const tp::AudioSel& sel) { return analysisWanted() || hasAnalysis(sel); }
    void forgetBatch(uint64_t id) {
        batchQ_.erase(id); batchCancelled_.erase(id);
        tbatchQ_.erase(id); tbatchCancelled_.erase(id);      // (U3: "Transcribe all" too)
        redone_.erase(id);                                   // (U2b: and its "redone with the default model")
        redoRan_.erase(id);                                  // (FX F5: what its row says of it)
    }
    // FX (F5): the batch's redo of these items STARTED (its FILE job sent) or ended without one (the default's analysis found
    // complete at its turn): only now does the row say "Redone with the default model" -- never while the redo only waits, and
    // never after a Cancel all that came before it ran (a file out of the batches is never marked)
    void redoStarted(uint64_t id) {
        if (!redone_.count(id) || !(batchQ_.count(id) || tbatchQ_.count(id))) return;
        if (redoRan_.insert(id).second) markItem(id);
    }
    // ---- U2b: THE BATCH'S ONE RETRY (the user, 02/11) -------------------------------------------------------------------------
    // The items of the queue a BATCH ("Translate all" / "Transcribe all") still has for this set's FILE -- by its identity, whatever
    // the key says (the models are what just moved).
    std::vector<uint64_t> batchItemsOf(const CueSet& S) const {
        std::vector<uint64_t> v;
        const std::wstring p = pbtrf::lowerW(S.path);
        for (const auto& it : items_)
            if ((batchQ_.count(it.id) || tbatchQ_.count(it.id)) && it.size == S.size && it.mtime == S.mtime
                && pbtrf::lowerW(it.path) == p)
                v.push_back(it.id);
        return v;
    }
    // The batch may redo that file with the DEFAULT: that kind is the default for what comes next (it falls back now, or the user
    // chose the default since -- never another imported model: "redone with the default model"), and the file was not redone in
    // this batch yet (a second failure is a normal failure).
    bool redoAllowed(const std::string& kind, const std::string& path, const std::vector<uint64_t>& ids) const {
        const ModelPick& next = kind == "asr" ? asrPick_ : mtPick_;
        if (!next.path.empty() && next.path != path) return false;
        for (uint64_t id : ids) if (redone_.count(id)) return false;
        return true;
    }
    static std::wstring redoSentence(const std::string& kind) {
        return kind == "asr" ? L"Redone with SuVtitle's own transcription model (large-v3-turbo)."
                             : L"Redone with SuVtitle's own translation model (Qwen3-4B).";
    }
    // The imported models the running jobs use changed: the host's menu follows (the x of a model in use disabled)
    void noteModelsInUse() {
        std::vector<std::string> v = modelsInUse();
        for (auto& p : v) for (auto& c : p) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        std::sort(v.begin(), v.end());
        std::string sig;
        for (const auto& p : v) sig += p + "\n";
        if (sig == inUseSig_) return;
        inUseSig_ = sig;
        host_.modelsInUseChanged();
    }
    // A job of `key` ended complete (done) or failed: its file leaves the batch (a failure is remembered: failedKeys_)
    void batchFinished(const std::string& key, bool failed) {
        for (const auto& it : items_) {
            if (keyOf(it) != key) continue;
            if (batchQ_.erase(it.id)) queueDirty_ = true;
            if (!failed) batchCancelled_.erase(it.id);
        }
    }

    // ---- U3: THE TRANSCRIPT --------------------------------------------------------------------------------------------
    // The playing file's transcript is WISHED: Transcribe pressed on it, or queued by "Transcribe all".
    bool transcribeWantedNow() const { return loaded_ && (s_.twanted || tbatchQ_.count(s_.itemId) > 0); }
    // Its ANALYSIS runs when either wish wants it -- whisper once, whatever is pressed (Translate's and the transcript's).
    bool analysisWanted() const { return wantedNow() || transcribeWantedNow(); }
    // The playing file's analysis already runs in the pair running now: a press of the other button JOINS it.
    bool runningHeard() const {
        if (!s_.wire) return false;
        for (const Job& j : jobs_) if (j.wire == s_.wire) return !j.voice && !j.cancelSent && j.set->key == heardKey();
        return false;
    }
    bool inTBatch(const std::string& key) const {
        for (const auto& it : items_) if (tbatchQ_.count(it.id) && keyOf(it) == key) return true;
        return false;
    }
    void tunqueue(uint64_t id) { if (tbatchQ_.erase(id)) markItem(id); }
    // A transcript of `key` ended (its voice pass done or failed, or its analysis failed): its file leaves "Transcribe all"
    void tbatchFinished(const std::string& key, bool failed) {
        for (const auto& it : items_) {
            if (keyOf(it) != key) continue;
            if (tbatchQ_.erase(it.id)) queueDirty_ = true;
            if (failed) tfailedKeys_.insert(key);
            else tbatchCancelled_.erase(it.id);
            markItem(it.id);
        }
    }
    // Complete, with its Word file up to date, as far as this run KNOWS (the playing file: one stat of its Word file; any other
    // file is queued and the scheduler reads it at its turn -- never 2000 records at the press).
    bool knownTranscribed(const Item& it) {
        if (loaded_ && s_.itemId == it.id && s_.set) return s_.set->complete && s_.set->hasVoices && docxState(*s_.set) == "current";
        return false;
    }
    void transcriptNow() { transcriptDirty_ = transcriptUrgent_ = true; }   // it CHANGED: posted at the next flush
    void transcriptGrow() { transcriptDirty_ = true; }                      // it only GREW: coalesced (kTranscriptGrowMs)
    // The voices record of this analysis, read once (a stale or malformed record is a miss: the voice pass runs again).
    void ensureVoices(CueSet& S) {
        if (S.voicesRead) return;
        S.voicesRead = true;
        pbtrf::VoiceRec r;
        if (!host_.readVoices(S.key, r) || !pbtrf::samePath(r.path, S.path) || r.size != S.size || r.mtime != S.mtime
            || r.audioOrd != S.audio.ord)
            return;
        S.voices = r;
        S.hasVoices = true;
        docxPathSet(S);                                      // (FX2-B: its Word file on the network?)
    }
    // FX2-B (03/11/2026, docs/PROGRESS_FX2.md): the record's Word file path was just SET (read from its record, written, saved):
    // on the network? -- asked of the host here, never in a tick (the watch only reads the answer)
    void docxPathSet(CueSet& S) { S.docxRemote = !S.voices.docxPath.empty() && host_.onNetwork(w16(S.voices.docxPath)); }
    pbtrf::VoiceRec voicesBase(const CueSet& S) const {
        pbtrf::VoiceRec v;
        v.path = u8(S.path); v.size = S.size; v.mtime = S.mtime;
        v.audioOrd = S.audio.ord; v.audioTrack = S.audio.trackId;
        return v;
    }
    // ---- the source timecode (per media: asked once on a worker, kTcKeep kept) ----
    const pbtc::SourceTc& tcOf(const std::string& mk) const {
        static const pbtc::SourceTc kNone;
        const auto f = tcs_.find(mk);
        return f != tcs_.end() && f->second.known ? f->second.tc : kNone;
    }
    bool tcSettled(const std::string& mk) const {
        const auto f = tcs_.find(mk);
        return f != tcs_.end() && (f->second.known || f->second.timedOut);
    }
    void askTimecode(const std::string& mk, const std::wstring& path) {
        const auto f = tcs_.find(mk);
        if (f != tcs_.end() && (f->second.known || f->second.req)) return;      // known, or asked already
        TcEntry& e = tcs_[mk];
        e.req = next();
        e.askedAt = host_.nowMs();
        tcReqs_[e.req] = mk;
        trimTcs();
        host_.probeTimecode(e.req, path);                    // (last: its answer may come back at once)
    }
    void trimTcs() {
        while (tcs_.size() > kTcKeep) {
            auto old = tcs_.end();
            for (auto it = tcs_.begin(); it != tcs_.end(); ++it)
                if ((!loaded_ || it->first != s_.mediaKey) && (old == tcs_.end() || it->second.askedAt < old->second.askedAt)) old = it;
            if (old == tcs_.end()) break;
            if (old->second.req) tcReqs_.erase(old->second.req);
            tcs_.erase(old);
        }
    }
    // FX (F1, the review 02/11): THE STAMPS' TIMECODE -- the file's SOURCE timecode when this run READ one; else the one its Word
    // file was written with (the voices record's DOCXTC: this media's, by identity -- ensureVoices checks path, size, write time
    // and track -- so a probe that has not answered yet, timed out on a sleeping disk or failed on a locked file never turns a
    // timecoded Word file into file-relative stamps: it is compared, and rewritten, on the basis it was written with); else
    // file-relative. A timecode read now that differs from the recorded one wins (a parser fix); one read on a Word file written
    // without one makes it "older" (true: the file lacks it).
    pbtc::SourceTc stampTc(const CueSet& S) const {
        const pbtc::SourceTc& live = tcOf(S.mediaKey);
        if (live.found) return live;
        if (S.voices.docxTc.found) return sourceTcOf(S.voices.docxTc);
        return pbtc::SourceTc{};
    }
    static pbtrf::VoiceTc voiceTcOf(const pbtc::SourceTc& c) {
        pbtrf::VoiceTc v;
        v.found = c.found; v.origin = c.origin; v.hasRate = c.hasRate; v.startFrames = c.startFrames; v.fpsNum = c.fpsNum;
        v.fpsDen = c.fpsDen; v.base = c.base; v.drop = c.drop; v.startSamples = c.startSamples; v.sampleRate = c.sampleRate;
        return v;
    }
    static pbtc::SourceTc sourceTcOf(const pbtrf::VoiceTc& v) {
        pbtc::SourceTc c;
        c.found = v.found; c.origin = v.origin; c.hasRate = v.hasRate; c.startFrames = v.startFrames; c.fpsNum = v.fpsNum;
        c.fpsDen = v.fpsDen; c.base = v.base; c.drop = v.drop; c.startSamples = v.startSamples; c.sampleRate = v.sampleRate;
        return c;
    }
    // ---- the turns, the Word file ----
    // The transcript of `S` as the tab and the Word file show it: its pieces, their speakers (its voices, by overlap), the
    // user's names, the stamps (the file's source timecode when known, else file-relative -- FX F1: stampTc).
    std::vector<pbtrs::Turn> turnsOf(const CueSet& S, uint64_t* unmatched = nullptr, uint64_t* total = nullptr) const {
        const std::vector<pbtrs::Piece> P = pbtrs::piecesOf(S.cues);
        std::vector<uint32_t> spk;
        if (S.hasVoices && S.voices.on) spk = pbtrs::speakersOf(P, S.voices.segs, unmatched, total);
        else spk.assign(P.size(), 0);
        const std::map<uint32_t, std::string> names(S.voices.names.begin(), S.voices.names.end());
        return pbtrs::buildTurns(P, spk, names, stampTc(S));
    }
    // The transcript's Word file BY ITS IDENTITY: "none" (never written), "gone" (deleted, moved), "edited" (changed since
    // SuVtitle wrote it: never overwritten), "same" (as SuVtitle wrote it -- docxState tells "current" from "older").
    // FX (F2, the user's rule 02/11: ONE Word file per media AND sound track): "other" = this sound track's Word file holds ANOTHER
    // analysis' transcript of it (another pair or model) -- this one has none of its own, or SuVtitle wrote its file for another
    // analysis since (a Save there). Never written by itself; its tab says so -- never "changed outside SuVtitle": SuVtitle wrote it.
    // FX2 (03/11/2026, docs/PROGRESS_FX2.md): the ONE reading the tab (docxState) AND the playing file's row (tdocOf) make of it,
    // so the two never disagree -- never the fingerprint (a row never rebuilds the turns). A reading back to normal (neither
    // "other" nor "gone") forgets what a row learned of that analysis' Word file while it played (docxSeen_).
    std::string docxIdentity(const CueSet& S) {
        const std::string id = docxIdentityOf(S);
        if (id != "other" && id != "gone") docxSeen_.erase(S.key);
        return id;
    }
    std::string docxIdentityOf(const CueSet& S) {
        const pbtrf::VoiceRec& V = S.voices;
        if (!S.hasVoices) return "none";
        if (V.docxPath.empty()) return heldByOther(S, std::wstring()) ? "other" : "none";
        uint64_t size = 0, mtime = 0;
        if (!host_.identity(w16(V.docxPath), size, mtime)) return "gone";
        if (size != V.docxSize || mtime != V.docxMtime) return heldByOther(S, w16(V.docxPath)) ? "other" : "edited";
        return "same";
    }
    // The transcript's Word file: its identity (docxIdentity), and one as SuVtitle wrote it told by what it holds -- "older" (ours,
    // holding an older transcript) or "current".
    std::string docxState(const CueSet& S, const std::vector<pbtrs::Turn>* turns = nullptr) {
        const std::string id = docxIdentity(S);
        if (id != "same") return id;
        return pbtrs::fingerprint(turns ? *turns : turnsOf(S)) == S.voices.docxSig ? "current" : "older";
    }
    // FX2-A (03/11/2026, docs/PROGRESS_FX2.md): THE PLAYING FILE'S WORD FILE, WATCHED ONCE A SECOND (tick) -- deleted, moved,
    // re-created or edited in Word while the film is open and nothing else is posted: the tab and the row read it again,
    // together (transcriptNow + the playing item marked: docxIdentity at their posts -- the ITEM, not markKey(S.key): after a
    // pair or model change while it plays, its shown transcript keeps the old key, which names no row). One stat a tick, of THAT
    // file only (its disk is awake: the file plays) -- its RAW identity, kept with the analysis key and the path it was read for:
    // a new key or path is only kept, a change for the same ones is posted. The registry is read by those posts only
    // (heldByOther), never once a second; a row not playing still reads no disk. The core's own writes (writeDocx, G2,
    // transcriptSaved) change that identity too: the next tick posts ONCE more, the same content (harmless, rare -- G2's own,
    // written in tick before this, rides the same flush).
    // FX2-B (03/11/2026, docs/PROGRESS_FX2.md): ITS GUARDS -- the stat runs on the UI thread, so: never while the window is hidden
    // (the reading kept: a change made meanwhile is posted at the first tick it shows); never for a file on the NETWORK, the
    // media's or the Word file's own (a share that drops blocks a stat up to the SMB timeout -- decided where the paths are set,
    // the tick only reads the answers); and ONE read that took kDocxWatchSlowMs or more stops the watch of that Word file
    // (analysis key + path) until the next load -- at worst one block, as a tab post before FX2-A. These files keep the
    // pre-FX2-A rule: their row and their tab follow at their next post.
    void watchDocx() {
        if (!loaded_ || !s_.set || !s_.set->hasVoices || s_.set->voices.docxPath.empty()) { docxWatch_ = DocxWatch{}; return; }
        if (!shown_) return;
        const CueSet& S = *s_.set;
        if (s_.mediaRemote || S.docxRemote) return;
        if (s_.watchSlowKey == S.key && s_.watchSlowPath == S.voices.docxPath) return;
        DocxWatch w;
        w.key = S.key;
        w.path = S.voices.docxPath;
        const uint64_t t0 = host_.nowMs();
        w.there = host_.identity(w16(w.path), w.size, w.mtime);
        if (host_.nowMs() - t0 >= kDocxWatchSlowMs) { s_.watchSlowKey = w.key; s_.watchSlowPath = w.path; }
        const bool changed = w.key == docxWatch_.key && w.path == docxWatch_.path
            && (w.there != docxWatch_.there || w.size != docxWatch_.size || w.mtime != docxWatch_.mtime);
        docxWatch_ = w;
        if (!changed) return;
        markItem(s_.itemId);
        transcriptNow();
    }
    // FX (F2): the registry says that Word file -- or, with no path, this set's sound track's -- holds ANOTHER analysis' transcript
    bool heldByOther(const CueSet& S, const std::wstring& path) {
        const std::string k = host_.docxWriter(S, path);
        return !k.empty() && k != S.key;
    }
    static const wchar_t* otherSentence() {
        return L"The Word file of this sound track holds another transcript of it (another language pair or model): it is kept.";
    }
    // FX (F6): this analysis has no up-to-date Word file NEXT TO its media -- in another folder (why ""), or none / not updated
    // (why): its row says it (tdocWhy_), and a file of "Transcribe all" counts in the batch's ONE notice (tdocMiss_)
    void docxNotNext(const CueSet& S, const std::wstring& why) {
        if (why.empty()) tdocWhy_.erase(S.key);
        else tdocWhy_[S.key] = why;
        if (inTBatch(S.key)) tdocMiss_.insert(S.key);
        markKey(S.key);
    }
    void postTdocNotice() {
        const size_t n = tdocMiss_.size();
        tdocMiss_.clear();
        notice(n == 1 ? std::wstring(L"Transcribe all: 1 file has no up-to-date Word file next to it -- its row in the Queue tab says why.")
                      : L"Transcribe all: " + std::to_wstring(n) + L" files have no up-to-date Word file next to them -- their rows in "
                        L"the Queue tab say why.", true);   // (FX2: news, not a failure -- `info`)
    }
    pbtrs::TranscriptOut transcriptOutOf(const CueSet& S) {
        pbtrs::TranscriptOut t;
        t.name = S.name;
        t.durMs = S.totalMs;
        t.turns = turnsOf(S);
        t.speakers = pbtrs::namedSpeakers(t.turns);
        t.namesOn = S.hasVoices && S.voices.on;
        t.why = S.hasVoices && !S.voices.on ? S.voices.why : std::string();
        t.langs = pbtrs::languagesHeard(S.cues);
        t.tc = stampTc(S);                                   // (FX F1: the stamps' timecode -- the header says it)
        t.sig = pbtrs::fingerprint(t.turns);
        const std::string st = docxState(S, &t.turns);
        if (st == "current" || st == "older") t.prefer = w16(S.voices.docxPath);
        return t;
    }
    // The Word file, written NOW (the transcript's completion, Re-transcribe, Transcribe all): next to the media -- the one
    // written before replaced in place while it is still ours -- and remembered in the voices record (identity + contents).
    // FX (02/11): F2 -- another analysis of this sound track holds its Word file: NOTHING is written by itself (the tab says so
    // through `other`, the row through `tdoc`); F1 -- the record keeps the timecode the stamps were made with; F4 -- a refusal of
    // the file written before (`t.prefer`: held by another program, read-only -- the host's sentence) is said with "older"
    // (docxErr), the record still naming that file next to the media (the next change, a Save, Transcribe all retry); F6 -- the
    // row's word on its Word file, and Transcribe all's count.
    void writeDocx(CueSet& S) {
        if (docxState(S) == "other") { docxNotNext(S, otherSentence()); return; }
        pbtrs::TranscriptOut t = transcriptOutOf(S);
        if (t.turns.empty()) { S.tnote = L"No speech was found in this file."; docxNotNext(S, S.tnote); return; }
        std::wstring note;
        uint64_t size = 0, mtime = 0;
        const std::wstring path = host_.writeTranscript(S, t, note, size, mtime);
        if (path.empty()) {
            const std::wstring why = note.empty() ? std::wstring(L"The Word file could not be written.") : note;
            if (!t.prefer.empty()) S.docxErr = why;          // (F4: the Word file written before refused -- said with "older")
            else S.tnote = why;
            docxNotNext(S, why);
            return;
        }
        S.tnote = note;                                      // (where it went, when not the usual name: said in the tab)
        S.docxErr.clear();                                   // (U2b G2: written -- an earlier failure to update it is over)
        S.voices.docxPath = u8(path); S.voices.docxSize = size; S.voices.docxMtime = mtime; S.voices.docxSig = t.sig;
        S.voices.docxTc = voiceTcOf(t.tc);                   // (F1: the timecode its stamps were made with)
        docxPathSet(S);                                      // (FX2-B: written on the network? -- a redirected Documents)
        host_.writeVoices(S.key, S.voices);
        vdocx_[S.key] = S.voices.docxPath;                   // (F6: the row's Word file)
        docxSeen_.erase(S.key);                              // (FX2: a gone one written again -- maybe under the same name)
        if (pbtrf::samePath(u8(pbtrf::dirOf(path)), S.folder)) { tdocWhy_.erase(S.key); tdocMiss_.erase(S.key); markKey(S.key); }
        else docxNotNext(S, std::wstring());                 // (F6: in another folder -- Documents\SuVtitle Transcripts)
    }
    // "Transcribe all"'s rule (and a press on a transcript already complete): the Word file when MISSING or OLDER (ours);
    // one the user edited since is left alone.
    void docxIfNeeded(CueSet& S) {
        if (!S.hasVoices) return;
        const std::string st = docxState(S);
        if (st == "none" || st == "gone" || st == "older") writeDocx(S);
        else if (st == "other") docxNotNext(S, otherSentence());   // FX (F2): another analysis' Word file: never written, said
    }
    // ---- U2b (G2, the user's answer 02/11: "a rename updates the Word file by itself") ----
    // A RENAME (and a subtitle EDIT: the same rule) of a COMPLETE transcript: its Word file follows it -- only when SuVtitle wrote
    // that file and nobody changed it since (docxState "older": ours, its identity as written) -- REWRITTEN IN PLACE
    // (Host::rewriteTranscript: never a new name, never another folder). A burst is ONE write: kDocxRenameMs after the LAST change
    // (tick), and at once when the file goes (endSession, a re-analysis / re-track replacing the set, shutdown, persist): the last
    // rename is never lost. "edited" / gone / never written -> never touched (the tab says so; Save .docx... writes a new one).
    void docxSoon(const std::shared_ptr<CueSet>& S) {
        if (!S || !S->complete || !S->hasVoices) return;
        if (docxDue_ && docxDue_ != S) flushDocxDue();      // (another set's burst: written first)
        docxDue_ = S;
        docxDueAt_ = host_.nowMs() + kDocxRenameMs;
    }
    void flushDocxDue() {
        if (!docxDue_) return;
        std::shared_ptr<CueSet> S = docxDue_;
        docxDue_.reset();
        rewriteDocxNow(*S);
    }
    void rewriteDocxNow(CueSet& S) {
        if (!S.complete || !S.hasVoices) return;
        const std::string st = docxState(S);
        if (st != "older") {                                 // current: nothing to do; edited / gone / none: never touched
            if (st == "current") S.docxErr.clear();
            transcriptNow();
            return;
        }
        pbtrs::TranscriptOut t = transcriptOutOf(S);         // (older: `prefer` = the file written for it, still ours --
        if (t.turns.empty()) return;                         //  "older" is the ONE guard here: U1's double-guard rule)
        std::wstring why;
        uint64_t size = 0, mtime = 0;
        if (host_.rewriteTranscript(S, t, why, size, mtime)) {
            S.voices.docxSize = size; S.voices.docxMtime = mtime; S.voices.docxSig = t.sig;   // (the same path: in place)
            S.voices.docxTc = voiceTcOf(t.tc);               // (FX F1: the timecode of what was written)
            S.docxErr.clear();
            host_.writeVoices(S.key, S.voices);
            tdocWhy_.erase(S.key);                           // (FX F6: its row has nothing to excuse any more)
        } else {
            S.docxErr = why;                                 // "open in another program": the next change, or a Save, retries
            if (!why.empty()) tdocWhy_[S.key] = why;         // (FX F6: its row says it too)
        }
        markKey(S.key);
        transcriptNow();
    }
    // ---- the voice pass ----
    // The playing file's voice pass: its transcript wished, its analysis complete (the set shown), no voices yet -- or
    // Re-transcribe -- and the engine free of its analysis (one slot: after it, never with it).
    void ensureCurrentVoice() {
        if (!loaded_ || s_.vwire || s_.wire || !s_.set || !s_.set->complete) return;
        if (!transcribeWantedNow() || s_.tUserCancelled || s_.tFailed) return;
        if (live_ || !helper_) return;
        ensureVoices(*s_.set);
        if (s_.set->hasVoices && !s_.tForce) return;
        for (Job& j : jobs_) if (j.pre) cancelJob(j);        // the engine goes to the playing file (a batch's job resumes later)
        s_.tForce = false;
        s_.vwire = startVoiceJob(s_.set, false);
        stateDirty_ = true;
        transcriptNow();
    }
    // FVOICE for a complete set: its pieces (a dialogue cue's lines split, the known changes marked: TrTranscript.h). 0 = no
    // pass was needed or possible -- no speech, or more pieces than one pass takes: the transcript then stands WITHOUT names
    // (its record says why) and its Word file is written at once.
    uint64_t startVoiceJob(const std::shared_ptr<CueSet>& set, bool pre) {
        const std::vector<pbtrs::Piece> P = pbtrs::piecesOf(set->cues);
        std::string spans, changes;
        std::vector<int> sent;
        pbtrs::voiceFields(P, spans, changes, sent);
        std::vector<tp::MsRange> r;
        tp::parseRanges(spans, r);
        if (r.empty() || r.size() > tp::kVoiceSpanMax) {
            pbtrf::VoiceRec v = voicesBase(*set);
            v.why = r.empty() ? "no speech was found" : "this file has more lines than the voice pass takes ("
                                                        + std::to_string(tp::kVoiceSpanMax) + ")";
            v.names = set->voices.names;
            set->voices = v;
            set->hasVoices = set->voicesRead = true;
            host_.writeVoices(set->key, set->voices);
            if (!r.empty()) writeDocx(*set);
            else docxNotNext(*set, L"No speech was found in this file.");   // (FX F6: its row says why there is no Word file)
            vsummaries_[set->key] = 2;
            markKey(set->key);
            return 0;
        }
        Job j;
        j.wire = next();
        j.set = set;
        j.pre = pre;
        j.voice = true;
        j.vspans = r;
        j.phase = "voices";
        j.vtotal = set->totalMs;
        tp::WireVoice w;
        w.jobId = j.wire;
        w.path = u8(set->path);
        // the sound track: startJob's rule ("" = the first one, a track of a list mpv gave always named) -- written apart, so
        // that startJob's own line stays the only one of its text (cmut.py's WH sentinel aims at it)
        const bool namedTrack = set->audio.ord > 0 || (set->audio.trackId != 0 && mpvListed(set->mediaKey));
        w.audio = namedTrack ? tp::packAudioSel(set->audio) : std::string();
        w.spans = spans;
        w.changes = changes;
        host_.sendVoice(w);
        jobs_.push_back(j);
        markKey(set->key);
        return j.wire;
    }
    // Re-transcribe: each name the user gave goes to the NEW speaker that covers the named one's time best (two names on one
    // new speaker: the larger overlap wins); a name whose speaker is not heard any more is dropped.
    static void carryNames(const pbtrf::VoiceRec& old, pbtrf::VoiceRec& neu) {
        if (old.names.empty()) return;
        std::map<uint32_t, std::map<uint32_t, uint64_t>> ov;            // old speaker -> new speaker -> overlap (ms)
        size_t b = 0;
        for (const auto& o : old.segs) {
            if (!o.spk) continue;
            while (b < neu.segs.size() && neu.segs[b].e <= o.s) ++b;
            for (size_t k = b; k < neu.segs.size() && neu.segs[k].s < o.e; ++k) {
                const pbtrf::VoiceSeg& n = neu.segs[k];
                const uint64_t lo = std::max(o.s, n.s), hi = std::min(o.e, n.e);
                if (n.spk && hi > lo) ov[o.spk][n.spk] += hi - lo;
            }
        }
        std::map<uint32_t, std::pair<std::string, uint64_t>> to;        // new speaker -> (name, the overlap that gave it)
        for (const auto& nm : old.names) {
            const auto f = ov.find(nm.first);
            if (f == ov.end()) continue;
            uint32_t best = 0;
            uint64_t bo = 0;
            for (const auto& x : f->second) if (x.second > bo) { bo = x.second; best = x.first; }
            if (!best) continue;
            auto& slot = to[best];
            if (bo > slot.second) slot = { nm.second, bo };
        }
        neu.names.clear();
        for (const auto& x : to) neu.names.push_back({ x.first, x.second.first });
    }
    // A voice pass ended (FDONE, real or synthesized). Ok with its FSPK: the voices record (its pieces and their labels, the
    // names carried over), then the Word file. Its file leaves "Transcribe all" when done or failed.
    void voiceDone(Job& j, const tp::WireFileDone& d) {
        CueSet& S = *j.set;
        const bool current = loaded_ && s_.vwire == j.wire;
        const bool liveErr = j.liveHit || live_;              // (SUVTITLE LIVE: the gate's refusal fails nothing)
        bool failed = d.result == "error" && !liveErr;
        std::wstring err = w16(d.err);
        if (d.result == "ok" && !j.spkGot) { failed = true; err = L"The voice pass gave no result."; }
        if (current) s_.vwire = 0;
        if (d.result == "ok" && j.spkGot) {
            pbtrf::VoiceRec v = voicesBase(S);
            v.on = j.spk.speakers > 0;
            if (v.on) v.model = j.spk.model;
            else v.why = j.spk.model.empty() ? std::string("the voices could not be told apart") : j.spk.model;
            v.speakers = j.spk.speakers;
            for (size_t i = 0; i < j.vspans.size(); ++i)
                v.segs.push_back(pbtrf::VoiceSeg{ j.vspans[i].s, j.vspans[i].e, v.on ? j.spk.labels[i] : 0u });
            if (S.hasVoices && S.voices.on && v.on) carryNames(S.voices, v);
            // the Word file written for the previous transcript stays this one's: replaced in place just below (still ours)
            v.docxPath = S.voices.docxPath; v.docxSize = S.voices.docxSize; v.docxMtime = S.voices.docxMtime;
            v.docxSig = S.voices.docxSig;
            v.docxTc = S.voices.docxTc;                      // (FX F1: what that file's stamps were made with, until rewritten)
            S.voices = v;
            ++S.vrev;                                        // (FX F8: the speakers are numbered again -- a name typed before is stale)
            S.hasVoices = S.voicesRead = true;
            host_.writeVoices(S.key, S.voices);
            writeDocx(S);
            vsummaries_[S.key] = v.on ? 1 : 2;
        }
        if (current) {
            if (failed) { s_.tFailed = true; s_.terr = err.empty() ? std::wstring(L"The voices could not be recognised.") : err; }
            stateDirty_ = true;
            transcriptNow();
        }
        if (d.result == "ok" || failed) tbatchFinished(S.key, failed);
        markKey(S.key);
        schedule();
        flush();
    }
    int vpct(const Job& j) const {
        return j.vtotal ? static_cast<int>(std::min<uint64_t>(100, j.vdone * 100 / j.vtotal)) : 0;
    }
    int analysisPct(const CueSet& S) const {
        const uint64_t t = S.totalMs;
        if (S.complete) return 100;
        return t ? static_cast<int>(std::min<uint64_t>(100, tp::rangesTotal(S.ranges) * 100 / t)) : 0;
    }
    // The playing file's transcript in one word (the Transcribe slot's tphase, tpct = the work's %):
    //   off      nothing asked (the tab shows its text, if the file was analysed)   waiting  wished: its analysis (pct) or the
    //   engine first   live     wished, held by the live mode   voices   its voice pass runs   done / nonames  complete, the
    //   speakers named / not (why)   error    wished and failed   cancelled  Transcribe's Cancel
    std::string tphaseOf(int* pct = nullptr) const {
        if (pct) *pct = 0;
        if (!loaded_ || !s_.set) return "off";
        const CueSet& S = *s_.set;
        if (s_.vwire) {
            for (const Job& j : jobs_) if (j.wire == s_.vwire && pct) *pct = vpct(j);
            return "voices";
        }
        const bool tw = transcribeWantedNow();
        if (S.hasVoices && S.complete && !(tw && s_.tForce)) return S.voices.on ? "done" : "nonames";
        if (s_.tUserCancelled) return "cancelled";
        if (s_.tFailed || (tw && s_.failed)) return "error";
        if (tw && live_) return "live";
        if (tw) { if (pct) *pct = analysisPct(S); return "waiting"; }
        return "off";
    }
    // The Transcript tab's state: idle | empty (no text, nothing asked) | nospeech | text (analysed, never transcribed) |
    // partial (an analysis stopped part-way) | analysing (its text grows) | waiting | voices | done | nonames | failed | live |
    // cancelled
    std::string tabStateOf(int& pct) const {
        pct = 0;
        if (!loaded_ || !s_.set) return "idle";
        const CueSet& S = *s_.set;
        const std::string ph = tphaseOf(&pct);
        if (S.complete && S.cues.empty()) return "nospeech";
        if (ph == "voices" || ph == "done" || ph == "nonames" || ph == "live" || ph == "cancelled") return ph;
        if (ph == "error") return "failed";
        if (s_.wire) { pct = analysisPct(S); return "analysing"; }
        if (ph == "waiting") return "waiting";
        if (S.cues.empty()) return "empty";
        if (S.complete) return "text";
        pct = analysisPct(S);
        return "partial";
    }
    // A queued file's TRANSCRIPT badge: "" (nothing to say) | queued | analysing (its analysis, pct) | voices (pct) | done |
    // nonames | error | cancelled -- the translation's badge (badge) is unchanged beside it.
    void tbadge(const Item& it, std::string& state, int& pct) {
        state.clear();
        pct = 0;
        if (it.missing) return;
        const std::string k = keyOf(it);
        for (const Job& j : jobs_) if (j.voice && j.set->key == k && !j.cancelSent) { state = "voices"; pct = vpct(j); return; }
        if (loaded_ && s_.itemId == it.id && s_.set) {
            const std::string ph = tphaseOf(&pct);
            if (ph == "waiting" || ph == "live") state = s_.wire ? "analysing" : "queued";
            else if (ph != "off") state = ph;
            else if (tbatchCancelled_.count(it.id)) state = "cancelled";
            else if (s_.set->hasVoices && !s_.set->complete) state = s_.set->voices.on ? "done" : "nonames";
            return;
        }
        if (tfailedKeys_.count(k)) { state = "error"; return; }
        const int v = voicesSummary(it);
        if (tbatchQ_.count(it.id)) {
            for (const Job& j : jobs_)
                if (!j.voice && j.set->key == k && !j.cancelSent) { state = "analysing"; pct = analysisPct(*j.set); return; }
            state = "queued";
            return;
        }
        if (v == 1) { state = "done"; return; }
        if (v == 2) { state = "nonames"; return; }
        if (tbatchCancelled_.count(it.id)) state = "cancelled";
    }
    // Has this file a transcript (its voices record, peeked: the first kSummaryPeekMax files whose analysis is complete)? 0
    // none / 1 named / 2 no names. Remembered per key (refreshSummaries forgets).
    int voicesSummary(const Item& it) {
        const std::string k = keyOf(it);
        const auto f = vsummaries_.find(k);
        if (f != vsummaries_.end()) return f->second;
        int v = 0;
        if (indexOf(it.id) < kSummaryPeekMax) {
            const pbtrf::Summary& sm = summaryFor(it);
            pbtrf::VoiceRec r;
            if (sm.ok && sm.complete && host_.peekVoices(k, r) && pbtrf::samePath(r.path, it.path) && r.size == it.size
                && r.mtime == it.mtime) {
                v = r.on ? 1 : 2;
                vdocx_[k] = r.docxPath;                      // (FX F6: the row's Word file, as its record names it)
            }
        }
        return vsummaries_[k] = v;
    }
    // FX (F6, the review 02/11): what a ROW says of its transcript's Word file -- "" = nothing to say (no transcript; or its Word
    // file is next to the media: only then may the badge's tooltip say "next to it"); else the row's line: the folder it is in
    // ("Word file in Documents\SuVtitle Transcripts"), or why there is none / why it is not up to date (this run's sentence:
    // tdocWhy_), or "No Word file". F2: the playing file whose sound track's Word file is another analysis' says so.
    // FX2 (03/11/2026, docs/PROGRESS_FX2.md): the PLAYING file's row reads its Word file as the tab does (docxIdentity -- never the
    // fingerprint): another analysis' transcript in it (n7: it was asked only for a record with no Word file) says otherSentence;
    // gone (A4: deleted or moved -- the tab drops its name) says "No Word file", over this run's reason and over the folder.
    // Remembered for the row once the file stops playing (docxSeen_); a row not playing never reads the media's disk -- only the
    // record's peek, as before, so a row never seen playing this run says what it said before FX2.
    std::wstring tdocOf(const Item& it) {
        if (it.missing) return std::wstring();
        const std::string k = keyOf(it);
        std::string path;
        if (loaded_ && s_.itemId == it.id && s_.set) {
            const CueSet& S = *s_.set;
            if (!S.hasVoices) return std::wstring();
            path = S.voices.docxPath;
            const std::string id = docxIdentity(S);
            if (id == "other" || id == "gone") { docxSeen_[S.key] = id; return seenWords(id); }
        } else {
            if (!voicesSummary(it)) return std::wstring();
            const auto m = docxSeen_.find(k);                // FX2: what it said while it played (no disk read: kept)
            if (m != docxSeen_.end()) return seenWords(m->second);
            const auto f = vdocx_.find(k);
            if (f != vdocx_.end()) path = f->second;
        }
        const auto w = tdocWhy_.find(k);
        const std::wstring why = w != tdocWhy_.end() ? w->second : std::wstring();
        if (path.empty()) return why.empty() ? std::wstring(L"No Word file") : why;
        const std::wstring dir = pbtrf::dirOf(w16(path));
        if (pbtrf::samePath(u8(dir), it.folder)) return why;   // next to it ("" -- or why it could not be updated)
        return L"Word file in " + shortFolder(dir);
    }
    // FX2: a row's words for what the playing file's reading said of its Word file -- another analysis' transcript in it (n7), or
    // gone (A4: deleted, moved -- F6's "No Word file", over this run's reason and over the folder it was in)
    static std::wstring seenWords(const std::string& id) {
        return id == "gone" ? std::wstring(L"No Word file") : id == "other" ? std::wstring(otherSentence()) : std::wstring();
    }
    // A folder by its last two names ("Documents\SuVtitle Transcripts"); a root's child whole ("D:\Out")
    static std::wstring shortFolder(const std::wstring& dir) {
        std::wstring d = dir;
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        const size_t a = d.find_last_of(L"\\/");
        if (a == std::wstring::npos || a == 0) return d;
        const size_t b = d.find_last_of(L"\\/", a - 1);
        return b == std::wstring::npos ? d : d.substr(b + 1);
    }
    // translatorTranscript: the Transcript tab's whole content (the turns rebuilt each time: names, merges, stamps follow).
    void postTranscriptNow() {
        transcriptDirty_ = transcriptUrgent_ = false;
        transcriptPostedAt_ = host_.nowMs();
        if (windowBusy()) return;                            // replayed on the page's `ready`
        CueSet* S = loaded_ ? s_.set.get() : nullptr;
        int pct = 0;
        const std::string state = tabStateOf(pct);
        uint64_t un = 0, tot = 0;
        const std::vector<pbtrs::Turn> turns = S ? turnsOf(*S, &un, &tot) : std::vector<pbtrs::Turn>();
        JsonW j;
        j.obj();
        j.kv(L"type", L"translatorTranscript");
        j.kv(L"jobId", static_cast<long long>(s_.gen));
        j.kv(L"vrev", static_cast<long long>(S ? S->vrev : 0));   // FX (F8): its speakers' numbering -- a rename names one of these
        j.kv(L"state", w16(state));
        j.kv(L"pct", pct);
        j.kv(L"speakers", static_cast<long long>(S && S->hasVoices ? S->voices.speakers : 0));
        j.key(L"names").arr();
        if (S && S->hasVoices)
            for (const auto& n : S->voices.names) { j.obj(); j.kv(L"id", static_cast<long long>(n.first)); j.kv(L"label", w16(n.second)); j.end(); }
        j.end();
        std::wstring why;
        if (state == "failed") why = s_.tFailed ? s_.terr : s_.err;
        else if (S && S->hasVoices && !S->voices.on) why = w16(S->voices.why);
        j.kv(L"why", why);
        j.key(L"langs").arr();
        if (S) for (const auto& l : pbtrs::languagesHeard(S->cues)) j.str(w16(l));
        j.end();
        j.kv(L"complete", S ? (S->complete && S->hasVoices) : false);
        j.kv(L"unmatched", tot ? static_cast<int>(un * 100 / tot) : 0);
        j.kv(L"suggest", tot && S && S->hasVoices && S->voices.on && static_cast<int>(un * 100 / tot) > kUnmatchedSuggestPct);
        const pbtc::SourceTc tc = S ? stampTc(*S) : pbtc::SourceTc{};   // (FX F1: the stamps' timecode)
        j.kv(L"tc", w16(pbtc::describeTc(tc)));
        j.kv(L"tcSource", tc.found);
        const std::string dst = S ? docxStateConst(*S, turns) : std::string("none");
        j.kv(L"file", S && (dst == "current" || dst == "older" || dst == "edited") ? pbtrf::leafOf(w16(S->voices.docxPath)) : std::wstring());
        // (U2b G2: a rewrite on its way -- a rename's burst -- is not "older": it is being brought up to date; a FAILED one is,
        //  and says why: docxErr; a file changed outside SuVtitle is never touched: `edited`)
        j.kv(L"older", dst == "older" && !(docxDue_ && docxDue_.get() == S));
        j.kv(L"docxErr", S && dst == "older" ? S->docxErr : std::wstring());
        j.kv(L"edited", dst == "edited");
        j.kv(L"other", dst == "other");                      // FX (F2): this sound track's Word file holds another analysis'
        j.kv(L"note", S ? S->tnote : std::wstring());
        j.kv(L"durMs", static_cast<long long>(S ? S->totalMs : 0));
        j.key(L"turns").arr();
        for (size_t i = 0; i < turns.size() && i < kTurnsPostMax; ++i) {
            const pbtrs::Turn& t = turns[i];
            j.obj();
            j.kv(L"s", static_cast<long long>(t.s));
            j.kv(L"e", static_cast<long long>(t.e));
            j.kv(L"spk", static_cast<long long>(t.spk));
            j.kv(L"name", w16(t.name));
            j.kv(L"tc", w16(t.stamp));
            j.kv(L"text", w16(t.text));
            j.end();
        }
        j.end();
        j.kv(L"cut", turns.size() > kTurnsPostMax);
        j.end();
        host_.postWindow(j.take());
    }
    // docxState for a post (the turns already built)
    std::string docxStateConst(const CueSet& S, const std::vector<pbtrs::Turn>& turns) { return docxState(S, &turns); }

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
        forgetBatch(it.id);                                // (U1: a file taken out of the queue leaves "Translate all")
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
        s->asrPath = asrPick_.path; s->mtPath = mtPick_.path; s->models = modelsTag_;   // U2
        s->key = pbtrf::cacheKey(path, size, mtime, langA_, langB_, mode_, quality_, audio.ord, modelsTag_);
        return s;
    }
    std::string keyOf(const Item& it) const {
        return pbtrf::cacheKey(it.path, it.size, it.mtime, langA_, langB_, mode_, quality_, audioFor(it).ord, modelsTag_);
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
            const int o = chooseOrd(p->second, pick);
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
    // D3's rule (initialAudioOrd) -- and for a list MPV gave (1.1.0 fix round, L6), with no pick of the viewer's, the track
    // mpv chose by itself at its first opening (autoOrd): every opening of the file in the run plays what the first one did
    static int chooseOrd(const ProbeEntry& p, const AudioPick* pick) {
        const pbmedia::Info& t = *p.info;
        const bool picked = pick && pick->ord < t.audio.size() && (pick->trackId == 0 || t.audio[pick->ord].track == pick->trackId);
        if (!picked && p.autoOrd >= 0 && static_cast<size_t>(p.autoOrd) < t.audio.size()) return p.autoOrd;
        return initialAudioOrd(t, pick);
    }
    // The media's track list is one MPV gave (adopted: container "mpv"), not MediaTracks'
    bool mpvListed(const std::string& mk) const {
        const auto p = probes_.find(mk);
        return p != probes_.end() && p->second.state == "ready" && p->second.info && p->second.info->container == "mpv";
    }
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
        // 1.1.0 (fix round, L6): MediaTracks could not read it ("none" / "error") and mpv listed its sound tracks already:
        // mpv's list is the item's then (adoptMpvTracks: the viewer's pick, else mpv's own choice), and the job starts
        if (!s_.tracks && s_.mpvTracks && !s_.mpvTracks->audio.empty()) {
            adoptMpvTracks();
            ensureCurrentJob(s_.lastPosMs);
            return;
        }
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
    // 1.1.0 (fix round, L6): mpv's list becomes the item's (see offerMpvTracks). Offered again (MPEG-TS: a stream that
    // appears later): the list is replaced, the track chosen is kept. The choice, the first time: the viewer's pick for this
    // file (player-audio.txt: persisted like a Matroska file's -- setAudio stores mpv's track id with it), else the track
    // mpv chose itself (aid=auto: the opening named none -- WAITED for, mpv's list can come before its choice), else D3.
    void adoptMpvTracks() {
        if (!loaded_ || !s_.mpvTracks || s_.mpvTracks->audio.empty()) return;
        if (s_.tracksState == "probing") return;                            // MediaTracks is reading it: its answer first
        if (s_.tracksState == "ready" && s_.tracks && !s_.tracksFromMpv) return;   // a MediaTracks list: NEVER overridden
        const pbmedia::Info& T = *s_.mpvTracks;
        const size_t n = T.audio.size();
        const bool first = !(s_.tracksFromMpv && s_.tracks && s_.audioOrd >= 0);
        const auto pk = picks_.find(s_.mediaKey);
        const AudioPick* pick = pk == picks_.end() ? nullptr : &pk->second;
        const bool picked = pick && pick->ord < n && (pick->trackId == 0 || T.audio[pick->ord].track == pick->trackId);
        const bool mpvChose = s_.mpvOrd >= 0 && static_cast<size_t>(s_.mpvOrd) < n;
        // (the first time with no pick: mpv's OWN choice first -- adopting before it would put D3's track over it)
        if (first && !picked && !mpvChose && !s_.mpvFinal) return;
        s_.tracks = s_.mpvTracks;
        s_.tracksState = "ready";
        s_.tracksFromMpv = true;
        // remembered for this media during the run (probes_: never over a probe still running): its next opening knows the
        // list at once -- mpv is asked for the track chosen, the analysis keys on it, the pre-analysis too
        ProbeEntry& pe = probes_[s_.mediaKey];
        if (pe.state != "probing") {
            pe.state = "ready"; pe.info = s_.mpvTracks; pe.req = 0; pe.seq = ++probeSeq_;
            if (first && mpvChose && !picked) pe.autoOrd = s_.mpvOrd;
        }
        trimProbes();
        int o;
        if (!first && static_cast<size_t>(s_.audioOrd) < n) o = s_.audioOrd;                 // offered again: kept
        else if (picked) o = static_cast<int>(pick->ord);
        else if (mpvChose) o = s_.mpvOrd;                                                      // what aid=auto picked
        else o = initialAudioOrd(T, nullptr);
        s_.audioOrd = o;
        stateDirty_ = true;
        markItem(s_.itemId);
        if (!first || !s_.set) return;
        // THE ANALYSIS. A set named after a track (the viewer's pick, provisional) moves if it is another one. The opening's
        // set knew no list: "" -- the engine's mpv decodes what aid=auto picks (TrMpvAudio.h), mpvOrd, once its job runs.
        // It STAYS when that is the track chosen (the director's rule: an analysis running on aid=auto's pick is never
        // restarted) and is NAMED now (a job sent again asks for it by its id, never aid=auto again). It moves when it is
        // another track: the viewer's pick -- or mpv's own choice when it is not the first and the job ran before mpv chose
        // (the wait for its list ran out): its cues are then said to be that track's (the AUD line: never loaded as the
        // first track's -- loadCacheInto's guard) and the track's own set comes in.
        // (U1: "translating" = the file is WANTED; one that is not moves to the track chosen only if that track has an
        //  analysis to show -- followsSound -- and nothing starts: the rule above is untouched, a running job is renamed.)
        tp::AudioSel& a = s_.set->audio;
        const pbmedia::AudioTrack& t = T.audio[static_cast<size_t>(o)];
        if (a.trackId != 0) {
            if (a.ord != t.ord && followsSound(heardSel())) retrack(heardSel());
            return;
        }
        const int decodes = s_.wire ? s_.mpvOrd : -1;
        if (decodes >= 0 && static_cast<uint32_t>(decodes) != a.ord) {
            a = tp::AudioSel{ static_cast<uint32_t>(decodes), T.audio[static_cast<size_t>(decodes)].track };
            retrack(heardSel());
            return;
        }
        if (a.ord == t.ord) { a.trackId = t.track; return; }
        if (followsSound(heardSel())) retrack(heardSel());
    }
    // The job held for mpv's list (a container MediaTracks does not read, that mpv plays) starts: the list is adopted, or
    // mpv lists no sound track, or its choice is final, or it let the item go. (Else the kProbeWaitMs timer starts it.)
    void mpvWaitDone() {
        if (!loaded_ || !s_.probeWait || probeWorthy(s_.path)) return;
        if (!s_.tracksFromMpv && s_.mpvTracks && !s_.mpvFinal) return;   // a list, mpv's own choice still to come
        s_.probeWait = false;
        ensureCurrentJob(s_.lastPosMs);
    }
    // A NEW GENERATION on another sound track of the same item (D5): the set going away writes what it owes (an edit, its
    // files), its job is cancelled (its FDONE saves its partial), and the track's own set comes in -- a pre-analysis'
    // job adopted, a cancelled job's vouched part, its cache record, or nothing yet; then its job (when translating).
    // An opening (the page has reported nothing yet) keeps its start and autoplay; later it is the same file going on
    // where the user has it (review of WE, C5). `fresh` = the set shown was a pre-WH record the probe now distrusts: none of
    // its cues come back (not through the cancelled job's set either) -- analysed from nothing.
    void retrack(const tp::AudioSel& want, bool fresh = false) {
        flushDocxDue();                                      // (U2b G2: the set going away first writes its renamed Word file)
        if (s_.set) {
            if (s_.set->editDirty) saveEdits(*s_.set);
            else if (s_.set->srtDirty) writeSrts(*s_.set);
        }
        if (s_.wire) { if (Job* j = job(s_.wire)) cancelJob(*j); s_.wire = 0; }
        if (s_.vwire) { if (Job* vj = job(s_.vwire)) cancelJob(*vj); s_.vwire = 0; }   // U3: the other track's voice pass
        auto set = makeSet(s_.path, s_.name, s_.folder, s_.size, s_.mtime, want);
        if (Job* pj = jobForKey(set->key)) { set = pj->set; pj->pre = false; s_.wire = pj->wire; }
        else if (Job* vj = voiceJobForKey(set->key)) { set = vj->set; vj->pre = false; s_.vwire = vj->wire; }   // (U3)
        else if (fresh) { /* nothing: a new analysis */ }
        else if (const CueSet* held = heldSet(set->key); held && held->audio.ord == want.ord) set = vouchedCopy(*held);
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
        ensureVoices(*s_.set);                               // U3: this track's transcript, if it has one
        transcriptNow();
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
        for (const auto& k : pbtrf::siblingKeys(S.path, S.size, S.mtime, lang, { S.langA, S.langB }, S.audio.ord, S.models)) keys.push_back(k.key);
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

    // U1: a file LOADED is read (its cache shown) and analysed only if wanted -- `want` (open's analyseFirst), or queued by
    // "Translate all"; otherwise nothing starts (the viewer's Translate does it: analyse).
    bool loadItem(size_t index, bool autoplay, long long startOverride, bool want = false) {
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
        n.mediaRemote = host_.onNetwork(it.path);            // (FX2-B: a file on a share is never watched once a second)
        n.host = L"f" + w16(pbtrf::folderKey(it.folder)) + kMediaHostSuffix;
        n.url = L"https://" + n.host + L"/" + w16(pbtrf::pctEncode(u8(it.name))) + L"?v=" + w16(n.mediaKey);
        n.autoplay = autoplay;
        n.wanted = want;                                     // U1: asked for with the opening (else: Translate, the batch)
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
        // (1.1.0 fix round, L6: a container MediaTracks does not read whose list MPV gave earlier in this run is KNOWN too:
        //  its "ready" entry IS that adopted list -- the opening asks mpv for the track chosen, the analysis keys on it)
        const auto pr0 = probes_.find(n.mediaKey);
        const bool adoptedBefore = !worthy && pr0 != probes_.end() && pr0->second.state == "ready" && pr0->second.info;
        const auto pr = worthy || adoptedBefore ? pr0 : probes_.end();
        const bool known = pr != probes_.end() && pr->second.state != "probing" && !pr->second.state.empty();
        n.tracksState = !worthy && !adoptedBefore ? "none" : known ? pr->second.state : "probing";
        n.probeWait = worthy && !known;
        n.tracksFromMpv = adoptedBefore;
        n.probeAt = host_.nowMs();
        // the cue set: a batch job ("Translate all", U1) of THIS file, pair and track is ADOPTED (no model reload); else
        // the cache -- SHOWN as it is, complete or partial, whether or not anything is analysed; else nothing yet
        const std::string key = keyOf(it);
        Job* pj = jobForKey(key);
        Job* vj = pj ? nullptr : voiceJobForKey(key);
        if (pj) { n.set = pj->set; n.wire = pj->wire; pj->pre = false; }
        else if (vj) { n.set = vj->set; n.vwire = vj->wire; vj->pre = false; }     // U3: "Transcribe all"'s voice pass goes on
        else { n.set = makeSet(it.path, it.name, it.folder, size, mtime, audioFor(it)); loadCacheInto(*n.set); }
        s_ = std::move(n);
        ensureVoices(*s_.set);                               // U3: its transcript, if it has one (shown with no press)
        transcriptNow();
        loaded_ = true;
        if (known) {                                             // (the same choice as audioFor: no re-track here)
            s_.tracks = pr->second.state == "ready" ? pr->second.info : nullptr;
            if (s_.tracks) for (const auto& st : s_.tracks->subs) s_.embedded.push_back(Session::Emb{ next(), st.track });
            const auto pk = picks_.find(s_.mediaKey);
            s_.audioOrd = s_.tracks ? chooseOrd(pr->second, pk == picks_.end() ? nullptr : &pk->second) : -1;
        }
        // 1.1.0 (fix round, L6): a container MediaTracks does not read, that MPV plays: the job WAITS for mpv's track list and
        // its choice (at most kProbeWaitMs, like a probe: mpv knows both within a fraction of a second) and then starts on the
        // track chosen, named -- not on "" (aid=auto's pick, maybe not the first) only to move a moment later
        if (!worthy && !adoptedBefore && host_.mpvPlays()) s_.probeWait = true;
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
        if (s_.probeWait && worthy) requestProbe(s_.mediaKey, s_.path);   // (last: its answer may come back at once)
        askTimecode(s_.mediaKey, s_.path);                   // U3: its source timecode, for the stamps (a worker; at once)
        schedule();
        return true;
    }
    // The current item goes: its job is cancelled (its FDONE saves the partial), its proxy deleted. U1: unless "Translate
    // all" still wants that file -- its job then goes on as the batch's (a job for a file that is not playing; the next
    // item, if it is wanted, takes the engine from it: ensureCurrentJob). A press was for THIS file while it played: its
    // job stops (only the batch analyses files that are not playing).
    void endSession() {
        if (!loaded_) return;
        flushDocxDue();                                      // U2b (G2): a renamed transcript's Word file at once -- never lost
        // WG: an item switched away from (or closed) leaves its files and its edits up to date
        if (s_.set && s_.set->editDirty) saveEdits(*s_.set);
        else if (s_.set && s_.set->srtDirty) writeSrts(*s_.set);
        if (s_.wire) {
            if (Job* j = job(s_.wire)) {
                if ((batchQ_.count(s_.itemId) || tbatchQ_.count(s_.itemId)) && !j->cancelSent) j->pre = true;
                else cancelJob(*j);
            }
        }
        if (s_.vwire) {                                      // U3: its voice pass -- the batch's, or this file's press only
            if (Job* vj = job(s_.vwire)) {
                if (tbatchQ_.count(s_.itemId) && !vj->cancelSent) vj->pre = true;
                else cancelJob(*vj);
            }
        }
        endProxy();
        cancelSubExtract();                                  // WH: an embedded track still being read is for this item only
        s_.extSubChunks.clear();
    }
    void ensureCurrentJob(uint64_t focus) {
        if (!loaded_ || s_.wire || !s_.set || s_.set->complete) return;
        if (!analysisWanted()) return;                       // U1: never by itself -- Translate, or "Translate all" (U3: or
                                                             // the transcript's wish: Transcribe, "Transcribe all")
        // (U2b, U1's double-guard rule: `s_.userCancelled ||` went from here -- it could never be true behind the line above.
        //  Every cancel that sets it also ends the wishes (cancel: wanted = false; transcriptCancel / cancelAll: both), and every
        //  path that wishes again clears it (analyse -> runAnalysis, analyseAll, transcribe, transcribeAll, the batch's redo,
        //  retrack, a new Session) or meets a COMPLETE set (returned above). Proof: docs/PROGRESS_U2b.md, DECISIONS. Its sentinel
        //  is aimed at cancel()'s `s_.wanted = false;`, the one guard left.)
        if (s_.failed) return;
        if (s_.probeWait) return;                            // WH: which sound track is not known yet (<= kProbeWaitMs)
        if (live_) return;                                   // SUVTITLE LIVE: held while the live mode listens (setLive)
        if (!helper_) {
            if (s_.err.empty()) { s_.err = L"The SuVtitle engine is not running: files cannot be analysed."; stateDirty_ = true; }
            return;
        }
        if (s_.err == L"The SuVtitle engine is not running: files cannot be analysed.") s_.err.clear();
        for (Job& j : jobs_) if (j.pre) cancelJob(j);     // the helper goes to the current item (a batch job resumes later)
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
        w.asrModel = set->asrPath;      // U2: the models this set is made with ("" = the default: the pre-U2 line)
        w.mtModel = set->mtPath;
        // WH: the sound track -- "" for the first one, so the wire and every cache record are exactly what they were. 1.1.0
        // (fix round, L6): except a track of a list MPV gave -- for the engine's mpv "" is aid=auto's pick (TrMpvAudio.h),
        // which may not be the first: a track chosen there is always named
        w.audio = set->audio.ord > 0 || (set->audio.trackId != 0 && mpvListed(set->mediaKey)) ? tp::packAudioSel(set->audio)
                                                                                               : std::string();
        set->savedAt = host_.nowMs();
        host_.sendFile(w);
        jobs_.push_back(j);
        noteModelsInUse();                                   // U2b: an imported model this job names is in use now
        for (uint64_t id : batchItemsOf(*set)) redoStarted(id);   // FX (F5): a redo granted to its file has STARTED
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
    Job* jobForKey(const std::string& key) {                 // the ANALYSIS job of this key (U3: never a voice pass)
        for (Job& j : jobs_) if (j.set->key == key && !j.cancelSent && !j.voice) return &j;
        return nullptr;
    }
    Job* voiceJobForKey(const std::string& key) {            // U3: the voice pass running on this key's analysis
        for (Job& j : jobs_) if (j.set->key == key && !j.cancelSent && j.voice) return &j;
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

    // ---- "Translate all" (U1 -- it replaced the pre-analysis of the next items, D14) ------------------------------------
    void cancelPre() { for (Job& j : jobs_) if (j.pre) cancelJob(j); }
    void cancelPreFor(const Item& it) {
        const std::string k = keyOf(it);
        for (Job& j : jobs_) if (j.pre && j.set->key == k) cancelJob(j);
    }
    void unqueue(uint64_t id) { if (batchQ_.erase(id)) markItem(id); }
    // At most ONE job runs: the playing file's first -- when it is WANTED (U1). Then "Translate all": the FIRST file it
    // still wants in the LIST's order (the playing one is ensureCurrentJob's), one at a time; a file done, failed or
    // missing leaves it. Nothing else ever starts: no file is analysed ahead because it comes next (U1).
    void schedule() {
        if (!helper_ || live_) { cancelPre(); return; }   // (SUVTITLE LIVE: the gate, setLive)
        if (loaded_ && s_.set && s_.set->complete) {
            redoStarted(s_.itemId);                                       // (FX F5: a redo met by the default's analysis: ended)
            unqueue(s_.itemId);                                           // the playing file, done
            // U3: ...and its transcript done: its Word file up to date (its stamps known first), it leaves "Transcribe all"
            if (tbatchQ_.count(s_.itemId) && s_.set->hasVoices && !s_.vwire && !s_.tForce && tcSettled(s_.mediaKey)) {
                docxIfNeeded(*s_.set);
                tunqueue(s_.itemId);
            }
        }
        ensureCurrentJob(loaded_ ? s_.lastPosMs : 0);
        ensureCurrentVoice();                                             // U3: then its voice pass (one slot: after it)
        for (const Job& j : jobs_) if (!j.pre && !j.cancelSent) return;   // the current item's job runs (FILE or FVOICE)
        // the playing file wants the engine and has it in a moment (its sound track is being read): nothing else meanwhile
        if (analysisWanted() && s_.set && !s_.set->complete && !s_.userCancelled && !s_.failed && s_.probeWait) return;
        // (a batch job whose file leaves the batch is cancelled where it leaves: Cancel, Cancel all, the file removed or
        //  cleared from the queue, a press-only file left -- endSession; a file done or failed leaves it in onDone)
        for (const Job& j : jobs_) if (j.pre && !j.cancelSent) return;                 // one at a time
        if (jobs_.size() >= 2) return;              // two cancelled jobs still dying: let them go first
        if (batchQ_.empty() && tbatchQ_.empty()) return;
        const uint64_t curId = loaded_ ? s_.itemId : 0;
        for (const Item& it : items_) {
            const bool tr = batchQ_.count(it.id) > 0, tx = tbatchQ_.count(it.id) > 0;   // U3: in either batch
            if ((!tr && !tx) || it.id == curId) continue;
            if (it.missing) { unqueue(it.id); tunqueue(it.id); continue; }
            // WH: a container's sound track must be known before it is analysed -- its probe first (on a worker; its
            // answer schedules again). One at a time, in the list's order: the next file is never overtaken.
            if (probeWorthy(it.path)) {
                const std::string mk = pbtrf::mediaKey(it.path, it.size, it.mtime);
                const auto p = probes_.find(mk);
                if (p == probes_.end() || p->second.state.empty()) { requestProbe(mk, it.path); return; }
                if (p->second.state == "probing") return;
            }
            // (a file that failed left the batch in onDone -- batchFinished: Translate all asks it again, nothing else)
            const pbtrf::Summary& sm = summaryFor(it);
            std::shared_ptr<CueSet> set;
            if (!(sm.ok && sm.complete)) {
                set = makeSet(it.path, it.name, it.folder, it.size, it.mtime, audioFor(it));
                loadCacheInto(*set, false);
                if (!set->complete) { startJob(set, 0, true); return; }       // its analysis, for either batch
            }
            redoStarted(it.id);                                           // (FX F5: a redo met by the default's analysis: ended)
            if (tr) unqueue(it.id);
            if (!tx) continue;
            // U3: "Transcribe all" -- the file's analysis is complete: its voice pass if it has none, else its Word file if
            // missing or older (ours); its stamps' timecode first (asked on a worker, kTcWaitMs at most)
            if (!set) {
                set = makeSet(it.path, it.name, it.folder, it.size, it.mtime, audioFor(it));
                loadCacheInto(*set, false);
                if (!set->complete) { startJob(set, 0, true); return; }       // (a summary that said more than its record)
            }
            ensureVoices(*set);
            if (!tcSettled(set->mediaKey)) { askTimecode(set->mediaKey, it.path); return; }   // (its answer schedules again)
            if (!set->hasVoices && startVoiceJob(set, true)) return;
            docxIfNeeded(*set);
            tunqueue(it.id);
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
            // 1.1.0 (fix round, L2): NOT while mpv plays the item (it reads the file itself; the copy is the <video>'s, and
            // forcing the first sound track for it would undo the viewer's pick in mpv): an item that falls back with no
            // URL asks for it then (the page's translatorFilePlayFailed -> playFailed).
            s_.url.clear();
            if (!host_.mpvPlays()) requestProxy();
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
        if (s_.set && s_.set->audio.ord != 0 && followsSound(heardSel())) retrack(heardSel());   // (U1: as setAudio)
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
        q.repeat = repeat_; q.shuffle = shuffle_;
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
        vsummaries_.clear();                                 // (U3: the transcripts' peeks follow the keys)
        vdocx_.clear();                                      // (FX F6: their Word files with them)
        for (size_t i = 0; i < items_.size() && i < kSummaryPeekMax; ++i) summaryFor(items_[i]);
        queueDirty_ = true;
    }
    // A file's badge in the queue: none | missing | analysing (pct) | ready (complete -- "done") | partial (pct) | error (the
    // analysis failed) | U1: queued ("Translate all" still has it to do; pct = what it has) | cancelled (its analysis was
    // cancelled: Cancel, Cancel all -- until it is asked again).
    void badge(const Item& it, std::string& state, int& pct) {
        state = "none";
        pct = 0;
        if (it.missing) { state = "missing"; return; }
        const std::string k = keyOf(it);
        auto pctOf = [](const std::vector<tp::MsRange>& r, uint64_t total) {
            return total ? static_cast<int>(std::min<uint64_t>(100, tp::rangesTotal(r) * 100 / total)) : 0;
        };
        for (const Job& j : jobs_)
            if (!j.voice && j.set->key == k && !j.cancelSent) { state = "analysing"; pct = pctOf(j.set->ranges, j.set->totalMs); return; }
        if (loaded_ && s_.itemId == it.id && s_.set) {
            if (s_.set->complete) { state = "ready"; pct = 100; return; }
            if (!s_.set->confirmed.empty()) { state = "partial"; pct = pctOf(s_.set->confirmed, s_.set->totalMs); }
            if (s_.failed) state = "error";
            else if (batchQ_.count(it.id)) state = "queued";
            else if (batchCancelled_.count(it.id)) state = "cancelled";
            return;
        }
        if (failedKeys_.count(k)) { state = "error"; return; }
        if (indexOf(it.id) < kSummaryPeekMax || summaries_.count(k)) {
            const pbtrf::Summary& s = summaryFor(it);
            if (s.ok && s.complete) { state = "ready"; pct = 100; return; }
            if (s.ok && !s.ranges.empty()) { state = "partial"; pct = pctOf(s.ranges, s.totalMs); }
        }
        if (batchQ_.count(it.id)) state = "queued";
        else if (batchCancelled_.count(it.id)) state = "cancelled";
    }
    void markItem(uint64_t id) { if (id) itemDirty_.insert(id); }
    void markKey(const std::string& key) {
        for (const auto& it : items_) if (keyOf(it) == key) itemDirty_.insert(it.id);
    }

    // ---- the posts ------------------------------------------------------------------------------------------------------
    std::string phaseOf() const {
        if (!loaded_) return "idle";
        // SUVTITLE LIVE: the analysis is HELD while the live mode listens -- said as such, never as loading (the page
        // would wait for subtitles that are not coming: "play when ready") nor as an error. A job the gate cancelled
        // may still be dying (its FDONE on its way): it is already this. (U1: for a file WANTED -- one nobody asked for is
        // "off" whatever Live does.)
        // U1: "off" = loaded, nothing asked for it (no press, not in "Translate all"): its cache -- complete, partial or
        // none -- is shown and nothing runs; the strip offers Translate.
        const bool want = analysisWanted();                // (U3: either wish)
        if (live_ && want && s_.set && !s_.set->complete && !s_.userCancelled && !s_.failed) return "live";
        if (s_.wire) {
            for (const Job& j : jobs_) if (j.wire == s_.wire) return j.phase;
            return "loading";
        }
        if (s_.set && s_.set->complete) return "done";
        if (s_.cancelling || s_.userCancelled) return "cancelled";
        if (s_.failed) return "error";                     // (U1: asked for, and failed -- said until asked again)
        if (!s_.err.empty()) return want ? "error" : "off";
        return want ? "loading" : "off";
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
        j.kv(L"modelsStale", S ? S->models != modelsTag_ : false);   // U2: made with other models than the ones chosen now
        j.kv(L"redo", s_.redo);                       // U2b: a batch redoes this file with the default model -- why ("" = not)
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
        j.kv(L"translate", wantedNow());              // U1: THIS file is being analysed or asked to be (press / batch)
        {   // U3: the TRANSCRIPT's wish (Transcribe pressed, or "Transcribe all"), its phase and its % (tphaseOf)
            int tpct = 0;
            const std::string tph = tphaseOf(&tpct);
            j.kv(L"transcribe", transcribeWantedNow());
            j.kv(L"tphase", w16(tph));
            j.kv(L"tpct", tpct);
        }
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
            std::string tst;                                   // U3: the transcript's badge
            int tpc = 0;
            tbadge(it, tst, tpc);
            j.kv(L"tstate", w16(tst));
            j.kv(L"tpct", tpc);
            j.kv(L"redo", redoRan_.count(it.id) > 0);          // U2b: a batch redid it with the default model (FX F5: STARTED)
            j.kv(L"batch", batchQ_.count(it.id) > 0 || tbatchQ_.count(it.id) > 0);   // FX (F7): what Cancel all acts on
            j.kv(L"tdoc", tdocOf(it));                         // FX (F6): where its Word file is, or why there is none
            j.end();
        }
        j.end();
        j.kv(L"cur", static_cast<long long>(cur_ != npos ? items_[cur_].id : 0));
        j.kv(L"loaded", loaded_);
        j.kv(L"repeat", w16(repeatName(repeat_)));
        j.kv(L"shuffle", shuffle_);
        // (U1: no `translate` any more -- there is no global switch; each item's badge says what is being done for it)
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
        std::string tst;                                       // U3: the transcript's badge
        int tpc = 0;
        tbadge(items_[i], tst, tpc);
        j.obj().kv(L"type", L"playerItem").kv(L"id", static_cast<long long>(id)).kv(L"state", w16(st)).kv(L"pct", pct)
               .kv(L"tstate", w16(tst)).kv(L"tpct", tpc).kv(L"redo", redoRan_.count(id) > 0)   // (U2b: redo; FX F5: STARTED)
               .kv(L"batch", batchQ_.count(id) > 0 || tbatchQ_.count(id) > 0)                // (FX F7)
               .kv(L"tdoc", tdocOf(items_[i])).end();                                       // (FX F6)
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
